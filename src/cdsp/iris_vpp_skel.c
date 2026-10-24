// SPDX-License-Identifier: MIT
#include "iris_vpp_rpc.h"

#ifndef IRIS_VPP_SKEL_NAME
#define IRIS_VPP_SKEL_NAME "libiris_vpp_skel.so"
#endif
// Keep loader metadata for the existing QAIC 1.00.51-compatible wire ABI.
__attribute__((visibility("default"))) uint32_t iris_vpp_skel_handle_invoke_qaic_version = 10051;
__attribute__((visibility("default"))) char iris_vpp_skel_handle_invoke_uri[] =
    "file:///" IRIS_VPP_SKEL_NAME "?iris_vpp_skel_handle_invoke&_modver=1.0";

static int buffer_fits(const remote_arg *arg, size_t bytes) {
    return arg->buf.nLen >= bytes && (!bytes || arg->buf.pv != NULL);
}

static int sequence_fits(const remote_arg *arg, int32_t count, size_t element_size) {
    return count >= 0 && (size_t)count <= arg->buf.nLen / element_size &&
           (!count || arg->buf.pv != NULL);
}

static void copy_inout(const remote_arg *input, const remote_arg *output, size_t bytes) {
    if (bytes && input->buf.pv != output->buf.pv)
        memmove(output->buf.pv, input->buf.pv, bytes);
}

static int dispatch_frame(remote_handle64 handle, unsigned method, remote_arg *args) {
    int32_t p[13] = {0};
    size_t size = (method == IRIS_VPP_RUN ? 10 : method == IRIS_VPP_PROCESS ? 12 : 13) * 4;
    if (!buffer_fits(&args[0], size) || !buffer_fits(&args[3], sizeof(uint64_t)))
        return AEE_EBADPARM;
    memcpy(p, args[0].buf.pv, size);
    if (!sequence_fits(&args[1], p[0], 1) || !sequence_fits(&args[2], p[1], 1) ||
        !sequence_fits(&args[4], p[2], 1) || p[1] > p[2])
        return AEE_EBADPARM;
    copy_inout(&args[2], &args[4], p[1]);
    const uint8_t *src = args[1].buf.pv;
    uint8_t *dst = args[4].buf.pv;
    uint64_t usec = 0;
    int err;
    if (method == IRIS_VPP_RUN)
        err = iris_vpp_run(handle, src, p[0], dst, p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
                           &usec);
    else if (method == IRIS_VPP_PROCESS)
        err = iris_vpp_process(handle, src, p[0], dst, p[2], p[3], p[4], p[5], p[6], p[7], p[8],
                               p[9], p[10], p[11], &usec);
    else
        err = iris_vpp_transform(handle, src, p[0], dst, p[2], p[3], p[4], p[5], p[6], p[7], p[8],
                                 p[9], p[10], p[11], p[12], &usec);
    if (!err)
        memcpy(args[3].buf.pv, &usec, sizeof(usec));
    return err;
}

static int dispatch_overlay(remote_handle64 handle, remote_arg *args) {
    int32_t p[6];
    if (!buffer_fits(&args[0], sizeof(p)) || !buffer_fits(&args[5], sizeof(uint64_t)))
        return AEE_EBADPARM;
    memcpy(p, args[0].buf.pv, sizeof(p));
    if (!sequence_fits(&args[1], p[0], 1) || !sequence_fits(&args[2], p[1], 1) ||
        !sequence_fits(&args[3], p[2], 1) || !sequence_fits(&args[6], p[3], 1) ||
        !sequence_fits(&args[4], p[4], 1) || p[2] > p[3])
        return AEE_EBADPARM;
    copy_inout(&args[3], &args[6], p[2]);
    uint64_t usec = 0;
    int err = iris_vpp_overlay(handle, args[1].buf.pv, p[0], args[2].buf.pv, p[1], args[6].buf.pv,
                               p[3], args[4].buf.pv, p[4], p[5], &usec);
    if (!err)
        memcpy(args[5].buf.pv, &usec, sizeof(usec));
    return err;
}

