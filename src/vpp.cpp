// SPDX-License-Identifier: MIT
#include "internal.hpp"
#ifdef HAVE_CDSP_VPP
#include "cdsp/client.h"
#include <dlfcn.h>
#include <climits>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace irisva {
#ifdef HAVE_CDSP_VPP
namespace {
struct CdspApi {
    void *library = nullptr;
    decltype(&cdsp_vpp_create) create = nullptr;
    decltype(&cdsp_vpp_destroy) destroy = nullptr;
    decltype(&cdsp_vpp_import) import = nullptr;
    decltype(&cdsp_vpp_release) release = nullptr;
    decltype(&cdsp_vpp_scale) scale = nullptr;
    ~CdspApi() {
        if (library)
            dlclose(library);
    }
};
std::shared_ptr<CdspApi> load_cdsp_api() {
    static std::mutex lock;
    static std::weak_ptr<CdspApi> cached;
    std::lock_guard<std::mutex> guard(lock);
    if (auto api = cached.lock())
        return api;
    auto api = std::make_shared<CdspApi>();
    api->library = dlopen("libiris_vpp.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->library)
        api->library = dlopen(IRIS_VPP_CLIENT_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        trace("CDSP VPP runtime unavailable: %s", dlerror());
        return {};
    }
#define LOAD(name)                                                                                 \
    api->name = reinterpret_cast<decltype(api->name)>(dlsym(api->library, "cdsp_vpp_" #name));     \
    if (!api->name)                                                                                \
        return {};
    LOAD(create)
    LOAD(destroy)
    LOAD(import)
    LOAD(release)
    LOAD(scale)
#undef LOAD
    cached = api;
    return api;
}
} // namespace
#endif
bool vpp_supported(VAProfile profile, VAEntrypoint entrypoint) {
#ifdef HAVE_CDSP_VPP
    return profile == VAProfileNone && entrypoint == VAEntrypointVideoProc && bool(load_cdsp_api());
#else
    (void)profile;
    (void)entrypoint;
    return false;
#endif
}
void vpp_caps(VAProcPipelineCaps &caps) {
    // These are driver-owned arrays, as consumed by FFmpeg and other VA clients.
    // Do not read the incoming structure: clients may pass it uninitialized.
    static VAProcColorStandardType colors[] = {VAProcColorStandardNone, VAProcColorStandardBT601,
                                               VAProcColorStandardBT709, VAProcColorStandardBT2020,
                                               VAProcColorStandardExplicit};
    static uint32_t formats[] = {VA_FOURCC_NV12};
    caps = {};
    caps.input_color_standards = caps.output_color_standards = colors;
    caps.num_input_color_standards = caps.num_output_color_standards =
        sizeof(colors) / sizeof(colors[0]);
    caps.input_pixel_format = caps.output_pixel_format = formats;
    caps.num_input_pixel_formats = caps.num_output_pixel_formats = 1;
    caps.min_input_width = caps.min_output_width = 16;
    caps.min_input_height = caps.min_output_height = 2;
    caps.max_input_width = caps.max_output_width = 8192;
    caps.max_input_height = caps.max_output_height = 8192;
}
VppPicture vpp_picture(const Buffer &buffer, const std::shared_ptr<Surface> &source,
                       const Surface &target) {
    VAProcPipelineParameterBuffer p{};
    std::memcpy(&p, buffer.data.data(), sizeof(p));
    check(!p.num_filters && !p.num_forward_references && !p.num_backward_references &&
              !p.rotation_state && !p.mirror_state && !p.blend_state && !p.num_additional_outputs &&
              !(p.pipeline_flags & ~VA_PROC_PIPELINE_FAST) &&
              !(p.filter_flags & ~VA_FILTER_SCALING_MASK) && !p.input_surface_flag &&
              !p.output_surface_flag && !p.output_hdr_metadata,
          "VPP supports progressive scaling only", VA_STATUS_ERROR_UNIMPLEMENTED);
    const auto &in = p.input_color_properties;
    const auto &out = p.output_color_properties;
    auto matches = [](unsigned a, unsigned b) { return !a || !b || a == b; };
    check(matches(p.surface_color_standard, p.output_color_standard) &&
              matches(in.color_range, out.color_range) &&
              matches(in.chroma_sample_location, out.chroma_sample_location) &&
              matches(in.colour_primaries, out.colour_primaries) &&
              matches(in.transfer_characteristics, out.transfer_characteristics) &&
              matches(in.matrix_coefficients, out.matrix_coefficients),
          "VPP color conversion unsupported", VA_STATUS_ERROR_UNIMPLEMENTED);
    check(!p.output_region ||
              (!p.output_region->x && !p.output_region->y &&
               p.output_region->width == target.width && p.output_region->height == target.height),
          "VPP output must cover the entire surface", VA_STATUS_ERROR_UNIMPLEMENTED);
    VppPicture picture;
    picture.source = source;
    picture.region = p.surface_region ? *p.surface_region
                                      : VARectangle{0, 0, static_cast<uint16_t>(source->width),
                                                    static_cast<uint16_t>(source->height)};
    auto r = picture.region;
    check(r.x >= 0 && r.y >= 0 && r.width && r.height && !(r.x & 15) && !(r.y & 1) &&
              !(r.width & 15) && !(r.height & 1) && !(target.width & 15) && !(target.height & 1) &&
              unsigned(r.x) + r.width <= source->width &&
              unsigned(r.y) + r.height <= source->height,
          "VPP requires 16-pixel widths/aligned crop x and even heights/crop y",
          VA_STATUS_ERROR_INVALID_PARAMETER);
    bool down = target.width <= r.width && target.height <= r.height;
    bool up = target.width >= r.width && target.height >= r.height;
    check((down || up) && uint64_t(target.width) * 20 >= r.width &&
              uint64_t(target.height) * 20 >= r.height,
          "VPP cannot mix up/down scaling axes or downscale beyond 20:1",
          VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    check(source.get() != &target && source->fourcc == VA_FOURCC_NV12 &&
              target.fourcc == VA_FOURCC_NV12,
          "VPP requires distinct NV12 surfaces", VA_STATUS_ERROR_INVALID_SURFACE);
    return picture;
}
#ifdef HAVE_CDSP_VPP
namespace {
std::shared_ptr<Memory> allocate_vpp_memory(const Surface &surface) {
    auto memory = std::make_shared<Memory>();
    memory->origin = MemoryOrigin::DmaHeap;
    memory->width = surface.width;
    memory->height = surface.height;
    memory->stride = (surface.width + 127) & ~127u;
    memory->storage_height = (surface.height + 31) & ~31u;
    memory->size = (size_t(memory->stride) * memory->storage_height * 3 / 2 + 4095) & ~size_t(4095);
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    check(heap >= 0, "open system DMA heap", VA_STATUS_ERROR_ALLOCATION_FAILED);
    dma_heap_allocation_data data{};
    data.len = memory->size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap);
    check(result == 0, "allocate VPP DMA-BUF", VA_STATUS_ERROR_ALLOCATION_FAILED);
    memory->fd = data.fd;
    return memory;
}
cdsp_vpp_frame import_vpp_frame(Memory &memory, const Surface &surface,
                                const std::shared_ptr<CdspApi> &api) {
    check(memory.fd >= 0 && memory.fourcc == VA_FOURCC_NV12 && memory.size <= INT_MAX &&
              memory.width >= surface.width && memory.height >= surface.height &&
              memory.stride >= surface.width && memory.storage_height >= surface.height &&
              uint64_t(memory.data_offset) +
                      uint64_t(memory.stride) *
                          (uint64_t(memory.storage_height) + surface.height / 2 - 1) +
                      surface.width <=
                  memory.size,
          "unsupported CDSP DMA-BUF layout", VA_STATUS_ERROR_INVALID_SURFACE);
    if (!memory.vpp_registration) {
        cdsp_vpp_buffer *buffer = nullptr;
        int e = api->import(memory.fd, memory.size, &buffer);
        check(e == 0, "CDSP DMA-BUF import failed");
        // shared_ptr invokes this deleter even if allocating its control block fails.
        // Keep the runtime loaded until every retained map has been released.
        memory.vpp_registration = std::shared_ptr<void>(
            buffer, [api](void *p) { api->release(static_cast<cdsp_vpp_buffer *>(p)); });
    }
    return {static_cast<cdsp_vpp_buffer *>(memory.vpp_registration.get()),
            surface.width,
            surface.height,
            memory.stride,
            memory.data_offset,
            memory.data_offset + size_t(memory.stride) * memory.storage_height};
}
} // namespace
struct Vpp::Impl {
    std::shared_ptr<CdspApi> api = load_cdsp_api();
    cdsp_vpp_context *context = nullptr;
    Impl() {
        check(bool(api), "CDSP VPP runtime unavailable", VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        int e = api->create(&context, CDSP_VPP_MAX_WORKERS, 1); // Capped by available HVX contexts.
        if (e) {
            trace("CDSP VPP initialization failed: %d", e);
            throw Error(VA_STATUS_ERROR_OPERATION_FAILED, "CDSP VPP initialization failed");
        }
    }
    ~Impl() {
        api->destroy(context);
    }
};
Vpp::Vpp() : impl_(std::make_unique<Impl>()) {}
void Vpp::scale(const VppPicture &picture, const std::shared_ptr<Surface> &target) {
    const auto &source = picture.source;
    check(source && source->memory, "VPP source has no storage", VA_STATUS_ERROR_INVALID_SURFACE);
    if (!target->memory)
        target->memory = allocate_vpp_memory(*target);
    auto &src = *source->memory;
    auto &dst = *target->memory;
    check(dst.origin != MemoryOrigin::Iris, "VPP cannot overwrite decoder reference storage",
          VA_STATUS_ERROR_INVALID_SURFACE);
    struct stat src_stat{}, dst_stat{};
    check(fstat(src.fd, &src_stat) == 0 && fstat(dst.fd, &dst_stat) == 0 &&
              (src_stat.st_dev != dst_stat.st_dev || src_stat.st_ino != dst_stat.st_ino),
          "VPP source and destination DMA-BUF alias", VA_STATUS_ERROR_INVALID_SURFACE);
    auto in = import_vpp_frame(src, *source, impl_->api),
         out = import_vpp_frame(dst, *target, impl_->api);
    auto region = picture.region;
    cdsp_vpp_crop crop{unsigned(region.x), unsigned(region.y), region.width, region.height};
    uint64_t dsp_us = 0;
    int e = impl_->api->scale(impl_->context, &in, &crop, &out, &dsp_us);
    if (e) {
        trace("CDSP VPP scale failed: %d", e);
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, "CDSP VPP scaling failed");
    }
    target->status = VA_STATUS_SUCCESS;
    trace("VPP CDSP %ux%u -> %ux%u src-fd=%d dst-fd=%d DSP=%llu us", region.width, region.height,
          target->width, target->height, src.fd, dst.fd, static_cast<unsigned long long>(dsp_us));
}
#else
struct Vpp::Impl {};
Vpp::Vpp() {
    throw Error(VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "CDSP VPP was disabled at build time");
}
void Vpp::scale(const VppPicture &, const std::shared_ptr<Surface> &) {
    throw Error(VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "CDSP VPP was disabled at build time");
}
#endif
Vpp::~Vpp() = default;
} // namespace irisva
