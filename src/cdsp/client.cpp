// SPDX-License-Identifier: MIT
#include "client.h"
#include "iris_vpp.h"
#include "skel.h"
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct cdsp_vpp_context {
    remote_handle64 handle = 0;
    int workers;
    unsigned src_width = 0, src_height = 0, dst_width = 0, dst_height = 0;
    std::mutex lock;
};
struct cdsp_vpp_buffer {
    int fd = -1;
    size_t size = 0;
    void *map = MAP_FAILED;
    dev_t dev;
    ino_t ino;
    bool dsp_mapped = false;
    cdsp_vpp_buffer *mapped_next = nullptr;
};
// The FastRPC library can close its session when the last RPC handle closes.
// Retire our persistent maps before that happens, even when imported buffers
// outlive all contexts. A later context will map those buffers again lazily.
static std::mutex mapping_lock;
static cdsp_vpp_buffer *mapped_buffers = nullptr;
static unsigned active_contexts = 0;
extern "C" int cdsp_vpp_create(cdsp_vpp_context **out, int workers, int max_power) {
    if (!out || workers < 0 || workers > CDSP_VPP_MAX_WORKERS)
        return -EINVAL;
    *out = nullptr;
    auto *ctx = new (std::nothrow) cdsp_vpp_context;
    if (!ctx)
        return -ENOMEM;
    std::lock_guard<std::mutex> mappings(mapping_lock);
    remote_rpc_control_unsigned_module unsigned_session{CDSP_DOMAIN_ID, 1};
    int e = remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE, &unsigned_session,
                                   sizeof(unsigned_session));
    if (!e)
        e = cdsp_vpp_select_workers(&workers);
    if (!e)
        e = cdsp_vpp_open_skel(&ctx->handle);
    if (!e && max_power) {
        int mask = 0;
        e = iris_vpp_power(ctx->handle, &mask);
        if (!e && mask != 7)
            e = -EIO;
    }
    if (e) {
        if (ctx->handle)
            iris_vpp_close(ctx->handle);
        delete ctx;
        return e;
    }
    ctx->workers = workers;
    active_contexts++;
    *out = ctx;
    return 0;
}
extern "C" void cdsp_vpp_destroy(cdsp_vpp_context *ctx) {
    if (ctx) {
        std::lock_guard<std::mutex> mappings(mapping_lock);
        if (active_contexts == 1) {
            while (mapped_buffers) {
                auto *buffer = mapped_buffers;
                fastrpc_munmap(CDSP_DOMAIN_ID, buffer->fd, buffer->map, buffer->size);
                buffer->dsp_mapped = false;
                mapped_buffers = buffer->mapped_next;
                buffer->mapped_next = nullptr;
            }
        }
        iris_vpp_close(ctx->handle);
        active_contexts--;
        delete ctx;
    }
}
extern "C" int cdsp_vpp_get_info(cdsp_vpp_context *ctx, uint32_t *hz, int *units) {
    if (!ctx || !hz || !units)
        return -EINVAL;
    std::lock_guard<std::mutex> guard(ctx->lock);
    return iris_vpp_info(ctx->handle, hz, units);
}
extern "C" int cdsp_vpp_import(int fd, size_t size, cdsp_vpp_buffer **out) {
    if (!out || fd < 0 || !size || size > INT_MAX)
        return -EINVAL;
    *out = nullptr;
    struct stat st{};
    if (fstat(fd, &st))
        return -errno;
    if (st.st_size <= 0 || size > (uint64_t)st.st_size)
        return -EINVAL;
    auto *buffer = new (std::nothrow) cdsp_vpp_buffer;
    if (!buffer)
        return -ENOMEM;
    buffer->size = size;
    buffer->dev = st.st_dev;
    buffer->ino = st.st_ino;
    buffer->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (buffer->fd < 0) {
        int e = -errno;
        delete buffer;
        return e;
    }
    buffer->map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if (buffer->map == MAP_FAILED) {
        int e = -errno;
        close(buffer->fd);
        delete buffer;
        return e;
    }
    remote_register_buf(buffer->map, (int)size, buffer->fd);
    *out = buffer;
    return 0;
}
extern "C" void cdsp_vpp_release(cdsp_vpp_buffer *buffer) {
    if (buffer) {
        std::lock_guard<std::mutex> mappings(mapping_lock);
        if (buffer->dsp_mapped) {
            fastrpc_munmap(CDSP_DOMAIN_ID, buffer->fd, buffer->map, buffer->size);
            auto **entry = &mapped_buffers;
            while (*entry != buffer)
                entry = &(*entry)->mapped_next;
            *entry = buffer->mapped_next;
        }
        remote_register_buf(buffer->map, (int)buffer->size, -1);
        munmap(buffer->map, buffer->size);
        close(buffer->fd);
        delete buffer;
    }
}
extern "C" void *cdsp_vpp_buffer_map(cdsp_vpp_buffer *buffer) {
    return buffer ? buffer->map : nullptr;
}
extern "C" int cdsp_vpp_buffer_fd(const cdsp_vpp_buffer *buffer) {
    return buffer ? buffer->fd : -1;
}
static bool valid_frame(const cdsp_vpp_frame &frame, int format) {
    if (format != CDSP_VPP_NV12 && format != CDSP_VPP_P010)
        return false;
    unsigned bytes = format == CDSP_VPP_P010 ? 2 : 1;
    if (!frame.buffer || frame.width < 16 || frame.width > 8192 || frame.height < 2 ||
        frame.height > 8192 || (frame.width & 15) || (frame.height & 1) ||
        frame.stride < frame.width * bytes || frame.stride > INT_MAX || frame.y_offset > INT_MAX ||
        frame.uv_offset > INT_MAX ||
        ((frame.stride | frame.y_offset | frame.uv_offset) & (bytes - 1)))
        return false;
    uint64_t y_end =
        frame.y_offset + (uint64_t)(frame.height - 1) * frame.stride + frame.width * bytes;
    uint64_t uv_end =
        frame.uv_offset + (uint64_t)(frame.height / 2 - 1) * frame.stride + frame.width * bytes;
    return y_end <= frame.buffer->size && uv_end <= frame.buffer->size &&
           (y_end <= frame.uv_offset || uv_end <= frame.y_offset);
}
static int wait_dma_buf_access(int fd, short events) {
    pollfd descriptor{fd, events, 0};
    int e;
    do {
        e = poll(&descriptor, 1, -1);
    } while (e < 0 && errno == EINTR);
    if (e < 0)
        return -errno;
    return descriptor.revents & (POLLERR | POLLNVAL | POLLHUP) ? -EIO : 0;
}
static int ensure_dsp_map(cdsp_vpp_buffer &buffer) {
    std::lock_guard<std::mutex> guard(mapping_lock);
    if (buffer.dsp_mapped)
        return 0;
    // STATIC keeps the DMA/SMMU and DSP mappings alive across frames while
    // retaining normal FastRPC cache maintenance. Map lazily after context
    // creation has selected the unsigned CDSP session.
    int e = fastrpc_mmap(CDSP_DOMAIN_ID, buffer.fd, buffer.map, 0, buffer.size, FASTRPC_MAP_STATIC);
    if (!e) {
        buffer.dsp_mapped = true;
        buffer.mapped_next = mapped_buffers;
        mapped_buffers = &buffer;
    }
    return e;
}
extern "C" int cdsp_vpp_profile(cdsp_vpp_context *ctx, int enable,
                                uint64_t counters[CDSP_VPP_PROFILE_COUNTERS]) {
    if (!ctx || !counters || (enable != 0 && enable != 1))
        return -EINVAL;
    std::lock_guard<std::mutex> guard(ctx->lock);
    return iris_vpp_profile(ctx->handle, enable, counters, CDSP_VPP_PROFILE_COUNTERS);
}
extern "C" int cdsp_vpp_scale(cdsp_vpp_context *ctx, const cdsp_vpp_frame *src,
                              const cdsp_vpp_crop *crop, const cdsp_vpp_frame *dst, uint64_t *us) {
    return cdsp_vpp_process(ctx, src, CDSP_VPP_NV12, crop, dst, CDSP_VPP_NV12, us);
}
extern "C" int cdsp_vpp_process(cdsp_vpp_context *ctx, const cdsp_vpp_frame *src, int src_format,
                                const cdsp_vpp_crop *crop, const cdsp_vpp_frame *dst,
                                int dst_format, uint64_t *us) {
    if (!ctx || !src || !dst || !us || !valid_frame(*src, src_format) ||
        !valid_frame(*dst, dst_format))
        return -EINVAL;
    auto &src_buffer = *src->buffer;
    auto &dst_buffer = *dst->buffer;
    if (src_buffer.dev == dst_buffer.dev && src_buffer.ino == dst_buffer.ino)
        return -EINVAL;
    cdsp_vpp_crop region = crop ? *crop : cdsp_vpp_crop{0, 0, src->width, src->height};
    if (!region.width || !region.height || ((region.x | region.width) & 15) ||
        ((region.y | region.height) & 1) || (uint64_t)region.x + region.width > src->width ||
        (uint64_t)region.y + region.height > src->height || region.width > 20u * dst->width ||
        region.height > 20u * dst->height ||
        !((region.width >= dst->width && region.height >= dst->height) ||
          (region.width <= dst->width && region.height <= dst->height)))
        return -EINVAL;
    std::lock_guard<std::mutex> guard(ctx->lock);
    int e = wait_dma_buf_access(src_buffer.fd, POLLIN);
    if (!e)
        e = wait_dma_buf_access(dst_buffer.fd, POLLOUT);
    if (e)
        return e;
    e = ensure_dsp_map(src_buffer);
    if (!e)
        e = ensure_dsp_map(dst_buffer);
    if (e)
        return e;
    if (ctx->src_width != region.width || ctx->src_height != region.height ||
        ctx->dst_width != dst->width || ctx->dst_height != dst->height) {
        e = iris_vpp_configure(ctx->handle, region.width, region.height, dst->width, dst->height);
        ctx->src_width = ctx->src_height = ctx->dst_width = ctx->dst_height = 0;
        if (e)
            return e;
        ctx->src_width = region.width;
        ctx->src_height = region.height;
        ctx->dst_width = dst->width;
        ctx->dst_height = dst->height;
    }
    unsigned src_bytes = src_format == CDSP_VPP_P010 ? 2 : 1;
    return iris_vpp_process(
        ctx->handle, (const uint8_t *)src_buffer.map, (int)src_buffer.size,
        (uint8_t *)dst_buffer.map, (int)dst_buffer.size, src->stride, dst->stride,
        src->y_offset + (uint64_t)region.y * src->stride + region.x * src_bytes,
        src->uv_offset + (uint64_t)(region.y / 2) * src->stride + region.x * src_bytes,
        dst->y_offset, dst->uv_offset, ctx->workers, src_format, dst_format, us);
}