__attribute__((visibility("default"))) int
iris_vpp_skel_handle_invoke(remote_handle64 handle, uint32_t scalars, remote_arg *args) {
    static const uint32_t signatures[] = {
        [IRIS_VPP_OPEN] = REMOTE_SCALARS_MAKEX(0, IRIS_VPP_OPEN, 2, 0, 0, 1),
        [IRIS_VPP_CLOSE] = REMOTE_SCALARS_MAKEX(0, IRIS_VPP_CLOSE, 0, 0, 1, 0),
        [IRIS_VPP_POWER] = REMOTE_SCALARS_MAKE(IRIS_VPP_POWER, 0, 1),
        [IRIS_VPP_CONFIGURE] = REMOTE_SCALARS_MAKE(IRIS_VPP_CONFIGURE, 1, 0),
        [IRIS_VPP_RUN] = REMOTE_SCALARS_MAKE(IRIS_VPP_RUN, 3, 2),
        [IRIS_VPP_INFO] = REMOTE_SCALARS_MAKE(IRIS_VPP_INFO, 0, 1),
        [IRIS_VPP_PROFILE] = REMOTE_SCALARS_MAKE(IRIS_VPP_PROFILE, 1, 1),
        [IRIS_VPP_PROCESS] = REMOTE_SCALARS_MAKE(IRIS_VPP_PROCESS, 3, 2),
        [IRIS_VPP_TRANSFORM] = REMOTE_SCALARS_MAKE(IRIS_VPP_TRANSFORM, 3, 2),
        [IRIS_VPP_OVERLAY] = REMOTE_SCALARS_MAKE(IRIS_VPP_OVERLAY, 5, 2),
    };
    unsigned method = REMOTE_SCALARS_METHOD(scalars);
    if (method >= sizeof(signatures) / sizeof(signatures[0]))
        return AEE_EUNSUPPORTED;
    // Attribute bits belong to the transport; validate all counts before
    // indexing args. The loader supplies the DSP context as the separate handle.
    if (!args || (scalars & UINT32_C(0x1fffffff)) != signatures[method])
        return AEE_EBADPARM;
    switch (method) {
    case IRIS_VPP_OPEN: {
        uint32_t length;
        if (!buffer_fits(&args[0], sizeof(length)))
            return AEE_EBADPARM;
        memcpy(&length, args[0].buf.pv, sizeof(length));
        if (!length || !buffer_fits(&args[1], length) ||
            ((const char *)args[1].buf.pv)[length - 1] != '\0')
            return AEE_EBADPARM;
        remote_handle64 result = 0;
        int err = iris_vpp_open(args[1].buf.pv, &result);
        if (!err)
            args[2].h64 = result;
        return err;
    }
    case IRIS_VPP_CLOSE:
        return iris_vpp_close(args[0].h64);
    case IRIS_VPP_POWER: {
        if (!buffer_fits(&args[0], sizeof(int32_t)))
            return AEE_EBADPARM;
        int mask = 0;
        int err = iris_vpp_power(handle, &mask);
        if (!err)
            memcpy(args[0].buf.pv, &mask, sizeof(mask));
        return err;
    }
    case IRIS_VPP_CONFIGURE: {
        int32_t p[4];
        if (!buffer_fits(&args[0], sizeof(p)))
            return AEE_EBADPARM;
        memcpy(p, args[0].buf.pv, sizeof(p));
        return iris_vpp_configure(handle, p[0], p[1], p[2], p[3]);
    }
    case IRIS_VPP_INFO: {
        if (!buffer_fits(&args[0], 8))
            return AEE_EBADPARM;
        unsigned int hz = 0;
        int units = 0;
        int err = iris_vpp_info(handle, &hz, &units);
        if (!err) {
            memcpy(args[0].buf.pv, &hz, 4);
            memcpy((uint8_t *)args[0].buf.pv + 4, &units, 4);
        }
        return err;
    }
    case IRIS_VPP_PROFILE: {
        int32_t p[2];
        if (!buffer_fits(&args[0], sizeof(p)))
            return AEE_EBADPARM;
        memcpy(p, args[0].buf.pv, sizeof(p));
        if (!sequence_fits(&args[1], p[1], sizeof(uint64_t)) ||
            (p[1] && (uintptr_t)args[1].buf.pv % _Alignof(uint64_t)))
            return AEE_EBADPARM;
        return iris_vpp_profile(handle, p[0], args[1].buf.pv, p[1]);
    }
    case IRIS_VPP_RUN:
    case IRIS_VPP_PROCESS:
    case IRIS_VPP_TRANSFORM:
        return dispatch_frame(handle, method, args);
    case IRIS_VPP_OVERLAY:
        return dispatch_overlay(handle, args);
    }
    return AEE_EUNSUPPORTED;
}
