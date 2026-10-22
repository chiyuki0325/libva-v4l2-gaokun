// SPDX-License-Identifier: MIT
#include "skel.h"
#include "iris_vpp.h"
#include "vpp_limits.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>

int cdsp_vpp_select_workers(int *workers) {
    if (!workers || *workers < 0 || *workers > CDSP_VPP_MAX_WORKERS)
        return -EINVAL;
    // Zero explicitly requests the scalar DSP diagnostic path.
    if (!*workers)
        return 0;
    fastrpc_capability request{CDSP_DOMAIN_ID, HVX_SUPPORT_128B, 0};
    int e = remote_handle_control(DSPRPC_GET_DSP_INFO, &request, sizeof(request));
    if (e) {
        fprintf(stderr, "CDSP VPP: HVX_SUPPORT_128B query failed: %d\n", e);
        return e;
    }
    if (!request.capability) {
        fprintf(stderr, "CDSP VPP: 128-byte HVX is unavailable\n");
        return -ENOTSUP;
    }
    // The kernels support any count up to CDSP_VPP_MAX_WORKERS. The request is a ceiling;
    // never dispatch more workers than the reported number of 128-byte contexts.
    if ((uint32_t)*workers > request.capability)
        *workers = (int)request.capability;
    return 0;
}

int cdsp_vpp_open_skel(remote_handle64 *handle) {
    fastrpc_capability request{CDSP_DOMAIN_ID, ARCH_VER, 0};
    int e = remote_handle_control(DSPRPC_GET_DSP_INFO, &request, sizeof(request));
    if (e) {
        fprintf(stderr, "CDSP VPP: ARCH_VER query failed: %d\n", e);
        return e;
    }
    // Like cdsp_peak, accept decimal versions and packed hexadecimal versions
    // (including revision bits above the low byte). Reject malformed BCD digits.
    unsigned arch = request.capability;
    unsigned low = arch & 0xff;
    if (!(arch >= 60 && arch <= 99) && low >= 0x60 && low <= 0x99 && (low & 0xf) <= 9)
        arch = (low >> 4) * 10 + (low & 0xf);
    switch (arch) {
    case 68:
    case 69:
    case 73:
    case 75:
    case 79:
    case 81:
        break;
    default:
        fprintf(stderr, "CDSP VPP: unsupported ARCH_VER 0x%x; no matching skeleton\n",
                request.capability);
        return -ENOTSUP;
    }
    constexpr char suffix[] = "?iris_vpp_skel_handle_invoke&_modver=1.0&_dom=cdsp";
    char uri[sizeof(IRIS_VPP_DSP_DIR) + sizeof(suffix) + 64];
    // FastRPC's file:/// prefix is followed by the module path; an absolute
    // install path therefore starts with four slashes. Prefer it unless the
    // caller explicitly supplies a search path (e.g. for an uninstalled build).
    const char *search_path = getenv("ADSP_LIBRARY_PATH");
    bool search_first = search_path && *search_path;
    auto open = [&](bool search) {
        snprintf(uri, sizeof(uri),
                 search ? "file:///vpp_v%u_skel.so%s"
                        : "file:///" IRIS_VPP_DSP_DIR "/vpp_v%u_skel.so%s",
                 arch, suffix);
        return iris_vpp_open(uri, handle);
    };
    e = open(search_first);
    if (e)
        e = open(!search_first);
    if (e)
        fprintf(stderr, "CDSP VPP: cannot load vpp_v%u_skel.so: %d\n", arch, e);
    return e;
}
