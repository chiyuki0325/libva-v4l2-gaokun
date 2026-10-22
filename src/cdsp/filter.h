// SPDX-License-Identifier: MIT
#ifndef VPP_FILTER_H
#define VPP_FILTER_H
#include <math.h>
#include <stdint.h>
#include <string.h>
// At most 20:1 reduction, plus room for partially covered endpoint samples.
#define FILTER_MAX_TAPS 22
// Q7 area reduction and Q14 Catmull-Rom enlargement. Weights sum to 1<<shift.
typedef struct {
    int tap_count, shift;
    int indices[FILTER_MAX_TAPS];
    int16_t weights[FILTER_MAX_TAPS];
} Filter;
static inline int clamp_int(int x, int minimum, int maximum) {
    return x < minimum ? minimum : x > maximum ? maximum : x;
}
static inline double catmull_rom_weight(double x) {
    x = fabs(x);
    if (x < 1)
        return (1.5 * x - 2.5) * x * x + 1;
    if (x < 2)
        return ((-.5 * x + 2.5) * x - 4) * x + 2;
    return 0;
}
static inline Filter make_filter(int src_size, int dst_size, int x) {
    Filter filter;
    memset(&filter, 0, sizeof(filter));
    double weights[FILTER_MAX_TAPS];
    int sum = 0;
    filter.shift = src_size >= dst_size ? 7 : 14;
    if (src_size > dst_size) {
        // Area weights are source-pixel overlap with this destination footprint.
        double left = (double)x * src_size / dst_size,
               right = (double)(x + 1) * src_size / dst_size;
        int first = (int)floor(left), last = (int)ceil(right);
        for (int i = first; i < last; i++) {
            double overlap_start = left > i ? left : i, overlap_end = right < i + 1 ? right : i + 1;
            filter.indices[filter.tap_count] = clamp_int(i, 0, src_size - 1);
            weights[filter.tap_count++] = (overlap_end - overlap_start) / (right - left);
        }
    } else if (src_size == dst_size) {
        filter.tap_count = 1;
        filter.indices[0] = x;
        weights[0] = 1;
    } else {
        // Align pixel centers. Clamp source indices to replicate the border,
        // while evaluating the four cubic weights at the unclamped positions.
        double src_position = ((double)x + .5) * src_size / dst_size - .5;
        int first = (int)floor(src_position) - 1;
        for (int i = first; i < first + 4; i++) {
            filter.indices[filter.tap_count] = clamp_int(i, 0, src_size - 1);
            weights[filter.tap_count++] = catmull_rom_weight(src_position - i);
        }
    }
    for (int k = 0; k < filter.tap_count; k++) {
        filter.weights[k] = (int16_t)floor(weights[k] * (1 << filter.shift) + .5);
        sum += filter.weights[k];
    }
    // Correct quantization residuals so a constant input stays constant. Adjust
    // the tap with the largest rounding error in the required direction first.
    while (sum != (1 << filter.shift)) {
        int direction = sum < (1 << filter.shift) ? 1 : -1, best = 0;
        for (int k = 1; k < filter.tap_count; k++)
            if (direction * (weights[k] * (1 << filter.shift) - filter.weights[k]) >
                direction * (weights[best] * (1 << filter.shift) - filter.weights[best]))
                best = k;
        filter.weights[best] += direction;
        sum += direction;
    }
    return filter;
}
static inline uint8_t round_clip_u8(int x, int shift) {
    return (uint8_t)clamp_int((x + (1 << (shift - 1))) >> shift, 0, 255);
}
#endif
