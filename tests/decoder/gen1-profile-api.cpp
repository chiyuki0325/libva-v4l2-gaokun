#include <va/va.h>
#include <va/va_backend.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace irisva {
std::string find_device() {
    return "/unused-gen1-api-test";
}

std::string find_encoder_device() {
    std::fputs("Gen1 must not probe encoder devices\n", stderr);
    std::exit(1);
}
} // namespace irisva

static void require(bool condition, const char *message) {
    if (!condition) {
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

int main(int argc, char **argv) {
    require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--vpp"),
            "usage: gen1-profile-api /path/to/v4l2_drv_video.so [--vpp]");
    bool have_vpp = argc == 3;
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(module != nullptr, "load driver");
    auto initialize =
        reinterpret_cast<VAStatus (*)(VADriverContextP)>(dlsym(module, "__vaDriverInit_1_0"));
    require(initialize != nullptr, "find driver entrypoint");
    VADriverVTable table{};
    VADriverContext driver{};
    driver.vtable = &table;
    expect(initialize(&driver));
    require(driver.max_profiles == 7 + have_vpp, "Gen1 profile allocation bound");
    require(driver.max_entrypoints == 1, "Gen1 must allocate one entrypoint per profile");
    std::vector<VAProfile> profiles(driver.max_profiles);
    int count = 0;
    expect(table.vaQueryConfigProfiles(&driver, profiles.data(), &count));
    const std::vector<VAProfile> expected = {
        VAProfileH264ConstrainedBaseline,
        VAProfileH264Main,
        VAProfileH264High,
        VAProfileHEVCMain,
        VAProfileHEVCMain10,
        VAProfileVP9Profile0,
        VAProfileVP9Profile2,
    };
    auto expected_profiles = expected;
    if (have_vpp)
        expected_profiles.push_back(VAProfileNone);
    require(count == driver.max_profiles && profiles == expected_profiles, "unexpected Gen1 profiles");
    for (VAProfile profile : expected) {
        VAEntrypoint entrypoint{};
        int entry_count = 0;
        expect(table.vaQueryConfigEntrypoints(&driver, profile, &entrypoint, &entry_count));
        require(entry_count == 1 && entrypoint == VAEntrypointVLD, "decode-only entrypoint");
        VAConfigID config;
        expect(table.vaCreateConfig(&driver, profile, VAEntrypointEncSlice, nullptr, 0, &config),
               VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        unsigned formats = VA_RT_FORMAT_YUV420;
        if (profile == VAProfileHEVCMain10)
            formats |= VA_RT_FORMAT_YUV420_10;
        if (profile == VAProfileVP9Profile2)
            formats = VA_RT_FORMAT_YUV420_10;
        VAConfigAttrib attribute{VAConfigAttribRTFormat, 0};
        expect(table.vaGetConfigAttributes(&driver, profile, VAEntrypointVLD, &attribute, 1));
        require(attribute.value == formats, "profile render-target formats");
        expect(table.vaCreateConfig(&driver, profile, VAEntrypointVLD, &attribute, 1, &config));
        unsigned surface_count = 0;
        expect(table.vaQuerySurfaceAttributes(&driver, config, nullptr, &surface_count));
        std::vector<VASurfaceAttrib> surface_attributes(surface_count);
        expect(table.vaQuerySurfaceAttributes(&driver, config, surface_attributes.data(),
                                              &surface_count));
        bool p010 = false;
        for (const auto &surface_attribute : surface_attributes)
            if (surface_attribute.type == VASurfaceAttribPixelFormat)
                p010 |= surface_attribute.value.value.i == VA_FOURCC_P010;
        require(p010 == bool(formats & VA_RT_FORMAT_YUV420_10), "P010 surface enumeration");
        unsigned surface_format =
            formats & VA_RT_FORMAT_YUV420_10 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
        std::vector<VASurfaceID> surfaces(29);
        expect(table.vaCreateSurfaces2(&driver, surface_format, 128, 128, surfaces.data(),
                                       surfaces.size(), nullptr, 0));
        VAContextID context;
        expect(table.vaCreateContext(&driver, config, 128, 128, VA_PROGRESSIVE, surfaces.data(), 28,
                                     &context));
        expect(table.vaDestroyContext(&driver, context));
        expect(table.vaCreateContext(&driver, config, 128, 128, VA_PROGRESSIVE, surfaces.data(), 29,
                                     &context),
               VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
        expect(table.vaDestroySurfaces(&driver, surfaces.data(), surfaces.size()));
        expect(table.vaDestroyConfig(&driver, config));
    }
    expect(table.vaTerminate(&driver));
    dlclose(module);
    std::puts("Gen1 profile API tests passed without opening hardware");
}
