// SPDX-License-Identifier: MIT
#include "iris_vpp_rpc.h"

int iris_vpp_open(const char *uri, remote_handle64 *handle) {
    if (!uri || !handle)
        return AEE_EBADPARM;
    return remote_handle64_open(uri, handle);
}

int iris_vpp_close(remote_handle64 handle) {
    return remote_handle64_close(handle);
}

int iris_vpp_power(remote_handle64 handle, int *mask) {
    if (!mask)
        return AEE_EBADPARM;
    int32_t result = 0;
    remote_arg args[] = {{.buf = {&result, sizeof(result)}}};
    int err = remote_handle64_invoke(handle, REMOTE_SCALARS_MAKE(IRIS_VPP_POWER, 0, 1), args);
    if (!err)
        *mask = result;
    return err;
}

int iris_vpp_configure(remote_handle64 handle, int src_width, int src_height, int dst_width,
                       int dst_height) {
    int32_t params[] = {src_width, src_height, dst_width, dst_height};
    remote_arg args[] = {{.buf = {params, sizeof(params)}}};
    return remote_handle64_invoke(handle, REMOTE_SCALARS_MAKE(IRIS_VPP_CONFIGURE, 1, 0), args);
}

int iris_vpp_info(remote_handle64 handle, unsigned int *hz, int *hvx_units) {
    if (!hz || !hvx_units)
        return AEE_EBADPARM;
    uint32_t result[2] = {0};
    remote_arg args[] = {{.buf = {result, sizeof(result)}}};
    int err = remote_handle64_invoke(handle, REMOTE_SCALARS_MAKE(IRIS_VPP_INFO, 0, 1), args);
    if (!err) {
        *hz = result[0];
        memcpy(hvx_units, &result[1], sizeof(result[1]));
    }
    return err;
}

int iris_vpp_profile(remote_handle64 handle, int enable, uint64_t *counters, int counter_count) {
    if (!iris_vpp_valid_buffer(counters, counter_count) ||
        (size_t)counter_count > SIZE_MAX / sizeof(*counters))
        return AEE_EBADPARM;
    int32_t params[] = {enable, counter_count};
    remote_arg args[] = {{.buf = {params, sizeof(params)}},
                         {.buf = {counters, (size_t)counter_count * sizeof(*counters)}}};
    return remote_handle64_invoke(handle, REMOTE_SCALARS_MAKE(IRIS_VPP_PROFILE, 1, 1), args);
}

static int invoke_frame(remote_handle64 handle, unsigned method, int32_t *params, size_t size,
                        const uint8_t *src, uint8_t *dst, uint64_t *usec) {
    if (!usec || !iris_vpp_valid_buffer(src, params[0]) || !iris_vpp_valid_buffer(dst, params[1]))
        return AEE_EBADPARM;
    uint64_t result = 0;
    // The same registered DMA-BUF pointer appears in both dst slots. FastRPC
    // retains its input/output cache semantics and persistent mapping lookup.
    remote_arg args[] = {{.buf = {params, size}},
                         {.buf = {(void *)src, (size_t)params[0]}},
                         {.buf = {dst, (size_t)params[1]}},
                         {.buf = {&result, sizeof(result)}},
                         {.buf = {dst, (size_t)params[2]}}};
    int err = remote_handle64_invoke(handle, REMOTE_SCALARS_MAKE(method, 3, 2), args);
    if (!err)
        *usec = result;
    return err;
}

int iris_vpp_run(remote_handle64 handle, const uint8_t *src, int src_size, uint8_t *dst,
                 int dst_size, int src_stride, int dst_stride, int src_y_offset, int src_uv_offset,
                 int dst_y_offset, int dst_uv_offset, int worker_count, uint64_t *usec) {
    int32_t params[] = {src_size,     dst_size,      dst_size,     src_stride,    dst_stride,
                        src_y_offset, src_uv_offset, dst_y_offset, dst_uv_offset, worker_count};
    return invoke_frame(handle, IRIS_VPP_RUN, params, sizeof(params), src, dst, usec);
}

int iris_vpp_process(remote_handle64 handle, const uint8_t *src, int src_size, uint8_t *dst,
                     int dst_size, int src_stride, int dst_stride, int src_y_offset,
                     int src_uv_offset, int dst_y_offset, int dst_uv_offset, int worker_count,
                     int src_format, int dst_format, uint64_t *usec) {
    int32_t params[] = {src_size,      dst_size,     dst_size,      src_stride,
                        dst_stride,    src_y_offset, src_uv_offset, dst_y_offset,
                        dst_uv_offset, worker_count, src_format,    dst_format};
    return invoke_frame(handle, IRIS_VPP_PROCESS, params, sizeof(params), src, dst, usec);
}

int iris_vpp_transform(remote_handle64 handle, const uint8_t *src, int src_size, uint8_t *dst,
                       int dst_size, int src_stride, int dst_stride, int src_y_offset,
                       int src_uv_offset, int dst_y_offset, int dst_uv_offset, int worker_count,
                       int src_format, int dst_format, int rotation, uint64_t *usec) {
    int32_t params[] = {src_size,     dst_size,      dst_size,     src_stride,    dst_stride,
                        src_y_offset, src_uv_offset, dst_y_offset, dst_uv_offset, worker_count,
                        src_format,   dst_format,    rotation};
    return invoke_frame(handle, IRIS_VPP_TRANSFORM, params, sizeof(params), src, dst, usec);
}

int iris_vpp_overlay(remote_handle64 handle, const uint8_t *base, int base_size,
                     const uint8_t *rgba, int rgba_size, uint8_t *dst, int dst_size,
                     const uint8_t *params, int params_size, int worker_count, uint64_t *usec) {
    if (!usec || !iris_vpp_valid_buffer(base, base_size) ||
        !iris_vpp_valid_buffer(rgba, rgba_size) || !iris_vpp_valid_buffer(dst, dst_size) ||
        !iris_vpp_valid_buffer(params, params_size))
        return AEE_EBADPARM;
    int32_t input[] = {base_size, rgba_size, dst_size, dst_size, params_size, worker_count};
    uint64_t result = 0;
    remote_arg args[] = {{.buf = {input, sizeof(input)}},
                         {.buf = {(void *)base, (size_t)base_size}},
                         {.buf = {(void *)rgba, (size_t)rgba_size}},
                         {.buf = {dst, (size_t)dst_size}},
                         {.buf = {(void *)params, (size_t)params_size}},
                         {.buf = {&result, sizeof(result)}},
                         {.buf = {dst, (size_t)dst_size}}};
    int err = remote_handle64_invoke(handle, REMOTE_SCALARS_MAKE(IRIS_VPP_OVERLAY, 5, 2), args);
    if (!err)
        *usec = result;
    return err;
}
