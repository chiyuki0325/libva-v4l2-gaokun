// SPDX-License-Identifier: MIT
// Board test: c++ -std=c++17 gen1-capabilities.cpp -lva -lva-drm -o gen1-capabilities
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
    VADisplay display = vaGetDisplayDRM(fd);
    int major, minor;
    expect(vaInitialize(display, &major, &minor));

    std::vector<VAProfile> profiles(vaMaxNumProfiles(display));
    int profile_count = 0;
    expect(vaQueryConfigProfiles(display, profiles.data(), &profile_count));
    const VAProfile expected_profiles[] = {VAProfileH264ConstrainedBaseline,
                                           VAProfileH264Main,
                                           VAProfileH264High,
                                           VAProfileHEVCMain,
                                           VAProfileHEVCMain10,
                                           VAProfileVP9Profile0,
                                           VAProfileVP9Profile2};
    require(profile_count == 7, "Gen1 must expose exactly seven decode profiles");
    for (int i = 0; i < profile_count; ++i)
        require(profiles[i] == expected_profiles[i], "unexpected Gen1 profile");

    for (VAProfile profile : expected_profiles) {
        std::vector<VAEntrypoint> entries(vaMaxNumEntrypoints(display));
        int entry_count = 0;
        expect(vaQueryConfigEntrypoints(display, profile, entries.data(), &entry_count));
        require(entry_count == 1 && entries[0] == VAEntrypointVLD,
                "Gen1 profile must expose only VLD");
        VAConfigID rejected;
        expect(vaCreateConfig(display, profile, VAEntrypointEncSlice, nullptr, 0, &rejected),
               VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
    }

    for (VAProfile profile : {VAProfileHEVCMain10, VAProfileVP9Profile2}) {
        VAConfigAttrib attr{VAConfigAttribRTFormat, 0};
        expect(vaGetConfigAttributes(display, profile, VAEntrypointVLD, &attr, 1));
        unsigned formats = VA_RT_FORMAT_YUV420_10;
        if (profile == VAProfileHEVCMain10)
            formats |= VA_RT_FORMAT_YUV420;
        require(attr.value == formats, "unexpected Gen1 10-bit render-target formats");
        attr.value = VA_RT_FORMAT_YUV420_10;
        VAConfigID config;
        expect(vaCreateConfig(display, profile, VAEntrypointVLD, &attr, 1, &config));
        unsigned attribute_count = 0;
        expect(vaQuerySurfaceAttributes(display, config, nullptr, &attribute_count));
        std::vector<VASurfaceAttrib> attributes(attribute_count);
        expect(vaQuerySurfaceAttributes(display, config, attributes.data(), &attribute_count));
        bool p010 = false;
        for (const auto &attribute : attributes)
            if (attribute.type == VASurfaceAttribPixelFormat)
                p010 |= attribute.value.value.i == VA_FOURCC_P010;
        require(p010, "Gen1 10-bit profiles must expose P010 surfaces");
        expect(vaDestroyConfig(display, config));
    }

    VAConfigID config;
    expect(vaCreateConfig(display, VAProfileH264High, VAEntrypointVLD, nullptr, 0, &config));
    std::vector<VASurfaceID> surfaces(29);
    expect(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 128, 128, surfaces.data(),
                            surfaces.size(), nullptr, 0));
    VAContextID context;
    expect(
        vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, surfaces.data(), 28, &context));
    expect(vaDestroyContext(display, context));
    expect(vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, surfaces.data(),
                           surfaces.size(), &context),
           VA_STATUS_ERROR_MAX_NUM_EXCEEDED);

    expect(vaDestroySurfaces(display, surfaces.data(), surfaces.size()));
    expect(vaDestroyConfig(display, config));
    expect(vaTerminate(display));
    close(fd);
    std::puts("Gen1 decode-only capability tests passed");
}
