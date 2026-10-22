// SPDX-License-Identifier: MIT
#include "iris_vpp.h"
#include "skel.h"
#include "vpp_limits.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static uint32_t capability;
static uint32_t expected_attribute = ARCH_VER;
static int query_error, failed_opens, queries;
static std::vector<std::string> opened;

static void check(bool ok) {
    if (!ok) {
        fprintf(stderr, "FAIL: CDSP selection (attribute=%u, capability=0x%x, failed opens=%d)\n",
                expected_attribute, capability, failed_opens);
        std::exit(1);
    }
}

extern "C" int remote_handle_control(uint32_t req, void *data, uint32_t size) {
    check(req == DSPRPC_GET_DSP_INFO && size == sizeof(fastrpc_capability));
    auto *request = static_cast<fastrpc_capability *>(data);
    check(request->domain == CDSP_DOMAIN_ID && request->attribute_ID == expected_attribute);
    queries++;
    request->capability = capability;
    return query_error;
}

extern "C" int iris_vpp_open(const char *uri, remote_handle64 *handle) {
    check(queries == 1);
    opened.emplace_back(uri);
    if (opened.size() <= static_cast<size_t>(failed_opens))
        return -ENOENT;
    *handle = 123;
    return 0;
}

static void reset(uint32_t arch, int failures = 0, int error = 0) {
    capability = arch;
    failed_opens = failures;
    query_error = error;
    queries = 0;
    opened.clear();
}

int main() {
    const std::string suffix = "?iris_vpp_skel_handle_invoke&_modver=1.0&_dom=cdsp";
    for (const char *search_path : {static_cast<const char *>(nullptr), "", "/tmp/vpp-test"}) {
        if (search_path)
            check(setenv("ADSP_LIBRARY_PATH", search_path, 1) == 0);
        else
            check(unsetenv("ADSP_LIBRARY_PATH") == 0);
        for (uint32_t arch : {68u, 69u, 73u, 75u, 79u, 81u}) {
            uint32_t packed = ((arch / 10) << 4) | (arch % 10);
            for (uint32_t raw : {arch, packed, 0x100u | packed}) {
                for (int failures : {0, 1, 2}) {
                    reset(raw, failures);
                    remote_handle64 handle = 0;
                    int e = cdsp_vpp_open_skel(&handle);
                    check(e == (failures == 2 ? -ENOENT : 0));
                    check(handle == (failures == 2 ? 0 : 123));
                    check(queries == 1 && opened.size() == (failures ? 2u : 1u));
                    std::string filename = "vpp_v" + std::to_string(arch) + "_skel.so";
                    std::string search_uri = "file:///" + filename + suffix;
                    std::string install_uri = "file:///" IRIS_VPP_DSP_DIR "/" + filename + suffix;
                    bool search_first = search_path && *search_path;
                    check(opened[0] == (search_first ? search_uri : install_uri));
                    if (failures)
                        check(opened[1] == (search_first ? install_uri : search_uri));
                }
            }
        }
    }
    for (uint32_t raw : {0u, 67u, 70u, 74u, 80u, 83u, 0x6fu, 0x74u, 0x83u, UINT32_MAX}) {
        reset(raw);
        remote_handle64 handle = 0;
        check(cdsp_vpp_open_skel(&handle) == -ENOTSUP);
        check(queries == 1 && opened.empty() && handle == 0);
    }
    reset(68, 0, -EIO);
    remote_handle64 handle = 0;
    check(cdsp_vpp_open_skel(&handle) == -EIO);
    check(queries == 1 && opened.empty() && handle == 0);

    expected_attribute = HVX_SUPPORT_128B;
    for (uint32_t count : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, UINT32_MAX}) {
        for (int requested = 0; requested <= CDSP_VPP_MAX_WORKERS; requested++) {
            reset(count);
            int workers = requested;
            int e = cdsp_vpp_select_workers(&workers);
            check(e == (!count && requested ? -ENOTSUP : 0));
            if (!e) {
                check(workers <= requested && (uint32_t)workers <= count);
                check(workers == requested || (uint32_t)workers == count);
            } else {
                check(workers == requested);
            }
            check(queries == (requested ? 1 : 0) && opened.empty());
        }
    }
    for (int requested = 0; requested <= CDSP_VPP_MAX_WORKERS; requested++) {
        reset(4, 0, -EIO);
        int workers = requested;
        check(cdsp_vpp_select_workers(&workers) == (requested ? -EIO : 0));
        check(workers == requested && queries == (requested ? 1 : 0) && opened.empty());
    }
    reset(8);
    check(cdsp_vpp_select_workers(nullptr) == -EINVAL);
    for (int requested : {-1, CDSP_VPP_MAX_WORKERS + 1}) {
        int workers = requested;
        check(cdsp_vpp_select_workers(&workers) == -EINVAL && workers == requested);
    }
    check(queries == 0 && opened.empty());
    puts("PASS: skeleton selection and HVX worker limits, including unavailable/query failure "
         "paths");
}
