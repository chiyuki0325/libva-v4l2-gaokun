// SPDX-License-Identifier: MIT
#ifndef IRIS_VPP_OVERLAY_H
#define IRIS_VPP_OVERLAY_H
#include <stdint.h>
// Shared host/DSP ABI and Vulkan push constants. All offsets/strides are bytes.
#define OVERLAY_P010 1
#define OVERLAY_BGRA 2
#define OVERLAY_PREMULTIPLIED 4
typedef struct {
    int32_t width, height, src_stride, src_y, src_uv, dst_stride, dst_y, dst_uv;
    int32_t rgb_stride, rgb_offset, rgb_width, rgb_height;
    // Destination rectangle, Q8 global alpha (256 = opaque), Q16 sampling steps.
    int32_t x, y, w, h, alpha, flags, step_x, step_y;
    // Q14 RGB-to-YUV coefficients and black level in eight-bit code values.
    int32_t matrix[9], black;
} OverlayParams;
static inline int overlay_plane_fits(uint64_t size, int offset, int stride, int width, int height) {
    return offset >= 0 && stride >= width && width > 0 && height > 0 &&
           (uint64_t)offset + (uint64_t)(height - 1) * stride + width <= size;
}
static inline int overlay_valid(const OverlayParams *p, uint64_t src_size, uint64_t rgb_size,
                                uint64_t dst_size) {
    int bytes = (p->flags & OVERLAY_P010) ? 2 : 1;
    if (p->width < 2 || p->width > 8192 || p->height < 2 || p->height > 8192 ||
        ((p->width | p->height) & 1) || p->rgb_width < 1 || p->rgb_width > 8192 ||
        p->rgb_height < 1 || p->rgb_height > 8192 || p->w < 1 || p->w > 8192 || p->h < 1 ||
        p->h > 8192 || p->x < -32768 || p->x > 32767 || p->y < -32768 || p->y > 32767 ||
        p->alpha < 0 || p->alpha > 256 || (p->flags & ~7) ||
        ((p->src_stride | p->dst_stride | p->src_y | p->src_uv | p->dst_y | p->dst_uv) & 1) ||
        ((p->rgb_stride | p->rgb_offset) & 3) || p->black < 0 || p->black > 16 ||
        p->step_x != (p->rgb_width << 16) / p->w || p->step_y != (p->rgb_height << 16) / p->h)
        return 0;
    for (int i = 0; i < 9; ++i)
        if (p->matrix[i] < -16384 || p->matrix[i] > 16384)
            return 0;
    for (int row = 0; row < 3; ++row) {
        int magnitude = 0;
        for (int col = 0; col < 3; ++col) {
            int v = p->matrix[row * 3 + col];
            magnitude += v < 0 ? -v : v;
        }
        if (magnitude > 32768)
            return 0;
    }
    // Separate Y/UV planes; permits an exactly matching base/output allocation.
    if ((uint64_t)p->src_y + (uint64_t)p->src_stride * p->height > (uint64_t)p->src_uv ||
        (uint64_t)p->dst_y + (uint64_t)p->dst_stride * p->height > (uint64_t)p->dst_uv)
        return 0;
    return overlay_plane_fits(src_size, p->src_y, p->src_stride, p->width * bytes, p->height) &&
           overlay_plane_fits(src_size, p->src_uv, p->src_stride, p->width * bytes,
                              p->height / 2) &&
           overlay_plane_fits(dst_size, p->dst_y, p->dst_stride, p->width * bytes, p->height) &&
           overlay_plane_fits(dst_size, p->dst_uv, p->dst_stride, p->width * bytes,
                              p->height / 2) &&
           overlay_plane_fits(rgb_size, p->rgb_offset, p->rgb_stride, p->rgb_width * 4,
                              p->rgb_height);
}
#endif
