// SPDX-License-Identifier: MIT
#ifndef CDSP_OVERLAY_H
#define CDSP_OVERLAY_H
#include "../overlay-params.h"
// Premultiply before interpolation: invisible RGB must not darken scaled edges.
typedef struct {
    int r, g, b, a;
} OverlayPixel;
static inline int overlay_clamp(int x, int lo, int hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
static inline OverlayPixel overlay_pixel(const OverlayParams *p, const uint8_t *rgb, int x, int y) {
    const uint8_t *v = rgb + p->rgb_offset + y * p->rgb_stride + x * 4;
    int a = v[3], factor = p->flags & OVERLAY_PREMULTIPLIED ? 255 : a;
    OverlayPixel result = {v[p->flags & OVERLAY_BGRA ? 2 : 0] * factor, v[1] * factor,
                           v[p->flags & OVERLAY_BGRA ? 0 : 2] * factor, a};
    return result;
}
static inline OverlayPixel overlay_lerp(OverlayPixel a, OverlayPixel b, int weight) {
    OverlayPixel result = {(a.r * (256 - weight) + b.r * weight + 128) >> 8,
                           (a.g * (256 - weight) + b.g * weight + 128) >> 8,
                           (a.b * (256 - weight) + b.b * weight + 128) >> 8,
                           (a.a * (256 - weight) + b.a * weight + 128) >> 8};
    return result;
}
static inline OverlayPixel overlay_sample(const OverlayParams *p, const uint8_t *rgb, int x,
                                          int y) {
    OverlayPixel v = {0, 0, 0, 0};
    x -= p->x;
    y -= p->y;
    if (!p->alpha || x < 0 || y < 0 || x >= p->w || y >= p->h)
        return v;
    if (p->w == p->rgb_width && p->h == p->rgb_height)
        v = overlay_pixel(p, rgb, x, y);
    else {
        int sx = overlay_clamp(x * p->step_x + p->step_x / 2 - 32768, 0, (p->rgb_width - 1) << 16);
        int sy = overlay_clamp(y * p->step_y + p->step_y / 2 - 32768, 0, (p->rgb_height - 1) << 16);
        int x0 = sx >> 16, y0 = sy >> 16;
        int x1 = overlay_clamp(x0 + 1, 0, p->rgb_width - 1),
            y1 = overlay_clamp(y0 + 1, 0, p->rgb_height - 1);
        v = overlay_lerp(overlay_lerp(overlay_pixel(p, rgb, x0, y0), overlay_pixel(p, rgb, x1, y0),
                                      (sx >> 8) & 255),
                         overlay_lerp(overlay_pixel(p, rgb, x0, y1), overlay_pixel(p, rgb, x1, y1),
                                      (sx >> 8) & 255),
                         (sy >> 8) & 255);
    }
    v.r = (v.r * p->alpha + 128) >> 8;
    v.g = (v.g * p->alpha + 128) >> 8;
    v.b = (v.b * p->alpha + 128) >> 8;
    v.a = (v.a * p->alpha + 128) >> 8;
    return v;
}
static inline int overlay_dot(const int32_t *c, OverlayPixel v) {
    return (c[0] * v.r + c[1] * v.g + c[2] * v.b + 8192) >> 14;
}
static inline int overlay_load(const uint8_t *src, int offset, int wide) {
    return wide ? *(const uint16_t *)(src + offset) >> 6 : src[offset];
}
static inline void overlay_store(uint8_t *dst, int offset, int value, int wide) {
    value = overlay_clamp(value, 0, wide ? 1023 : 255);
    if (wide)
        *(uint16_t *)(dst + offset) = value << 6;
    else
        dst[offset] = value;
}
static inline void overlay_block(const OverlayParams *p, const uint8_t *src, const uint8_t *rgb,
                                 uint8_t *dst, int x, int y) {
    int wide = !!(p->flags & OVERLAY_P010), bytes = 1 + wide, scale = wide ? 4 : 1;
    int sum_a = 0, sum_u = 0, sum_v = 0;
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 2; ++col) {
            OverlayPixel v = overlay_sample(p, rgb, x + col, y + row);
            int base =
                overlay_load(src, p->src_y + (y + row) * p->src_stride + (x + col) * bytes, wide);
            int value = (base * (255 - v.a) + p->black * scale * v.a +
                         overlay_dot(p->matrix, v) * scale + 127) /
                        255;
            overlay_store(dst, p->dst_y + (y + row) * p->dst_stride + (x + col) * bytes, value,
                          wide);
            sum_a += v.a;
            sum_u += overlay_dot(p->matrix + 3, v);
            sum_v += overlay_dot(p->matrix + 6, v);
        }
    // One owner per 2x2 block. Average premultiplied chroma and coverage together,
    // including uncovered pixels at odd/clipped rectangle edges.
    for (int c = 0; c < 2; ++c) {
        int base = overlay_load(src, p->src_uv + (y / 2) * p->src_stride + (x + c) * bytes, wide);
        int value =
            (base * (1020 - sum_a) + 128 * scale * sum_a + (c ? sum_v : sum_u) * scale + 510) /
            1020;
        overlay_store(dst, p->dst_uv + (y / 2) * p->dst_stride + (x + c) * bytes, value, wide);
    }
}
// Thirty-two RGBA pixels occupy one HVX register. Keep color and coverage in
// 32-bit lanes until the final pack; this also preserves P010 rounding.
typedef int32_t OverlayWords __attribute__((ext_vector_type(32)));
static inline OverlayWords overlay_div255(OverlayWords n) {
    // Exact for the nonnegative blend numerators (at most 1020*1023+510).
    OverlayWords q = (n + (n >> 8) + (n >> 16)) >> 8;
    return q + ((n - q * 255 > 254) & 1);
}
static inline OverlayWords overlay_mul(OverlayWords a, OverlayWords b) {
    return (OverlayWords)Q6_Vw_vmpyie_VwVuh((HVX_Vector)a, (HVX_Vector)b);
}
static inline OverlayWords overlay_dot_vector(const int32_t *c, OverlayWords r, OverlayWords g,
                                              OverlayWords b) {
    HVX_Vector v = Q6_Vw_vmpyi_VwRh((HVX_Vector)r, (uint16_t)c[0] * 0x10001u);
    v = Q6_Vw_vmpyiacc_VwVwRh(v, (HVX_Vector)g, (uint16_t)c[1] * 0x10001u);
    v = Q6_Vw_vmpyiacc_VwVwRh(v, (HVX_Vector)b, (uint16_t)c[2] * 0x10001u);
    return ((OverlayWords)v + 8192) >> 14;
}
static inline OverlayWords overlay_load_vector(const uint8_t *src, int wide, int remaining) {
    HVX_Vector raw;
    if (remaining >= 128)
        raw = *(const UnalignedVector *)src;
    else {
        raw = Q6_V_vzero();
        if (wide)
            __builtin_memcpy_inline(&raw, src, 64);
        else
            __builtin_memcpy_inline(&raw, src, 32);
    }
    if (!wide)
        raw = Q6_V_lo_W(Q6_Wuh_vunpack_Vub(raw));
    OverlayWords v = (OverlayWords)Q6_V_lo_W(Q6_Wuw_vunpack_Vuh(raw));
    return wide ? v >> 6 : v;
}
static inline void overlay_store_vector(uint8_t *dst, OverlayWords v, int wide) {
    v = v < 0 ? 0 : v;
    int max = wide ? 1023 : 255;
    v = v > max ? max : v;
    if (wide)
        v <<= 6;
    HVX_Vector packed = Q6_Vh_vpacke_VwVw((HVX_Vector)v, (HVX_Vector)v);
    if (!wide)
        packed = Q6_Vb_vpacke_VhVh(packed, packed);
    store_partial_vector(dst, packed, 32 * (wide ? 2 : 1));
}
static inline OverlayWords overlay_pairs(OverlayWords v) {
    return v + (OverlayWords)Q6_V_vror_VR((HVX_Vector)v, 4);
}
static inline void overlay_copy(uint8_t *dst, const uint8_t *src, int count, int hvx) {
    if (src == dst || !count)
        return;
    if (hvx) {
        for (; count >= 128; count -= 128, src += 128, dst += 128)
            *(UnalignedVector *)dst = *(const UnalignedVector *)src;
    }
    for (; count >= 8; count -= 8, src += 8, dst += 8) {
        uint64_t word;
        __builtin_memcpy_inline(&word, src, 8);
        __builtin_memcpy_inline(dst, &word, 8);
    }
    for (int i = 0; i < count; ++i)
        dst[i] = src[i];
}
typedef struct {
    OverlayWords r, g, b, a;
} OverlayVectorPixel;
static inline OverlayVectorPixel overlay_unpack(const OverlayParams *p, OverlayWords v) {
    OverlayWords a = (v >> 24) & 255,
                 factor = p->flags & OVERLAY_PREMULTIPLIED ? (OverlayWords)255 : a;
    OverlayVectorPixel result = {
        overlay_mul((v >> (p->flags & OVERLAY_BGRA ? 16 : 0)) & 255, factor),
        overlay_mul((v >> 8) & 255, factor),
        overlay_mul((v >> (p->flags & OVERLAY_BGRA ? 0 : 16)) & 255, factor), a};
    return result;
}
static inline OverlayVectorPixel overlay_mix(OverlayVectorPixel a, OverlayVectorPixel b,
                                             OverlayWords weight) {
    OverlayWords inverse = 256 - weight;
    OverlayVectorPixel result = {(overlay_mul(a.r, inverse) + overlay_mul(b.r, weight) + 128) >> 8,
                                 (overlay_mul(a.g, inverse) + overlay_mul(b.g, weight) + 128) >> 8,
                                 (overlay_mul(a.b, inverse) + overlay_mul(b.b, weight) + 128) >> 8,
                                 (overlay_mul(a.a, inverse) + overlay_mul(b.a, weight) + 128) >> 8};
    return result;
}
// A contiguous 32-pixel source window covers the bilinear taps for enlargement.
// A byte LUT selects RGBA neighbors without scalar gathers.
static inline HVX_Vector overlay_lookup(HVX_Vector indices, HVX_Vector table) {
    table = Q6_Vb_vshuff_Vb(table);
    HVX_Vector v = Q6_Vb_vlut32_VbVbI(indices, table, 0);
    v = Q6_Vb_vlut32or_VbVbVbI(v, indices, table, 1);
    v = Q6_Vb_vlut32or_VbVbVbI(v, indices, table, 2);
    return Q6_Vb_vlut32or_VbVbVbI(v, indices, table, 3);
}
static OverlayVectorPixel overlay_sample_vector(const OverlayParams *p, const uint8_t *rgb, int x,
                                                int y) {
    const OverlayWords lanes = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    OverlayWords pos = (lanes + x - p->x) * p->step_x + p->step_x / 2 - 32768;
    pos = pos < 0 ? 0 : pos;
    int max = (p->rgb_width - 1) << 16;
    pos = pos > max ? max : pos;
    int first = overlay_clamp((x - p->x) * p->step_x + p->step_x / 2 - 32768, 0, max) >> 16;
    int table_start = first < p->rgb_width - 32 ? first : p->rgb_width - 32;
    OverlayWords left = pos >> 16, right = left + 1;
    right = right > p->rgb_width - 1 ? p->rgb_width - 1 : right;
    HVX_Vector il = (HVX_Vector)((left - table_start) * 0x04040404 + 0x03020100);
    HVX_Vector ir = (HVX_Vector)((right - table_start) * 0x04040404 + 0x03020100);
    int sy =
        overlay_clamp((y - p->y) * p->step_y + p->step_y / 2 - 32768, 0, (p->rgb_height - 1) << 16);
    int top = sy >> 16, bottom = top + 1 < p->rgb_height ? top + 1 : top;
    const uint8_t *row = rgb + p->rgb_offset + table_start * 4;
    HVX_Vector a = *(const UnalignedVector *)(row + top * p->rgb_stride),
               b = *(const UnalignedVector *)(row + bottom * p->rgb_stride);
    OverlayVectorPixel upper =
        overlay_mix(overlay_unpack(p, (OverlayWords)overlay_lookup(il, a)),
                    overlay_unpack(p, (OverlayWords)overlay_lookup(ir, a)), (pos >> 8) & 255);
    OverlayVectorPixel lower =
        overlay_mix(overlay_unpack(p, (OverlayWords)overlay_lookup(il, b)),
                    overlay_unpack(p, (OverlayWords)overlay_lookup(ir, b)), (pos >> 8) & 255);
    return overlay_mix(upper, lower, (OverlayWords)((sy >> 8) & 255));
}
static void overlay_vector(const OverlayParams *p, const uint8_t *src, const uint8_t *rgb,
                           uint8_t *dst, int x, int y) {
    int wide = !!(p->flags & OVERLAY_P010), bytes = 1 + wide, scale = wide ? 4 : 1;
    OverlayVectorPixel pixels[2];
    int scaled = p->w != p->rgb_width || p->h != p->rgb_height;
    for (int row = 0; row < 2; ++row) {
        if (scaled)
            pixels[row] = overlay_sample_vector(p, rgb, x, y + row);
        else
            pixels[row] = overlay_unpack(
                p, (OverlayWords) * (const UnalignedVector *)(rgb + p->rgb_offset +
                                                              (y + row - p->y) * p->rgb_stride +
                                                              (x - p->x) * 4));
    }
    if (!__builtin_reduce_or(pixels[0].a | pixels[1].a)) {
        for (int row = 0; row < 3; ++row) {
            const uint8_t *in = src +
                                (row < 2 ? p->src_y + (y + row) * p->src_stride
                                         : p->src_uv + (y / 2) * p->src_stride) +
                                x * bytes;
            uint8_t *out = dst +
                           (row < 2 ? p->dst_y + (y + row) * p->dst_stride
                                    : p->dst_uv + (y / 2) * p->dst_stride) +
                           x * bytes;
            if (in != out) {
                HVX_Vector value;
                if ((p->width - x) * bytes >= 128)
                    value = *(const UnalignedVector *)in;
                else if (wide)
                    __builtin_memcpy_inline(&value, in, 64);
                else
                    __builtin_memcpy_inline(&value, in, 32);
                store_partial_vector(out, value, 32 * bytes);
            }
        }
        return;
    }
    OverlayWords sum_a = 0, sum_u = 0, sum_v = 0;
    for (int row = 0; row < 2; ++row) {
        OverlayWords a = pixels[row].a, r = pixels[row].r, g = pixels[row].g, b = pixels[row].b;
        a = (overlay_mul(a, (OverlayWords)p->alpha) + 128) >> 8;
        r = (overlay_mul(r, (OverlayWords)p->alpha) + 128) >> 8;
        g = (overlay_mul(g, (OverlayWords)p->alpha) + 128) >> 8;
        b = (overlay_mul(b, (OverlayWords)p->alpha) + 128) >> 8;
        OverlayWords yy = overlay_dot_vector(p->matrix, r, g, b);
        sum_u += overlay_dot_vector(p->matrix + 3, r, g, b);
        sum_v += overlay_dot_vector(p->matrix + 6, r, g, b);
        sum_a += a;
        OverlayWords base = overlay_load_vector(
            src + p->src_y + (y + row) * p->src_stride + x * bytes, wide, (p->width - x) * bytes);
        OverlayWords value =
            overlay_div255(base * (255 - a) + p->black * scale * a + yy * scale + 127);
        overlay_store_vector(dst + p->dst_y + (y + row) * p->dst_stride + x * bytes, value, wide);
    }
    OverlayWords u = overlay_pairs(sum_u), v = overlay_pairs(sum_v), a = overlay_pairs(sum_a);
    OverlayWords uv =
        __builtin_shufflevector(u, v, 0, 32, 2, 34, 4, 36, 6, 38, 8, 40, 10, 42, 12, 44, 14, 46, 16,
                                48, 18, 50, 20, 52, 22, 54, 24, 56, 26, 58, 28, 60, 30, 62);
    OverlayWords alpha =
        __builtin_shufflevector(a, a, 0, 0, 2, 2, 4, 4, 6, 6, 8, 8, 10, 10, 12, 12, 14, 14, 16, 16,
                                18, 18, 20, 20, 22, 22, 24, 24, 26, 26, 28, 28, 30, 30);
    OverlayWords base = overlay_load_vector(src + p->src_uv + (y / 2) * p->src_stride + x * bytes,
                                            wide, (p->width - x) * bytes);
    OverlayWords value =
        overlay_div255(base * (1020 - alpha) + 128 * scale * alpha + uv * scale + 510) >> 2;
    overlay_store_vector(dst + p->dst_uv + (y / 2) * p->dst_stride + x * bytes, value, wide);
}
static void overlay_rows(const OverlayParams *p, const uint8_t *src, const uint8_t *rgb,
                         uint8_t *dst, int worker, int workers, int hvx) {
    int bytes = p->flags & OVERLAY_P010 ? 2 : 1;
    int left = overlay_clamp(p->x & ~1, 0, p->width),
        right = overlay_clamp((p->x + p->w + 1) & ~1, 0, p->width);
    // Interleave chroma rows so a subtitle near the bottom uses every worker.
    for (int y = worker * 2; y < p->height; y += workers * 2) {
        int active = p->alpha && y < p->y + p->h && y + 2 > p->y && right > left;
        int start = active ? left : p->width, stop = active ? right : p->width;
        for (int row = 0; row < 3; ++row) {
            const uint8_t *in = src + (row < 2 ? p->src_y + (y + row) * p->src_stride
                                               : p->src_uv + (y / 2) * p->src_stride);
            uint8_t *out = dst + (row < 2 ? p->dst_y + (y + row) * p->dst_stride
                                          : p->dst_uv + (y / 2) * p->dst_stride);
            if (in != out) {
                overlay_copy(out, in, start * bytes, hvx);
                overlay_copy(out + stop * bytes, in + stop * bytes, (p->width - stop) * bytes, hvx);
            }
        }
        for (int x = start; x < stop;) {
            if (hvx && p->w >= p->rgb_width && p->rgb_width >= 32 && x >= p->x &&
                x + 32 <= p->x + p->w && x + 32 <= stop && y >= p->y && y + 2 <= p->y + p->h) {
                int first = overlay_clamp((x - p->x) * p->step_x + p->step_x / 2 - 32768, 0,
                                          (p->rgb_width - 1) << 16) >>
                            16;
                int last = overlay_clamp((x + 31 - p->x) * p->step_x + p->step_x / 2 - 32768, 0,
                                         (p->rgb_width - 1) << 16) >>
                           16;
                if ((p->w != p->rgb_width || p->h != p->rgb_height) && last - first >= 31) {
                    overlay_block(p, src, rgb, dst, x, y);
                    x += 2;
                    continue;
                }
                overlay_vector(p, src, rgb, dst, x, y);
                x += 32;
            } else {
                overlay_block(p, src, rgb, dst, x, y);
                x += 2;
            }
        }
    }
}
#endif
