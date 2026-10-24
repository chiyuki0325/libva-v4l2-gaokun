// SPDX-License-Identifier: MIT
#ifndef IRIS_VPP_H
#define IRIS_VPP_H

#include <stdint.h>
#include <remote.h>

#ifdef __cplusplus
extern "C" {
#endif

int iris_vpp_open(const char *uri, remote_handle64 *handle);
int iris_vpp_close(remote_handle64 handle);
int iris_vpp_power(remote_handle64 handle, int *mask);
int iris_vpp_configure(remote_handle64 handle, int src_width, int src_height, int dst_width,
                       int dst_height);
int iris_vpp_run(remote_handle64 handle, const uint8_t *src, int src_size, uint8_t *dst,
                 int dst_size, int src_stride, int dst_stride, int src_y_offset, int src_uv_offset,
                 int dst_y_offset, int dst_uv_offset, int worker_count, uint64_t *usec);
int iris_vpp_info(remote_handle64 handle, unsigned int *hz, int *hvx_units);
int iris_vpp_profile(remote_handle64 handle, int enable, uint64_t *counters, int counter_count);
int iris_vpp_process(remote_handle64 handle, const uint8_t *src, int src_size, uint8_t *dst,
                     int dst_size, int src_stride, int dst_stride, int src_y_offset,
                     int src_uv_offset, int dst_y_offset, int dst_uv_offset, int worker_count,
                     int src_format, int dst_format, uint64_t *usec);
int iris_vpp_transform(remote_handle64 handle, const uint8_t *src, int src_size, uint8_t *dst,
                       int dst_size, int src_stride, int dst_stride, int src_y_offset,
                       int src_uv_offset, int dst_y_offset, int dst_uv_offset, int worker_count,
                       int src_format, int dst_format, int rotation, uint64_t *usec);
int iris_vpp_overlay(remote_handle64 handle, const uint8_t *base, int base_size,
                     const uint8_t *rgba, int rgba_size, uint8_t *dst, int dst_size,
                     const uint8_t *params, int params_size, int worker_count, uint64_t *usec);

#ifdef __cplusplus
}
#endif
#endif
