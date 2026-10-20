// SPDX-License-Identifier: MIT
// Board test: c++ -std=c++17 main10.cpp -lva -lva-drm -o main10
#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
static void expect(VAStatus status, VAStatus wanted = VA_STATUS_SUCCESS) {
    if (status != wanted) {
        std::fprintf(stderr, "Expected %s, got %s\n", vaErrorStr(wanted), vaErrorStr(status));
        std::exit(1);
    }
}
int main() {
    int fd = open("/dev/dri/renderD128", O_RDWR);
    require(fd >= 0, "open DRM device");
    auto display = vaGetDisplayDRM(fd);
    int major, minor;
    expect(vaInitialize(display, &major, &minor));
    constexpr unsigned formats = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    VAConfigAttrib attr{VAConfigAttribRTFormat, 0};
    expect(vaGetConfigAttributes(display, VAProfileHEVCMain10, VAEntrypointVLD, &attr, 1));
    require(attr.value == formats, "Main10 must advertise both 8-bit and 10-bit");
    VAConfigID default_config;
    expect(
        vaCreateConfig(display, VAProfileHEVCMain10, VAEntrypointVLD, nullptr, 0, &default_config));
    VAProfile profile;
    VAEntrypoint entrypoint;
    std::vector<VAConfigAttrib> config_attrs(vaMaxNumConfigAttributes(display));
    int config_count = 0;
    expect(vaQueryConfigAttributes(display, default_config, &profile, &entrypoint,
                                   config_attrs.data(), &config_count));
    require(profile == VAProfileHEVCMain10 && entrypoint == VAEntrypointVLD && config_count == 1 &&
                config_attrs[0].type == VAConfigAttribRTFormat && config_attrs[0].value == formats,
            "default Main10 config formats");
    expect(vaDestroyConfig(display, default_config));
    const unsigned requests[] = {VA_RT_FORMAT_YUV420, VA_RT_FORMAT_YUV420_10,       formats, 0,
                                 VA_RT_FORMAT_YUV422, formats | VA_RT_FORMAT_YUV422};
    for (unsigned requested : requests) {
        attr.value = requested;
        VAConfigID config;
        bool valid = requested && !(requested & ~formats);
        expect(vaCreateConfig(display, VAProfileHEVCMain10, VAEntrypointVLD, &attr, 1, &config),
               valid ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        if (!valid)
            continue;
        unsigned n = 0;
        expect(vaQuerySurfaceAttributes(display, config, nullptr, &n));
        std::vector<VASurfaceAttrib> attrs(n);
        unsigned too_small = n - 1;
        expect(vaQuerySurfaceAttributes(display, config, attrs.data(), &too_small),
               VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
        require(too_small == n, "surface attribute count");
        expect(vaQuerySurfaceAttributes(display, config, attrs.data(), &n));
        bool nv12 = false, p010 = false;
        for (const auto &a : attrs) {
            if (a.type == VASurfaceAttribPixelFormat) {
                nv12 |= a.value.value.i == VA_FOURCC_NV12;
                p010 |= a.value.value.i == VA_FOURCC_P010;
            }
        }
        require(nv12 && p010, "Main10 surface formats");
        for (unsigned format : {VA_RT_FORMAT_YUV420, VA_RT_FORMAT_YUV420_10}) {
            VASurfaceID surface, other;
            expect(vaCreateSurfaces(display, format, 128, 128, &surface, 1, nullptr, 0));
            expect(vaCreateSurfaces(display, format ^ formats, 128, 128, &other, 1, nullptr, 0));
            VAContextID context;
            // Chromium can create the context before allocating its surfaces.
            expect(
                vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, nullptr, 0, &context));
            expect(vaBeginPicture(display, context, surface));
            expect(vaEndPicture(display, context), VA_STATUS_ERROR_INVALID_BUFFER);
            expect(vaBeginPicture(display, context, other), VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
            expect(vaDestroyContext(display, context));
            expect(vaDestroySurfaces(display, &surface, 1));
            expect(vaDestroySurfaces(display, &other, 1));
        }
        expect(vaDestroyConfig(display, config));
    }
    expect(vaGetConfigAttributes(display, VAProfileVP9Profile2, VAEntrypointVLD, &attr, 1));
    require(attr.value == VA_RT_FORMAT_YUV420_10, "VP9 profile 2 remains 10-bit");
    expect(vaTerminate(display));
    close(fd);
    std::puts("Main10 capability/config/surface tests passed");
}
