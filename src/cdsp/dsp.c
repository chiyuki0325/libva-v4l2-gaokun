// SPDX-License-Identifier: MIT
#include "filter.h"
#include "iris_vpp.h"
#include "qurt_abi.h"
#include "vpp_limits.h"
#include <hexagon_protos.h>
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Horizontal gathers address at most two 128-byte input vectors. indices are
// byte offsets relative to src_base; dst_offset/output_bytes describe output.
typedef struct AreaTile {
    uint8_t indices[FILTER_MAX_TAPS][128];
    int8_t weights[FILTER_MAX_TAPS][128]; // Q7 bits, read as unsigned for area reduction.
    uint8_t delta_controls[FILTER_MAX_TAPS][2][128];
    int use_delta[FILTER_MAX_TAPS];
    int src_base, dst_offset, output_bytes, tap_count, spans_two_vectors;
    struct AreaTile *shared_pattern;
} __attribute__((aligned(128))) AreaTile;
// Cubic intermediates are int16_t: src_base counts components, but the gather
// indices still address bytes. One tile produces up to 64 output components.
typedef struct CubicTile {
    uint8_t indices[4][128], delta_controls[4][2][128];
    int16_t weights[4][64];
    int use_delta[4], src_base, dst_offset, output_bytes, tap_count, spans_two_vectors;
    struct CubicTile *shared_pattern;
} __attribute__((aligned(128))) CubicTile;
typedef struct ScalerState ScalerState;
typedef struct {
    ScalerState *state;
    int worker_id, status;
    qurt_thread_t thread_id;
    qurt_sem_t work, done;
    void *stack;
} Worker;
// One separable pass per axis: vertical filtering builds a worker-private row,
// then horizontal filtering emits it. Plane 0 is Y; plane 1 is interleaved UV.
struct ScalerState {
    int src_width, src_height, dst_width, dst_height;
    Filter *horizontal_filters[2], *vertical_filters[2];
    uint8_t *area_rows;
    AreaTile *area_tiles[2];
    int area_tile_count[2];
    CubicTile *cubic_tiles[2];
    int cubic_tile_count[2];
    int16_t *cubic_rows;
    Worker workers[CDSP_VPP_MAX_WORKERS]; // Spare workers stay asleep.
    int created_workers, stop;
    const uint8_t *src;
    uint8_t *dst;
    int src_stride, dst_stride, src_y_offset, src_uv_offset, dst_y_offset, dst_uv_offset,
        worker_count;
    int profiling;
    uint64_t profile_us[CDSP_VPP_PROFILE_COUNTERS]; // Per worker: Y/UV vertical/horizontal.
};
static void worker_main(void *opaque);
// Per-worker byte strides: vector-aligned rows plus padding for full-vector
// gathers. Cubic rows need a leading border vector as well as trailing padding.
static inline int area_row_stride_bytes(int width) {
    return (width + 383) & ~127;
}
static inline int cubic_row_stride_bytes(int width) {
    return (2 * width + 511) & ~127;
}
// Route each requested byte backwards through the butterfly network. Conflicts
// fall back to LUT; duplicates are valid when the shared path agrees.
static int build_area_delta_route(AreaTile *tile, int tap, int reverse) {
    uint8_t used[2][128] = {{0}};
    int conflict_free = 1;
    for (int i = 0; i < tile->output_bytes; i++)
        if (tile->weights[tap][i]) {
            int source_byte = tile->indices[tap][i], table = source_byte / 128, node = i;
            for (int j = 0; j < 7; j++) {
                int bit = reverse ? (64 >> j) : (1 << j), switch_bit = (node ^ source_byte) & bit;
                if ((used[table][node] & bit) &&
                    ((tile->delta_controls[tap][table][node] & bit) != switch_bit))
                    conflict_free = 0;
                used[table][node] |= bit;
                tile->delta_controls[tap][table][node] |= switch_bit;
                node ^= switch_bit;
            }
        }
    return conflict_free;
}
int iris_vpp_open(const char *uri, remote_handle64 *h) {
    (void)uri;
    ScalerState *state = calloc(1, sizeof(*state));
    if (!state)
        return AEE_ENOMEMORY;
    for (int i = 0; i < CDSP_VPP_MAX_WORKERS; i++) {
        Worker *worker = &state->workers[i];
        worker->state = state;
        worker->worker_id = i;
        worker->stack = memalign(128, 8192);
        if (!worker->stack) {
            iris_vpp_close((uintptr_t)state);
            return AEE_ENOMEMORY;
        }
        qurt_sem_init_val(&worker->work, 0);
        qurt_sem_init_val(&worker->done, 0);
        qurt_thread_attr_t attributes;
        qurt_thread_attr_init(&attributes);
        qurt_thread_attr_set_stack_addr(&attributes, worker->stack);
        qurt_thread_attr_set_stack_size(&attributes, 8192);
        qurt_thread_attr_set_priority(&attributes, QURT_THREAD_ATTR_PRIORITY_DEFAULT / 2);
        qurt_thread_attr_set_name(&attributes, "vpp-worker");
        if (qurt_thread_create(&worker->thread_id, &attributes, worker_main, worker) != QURT_EOK) {
            qurt_sem_destroy(&worker->work);
            qurt_sem_destroy(&worker->done);
            free(worker->stack);
            iris_vpp_close((uintptr_t)state);
            return AEE_EQURTTHREADCREATE;
        }
        state->created_workers++;
    }
    *h = (uintptr_t)state;
    return 0;
}
static void clear_filters(ScalerState *state) {
    for (int plane_index = 0; plane_index < 2; plane_index++) {
        free(state->horizontal_filters[plane_index]);
        free(state->vertical_filters[plane_index]);
        free(state->area_tiles[plane_index]);
        free(state->cubic_tiles[plane_index]);
        state->cubic_tiles[plane_index] = 0;
        state->area_tiles[plane_index] = 0;
        state->horizontal_filters[plane_index] = state->vertical_filters[plane_index] = 0;
    }
    free(state->area_rows);
    free(state->cubic_rows);
    state->cubic_rows = 0;
    state->area_rows = 0;
    state->src_width = 0;
}
int iris_vpp_close(remote_handle64 h) {
    ScalerState *state = (ScalerState *)(uintptr_t)h;
    state->stop = 1;
    for (int i = 0; i < state->created_workers; i++)
        qurt_sem_up(&state->workers[i].work);
    for (int i = 0; i < state->created_workers; i++) {
        Worker *worker = &state->workers[i];
        int status;
        qurt_thread_join(worker->thread_id, &status);
        qurt_sem_destroy(&worker->work);
        qurt_sem_destroy(&worker->done);
        free(worker->stack);
    }
    clear_filters(state);
    HAP_power_destroy_client(state);
    free(state);
    return 0;
}
int iris_vpp_power(remote_handle64 h, int *mask) {
    ScalerState *state = (ScalerState *)(uintptr_t)h;
    HAP_power_request_t r;
    int e;
    *mask = 0;
    memset(&r, 0, sizeof(r));
    r.type = HAP_power_set_apptype;
    r.apptype = HAP_POWER_COMPUTE_CLIENT_CLASS;
    if ((e = HAP_power_set(state, &r)))
        return e;
    *mask |= 1;
    memset(&r, 0, sizeof(r));
    r.type = HAP_power_set_DCVS_v3;
    r.dcvs_v3.set_dcvs_enable = TRUE;
    r.dcvs_v3.dcvs_enable = TRUE;
    r.dcvs_v3.dcvs_option = HAP_DCVS_V2_PERFORMANCE_MODE;
    r.dcvs_v3.set_core_params = TRUE;
    r.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.core_params.max_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.set_bus_params = TRUE;
    r.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.bus_params.max_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_MAX;
    r.dcvs_v3.set_sleep_disable = TRUE;
    r.dcvs_v3.sleep_disable = HAP_DCVS_LPM_LEVEL1;
    if ((e = HAP_power_set(state, &r)))
        return e;
    *mask |= 2;
    memset(&r, 0, sizeof(r));
    r.type = HAP_power_set_HVX;
    r.hvx.power_up = TRUE;
    if ((e = HAP_power_set(state, &r)))
        return e;
    *mask |= 4;
    return 0;
}
int iris_vpp_info(remote_handle64 h, unsigned int *hz, int *hvx_units) {
    (void)h;
    HAP_power_response_t r;
    memset(&r, 0, sizeof(r));
    r.type = HAP_power_get_clk_Freq;
    int e = HAP_power_get(0, &r);
    if (e)
        return e;
    *hz = r.clkFreqHz;
    *hvx_units = (qurt_hvx_get_units() >> 8) & 255;
    return 0;
}
int iris_vpp_profile(remote_handle64 h, int enable, uint64_t *counters, int counter_count) {
    ScalerState *state = (ScalerState *)(uintptr_t)h;
    if (!counters || counter_count != CDSP_VPP_PROFILE_COUNTERS || (enable != 0 && enable != 1))
        return AEE_EBADPARM;
    memcpy(counters, state->profile_us, sizeof(state->profile_us));
    state->profiling = enable;
    return 0;
}
int iris_vpp_configure(remote_handle64 h, int src_width, int src_height, int dst_width,
                       int dst_height) {
    ScalerState *state = (ScalerState *)(uintptr_t)h;
    if (src_width < 16 || dst_width < 16 || src_height < 2 || dst_height < 2 ||
        (src_width | dst_width) & 15 || (src_height | dst_height) & 1 || src_width > 8192 ||
        src_height > 8192 || dst_width > 8192 || dst_height > 8192 || src_width > dst_width * 20 ||
        src_height > dst_height * 20 ||
        !((src_width >= dst_width && src_height >= dst_height) ||
          (src_width <= dst_width && src_height <= dst_height)))
        return AEE_EBADPARM;
    clear_filters(state);
    state->src_width = src_width;
    state->src_height = src_height;
    state->dst_width = dst_width;
    state->dst_height = dst_height;
    state->area_rows = memalign(128, CDSP_VPP_MAX_WORKERS * area_row_stride_bytes(src_width));
    if (src_width < dst_width || src_height < dst_height) {
        state->cubic_rows = memalign(128, CDSP_VPP_MAX_WORKERS * cubic_row_stride_bytes(src_width));
        if (!state->cubic_rows) {
            clear_filters(state);
            return AEE_ENOMEMORY;
        }
        memset(state->cubic_rows, 0, CDSP_VPP_MAX_WORKERS * cubic_row_stride_bytes(src_width));
    }
    for (int plane_index = 0; plane_index < 2; plane_index++) {
        // NV12 UV has half as many samples in each axis, with two bytes per
        // sample. The channel offset below keeps U and V from being mixed.
        int channels = plane_index ? 2 : 1;
        state->horizontal_filters[plane_index] = malloc(dst_width / channels * sizeof(Filter));
        state->vertical_filters[plane_index] = malloc(dst_height / channels * sizeof(Filter));
        if (!state->horizontal_filters[plane_index] || !state->vertical_filters[plane_index] ||
            !state->area_rows) {
            clear_filters(state);
            return AEE_ENOMEMORY;
        }
        for (int x = 0; x < dst_width / channels; x++)
            state->horizontal_filters[plane_index][x] =
                make_filter(src_width / channels, dst_width / channels, x);
        for (int y = 0; y < dst_height / channels; y++)
            state->vertical_filters[plane_index][y] =
                make_filter(src_height / channels, dst_height / channels, y);
        if (state->cubic_rows) {
            state->cubic_tile_count[plane_index] = (dst_width + 63) / 64;
            state->cubic_tiles[plane_index] =
                memalign(128, state->cubic_tile_count[plane_index] * sizeof(CubicTile));
            if (!state->cubic_tiles[plane_index]) {
                clear_filters(state);
                return AEE_ENOMEMORY;
            }
            memset(state->cubic_tiles[plane_index], 0,
                   state->cubic_tile_count[plane_index] * sizeof(CubicTile));
            for (int tile_index = 0; tile_index < state->cubic_tile_count[plane_index];
                 tile_index++) {
                CubicTile *tile = &state->cubic_tiles[plane_index][tile_index];
                tile->dst_offset = tile_index * 64;
                tile->output_bytes = dst_width - tile->dst_offset;
                if (tile->output_bytes > 64)
                    tile->output_bytes = 64;
                tile->src_base =
                    state->horizontal_filters[plane_index][tile->dst_offset / channels].indices[0] *
                    channels;
                for (int i = 0; i < tile->output_bytes; i++) {
                    Filter *filter =
                        &state->horizontal_filters[plane_index][(tile->dst_offset + i) / channels];
                    if (filter->tap_count > tile->tap_count)
                        tile->tap_count = filter->tap_count;
                    for (int k = 0; k < filter->tap_count; k++) {
                        int source_byte =
                            (filter->indices[k] * channels + i % channels - tile->src_base) * 2;
                        if (source_byte > 254) {
                            clear_filters(state);
                            return AEE_EBADPARM;
                        }
                        tile->indices[k][2 * i] = source_byte;
                        tile->indices[k][2 * i + 1] = source_byte + 1;
                        tile->weights[k][i] = filter->weights[k];
                        if (source_byte >= 128)
                            tile->spans_two_vectors = 1;
                    }
                }
                for (int k = 0; k < tile->tap_count; k++) {
                    uint8_t used[2][128] = {{0}};
                    int conflict_free = 1;
                    for (int i = 0; i < tile->output_bytes * 2; i++)
                        if (tile->weights[k][i / 2]) {
                            int source_byte = tile->indices[k][i], table = source_byte / 128,
                                node = i;
                            for (int bit = 1; bit <= 64; bit *= 2) {
                                int switch_bit = (node ^ source_byte) & bit;
                                if ((used[table][node] & bit) &&
                                    ((tile->delta_controls[k][table][node] & bit) != switch_bit))
                                    conflict_free = 0;
                                used[table][node] |= bit;
                                tile->delta_controls[k][table][node] |= switch_bit;
                                node ^= switch_bit;
                            }
                        }
                    tile->use_delta[k] = conflict_free;
                }
                tile->shared_pattern = tile;
                for (int j = 0; j < tile_index; j++) {
                    CubicTile *pattern = &state->cubic_tiles[plane_index][j];
                    if (pattern->shared_pattern == pattern &&
                        pattern->tap_count == tile->tap_count &&
                        pattern->output_bytes == tile->output_bytes &&
                        pattern->spans_two_vectors == tile->spans_two_vectors &&
                        !memcmp(pattern->indices, tile->indices, tile->tap_count * 128) &&
                        !memcmp(pattern->weights, tile->weights, tile->tap_count * 128)) {
                        tile->shared_pattern = pattern;
                        break;
                    }
                }
            }
            continue;
        }
        // Keep a tile's source footprint within 256 bytes. Noninteger ratios
        // need margin for fractional endpoints; even tile sizes preserve UV pairs.
        int tile_bytes = src_width > dst_width
                             ? ((src_width % dst_width ? 252 : 256) * dst_width / src_width) & ~1
                             : 128;
        if (tile_bytes < 2)
            tile_bytes = 2;
        if (tile_bytes > 128)
            tile_bytes = 128;
        state->area_tile_count[plane_index] = (dst_width + tile_bytes - 1) / tile_bytes;
        state->area_tiles[plane_index] =
            memalign(128, state->area_tile_count[plane_index] * sizeof(AreaTile));
        if (!state->area_tiles[plane_index]) {
            clear_filters(state);
            return AEE_ENOMEMORY;
        }
        memset(state->area_tiles[plane_index], 0,
               state->area_tile_count[plane_index] * sizeof(AreaTile));
        for (int tile_index = 0; tile_index < state->area_tile_count[plane_index]; tile_index++) {
            AreaTile *tile = &state->area_tiles[plane_index][tile_index];
            tile->dst_offset = tile_index * tile_bytes;
            tile->output_bytes = dst_width - tile->dst_offset;
            if (tile->output_bytes > tile_bytes)
                tile->output_bytes = tile_bytes;
            tile->src_base =
                state->horizontal_filters[plane_index][tile->dst_offset / channels].indices[0] *
                channels;
            int max_source_byte = 0;
            for (int i = 0; i < tile->output_bytes; i++) {
                Filter *filter =
                    &state->horizontal_filters[plane_index][(tile->dst_offset + i) / channels];
                if (filter->tap_count > tile->tap_count)
                    tile->tap_count = filter->tap_count;
                for (int k = 0; k < filter->tap_count; k++) {
                    int index = filter->indices[k] * channels + i % channels - tile->src_base;
                    if (index > 255) {
                        clear_filters(state);
                        return AEE_EBADPARM;
                    }
                    tile->indices[k][i] = index;
                    tile->weights[k][i] = filter->weights[k];
                    if (index > max_source_byte)
                        max_source_byte = index;
                }
            }
            tile->spans_two_vectors = max_source_byte >= 128;
            for (int k = 0; k < tile->tap_count; k++)
                tile->use_delta[k] = build_area_delta_route(tile, k, src_width > dst_width);
            // Rational ratios repeat the same lane pattern. Share its hot
            // coefficient/routing cache lines across spatial tiles.
            tile->shared_pattern = tile;
            for (int j = 0; j < tile_index; j++) {
                AreaTile *pattern = &state->area_tiles[plane_index][j];
                if (pattern->shared_pattern != pattern || pattern->tap_count != tile->tap_count ||
                    pattern->spans_two_vectors != tile->spans_two_vectors ||
                    pattern->output_bytes != tile->output_bytes)
                    continue;
                if (!memcmp(pattern->indices, tile->indices, tile->tap_count * 128) &&
                    !memcmp(pattern->weights, tile->weights, tile->tap_count * 128)) {
                    tile->shared_pattern = pattern;
                    break;
                }
            }
        }
    }
    return 0;
}
typedef HVX_Vector UnalignedVector __attribute__((aligned(1)));
// vlut32 selects 32-byte banks. Its inputs are byte-shuffled by the caller;
// delta routing is the faster path when all requested lanes are conflict-free.
static inline HVX_Vector gather_bytes(HVX_Vector indices, HVX_Vector lower, HVX_Vector upper,
                                      int spans_two_vectors) {
    HVX_Vector gathered = Q6_Vb_vlut32_VbVbI(indices, lower, 0);
    gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, lower, 1);
    gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, lower, 2);
    gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, lower, 3);
    if (spans_two_vectors) {
        gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, upper, 4);
        gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, upper, 5);
        gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, upper, 6);
        gathered = Q6_Vb_vlut32or_VbVbVbI(gathered, indices, upper, 7);
    }
    return gathered;
}
static void area_horizontal(ScalerState *state, int plane_index, uint8_t *dst, uint8_t *row) {
    for (int tile_index = 0; tile_index < state->area_tile_count[plane_index]; tile_index++) {
        AreaTile *tile = &state->area_tiles[plane_index][tile_index];
        AreaTile *pattern = tile->shared_pattern;
        HVX_Vector lower = *(const UnalignedVector *)(row + tile->src_base);
        HVX_Vector upper = *(const UnalignedVector *)(row + tile->src_base + 128);
        HVX_VectorPair accumulator = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
        for (int k = 0; k < tile->tap_count; k++) {
            HVX_Vector indices = *(HVX_Vector *)pattern->indices[k], samples;
            if (pattern->use_delta[k]) {
                HVX_Vector lower_control = *(HVX_Vector *)pattern->delta_controls[k][0],
                           upper_control = *(HVX_Vector *)pattern->delta_controls[k][1];
                HVX_Vector lower_samples = state->src_width > state->dst_width
                                               ? Q6_V_vrdelta_VV(lower, lower_control)
                                               : Q6_V_vdelta_VV(lower, lower_control);
                if (tile->spans_two_vectors) {
                    HVX_Vector upper_samples = state->src_width > state->dst_width
                                                   ? Q6_V_vrdelta_VV(upper, upper_control)
                                                   : Q6_V_vdelta_VV(upper, upper_control);
                    samples = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VubVub(indices, Q6_V_vsplat_R(0x7f7f7f7f)),
                                            upper_samples, lower_samples);
                } else
                    samples = lower_samples;
            } else
                samples = gather_bytes(indices, Q6_Vb_vshuff_Vb(lower), Q6_Vb_vshuff_Vb(upper),
                                       tile->spans_two_vectors);
            accumulator = state->src_width >= state->dst_width
                              ? Q6_Wuh_vmpyacc_WuhVubVub(accumulator, samples,
                                                         *(HVX_Vector *)pattern->weights[k])
                              : Q6_Wh_vmpyacc_WhVubVb(accumulator, samples,
                                                      *(HVX_Vector *)pattern->weights[k]);
        }
        HVX_Vector out = Q6_Vub_vasr_VhVhR_rnd_sat(Q6_V_hi_W(accumulator), Q6_V_lo_W(accumulator),
                                                   state->src_width >= state->dst_width ? 7 : 6);
        if (tile->output_bytes == 128)
            *(UnalignedVector *)(dst + tile->dst_offset) = out;
        else {
            uint8_t tmp[128] __attribute__((aligned(128)));
            *(HVX_Vector *)tmp = out;
            memcpy(dst + tile->dst_offset, tmp, tile->output_bytes);
        }
    }
}
static void area_vertical(const uint8_t *src, int stride, uint8_t *row, int width, Filter *filter,
                          int hvx) {
    int x = 0;
    if (hvx)
        for (; x + 128 <= width; x += 128) {
            HVX_VectorPair sum = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
            for (int k = 0; k < filter->tap_count; k++) {
                HVX_Vector samples =
                    *(const UnalignedVector *)(src + filter->indices[k] * stride + x);
                int packed_weight = (uint8_t)filter->weights[k] * 0x01010101;
                sum = filter->shift == 7 ? Q6_Wuh_vmpyacc_WuhVubRub(sum, samples, packed_weight)
                                         : Q6_Wh_vmpyacc_WhVubRb(sum, samples, packed_weight);
            }
            *(HVX_Vector *)(row + x) =
                Q6_Vub_vasr_VhVhR_rnd_sat(Q6_V_hi_W(sum), Q6_V_lo_W(sum), filter->shift);
        }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < filter->tap_count; k++)
            sum += src[filter->indices[k] * stride + x] * filter->weights[k];
        row[x] = round_clip_u8(sum, filter->shift);
    }
}
// Preserve signed Q6 intermediates, including cubic undershoot/overshoot.
// Q14 coefficients and 32-bit accumulators avoid early clipping and phase loss.
static void cubic_vertical(const uint8_t *src, int stride, int16_t *row, int width, Filter *filter,
                           int hvx) {
    int x = 0, shift = filter->shift - 6;
    if (hvx)
        for (; x + 128 <= width; x += 128) {
            HVX_VectorPair even = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero()), odd = even;
            for (int k = 0; k < filter->tap_count; k++) {
                HVX_Vector v = *(const UnalignedVector *)(src + filter->indices[k] * stride + x);
                HVX_VectorPair widened = Q6_Wuh_vzxt_Vub(v);
                int packed_weight = (uint32_t)(uint16_t)filter->weights[k] * 0x00010001u;
                even = Q6_Ww_vmpyacc_WwVhRh(even, Q6_V_lo_W(widened), packed_weight);
                odd = Q6_Ww_vmpyacc_WwVhRh(odd, Q6_V_hi_W(widened), packed_weight);
            }
            HVX_Vector even_samples =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(even), Q6_V_lo_W(even), shift);
            HVX_Vector odd_samples =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(odd), Q6_V_lo_W(odd), shift);
            HVX_VectorPair v = Q6_W_vshuff_VVR(odd_samples, even_samples, -2);
            *(HVX_Vector *)(row + x) = Q6_V_lo_W(v);
            *(HVX_Vector *)(row + x + 64) = Q6_V_hi_W(v);
        }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < filter->tap_count; k++)
            sum += src[filter->indices[k] * stride + x] * filter->weights[k];
        row[x] = (sum + (1 << (shift - 1))) >> shift;
    }
}
// Exact 2x Catmull-Rom has two fixed Q7 phases. The generic Q14
// coefficients are exactly 128 times these integers, so this is bit exact
// even with signed Q6 vertical intermediates.
static void cubic_vertical_2x(const uint8_t *src, int stride, int16_t *row, int width,
                              const Filter *filter) {
    int x = 0;
    HVX_Vector bias = Q6_V_vsplat_R(0xc000c000u);   // -128 * 128
    HVX_Vector restore = Q6_V_vsplat_R(0x20002000); // +128 in Q6
    for (; x + 128 <= width; x += 128) {
        // Center the sum before multiplying. With positive weights summing to
        // 140 and negative weights to -12, every partial sum fits signed 16 bits.
        HVX_VectorPair accumulator = Q6_W_vcombine_VV(bias, bias);
        for (int k = 0; k < 4; k++) {
            HVX_Vector v = *(const UnalignedVector *)(src + filter->indices[k] * stride + x);
            uint32_t packed_weight = (uint8_t)(filter->weights[k] / 128) * 0x01010101u;
            accumulator = Q6_Wh_vmpyacc_WhVubRb(accumulator, v, packed_weight);
        }
        HVX_Vector even =
            Q6_Vh_vadd_VhVh(Q6_Vh_vavg_VhVh_rnd(Q6_V_lo_W(accumulator), Q6_V_vzero()), restore);
        HVX_Vector odd =
            Q6_Vh_vadd_VhVh(Q6_Vh_vavg_VhVh_rnd(Q6_V_hi_W(accumulator), Q6_V_vzero()), restore);
        HVX_VectorPair v = Q6_W_vshuff_VVR(odd, even, -2);
        *(HVX_Vector *)(row + x) = Q6_V_lo_W(v);
        *(HVX_Vector *)(row + x + 64) = Q6_V_hi_W(v);
    }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < 4; k++)
            sum += src[filter->indices[k] * stride + x] * filter->weights[k];
        row[x] = (sum + 128) >> 8;
    }
}
static void cubic_horizontal_2x(ScalerState *state, int plane_index, uint8_t *dst, int16_t *row) {
    int channels = plane_index ? 2 : 1;
    // Extend both borders independently per channel, then evaluate the two Q7
    // phases [-3, 29, 111, -9] and [-9, 111, 29, -3]. Q6 * Q7 needs a 13-bit shift.
    for (int j = 0; j < 2 * channels; j++) {
        row[j - 2 * channels] = row[j % channels];
        row[state->src_width + j] = row[state->src_width - channels + j % channels];
    }
    for (int x = 0; x < state->src_width; x += 64) {
        HVX_Vector left2 = *(const UnalignedVector *)(row + x - 2 * channels);
        HVX_Vector left1 = *(const UnalignedVector *)(row + x - channels);
        HVX_Vector center = *(const UnalignedVector *)(row + x);
        HVX_Vector right1 = *(const UnalignedVector *)(row + x + channels);
        HVX_Vector right2 = *(const UnalignedVector *)(row + x + 2 * channels);
        HVX_VectorPair even = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
        HVX_VectorPair odd = even;
        even = Q6_Ww_vmpyacc_WwVhRh(even, left2, (int)0xfffdfffdu);  // -3
        even = Q6_Ww_vmpyacc_WwVhRh(even, left1, 0x001d001d);        // 29
        even = Q6_Ww_vmpyacc_WwVhRh(even, center, 0x006f006f);       // 111
        even = Q6_Ww_vmpyacc_WwVhRh(even, right1, (int)0xfff7fff7u); // -9
        odd = Q6_Ww_vmpyacc_WwVhRh(odd, left1, (int)0xfff7fff7u);
        odd = Q6_Ww_vmpyacc_WwVhRh(odd, center, 0x006f006f);
        odd = Q6_Ww_vmpyacc_WwVhRh(odd, right1, 0x001d001d);
        odd = Q6_Ww_vmpyacc_WwVhRh(odd, right2, (int)0xfffdfffdu);
        HVX_Vector even_samples = Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(even), Q6_V_lo_W(even), 13);
        HVX_Vector odd_samples = Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(odd), Q6_V_lo_W(odd), 13);
        HVX_VectorPair interleaved =
            Q6_W_vshuff_VVR(odd_samples, even_samples, plane_index ? -4 : -2);
        HVX_Vector out = Q6_Vub_vpack_VhVh_sat(Q6_V_hi_W(interleaved), Q6_V_lo_W(interleaved));
        int output_bytes = 2 * (state->src_width - x);
        if (output_bytes >= 128)
            *(UnalignedVector *)(dst + 2 * x) = out;
        else {
            uint8_t tail[128] __attribute__((aligned(128)));
            *(HVX_Vector *)tail = out;
            memcpy(dst + 2 * x, tail, output_bytes);
        }
    }
}
static void cubic_horizontal(ScalerState *state, int plane_index, uint8_t *dst, int16_t *row) {
    if (state->dst_width == 2 * state->src_width) {
        cubic_horizontal_2x(state, plane_index, dst, row);
        return;
    }
    int shift = state->horizontal_filters[plane_index][0].shift;
    HVX_Vector previous = Q6_V_vzero();
    for (int tile_index = 0; tile_index < state->cubic_tile_count[plane_index]; tile_index++) {
        CubicTile *tile = &state->cubic_tiles[plane_index][tile_index],
                  *pattern = tile->shared_pattern;
        HVX_Vector lower = *(const UnalignedVector *)(row + tile->src_base),
                   upper =
                       *(const UnalignedVector *)((const uint8_t *)(row + tile->src_base) + 128);
        HVX_VectorPair accumulator = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
        for (int k = 0; k < tile->tap_count; k++) {
            HVX_Vector indices = *(HVX_Vector *)pattern->indices[k], samples;
            if (pattern->use_delta[k]) {
                HVX_Vector lower_samples =
                    Q6_V_vdelta_VV(lower, *(HVX_Vector *)pattern->delta_controls[k][0]);
                if (tile->spans_two_vectors) {
                    HVX_Vector upper_samples =
                        Q6_V_vdelta_VV(upper, *(HVX_Vector *)pattern->delta_controls[k][1]);
                    samples = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VubVub(indices, Q6_V_vsplat_R(0x7f7f7f7f)),
                                            upper_samples, lower_samples);
                } else
                    samples = lower_samples;
            } else
                samples = gather_bytes(indices, Q6_Vb_vshuff_Vb(lower), Q6_Vb_vshuff_Vb(upper),
                                       tile->spans_two_vectors);
            accumulator =
                Q6_Ww_vmpyacc_WwVhVh(accumulator, samples, *(HVX_Vector *)pattern->weights[k]);
        }
        // The narrowing instruction masks its shift count to four bits. Drop the
        // six fractional intermediate bits first; the final rounding is unchanged.
        HVX_Vector narrowed =
            Q6_Vh_vasr_VwVwR_rnd_sat(Q6_Vw_vasr_VwR(Q6_V_hi_W(accumulator), 6),
                                     Q6_Vw_vasr_VwR(Q6_V_lo_W(accumulator), 6), shift);
        if (!(tile_index & 1) && tile_index + 1 < state->cubic_tile_count[plane_index]) {
            previous = narrowed;
            continue;
        }
        HVX_Vector out = (tile_index & 1) ? Q6_Vub_vpack_VhVh_sat(narrowed, previous)
                                          : Q6_Vub_vpack_VhVh_sat(Q6_V_vzero(), narrowed);
        int output_bytes = tile->output_bytes + ((tile_index & 1) ? 64 : 0);
        uint8_t *target = dst + tile->dst_offset - ((tile_index & 1) ? 64 : 0);
        if (output_bytes == 128)
            *(UnalignedVector *)target = out;
        else if (!((uintptr_t)target & 127))
            Q6_vmem_QRIV(Q6_Q_vsetq_R(output_bytes), (HVX_Vector *)target, out);
        else {
            uint8_t tmp[128] __attribute__((aligned(128)));
            *(HVX_Vector *)tmp = out;
            memcpy(target, tmp, output_bytes);
        }
    }
}
static void scale_cubic_plane(ScalerState *state, const uint8_t *src, uint8_t *dst, int src_stride,
                              int dst_stride, int plane_index, int hvx, int worker_id,
                              int worker_count) {
    int channels = plane_index ? 2 : 1;
    int16_t *row = (int16_t *)((uint8_t *)state->cubic_rows +
                               worker_id * cubic_row_stride_bytes(state->src_width) + 128);
    // Integer boundaries partition every output row exactly once, even when
    // height is not divisible by worker_count or some workers receive no rows.
    for (int y = (state->dst_height / channels) * worker_id / worker_count;
         y < (state->dst_height / channels) * (worker_id + 1) / worker_count; y++) {
        if (hvx && src_stride <= 65535) {
            int ahead = y + 2;
            if (ahead >= state->dst_height / channels)
                ahead = y;
            Filter *filter = &state->vertical_filters[plane_index][ahead];
            Q6_l2fetch_AP((void *)(src + filter->indices[0] * src_stride),
                          ((uint64_t)src_stride << 32) | ((uint64_t)state->src_width << 16) |
                              (filter->indices[filter->tap_count - 1] - filter->indices[0] + 1));
        }
        uint64_t start = state->profiling ? HAP_perf_get_time_us() : 0;
        if (hvx && state->dst_height == 2 * state->src_height)
            cubic_vertical_2x(src, src_stride, row, state->src_width,
                              &state->vertical_filters[plane_index][y]);
        else
            cubic_vertical(src, src_stride, row, state->src_width,
                           &state->vertical_filters[plane_index][y], hvx);
        uint64_t middle = state->profiling ? HAP_perf_get_time_us() : 0;
        if (state->profiling)
            state->profile_us[worker_id * 4 + plane_index * 2] += middle - start;
        if (hvx) {
            cubic_horizontal(state, plane_index, dst + y * dst_stride, row);
            if (state->profiling)
                state->profile_us[worker_id * 4 + plane_index * 2 + 1] +=
                    HAP_perf_get_time_us() - middle;
            continue;
        }
        for (int x = 0; x < state->dst_width / channels; x++)
            for (int ch = 0; ch < channels; ch++) {
                Filter *filter = &state->horizontal_filters[plane_index][x];
                int sum = 0;
                for (int k = 0; k < filter->tap_count; k++)
                    sum += row[filter->indices[k] * channels + ch] * filter->weights[k];
                dst[y * dst_stride + x * channels + ch] = round_clip_u8(sum, filter->shift + 6);
            }
    }
}
static void scale_plane(ScalerState *state, const uint8_t *src, uint8_t *dst, int src_stride,
                        int dst_stride, int plane_index, int hvx, int worker_id, int worker_count) {
    if (state->cubic_rows) {
        scale_cubic_plane(state, src, dst, src_stride, dst_stride, plane_index, hvx, worker_id,
                          worker_count);
        return;
    }
    int channels = plane_index ? 2 : 1;
    uint8_t *row = state->area_rows + worker_id * area_row_stride_bytes(state->src_width);
    for (int y = (state->dst_height / channels) * worker_id / worker_count;
         y < (state->dst_height / channels) * (worker_id + 1) / worker_count; y++) {
        if (hvx && src_stride <= 65535) {
            int ahead = y + 2;
            if (ahead >= state->dst_height / channels)
                ahead = y;
            Filter *filter = &state->vertical_filters[plane_index][ahead];
            uint64_t config = ((uint64_t)src_stride << 32) | ((uint64_t)state->src_width << 16) |
                              (filter->indices[filter->tap_count - 1] - filter->indices[0] + 1);
            Q6_l2fetch_AP((void *)(src + filter->indices[0] * src_stride), config);
        }
        if (hvx && state->src_width == state->dst_width && state->src_height == state->dst_height) {
            int x = 0;
            for (; x + 128 <= state->dst_width; x += 128)
                *(UnalignedVector *)(dst + y * dst_stride + x) =
                    *(const UnalignedVector *)(src + y * src_stride + x);
            if (x < state->dst_width)
                memcpy(dst + y * dst_stride + x, src + y * src_stride + x, state->dst_width - x);
            continue;
        }
        if (hvx && state->src_width == 2 * state->dst_width &&
            state->src_height == 2 * state->dst_height) {
            // Match separable area filtering: round the vertical averages first,
            // then the horizontal averages. UV packing preserves channel pairs.
            const uint8_t *r0 = src + 2 * y * src_stride, *r1 = r0 + src_stride;
            int x = 0;
            for (; x + 128 <= state->dst_width; x += 128) {
                HVX_Vector a = Q6_Vub_vavg_VubVub_rnd(*(const UnalignedVector *)(r0 + 2 * x),
                                                      *(const UnalignedVector *)(r1 + 2 * x));
                HVX_Vector b = Q6_Vub_vavg_VubVub_rnd(*(const UnalignedVector *)(r0 + 2 * x + 128),
                                                      *(const UnalignedVector *)(r1 + 2 * x + 128));
                HVX_Vector even = plane_index ? Q6_Vh_vpacke_VwVw(b, a) : Q6_Vb_vpacke_VhVh(b, a);
                HVX_Vector odd = plane_index ? Q6_Vh_vpacko_VwVw(b, a) : Q6_Vb_vpacko_VhVh(b, a);
                *(UnalignedVector *)(dst + y * dst_stride + x) = Q6_Vub_vavg_VubVub_rnd(even, odd);
            }
            if (x < state->dst_width) {
                // Copy only a partial vector's live input, never read past an
                // imported plane. The fast path above has no intermediate row.
                uint8_t t0[256] __attribute__((aligned(128))) = {0};
                uint8_t t1[256] __attribute__((aligned(128))) = {0};
                int output_bytes = state->dst_width - x;
                memcpy(t0, r0 + 2 * x, 2 * output_bytes);
                memcpy(t1, r1 + 2 * x, 2 * output_bytes);
                HVX_Vector a = Q6_Vub_vavg_VubVub_rnd(*(HVX_Vector *)t0, *(HVX_Vector *)t1);
                HVX_Vector b =
                    Q6_Vub_vavg_VubVub_rnd(*(HVX_Vector *)(t0 + 128), *(HVX_Vector *)(t1 + 128));
                HVX_Vector even = plane_index ? Q6_Vh_vpacke_VwVw(b, a) : Q6_Vb_vpacke_VhVh(b, a);
                HVX_Vector odd = plane_index ? Q6_Vh_vpacko_VwVw(b, a) : Q6_Vb_vpacko_VhVh(b, a);
                HVX_Vector out = Q6_Vub_vavg_VubVub_rnd(even, odd);
                uint8_t *target = dst + y * dst_stride + x;
                if (!((uintptr_t)target & 127))
                    Q6_vmem_QRIV(Q6_Q_vsetq_R(output_bytes), (HVX_Vector *)target, out);
                else {
                    *(HVX_Vector *)t0 = out;
                    memcpy(target, t0, output_bytes);
                }
            }
            continue;
        }
        area_vertical(src, src_stride, row, state->src_width,
                      &state->vertical_filters[plane_index][y], hvx);
        if (hvx) {
            area_horizontal(state, plane_index, dst + y * dst_stride, row);
            continue;
        }
        for (int x = 0; x < state->dst_width / channels; x++)
            for (int ch = 0; ch < channels; ch++) {
                Filter *filter = &state->horizontal_filters[plane_index][x];
                int a = 0;
                for (int k = 0; k < filter->tap_count; k++)
                    a += row[filter->indices[k] * channels + ch] * filter->weights[k];
                dst[y * dst_stride + x * channels + ch] = round_clip_u8(a, filter->shift);
            }
    }
}
static void worker_main(void *opaque) {
    Worker *worker = opaque;
    ScalerState *state = worker->state;
    for (;;) {
        qurt_sem_down(&worker->work);
        if (state->stop)
            break;
        worker->status = qurt_hvx_lock(QURT_HVX_MODE_128B);
        if (!worker->status) {
            scale_plane(state, state->src + state->src_y_offset, state->dst + state->dst_y_offset,
                        state->src_stride, state->dst_stride, 0, 1, worker->worker_id,
                        state->worker_count);
            scale_plane(state, state->src + state->src_uv_offset, state->dst + state->dst_uv_offset,
                        state->src_stride, state->dst_stride, 1, 1, worker->worker_id,
                        state->worker_count);
            // Publish HVX stores before the RPC thread observes completion.
            qurt_mem_syncht();
            qurt_hvx_unlock();
        }
        qurt_sem_up(&worker->done);
    }
    qurt_thread_exit(0);
}
static int plane_fits_buffer(int buffer_size, int offset, int stride, int width, int height) {
    return offset >= 0 && stride >= width &&
           (uint64_t)offset + (uint64_t)(height - 1) * stride + width <= (uint64_t)buffer_size;
}
int iris_vpp_run(remote_handle64 h, const uint8_t *src, int src_size, uint8_t *dst, int dst_size,
                 int src_stride, int dst_stride, int src_y_offset, int src_uv_offset,
                 int dst_y_offset, int dst_uv_offset, int worker_count, uint64_t *us) {
    ScalerState *state = (ScalerState *)(uintptr_t)h;
    if (!state->src_width || !src || !dst || src_size < 0 || dst_size < 0 || worker_count < 0 ||
        worker_count > CDSP_VPP_MAX_WORKERS ||
        !plane_fits_buffer(src_size, src_y_offset, src_stride, state->src_width,
                           state->src_height) ||
        !plane_fits_buffer(src_size, src_uv_offset, src_stride, state->src_width,
                           state->src_height / 2) ||
        !plane_fits_buffer(dst_size, dst_y_offset, dst_stride, state->dst_width,
                           state->dst_height) ||
        !plane_fits_buffer(dst_size, dst_uv_offset, dst_stride, state->dst_width,
                           state->dst_height / 2))
        return AEE_EBADPARM;
    uint64_t start_us = HAP_perf_get_time_us();
    if (state->profiling)
        memset(state->profile_us, 0, sizeof(state->profile_us));
    if (worker_count > 1) {
        state->src = src;
        state->dst = dst;
        state->src_stride = src_stride;
        state->dst_stride = dst_stride;
        state->src_y_offset = src_y_offset;
        state->src_uv_offset = src_uv_offset;
        state->dst_y_offset = dst_y_offset;
        state->dst_uv_offset = dst_uv_offset;
        state->worker_count = worker_count;
        for (int i = 0; i < worker_count; i++)
            qurt_sem_up(&state->workers[i].work);
        int status = 0;
        for (int i = 0; i < worker_count; i++) {
            qurt_sem_down(&state->workers[i].done);
            if (state->workers[i].status)
                status = AEE_EFAILED;
        }
        *us = HAP_perf_get_time_us() - start_us;
        return status;
    }
    if (worker_count && qurt_hvx_lock(QURT_HVX_MODE_128B) != 0)
        return AEE_EFAILED;
    scale_plane(state, src + src_y_offset, dst + dst_y_offset, src_stride, dst_stride, 0,
                worker_count, 0, 1);
    scale_plane(state, src + src_uv_offset, dst + dst_uv_offset, src_stride, dst_stride, 1,
                worker_count, 0, 1);
    if (worker_count)
        qurt_hvx_unlock();
    *us = HAP_perf_get_time_us() - start_us;
    return 0;
}
