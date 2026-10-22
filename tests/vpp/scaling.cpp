// SPDX-License-Identifier: MIT
// Hardware integration: scalar reference, PRIME2 identity and DMA-BUF lifetime.
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <drm_fourcc.h>
#include "../../src/cdsp/filter.h"
#include "../../src/cdsp/client.h"
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <misc/fastrpc.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

static int input_fd = -1, output_fd = -1, direct_calls = 0;
static int maps = 0, unmaps = 0;
static bool same_buffer(int a, int b) {
    struct stat x{}, y{};
    return a >= 0 && b >= 0 && !fstat(a, &x) && !fstat(b, &y) && x.st_dev == y.st_dev &&
           x.st_ino == y.st_ino;
}
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    static auto real =
        reinterpret_cast<int (*)(int, unsigned long, ...)>(dlsym(RTLD_NEXT, "ioctl"));
    bool both = false;
    if (request == FASTRPC_IOCTL_INVOKE && arg) {
        auto *call = static_cast<fastrpc_invoke *>(arg);
        auto *buffers = reinterpret_cast<fastrpc_invoke_args *>(call->args);
        unsigned n = ((call->sc >> 16) & 255) + ((call->sc >> 8) & 255) + ((call->sc >> 4) & 15) +
                     (call->sc & 15);
        bool in = false, out = false;
        for (unsigned i = 0; i < n; ++i) {
            in |= same_buffer(buffers[i].fd, input_fd);
            out |= same_buffer(buffers[i].fd, output_fd);
        }
        both = in && out;
    }
    int result = real(fd, request, arg);
    if (!result && request == FASTRPC_IOCTL_MEM_MAP)
        ++maps;
    if (!result && request == FASTRPC_IOCTL_MEM_UNMAP)
        ++unmaps;
    if (!result && both)
        ++direct_calls;
    return result;
}
static void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
static void va(VAStatus status) {
    if (status) {
        std::fprintf(stderr, "VA: %s\n", vaErrorStr(status));
        std::exit(1);
    }
}
static void sync(int fd, uint64_t flags) {
    dma_buf_sync s{flags};
    require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA-BUF CPU sync");
}
struct Allocation {
    VADRMPRIMESurfaceDescriptor desc{};
    uint8_t *data;
    Allocation(unsigned w, unsigned h) {
        auto &obj = desc.objects[0];
        auto &l = desc.layers[0];
        desc.fourcc = l.drm_format = DRM_FORMAT_NV12;
        desc.width = w;
        desc.height = h;
        desc.num_objects = desc.num_layers = 1;
        l.num_planes = 2;
        l.pitch[0] = l.pitch[1] = (w + 127) & ~127u;
        l.offset[0] = 128; // Nonzero base offset and padded Y rows are intentional.
        l.offset[1] = 128 + l.pitch[0] * ((h + 31) & ~31u);
        obj.size = (l.offset[1] + l.pitch[1] * ((h + 1) / 2) + 4095) & ~4095u;
        obj.drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        require(heap >= 0, "open system heap");
        dma_heap_allocation_data a{};
        a.len = obj.size;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        require(!ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a), "allocate DMA-BUF");
        close(heap);
        obj.fd = a.fd;
        data = static_cast<uint8_t *>(
            mmap(nullptr, obj.size, PROT_READ | PROT_WRITE, MAP_SHARED, obj.fd, 0));
        require(data != MAP_FAILED, "map allocation");
    }
    ~Allocation() {
        munmap(data, desc.objects[0].size);
        close(desc.objects[0].fd);
    }
};
static VASurfaceID import(VADisplay display, VADRMPRIMESurfaceDescriptor &desc) {
    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    auto owned = desc;
    owned.objects[0].fd = dup(desc.objects[0].fd);
    require(owned.objects[0].fd >= 0, "duplicate descriptor fd");
    attrs[1].value.value.p = &owned;
    VASurfaceID id;
    va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, desc.width, desc.height, &id, 1, attrs, 2));
    close(owned.objects[0].fd); // Driver must own its own fd after import.
    return id;
}
struct Case {
    unsigned sw, sh, dw, dh;
    bool crop;
};
static void run_case(Case c) {
    Allocation input(c.sw, c.sh), reference(c.dw, c.dh), destination(c.dw, c.dh);
    auto &sl = input.desc.layers[0];
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memset(input.data, 0xa5, input.desc.objects[0].size);
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned y = 0; y < (p ? c.sh / 2 : c.sh); ++y)
            for (unsigned x = 0; x < c.sw; ++x)
                input.data[sl.offset[p] + y * sl.pitch[p] + x] =
                    p ? (x & 1 ? 150 + (x / 2 + y) % 80 : 20 + (x / 2 + y) % 80)
                      : 16 + (x / 8 + y / 4) % 220;
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    VARectangle region{static_cast<int16_t>(c.crop ? 16 : 0), static_cast<int16_t>(c.crop ? 2 : 0),
                       static_cast<uint16_t>(c.crop ? 608 : c.sw),
                       static_cast<uint16_t>(c.crop ? 360 : c.sh)};
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    bool enlarging = region.width < c.dw || region.height < c.dh;
    std::vector<int> row(region.width);
    for (unsigned p = 0; p < 2; ++p) {
        int channels = p ? 2 : 1;
        const auto *src =
            input.data + sl.offset[p] + (region.y / channels) * sl.pitch[p] + region.x;
        auto *dst = reference.data + reference.desc.layers[0].offset[p];
        for (unsigned y = 0; y < c.dh / channels; ++y) {
            auto fy = make_filter(region.height / channels, c.dh / channels, y);
            for (unsigned x = 0; x < region.width; ++x) {
                int sum = 0;
                for (int k = 0; k < fy.tap_count; ++k)
                    sum += src[fy.indices[k] * sl.pitch[p] + x] * fy.weights[k];
                row[x] = enlarging ? (sum + (1 << (fy.shift - 7))) >> (fy.shift - 6)
                                   : round_clip_u8(sum, fy.shift);
            }
            for (unsigned x = 0; x < c.dw / channels; ++x) {
                auto fx = make_filter(region.width / channels, c.dw / channels, x);
                for (int ch = 0; ch < channels; ++ch) {
                    int sum = 0;
                    for (int k = 0; k < fx.tap_count; ++k)
                        sum += row[fx.indices[k] * channels + ch] * fx.weights[k];
                    dst[y * reference.desc.layers[0].pitch[p] + x * channels + ch] =
                        round_clip_u8(sum, fx.shift + (enlarging ? 6 : 0));
                }
            }
        }
    }
    sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    require(drm >= 0, "open DRM");
    VADisplay display = vaGetDisplayDRM(drm);
    int major, minor;
    va(vaInitialize(display, &major, &minor));
    VAConfigID config;
    VAContextID context;
    va(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &config));
    va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
    unsigned n = 0;
    va(vaQueryVideoProcFilters(display, context, nullptr, &n));
    require(n == 0, "scaling has no filters");
    VAProcPipelineCaps caps{};
    va(vaQueryVideoProcPipelineCaps(display, context, nullptr, 0, &caps));
    require(caps.num_input_pixel_formats == 1, "NV12 capability count");
    require(caps.input_pixel_format && caps.input_pixel_format[0] == VA_FOURCC_NV12,
            "NV12 capability value");
    auto in = import(display, input.desc);
    // Test composed and separate-layer PRIME2 imports.
    auto separate = destination.desc;
    separate.num_layers = 2;
    separate.layers[0].drm_format = DRM_FORMAT_R8;
    separate.layers[0].num_planes = 1;
    separate.layers[1].drm_format = DRM_FORMAT_GR88;
    separate.layers[1].num_planes = 1;
    separate.layers[1].pitch[0] = destination.desc.layers[0].pitch[1];
    separate.layers[1].offset[0] = destination.desc.layers[0].offset[1];
    VASurfaceID outputs[3];
    outputs[0] = import(display, separate);
    va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, c.dw, c.dh, &outputs[1], 2, nullptr, 0));
    int initial_maps = maps, initial_unmaps = unmaps;
    for (unsigned kind = 0; kind < 3; ++kind) {
        VADRMPRIMESurfaceDescriptor exported{};
        // kind 0: external heap; kind 1: pre-exported MSM; kind 2: lazy VPP heap.
        if (kind != 2) {
            va(vaExportSurfaceHandle(
                display, outputs[kind], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported));
            output_fd = exported.objects[0].fd;
        } else
            output_fd = -1;
        input_fd = input.desc.objects[0].fd;
        direct_calls = 0;
        for (int repeat = 0; repeat < 3; ++repeat) {
            VAProcPipelineParameterBuffer params{};
            params.surface = in;
            params.surface_region = &region;
            VABufferID buffer;
            va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(params),
                              1, &params, &buffer));
            va(vaBeginPicture(display, context, outputs[kind]));
            va(vaRenderPicture(display, context, &buffer, 1));
            va(vaDestroyBuffer(display, buffer)); // Parameters must already be snapshotted.
            va(vaEndPicture(display, context));
            va(vaSyncSurface(display, outputs[kind]));
            if (kind == 2 && repeat == 0) {
                va(vaExportSurfaceHandle(
                    display, outputs[kind], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported));
                output_fd = exported.objects[0].fd;
            }
        }
        require(maps - initial_maps == int(kind) + 2, "one persistent map per source/destination");
        require(unmaps == initial_unmaps, "maps survive across frames");
        require(direct_calls == (kind == 2 ? 2 : 3),
                "both original DMA-BUFs must reach successful FastRPC calls");
        VAImage image{};
        va(vaDeriveImage(display, outputs[kind], &image));
        void *mapped;
        va(vaMapBuffer(display, image.buf, &mapped));
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        unsigned maxdiff[2]{};
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned y = 0; y < (p ? c.dh / 2 : c.dh); ++y)
                for (unsigned x = 0; x < c.dw; ++x) {
                    int a =
                        static_cast<uint8_t *>(mapped)[image.offsets[p] + y * image.pitches[p] + x];
                    int b = reference.data[reference.desc.layers[0].offset[p] +
                                           y * reference.desc.layers[0].pitch[p] + x];
                    maxdiff[p] = std::max(maxdiff[p], unsigned(std::abs(a - b)));
                }
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        va(vaUnmapBuffer(display, image.buf));
        va(vaDestroyImage(display, image.image_id));
        std::printf("%ux%u -> %ux%u crop=%d output=%u RPC=%d maxdiff Y=%u UV=%u\n", c.sw, c.sh,
                    c.dw, c.dh, c.crop, kind, direct_calls, maxdiff[0], maxdiff[1]);
        require(maxdiff[0] <= 1 && maxdiff[1] <= 1,
                "DSP matches CPU reference within rounding tolerance");
        close(exported.objects[0].fd);
    }
    // Unsupported operations must fail before a DSP invocation.
    VAProcPipelineParameterBuffer bad{};
    bad.surface = in;
    bad.rotation_state = VA_ROTATION_90;
    VABufferID buffer;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(bad), 1, &bad,
                      &buffer));
    va(vaBeginPicture(display, context, outputs[0]));
    require(vaRenderPicture(display, context, &buffer, 1) == VA_STATUS_ERROR_UNIMPLEMENTED,
            "reject rotation");
    require(vaEndPicture(display, context) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "reject missing parameters");
    va(vaDestroyBuffer(display, buffer));
    auto alias = import(display, input.desc);
    bad = {};
    bad.surface = in;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(bad), 1, &bad,
                      &buffer));
    va(vaBeginPicture(display, context, alias));
    va(vaRenderPicture(display, context, &buffer, 1));
    int before = direct_calls;
    require(vaEndPicture(display, context) == VA_STATUS_ERROR_INVALID_SURFACE,
            "reject DMA-BUF alias");
    require(direct_calls == before, "alias must not reach DSP");
    va(vaDestroyBuffer(display, buffer));
    va(vaDestroySurfaces(display, &alias, 1));
    va(vaDestroyContext(display, context));
    require(unmaps - initial_unmaps == 4, "last context retires all persistent maps");
    // Surfaces outlive the last context, then get mapped into a new session.
    va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
    VAProcPipelineParameterBuffer again{};
    again.surface = in;
    again.surface_region = &region;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(again), 1, &again,
                      &buffer));
    va(vaBeginPicture(display, context, outputs[0]));
    va(vaRenderPicture(display, context, &buffer, 1));
    va(vaEndPicture(display, context));
    va(vaDestroyBuffer(display, buffer));
    require(maps - initial_maps == 6, "retained buffers remap into a new context");
    va(vaDestroyContext(display, context));
    require(unmaps - initial_unmaps == 6, "recreated context cleans up maps");
    va(vaDestroySurfaces(display, &in, 1));
    va(vaDestroySurfaces(display, outputs, 3));
    va(vaDestroyConfig(display, config));
    va(vaTerminate(display));
    close(drm);
}
static cdsp_vpp_buffer *import_test_buffer(size_t size) {
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    require(heap >= 0, "heap open");
    dma_heap_allocation_data a{};
    a.len = (size + 4095) & ~4095;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    require(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) == 0, "heap allocate");
    close(heap);
    cdsp_vpp_buffer *b = nullptr;
    // Deliberately import a size that is not a page multiple.
    require(cdsp_vpp_import(a.fd, size, &b) == 0, "import");
    close(a.fd);
    return b;
}
static std::vector<uint8_t> pixels(const cdsp_vpp_frame &f, size_t size, int value) {
    std::vector<uint8_t> data(size, 0xa5);
    for (unsigned p = 0; p < 2; p++)
        for (unsigned y = 0; y < f.height / (p ? 2 : 1); y++)
            for (unsigned x = 0; x < f.width; x++)
                data[(p ? f.uv_offset : f.y_offset) + y * f.stride + x] =
                    p ? ((x & 1) ? 255 - value : value / 2) : value;
    return data;
}
static void check_buffer_lifetime() {
    struct Profile {
        uint64_t before = UINT64_C(0x123456789abcdef0);
        uint64_t counters[CDSP_VPP_PROFILE_COUNTERS]{};
        uint64_t after = UINT64_C(0x123456789abcdef0);
    } profile;
    constexpr size_t source_size = 4081, dest_size = 12271;
    // Import before opening the first context: mapping must remain lazy.
    auto *s = import_test_buffer(source_size), *d = import_test_buffer(dest_size);
    cdsp_vpp_frame src{s, 80, 18, 104, 16, 16 + 104 * 20};
    cdsp_vpp_frame dst{d, 160, 36, 184, 16, 16 + 184 * 38};
    auto *sp = (uint8_t *)cdsp_vpp_buffer_map(s);
    auto *dp = (uint8_t *)cdsp_vpp_buffer_map(d);
    constexpr int cycles = 2, frames = 3;
    for (int cycle = 0; cycle < cycles; cycle++) {
        cdsp_vpp_context *ctx = nullptr;
        require(cdsp_vpp_create(&ctx, CDSP_VPP_MAX_WORKERS, 1) == 0, "context create");
        require(cdsp_vpp_profile(ctx, 1, profile.counters) == 0, "enable profiling");
        for (int frame = 0; frame < frames; frame++) {
            int value = (cycle * 31 + frame * 17) & 255;
            auto input = pixels(src, source_size, value);
            auto expected = pixels(dst, dest_size, value);
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memcpy(sp, input.data(), source_size);
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memset(dp, 0xa5, dest_size);
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            uint64_t us;
            require(cdsp_vpp_scale(ctx, &src, nullptr, &dst, &us) == 0, "persistent scale");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            require(memcmp(dp, expected.data(), dest_size) == 0, "pixels and all padding");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            require(memcmp(sp, input.data(), source_size) == 0, "source unchanged");
            sync(cdsp_vpp_buffer_fd(s), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            // A second import of the same source gets its own mapping lifetime.
            cdsp_vpp_buffer *alias = nullptr;
            require(cdsp_vpp_import(cdsp_vpp_buffer_fd(s), source_size, &alias) == 0,
                    "reimport source");
            auto aliased_src = src;
            aliased_src.buffer = alias;
            require(cdsp_vpp_scale(ctx, &aliased_src, nullptr, &dst, &us) == 0,
                    "scale reimported source");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            require(memcmp(dp, expected.data(), dest_size) == 0, "reimported pixels");
            sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            cdsp_vpp_release(alias);
        }
        require(cdsp_vpp_profile(ctx, 0, profile.counters) == 0, "read all worker counters");
        require(profile.before == UINT64_C(0x123456789abcdef0) &&
                    profile.after == UINT64_C(0x123456789abcdef0),
                "profiling buffer guards");
        // Retain the original mapped buffers across context destruction/recreation.
        cdsp_vpp_destroy(ctx);
    }
    cdsp_vpp_context *a = nullptr, *b = nullptr;
    require(cdsp_vpp_create(&a, CDSP_VPP_MAX_WORKERS, 1) == 0 && cdsp_vpp_create(&b, 2, 1) == 0,
            "two live contexts");
    uint64_t us;
    require(cdsp_vpp_scale(a, &src, nullptr, &dst, &us) == 0, "first shared context");
    require(cdsp_vpp_scale(b, &src, nullptr, &dst, &us) == 0, "second shared context");
    cdsp_vpp_destroy(a);
    require(cdsp_vpp_scale(b, &src, nullptr, &dst, &us) == 0, "surviving context retains mappings");
    auto expected = pixels(dst, dest_size, ((cycles - 1) * 31 + (frames - 1) * 17) & 255);
    sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    require(memcmp(dp, expected.data(), dest_size) == 0, "surviving context pixels");
    sync(cdsp_vpp_buffer_fd(d), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    cdsp_vpp_destroy(b);
    cdsp_vpp_release(d);
    cdsp_vpp_release(s);
    puts("PASS: buffer lifetime, changing pixels, padding, duplicate imports and profiling");
}

int main() {
    const Case cases[] = {{1920, 1080, 3840, 2160, false},
                          {1920, 1080, 1280, 720, false},
                          {1280, 720, 1920, 1080, false},
                          {3840, 2160, 1920, 1080, false},
                          {640, 360, 1280, 720, false},
                          {640, 360, 640, 360, false},
                          {640, 384, 320, 180, true},
                          {16, 2, 32, 4, false},
                          {32, 4, 16, 2, false}};
    // FastRPC owns threads and process-global state. Never fork a process
    // that has already initialized them: each case starts in a fresh child.
    for (auto c : cases) {
        pid_t child = fork();
        require(child >= 0, "fork test case");
        if (!child) {
            run_case(c);
            std::exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && status == 0, "test case child");
    }
    check_buffer_lifetime();
    std::puts("PASS");
}
