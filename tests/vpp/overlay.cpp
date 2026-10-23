// SPDX-License-Identifier: MIT
// c++ -O2 -std=c++17 tests/vpp/overlay.cpp -o build/vpp-overlay $(pkg-config --cflags --libs libva
// libva-drm) Runs both backends unless one is named on the command line; --bench adds timings.
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static void require(bool value, const char *what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
static void va(VAStatus status) {
    if (status) {
        std::fprintf(stderr, "VA: %s\n", vaErrorStr(status));
        std::exit(1);
    }
}
struct Session {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    VADisplay display;
    VAConfigID config;
    VAContextID context;
    Session() {
        require(fd >= 0, "render node");
        display = vaGetDisplayDRM(fd);
        int major, minor;
        va(vaInitialize(display, &major, &minor));
        va(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &config));
        va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
        VAProcPipelineCaps caps{};
        va(vaQueryVideoProcPipelineCaps(display, context, nullptr, 0, &caps));
        require((caps.blend_flags & (VA_BLEND_GLOBAL_ALPHA | VA_BLEND_PREMULTIPLIED_ALPHA)) ==
                    (VA_BLEND_GLOBAL_ALPHA | VA_BLEND_PREMULTIPLIED_ALPHA),
                "blend caps");
    }
    ~Session() {
        va(vaDestroyContext(display, context));
        va(vaDestroyConfig(display, config));
        va(vaTerminate(display));
        close(fd);
    }
};
struct Surface {
    Session &s;
    VASurfaceID id;
    unsigned w, h, format;
    Surface(Session &s, unsigned w, unsigned h, unsigned f) : s(s), w(w), h(h), format(f) {
        VASurfaceAttrib attr{};
        attr.type = VASurfaceAttribPixelFormat;
        attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attr.value.type = VAGenericValueTypeInteger;
        attr.value.value.i = f;
        va(vaCreateSurfaces(s.display,
                            f == VA_FOURCC_P010   ? VA_RT_FORMAT_YUV420_10
                            : f == VA_FOURCC_NV12 ? VA_RT_FORMAT_YUV420
                                                  : VA_RT_FORMAT_RGB32,
                            w, h, &id, 1, &attr, 1));
    }
    ~Surface() {
        va(vaDestroySurfaces(s.display, &id, 1));
    }
    unsigned bytes() const {
        return format == VA_FOURCC_NV12 ? 1 : format == VA_FOURCC_P010 ? 2 : 4;
    }
    std::vector<uint8_t> pixels(const std::vector<uint8_t> *upload = nullptr) {
        VAImage image;
        va(vaDeriveImage(s.display, id, &image));
        void *mapped;
        va(vaMapBuffer(s.display, image.buf, &mapped));
        unsigned rows = bytes() == 4 ? h : h * 3 / 2;
        std::vector<uint8_t> data(w * rows * bytes());
        for (unsigned row = 0; row < rows; ++row) {
            unsigned plane = row >= h;
            auto *p = static_cast<uint8_t *>(mapped) + image.offsets[plane] +
                      (row - plane * h) * image.pitches[plane];
            if (upload)
                std::memcpy(p, upload->data() + row * w * bytes(), w * bytes());
            else
                std::memcpy(data.data() + row * w * bytes(), p, w * bytes());
        }
        va(vaUnmapBuffer(s.display, image.buf));
        va(vaDestroyImage(s.display, image.image_id));
        return data;
    }
};
static void rgb_interop(Surface &surface, const std::vector<uint8_t> &pixels) {
    auto &s = surface.s;
    std::vector<VAImageFormat> formats(vaMaxNumImageFormats(s.display));
    int count = 0;
    va(vaQueryImageFormats(s.display, formats.data(), &count));
    auto found = std::find_if(formats.begin(), formats.begin() + count,
                              [&](const auto &f) { return f.fourcc == surface.format; });
    require(found != formats.begin() + count && found->bits_per_pixel == 32, "RGB image format");
    VAImage image;
    va(vaCreateImage(s.display, &*found, surface.w, surface.h, &image));
    void *data;
    va(vaMapBuffer(s.display, image.buf, &data));
    for (unsigned y = 0; y < surface.h; ++y)
        std::memcpy(static_cast<uint8_t *>(data) + y * image.pitches[0],
                    pixels.data() + y * surface.w * 4, surface.w * 4);
    va(vaUnmapBuffer(s.display, image.buf));
    va(vaPutImage(s.display, surface.id, image.image_id, 0, 0, surface.w, surface.h, 0, 0,
                  surface.w, surface.h));
    va(vaGetImage(s.display, surface.id, 0, 0, surface.w, surface.h, image.image_id));
    va(vaMapBuffer(s.display, image.buf, &data));
    for (unsigned y = 0; y < surface.h; ++y)
        require(!std::memcmp(static_cast<uint8_t *>(data) + y * image.pitches[0],
                             pixels.data() + y * surface.w * 4, surface.w * 4),
                "RGB upload/download");
    va(vaUnmapBuffer(s.display, image.buf));
    va(vaDestroyImage(s.display, image.image_id));
    VADRMPRIMESurfaceDescriptor desc{};
    va(vaExportSurfaceHandle(s.display, surface.id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                             VA_EXPORT_SURFACE_READ_ONLY, &desc));
    require(desc.num_layers == 1 && desc.layers[0].num_planes == 1, "RGB PRIME export");
    VASurfaceAttrib attrs[3]{};
    attrs[0].type = VASurfaceAttribPixelFormat;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = surface.format;
    attrs[1].type = VASurfaceAttribMemoryType;
    attrs[1].value.type = VAGenericValueTypeInteger;
    attrs[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[2].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[2].value.type = VAGenericValueTypePointer;
    attrs[2].value.value.p = &desc;
    for (auto &a : attrs)
        a.flags = VA_SURFACE_ATTRIB_SETTABLE;
    VASurfaceID imported;
    va(vaCreateSurfaces(s.display, VA_RT_FORMAT_RGB32, surface.w, surface.h, &imported, 1, attrs,
                        3));
    VADRMPRIMESurfaceDescriptor exported{};
    va(vaExportSurfaceHandle(s.display, imported, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                             VA_EXPORT_SURFACE_READ_ONLY, &exported));
    struct stat original{}, roundtrip{};
    require(!fstat(desc.objects[0].fd, &original) && !fstat(exported.objects[0].fd, &roundtrip) &&
                original.st_dev == roundtrip.st_dev && original.st_ino == roundtrip.st_ino,
            "RGB DMA-BUF identity");
    close(desc.objects[0].fd);
    close(exported.objects[0].fd);
    va(vaDestroySurfaces(s.display, &surface.id, 1));
    surface.id = imported;
    require(surface.pixels() == pixels, "RGB imported pixels");
}

struct Case {
    const char *name;
    unsigned w, h, rw, rh;
    VARectangle crop, out;
    unsigned format = VA_FOURCC_NV12, rgb = VA_FOURCC_BGRA;
    bool premult = false, sparse = false, full = false;
    float alpha = 1;
    VAProcColorStandardType matrix = VAProcColorStandardBT709;
};
static std::vector<uint8_t> background(const Case &c) {
    unsigned bytes = c.format == VA_FOURCC_P010 ? 2 : 1;
    std::vector<uint8_t> data(c.w * c.h * 3 / 2 * bytes);
    for (unsigned i = 0; i < c.w * c.h * 3 / 2; ++i) {
        unsigned value = (i * 13 + (i / c.w) * 23) % 220 + 16;
        if (bytes == 2)
            reinterpret_cast<uint16_t *>(data.data())[i] = ((value * 4 + i % 4) & 1023) << 6;
        else
            data[i] = value;
    }
    return data;
}
static std::vector<uint8_t> foreground(const Case &c) {
    std::vector<uint8_t> data(c.rw * c.rh * 4);
    for (unsigned y = 0; y < c.rh; ++y)
        for (unsigned x = 0; x < c.rw; ++x) {
            auto *p = &data[(y * c.rw + x) * 4];
            p[3] = c.sparse ? ((x % 80 < 40 && y % 40 < 6) ? 200 : 0) : (x * 19 + y * 37) % 256;
            p[0] = (x * 3 + y * 5) % 256;
            p[1] = (x * 13 + y * 7) % 256;
            p[2] = (x * 17 + y * 11) % 256;
            if (c.premult)
                for (int k = 0; k < 3; ++k)
                    p[k] = (unsigned(p[k]) * p[3] + 127) / 255;
        }
    return data;
}
using Pixel = std::array<double, 4>;
static Pixel sample(const Case &c, const std::vector<uint8_t> &rgb, int x, int y) {
    Pixel result{};
    x -= c.out.x;
    y -= c.out.y;
    if (x < 0 || y < 0 || x >= c.out.width || y >= c.out.height)
        return result;
    double px =
        std::clamp((x + .5) * c.crop.width / c.out.width - .5, 0.0, double(c.crop.width - 1));
    double py =
        std::clamp((y + .5) * c.crop.height / c.out.height - .5, 0.0, double(c.crop.height - 1));
    int ix = int(px), iy = int(py);
    double fx = px - ix, fy = py - iy;
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 2; ++col) {
            int sx = c.crop.x + std::min(ix + col, int(c.crop.width) - 1),
                sy = c.crop.y + std::min(iy + row, int(c.crop.height) - 1);
            auto p = &rgb[(sy * c.rw + sx) * 4];
            double a = p[3] / 255.0;
            double weight = (row ? fy : 1 - fy) * (col ? fx : 1 - fx) * c.alpha;
            result[3] += a * weight;
            for (int channel = 0; channel < 3; ++channel)
                result[channel] += p[c.rgb == VA_FOURCC_BGRA ? 2 - channel : channel] / 255.0 *
                                   (c.premult ? 1 : a) * weight;
        }
    return result;
}
static int reference_error(const Case &c, const std::vector<uint8_t> &base,
                           const std::vector<uint8_t> &rgb, const std::vector<uint8_t> &result) {
    bool wide = c.format == VA_FOURCC_P010;
    int scale = wide ? 4 : 1, maximum = wide ? 1023 : 255, error = 0;
    auto load = [&](const std::vector<uint8_t> &v, unsigned i) {
        return wide ? reinterpret_cast<const uint16_t *>(v.data())[i] >> 6 : v[i];
    };
    double kr = c.matrix == VAProcColorStandardBT601    ? .299
                : c.matrix == VAProcColorStandardBT2020 ? .2627
                                                        : .2126;
    double kb = c.matrix == VAProcColorStandardBT601    ? .114
                : c.matrix == VAProcColorStandardBT2020 ? .0593
                                                        : .0722;
    for (unsigned y = 0; y < c.h; y += 2)
        for (unsigned x = 0; x < c.w; x += 2) {
            double sum_a = 0, sum_u = 0, sum_v = 0;
            for (unsigned row = 0; row < 2; ++row)
                for (unsigned col = 0; col < 2; ++col) {
                    auto p = sample(c, rgb, x + col, y + row);
                    double yy = kr * p[0] + (1 - kr - kb) * p[1] + kb * p[2];
                    unsigned index = (y + row) * c.w + x + col;
                    double value = load(base, index) * (1 - p[3]) +
                                   scale * ((c.full ? 0 : 16) * p[3] + (c.full ? 255 : 219) * yy);
                    error =
                        std::max(error, std::abs(load(result, index) -
                                                 std::clamp(int(std::lround(value)), 0, maximum)));
                    sum_a += p[3];
                    sum_u += (p[2] - yy) / (2 * (1 - kb));
                    sum_v += (p[0] - yy) / (2 * (1 - kr));
                }
            for (unsigned channel = 0; channel < 2; ++channel) {
                unsigned index = c.w * c.h + (y / 2) * c.w + x + channel;
                double value = load(base, index) * (1 - sum_a / 4) +
                               scale * (128 * sum_a / 4 +
                                        (c.full ? 255 : 224) * (channel ? sum_v : sum_u) / 4);
                error = std::max(error, std::abs(load(result, index) -
                                                 std::clamp(int(std::lround(value)), 0, maximum)));
            }
        }
    return error;
}
struct Pipeline {
    Session &s;
    std::vector<VABufferID> buffers;
    VARectangle crop, out;
    VABlendState blend{};
    Pipeline(Session &s, Surface &base, Surface &rgb, const Case &c)
        : s(s), crop(c.crop), out(c.out) {
        VAProcPipelineParameterBuffer p{};
        p.surface = base.id;
        p.surface_color_standard = p.output_color_standard = c.matrix;
        p.input_color_properties.color_range = p.output_color_properties.color_range =
            c.full ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
        add(p);
        p.surface = rgb.id;
        p.surface_region = &crop;
        p.output_region = &out;
        blend.flags = VA_BLEND_GLOBAL_ALPHA | (c.premult ? VA_BLEND_PREMULTIPLIED_ALPHA : 0);
        blend.global_alpha = c.alpha;
        p.blend_state = &blend;
        add(p);
    }
    void add(VAProcPipelineParameterBuffer &p) {
        VABufferID id;
        va(vaCreateBuffer(s.display, s.context, VAProcPipelineParameterBufferType, sizeof(p), 1, &p,
                          &id));
        buffers.push_back(id);
    }
    void submit(Surface &output, bool base_only = false, bool snapshot = false) {
        va(vaBeginPicture(s.display, s.context, output.id));
        // FFmpeg may submit the layers in separate calls.
        for (size_t i = 0; i < (base_only ? 1 : buffers.size()); ++i)
            va(vaRenderPicture(s.display, s.context, &buffers[i], 1));
        auto saved_crop = crop, saved_out = out;
        auto saved_blend = blend;
        if (snapshot) {
            crop = {0, 0, 1, 1};
            out = {0, 0, 1, 1};
            blend.global_alpha = 0;
        }
        va(vaEndPicture(s.display, s.context));
        crop = saved_crop;
        out = saved_out;
        blend = saved_blend;
        va(vaSyncSurface(s.display, output.id));
    }
    ~Pipeline() {
        for (auto id : buffers)
            va(vaDestroyBuffer(s.display, id));
    }
};
static void run(Session &s, const Case &c, const std::vector<const char *> &backends, int rounds) {
    Surface base(s, c.w, c.h, c.format), rgb(s, c.rw, c.rh, c.rgb);
    auto b = background(c), r = foreground(c);
    base.pixels(&b);
    if (rounds == 3)
        rgb_interop(rgb, r);
    else
        rgb.pixels(&r);
    Pipeline pipeline(s, base, rgb, c);
    std::vector<uint8_t> previous;
    for (auto backend : backends) {
        setenv("IRIS_VAAPI_OVERLAY", backend, 1);
        Surface output(s, c.w, c.h, c.format);
        pipeline.submit(output);
        auto data = output.pixels();
        int error = reference_error(c, b, r, data);
        if (error > (c.format == VA_FOURCC_P010 ? 5 : 2)) {
            std::fprintf(stderr, "%s %s max_error=%d\n", c.name, backend, error);
            if (!previous.empty())
                for (size_t j = 0, count = 0; j < data.size() && count < 20; ++j)
                    if (data[j] != previous[j]) {
                        std::fprintf(stderr, "offset=%zu got=%u want=%u\n", j, data[j],
                                     previous[j]);
                        ++count;
                    }
        }
        require(error <= (c.format == VA_FOURCC_P010 ? 5 : 2), "double precision blend reference");
        if (!previous.empty())
            require(data == previous, "CDSP/Vulkan pixel equality");
        previous = data;
        if (rounds == 3) {
            pipeline.submit(output, true);
            require(output.pixels() == b, "base-only copy after overlay");
            pipeline.submit(output, false, true);
            require(output.pixels() == data, "snapshot caller rectangles and blend state");
            unsigned flags = pipeline.blend.flags;
            pipeline.blend.flags = VA_BLEND_LUMA_KEY;
            va(vaBeginPicture(s.display, s.context, output.id));
            va(vaRenderPicture(s.display, s.context, &pipeline.buffers[0], 1));
            require(vaRenderPicture(s.display, s.context, &pipeline.buffers[1], 1) !=
                        VA_STATUS_SUCCESS,
                    "reject luma key");
            require(vaEndPicture(s.display, s.context) != VA_STATUS_SUCCESS,
                    "reject partial composition");
            pipeline.blend.flags = flags;
            require(output.pixels() == data, "rejected composition leaves output unchanged");
            VAProcPipelineParameterBuffer p{};
            p.surface = rgb.id;
            p.surface_region = &pipeline.crop;
            p.output_region = &pipeline.out;
            p.blend_state = &pipeline.blend;
            pipeline.add(p);
            pipeline.submit(output);
            auto twice = output.pixels();
            require(reference_error(c, data, r, twice) <= (c.format == VA_FOURCC_P010 ? 5 : 2),
                    "multiple overlay layers");
            va(vaDestroyBuffer(s.display, pipeline.buffers.back()));
            pipeline.buffers.pop_back();
        }
        for (int i = 0; i < 8; ++i)
            pipeline.submit(output);
        std::vector<double> times;
        for (int i = 0; i < rounds; ++i) {
            auto begin = std::chrono::steady_clock::now();
            pipeline.submit(output);
            times.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                    .count());
        }
        std::sort(times.begin(), times.end());
        std::printf("%-18s %-6s max_error=%d median=%.3f ms p95=%.3f ms\n", c.name, backend, error,
                    times[times.size() / 2], times[times.size() * 95 / 100]);
        std::fflush(stdout);
    }
}
static void geometry(Session &s, const std::vector<const char *> &backends) {
    Case c{"geometry", 64, 48, 32, 16, {0, 0, 32, 16}, {9, 7, 51, 27}};
    Case larger = c;
    larger.w = 128;
    larger.h = 96;
    Surface base(s, 128, 96, c.format), rgb(s, c.rw, c.rh, c.rgb), output(s, c.w, c.h, c.format);
    auto b = background(larger), r = foreground(c);
    base.pixels(&b);
    rgb.pixels(&r);
    Pipeline pipeline(s, base, rgb, c);
    pipeline.submit(output, true);
    auto resized = output.pixels();
    for (auto backend : backends) {
        setenv("IRIS_VAAPI_OVERLAY", backend, 1);
        pipeline.submit(output);
        require(reference_error(c, resized, r, output.pixels()) <= 2,
                "scale base and overlay in one submission");
    }
    std::puts("PASS combined base scaling and overlay");
}
int main(int argc, char **argv) {
    bool bench = false, with_geometry = false;
    std::vector<const char *> backends;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bench"))
            bench = true;
        else if (!std::strcmp(argv[i], "--geometry"))
            with_geometry = true;
        else
            backends.push_back(argv[i]);
    }
    if (backends.empty())
        backends = {"cdsp", "vulkan"};
    Session s;
    std::vector<Case> cases = {
        {"straight", 128, 96, 64, 48, {0, 0, 64, 48}, {32, 24, 64, 48}},
        {"odd-crop-scale", 130, 98, 67, 51, {3, 5, 61, 43}, {-5, 7, 119, 85}},
        {"clipped", 130, 98, 67, 51, {0, 0, 67, 51}, {121, 91, 67, 51}},
        {"offscreen", 128, 96, 64, 48, {0, 0, 64, 48}, {-100, -100, 64, 48}},
        {"p010", 128, 96, 64, 48, {0, 0, 64, 48}, {16, 10, 64, 48}, VA_FOURCC_P010},
        {"premult-full601",
         130,
         98,
         67,
         51,
         {1, 1, 65, 49},
         {5, 3, 121, 91},
         VA_FOURCC_NV12,
         VA_FOURCC_RGBA,
         true,
         false,
         true,
         .625f,
         VAProcColorStandardBT601},
        {"bt2020-alpha",
         128,
         96,
         64,
         48,
         {0, 0, 64, 48},
         {10, 11, 64, 48},
         VA_FOURCC_P010,
         VA_FOURCC_RGBA,
         false,
         false,
         false,
         .5f,
         VAProcColorStandardBT2020},
        {"p010-scaled",
         130,
         98,
         67,
         51,
         {2, 1, 63, 49},
         {3, 7, 119, 85},
         VA_FOURCC_P010,
         VA_FOURCC_BGRA,
         false,
         false,
         false,
         .3f},
        {"downscale", 128, 96, 93, 71, {0, 0, 93, 71}, {5, 9, 37, 29}},
        {"narrow", 128, 96, 7, 3, {1, 0, 5, 3}, {-1, 15, 111, 51}},
        {"transparent",
         128,
         96,
         64,
         48,
         {0, 0, 64, 48},
         {32, 24, 64, 48},
         VA_FOURCC_NV12,
         VA_FOURCC_BGRA,
         false,
         false,
         false,
         0.f}};
    for (const auto &c : cases)
        run(s, c, backends, 3);
    if (with_geometry)
        geometry(s, backends);
    if (bench) {
        std::vector<Case> timings = {
            {"1080p-subtitle",
             1920,
             1080,
             800,
             160,
             {0, 0, 800, 160},
             {560, 880, 800, 160},
             VA_FOURCC_NV12,
             VA_FOURCC_BGRA,
             false,
             true},
            {"4k-subtitle",
             3840,
             2160,
             1600,
             320,
             {0, 0, 1600, 320},
             {1120, 1760, 1600, 320},
             VA_FOURCC_NV12,
             VA_FOURCC_BGRA,
             false,
             true},
            {"4k-full-sparse",
             3840,
             2160,
             3840,
             2160,
             {0, 0, 3840, 2160},
             {0, 0, 3840, 2160},
             VA_FOURCC_NV12,
             VA_FOURCC_BGRA,
             false,
             true},
            {"1080p-full", 1920, 1080, 1920, 1080, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
            {"4k-full", 3840, 2160, 3840, 2160, {0, 0, 3840, 2160}, {0, 0, 3840, 2160}},
            {"4k-scaled-subtitle",
             3840,
             2160,
             800,
             160,
             {0, 0, 800, 160},
             {1120, 1760, 1600, 320},
             VA_FOURCC_NV12,
             VA_FOURCC_BGRA,
             false,
             true},
            {"4k-p010-subtitle",
             3840,
             2160,
             1600,
             320,
             {0, 0, 1600, 320},
             {1120, 1760, 1600, 320},
             VA_FOURCC_P010,
             VA_FOURCC_BGRA,
             false,
             true}};
        for (const auto &c : timings)
            run(s, c, backends, 100);
    }
    std::puts("PASS overlay");
}
