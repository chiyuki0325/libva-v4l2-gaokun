// SPDX-License-Identifier: MIT
#ifndef CDSP_VPP_CLIENT_H
#define CDSP_VPP_CLIENT_H
#include "vpp_limits.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cdsp_vpp_context cdsp_vpp_context;
typedef struct cdsp_vpp_buffer cdsp_vpp_buffer;
typedef struct {
    cdsp_vpp_buffer *buffer;
    unsigned width, height, stride;
    size_t y_offset, uv_offset;
} cdsp_vpp_frame;
typedef struct {
    unsigned x, y, width, height;
} cdsp_vpp_crop;

// 0 on success; negative errno for host validation/syscalls, positive FastRPC
// error otherwise. One context serializes submissions. Caller must not destroy
// a context/buffer while an operation is using it. Import duplicates the fd.
// DMA/SMMU and DSP mappings are retained until buffer release or destruction of
// the last context. Buffers can outlive contexts and are remapped on later use.
// workers: 0 = scalar diagnostic; 1..CDSP_VPP_MAX_WORKERS = maximum HVX workers.
// Actual concurrency is capped by HVX_SUPPORT_128B. Pass CDSP_VPP_MAX_WORKERS to use the
// largest supported count automatically. HVX query failure or zero 128-byte
// contexts fails creation. max_power enables a per-context
// core/bus MAX vote, released on destroy. Firmware can still thermally
// throttle.
int cdsp_vpp_create(cdsp_vpp_context **out, int workers, int max_power);
void cdsp_vpp_destroy(cdsp_vpp_context *ctx);
int cdsp_vpp_get_info(cdsp_vpp_context *ctx, uint32_t *hz, int *hvx_units);
// Diagnostic only: per-row timestamps perturb timings. Returns the last frame's
// microseconds, four entries per worker: Y vertical/horizontal, UV vertical/horizontal.
int cdsp_vpp_profile(cdsp_vpp_context *ctx, int enable,
                     uint64_t counters[CDSP_VPP_PROFILE_COUNTERS]);
int cdsp_vpp_import(int fd, size_t size, cdsp_vpp_buffer **out);
void cdsp_vpp_release(cdsp_vpp_buffer *buffer);
// The map is provided for diagnostic CPU access only; pair access with
// DMA_BUF_IOCTL_SYNC. Scaling performs no CPU access to frame pixels.
void *cdsp_vpp_buffer_map(cdsp_vpp_buffer *buffer);
int cdsp_vpp_buffer_fd(const cdsp_vpp_buffer *buffer); // borrowed fd
int cdsp_vpp_scale(cdsp_vpp_context *ctx, const cdsp_vpp_frame *src, const cdsp_vpp_crop *crop,
                   const cdsp_vpp_frame *dst, uint64_t *dsp_usec);
#ifdef __cplusplus
}
#endif
#endif
