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
    int use_delta[4], single_vector_delta, src_base, dst_offset, output_bytes, tap_count,
        spans_two_vectors;
    struct CubicTile *shared_pattern;
} __attribute__((aligned(128))) CubicTile;
// P010 filtering uses signed 16-bit rows and 32-bit accumulators. A tile
// gathers up to 64 components from at most two vectors of intermediate data.
typedef struct {
    uint8_t indices[FILTER_MAX_TAPS][128];
    int16_t weights[FILTER_MAX_TAPS][64];
    int src_base, dst_offset, components, tap_count, spans_two_vectors;
} __attribute__((aligned(128))) WideTile;
#define ROTATION_MAX_ROWS 128
#define TRANSPOSE_WORKER_BYTES (2 * ROTATION_MAX_ROWS * 128)

typedef struct {
    uint8_t *data;
    unsigned bytes, vtcm_context;
} RotationBuffer;

static void release_rotation_buffer(RotationBuffer *buffer) {
    if (buffer->vtcm_context) {
        qurt_mem_syncht();
        compute_resource_release(buffer->vtcm_context);
    } else
        free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int resize_rotation_buffer(RotationBuffer *buffer, unsigned bytes, int use_vtcm) {
    if (buffer->bytes == bytes && (!buffer->vtcm_context || use_vtcm))
        return 0;
    // Workers are idle here. Release old storage before requesting scarce VTCM.
    release_rotation_buffer(buffer);
    if (!bytes)
        return 0;
    if (use_vtcm && compute_resource_attr_init && compute_resource_attr_set_vtcm_param &&
        compute_resource_acquire && compute_resource_attr_get_vtcm_ptr &&
        compute_resource_release) {
        compute_res_attr_t attr;
        if (!compute_resource_attr_init(&attr) &&
            !compute_resource_attr_set_vtcm_param(&attr, bytes, 0)) {
            // Never wait for other clients or request exclusive access to the DSP.
            unsigned context = compute_resource_acquire(&attr, 0);
            if (context) {
                uint8_t *data = compute_resource_attr_get_vtcm_ptr(&attr);
                if (data && !((uintptr_t)data & 127)) {
                    buffer->data = data;
                    buffer->vtcm_context = context;
                } else
                    compute_resource_release(context);
            }
        }
    }
    // Missing firmware APIs and exhausted VTCM both use ordinary aligned memory.
    // Retain that choice until the layout changes, instead of retrying every frame.
    if (!buffer->data)
        buffer->data = memalign(128, bytes);
    if (!buffer->data)
        return AEE_ENOMEMORY;
    buffer->bytes = bytes;
    return 0;
}

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
    int16_t *cubic_rows, *wide_rows;
    WideTile *wide_tiles[2];
    int wide_tile_count[2], src_format, dst_format;
    RotationBuffer rotation_rows, transpose_workspace;
    int rotation, rotation_stride, rotation_worker_rows;
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
        free(state->wide_tiles[plane_index]);
        state->wide_tiles[plane_index] = 0;
        state->wide_tile_count[plane_index] = 0;
        state->cubic_tiles[plane_index] = 0;
        state->area_tiles[plane_index] = 0;
        state->horizontal_filters[plane_index] = state->vertical_filters[plane_index] = 0;
    }
    release_rotation_buffer(&state->rotation_rows);
    release_rotation_buffer(&state->transpose_workspace);
    state->rotation_stride = 0;
    free(state->wide_rows);
    state->wide_rows = 0;
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
    if (src_width < 2 || dst_width < 2 || src_height < 2 || dst_height < 2 ||
        (src_width | dst_width) & 1 || (src_height | dst_height) & 1 || src_width > 8192 ||
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
                tile->single_vector_delta = tile->tap_count == 4 && !tile->spans_two_vectors &&
                                            tile->use_delta[0] && tile->use_delta[1] &&
                                            tile->use_delta[2] && tile->use_delta[3];
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
// P010 reduction uses wide tiles; enlargement shares the cubic routing cache.
// Allocate a signed intermediate row only when a P010 operation resamples.
static int prepare_wide_filters(ScalerState *state) {
    if (state->wide_rows)
        return 0;
    for (int plane = 0; plane < 2 && !state->cubic_rows; plane++) {
        int channels = plane ? 2 : 1;
        int capacity = 0;
        for (int x = 0; x < state->dst_width;) {
            Filter *first = &state->horizontal_filters[plane][x / channels];
            int base = first->indices[0] * channels;
            int count = 0;
            while (count + channels <= 64 && x + count < state->dst_width) {
                Filter *filter = &state->horizontal_filters[plane][(x + count) / channels];
                int last = (filter->indices[filter->tap_count - 1] + 1) * channels;
                if (last - base > 128)
                    break;
                count += channels;
            }
            if (!count)
                goto fail;
            if (state->wide_tile_count[plane] == capacity) {
                int next = capacity ? capacity * 2 : 16;
                WideTile *tiles = memalign(128, next * sizeof(*tiles));
                if (!tiles)
                    goto fail;
                if (capacity)
                    memcpy(tiles, state->wide_tiles[plane], capacity * sizeof(*tiles));
                free(state->wide_tiles[plane]);
                state->wide_tiles[plane] = tiles;
                capacity = next;
            }
            WideTile *tile = &state->wide_tiles[plane][state->wide_tile_count[plane]++];
            memset(tile, 0, sizeof(*tile));
            tile->src_base = base;
            tile->dst_offset = x;
            tile->components = count;
            for (int i = 0; i < count; i++) {
                Filter *filter = &state->horizontal_filters[plane][(x + i) / channels];
                if (filter->tap_count > tile->tap_count)
                    tile->tap_count = filter->tap_count;
                for (int k = 0; k < filter->tap_count; k++) {
                    int byte = 2 * (filter->indices[k] * channels + i % channels - base);
                    tile->indices[k][2 * i] = byte;
                    tile->indices[k][2 * i + 1] = byte + 1;
                    tile->weights[k][i] = filter->weights[k];
                    tile->spans_two_vectors |= byte >= 128;
                }
            }
            x += count;
        }
    }
    state->wide_rows =
        memalign(128, CDSP_VPP_MAX_WORKERS * cubic_row_stride_bytes(state->src_width));
    if (!state->wide_rows)
        goto fail;
    memset(state->wide_rows, 0, CDSP_VPP_MAX_WORKERS * cubic_row_stride_bytes(state->src_width));
    return 0;
fail:
    for (int plane = 0; plane < 2; plane++) {
        free(state->wide_tiles[plane]);
        state->wide_tiles[plane] = 0;
        state->wide_tile_count[plane] = 0;
    }
    return AEE_ENOMEMORY;
}
typedef HVX_Vector UnalignedVector __attribute__((aligned(1)));
// Store 1..128 live bytes without issuing an all-false store beyond the
// buffer's last page. vsetq masks its count to seven bits: 128 is not all-ones.
static void store_partial_vector(uint8_t *dst, HVX_Vector value, int bytes) {
    int offset = (uintptr_t)dst & 127, end = offset + bytes;
    HVX_Vector *aligned = (HVX_Vector *)((uintptr_t)dst & ~(uintptr_t)127);
    value = Q6_V_vror_VR(value, (128 - offset) & 127);
    HVX_VectorPred mask = Q6_Q_not_Q(Q6_Q_vsetq_R(offset));
    if (end < 128)
        mask = Q6_Q_and_QQ(mask, Q6_Q_vsetq_R(end));
    Q6_vmem_QRIV(mask, aligned, value);
    if (end > 128)
        Q6_vmem_QRIV(Q6_Q_vsetq_R(end - 128), aligned + 1, value);
}
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
// Keep vertical kernels out of the row dispatcher to limit register pressure.
static __attribute__((noinline)) void cubic_vertical(const uint8_t *src, int stride,
                                                     int16_t *restrict row, int width,
                                                     const Filter *filter, int hvx) {
    int x = 0, shift = filter->shift - 6;
    if (hvx && width >= 128 && filter->tap_count == 4) {
        const uint8_t *lines[4];
        int weights[4];
        for (int k = 0; k < 4; k++) {
            lines[k] = src + filter->indices[k] * stride;
            weights[k] = (uint32_t)(uint16_t)filter->weights[k] * 0x00010001u;
        }
        for (;;) {
            // Recompute the last full vector instead of filtering a scalar
            // tail. The overlapping read and write stay within this row.
            if (x + 128 > width)
                x = width - 128;
            HVX_VectorPair even = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero()), odd = even;
#pragma clang loop unroll(full)
            for (int k = 0; k < 4; k++) {
                HVX_VectorPair widened = Q6_Wuh_vzxt_Vub(*(const UnalignedVector *)(lines[k] + x));
                even = Q6_Ww_vmpyacc_WwVhRh(even, Q6_V_lo_W(widened), weights[k]);
                odd = Q6_Ww_vmpyacc_WwVhRh(odd, Q6_V_hi_W(widened), weights[k]);
            }
            HVX_Vector even_samples =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(even), Q6_V_lo_W(even), shift);
            HVX_Vector odd_samples =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(odd), Q6_V_lo_W(odd), shift);
            HVX_VectorPair v = Q6_W_vshuff_VVR(odd_samples, even_samples, -2);
            *(UnalignedVector *)(row + x) = Q6_V_lo_W(v);
            *(UnalignedVector *)(row + x + 64) = Q6_V_hi_W(v);
            x += 128;
            if (x == width)
                break;
        }
    } else if (hvx)
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
    int p010 = state->dst_format == CDSP_VPP_P010;
    int shift = p010 ? 11 : 13;
    // Extend both borders independently per channel, then evaluate the two Q7
    // phases [-3, 29, 111, -9] and [-9, 111, 29, -3]. The row is 8-bit Q6
    // (equivalently 10-bit Q4); round directly to the destination bit depth.
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
        HVX_Vector even_samples = Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(even), Q6_V_lo_W(even), shift);
        HVX_Vector odd_samples = Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(odd), Q6_V_lo_W(odd), shift);
        HVX_VectorPair interleaved =
            Q6_W_vshuff_VVR(odd_samples, even_samples, plane_index ? -4 : -2);
        int components = 2 * (state->src_width - x);
        if (p010) {
            HVX_Vector lo = Q6_Vh_vmax_VhVh(Q6_V_lo_W(interleaved), Q6_V_vzero());
            HVX_Vector hi = Q6_Vh_vmax_VhVh(Q6_V_hi_W(interleaved), Q6_V_vzero());
            lo = Q6_Vh_vasl_VhR(Q6_Vh_vmin_VhVh(lo, Q6_V_vsplat_R(0x03ff03ff)), 6);
            hi = Q6_Vh_vasl_VhR(Q6_Vh_vmin_VhVh(hi, Q6_V_vsplat_R(0x03ff03ff)), 6);
            if (components >= 128) {
                *(UnalignedVector *)(dst + 4 * x) = lo;
                *(UnalignedVector *)(dst + 4 * x + 128) = hi;
            } else {
                uint8_t tail[256] __attribute__((aligned(128)));
                *(HVX_Vector *)tail = lo;
                *(HVX_Vector *)(tail + 128) = hi;
                memcpy(dst + 4 * x, tail, 2 * components);
            }
        } else {
            HVX_Vector out = Q6_Vub_vpack_VhVh_sat(Q6_V_hi_W(interleaved), Q6_V_lo_W(interleaved));
            if (components >= 128)
                *(UnalignedVector *)(dst + 2 * x) = out;
            else {
                uint8_t tail[128] __attribute__((aligned(128)));
                *(HVX_Vector *)tail = out;
                memcpy(dst + 2 * x, tail, components);
            }
        }
    }
}
// NV12 Q6 and 10-bit Q4 intermediates share a numeric scale. Reuse the
// gather patterns for both formats and round directly to the output bit depth.
static void cubic_horizontal(ScalerState *state, int plane_index, uint8_t *dst, int16_t *row) {
    if (state->dst_width == 2 * state->src_width) {
        cubic_horizontal_2x(state, plane_index, dst, row);
        return;
    }
    int shift = state->horizontal_filters[plane_index][0].shift;
    int p010 = state->dst_format == CDSP_VPP_P010;
    int extra = p010 ? 4 : 6;
    HVX_Vector previous = Q6_V_vzero();
    for (int tile_index = 0; tile_index < state->cubic_tile_count[plane_index]; tile_index++) {
        CubicTile *tile = &state->cubic_tiles[plane_index][tile_index],
                  *pattern = tile->shared_pattern;
        HVX_Vector lower = *(const UnalignedVector *)(row + tile->src_base),
                   upper =
                       *(const UnalignedVector *)((const uint8_t *)(row + tile->src_base) + 128);
        HVX_VectorPair accumulator = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
        if (pattern->single_vector_delta) {
            // Upsampling usually gathers all four taps from one vector. Keep
            // that case branch-free inside the tap loop so LLVM can schedule
            // the independent gathers and coefficient loads together.
#pragma clang loop unroll(full)
            for (int k = 0; k < 4; k++) {
                HVX_Vector samples =
                    Q6_V_vdelta_VV(lower, *(HVX_Vector *)pattern->delta_controls[k][0]);
                accumulator =
                    Q6_Ww_vmpyacc_WwVhVh(accumulator, samples, *(HVX_Vector *)pattern->weights[k]);
            }
        } else
            for (int k = 0; k < tile->tap_count; k++) {
                HVX_Vector indices = *(HVX_Vector *)pattern->indices[k], samples;
                if (pattern->use_delta[k]) {
                    HVX_Vector lower_samples =
                        Q6_V_vdelta_VV(lower, *(HVX_Vector *)pattern->delta_controls[k][0]);
                    if (tile->spans_two_vectors) {
                        HVX_Vector upper_samples =
                            Q6_V_vdelta_VV(upper, *(HVX_Vector *)pattern->delta_controls[k][1]);
                        samples =
                            Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VubVub(indices, Q6_V_vsplat_R(0x7f7f7f7f)),
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
        // fractional intermediate bits first; the final rounding is unchanged.
        HVX_Vector narrowed =
            Q6_Vh_vasr_VwVwR_rnd_sat(Q6_Vw_vasr_VwR(Q6_V_hi_W(accumulator), extra),
                                     Q6_Vw_vasr_VwR(Q6_V_lo_W(accumulator), extra), shift);
        if (p010) {
            narrowed = Q6_Vh_vmax_VhVh(narrowed, Q6_V_vzero());
            narrowed = Q6_Vh_vmin_VhVh(narrowed, Q6_V_vsplat_R(0x03ff03ff));
            HVX_Vector out = Q6_Vh_vasl_VhR(narrowed, 6);
            uint8_t *target = dst + 2 * tile->dst_offset;
            if (tile->output_bytes == 64)
                *(UnalignedVector *)target = out;
            else
                store_partial_vector(target, out, 2 * tile->output_bytes);
            continue;
        }
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
                              int first_row, int end_row) {
    int channels = plane_index ? 2 : 1;
    int16_t *row = (int16_t *)((uint8_t *)state->cubic_rows +
                               worker_id * cubic_row_stride_bytes(state->src_width) + 128);
    for (int y = first_row; y < end_row; y++) {
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
            cubic_horizontal(state, plane_index, dst + (y - first_row) * dst_stride, row);
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
                dst[(y - first_row) * dst_stride + x * channels + ch] =
                    round_clip_u8(sum, filter->shift + 6);
            }
    }
}
static void scale_plane(ScalerState *state, const uint8_t *src, uint8_t *dst, int src_stride,
                        int dst_stride, int plane_index, int hvx, int worker_id, int first_row,
                        int end_row) {
    if (state->cubic_rows) {
        scale_cubic_plane(state, src, dst, src_stride, dst_stride, plane_index, hvx, worker_id,
                          first_row, end_row);
        return;
    }
    int channels = plane_index ? 2 : 1;
    uint8_t *row = state->area_rows + worker_id * area_row_stride_bytes(state->src_width);
    for (int y = first_row; y < end_row; y++) {
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
                *(UnalignedVector *)(dst + (y - first_row) * dst_stride + x) =
                    *(const UnalignedVector *)(src + y * src_stride + x);
            if (x < state->dst_width)
                memcpy(dst + (y - first_row) * dst_stride + x, src + y * src_stride + x,
                       state->dst_width - x);
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
                *(UnalignedVector *)(dst + (y - first_row) * dst_stride + x) =
                    Q6_Vub_vavg_VubVub_rnd(even, odd);
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
                uint8_t *target = dst + (y - first_row) * dst_stride + x;
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
            area_horizontal(state, plane_index, dst + (y - first_row) * dst_stride, row);
            continue;
        }
        for (int x = 0; x < state->dst_width / channels; x++)
            for (int ch = 0; ch < channels; ch++) {
                Filter *filter = &state->horizontal_filters[plane_index][x];
                int a = 0;
                for (int k = 0; k < filter->tap_count; k++)
                    a += row[filter->indices[k] * channels + ch] * filter->weights[k];
                dst[(y - first_row) * dst_stride + x * channels + ch] =
                    round_clip_u8(a, filter->shift);
            }
    }
}
// P010 stores ten significant bits in the high bits of little-endian words.
// Preserve code values/range: 8->10 is a two-bit shift, 10->8 rounds and clips.
static void convert_row(const uint8_t *src, uint8_t *dst, int width, int src_format, int dst_format,
                        int hvx) {
    if (src_format == dst_format) {
        int bytes = width * (src_format == CDSP_VPP_P010 ? 2 : 1), x = 0;
        if (hvx)
            for (; x + 128 <= bytes; x += 128)
                *(UnalignedVector *)(dst + x) = *(const UnalignedVector *)(src + x);
        memcpy(dst + x, src + x, bytes - x);
        return;
    }
    int x = 0;
    if (hvx)
        for (; x + 128 <= width; x += 128) {
            if (src_format == CDSP_VPP_NV12) {
                HVX_VectorPair words = Q6_Wuh_vunpack_Vub(*(const UnalignedVector *)(src + x));
                *(UnalignedVector *)(dst + 2 * x) = Q6_Vh_vasl_VhR(Q6_V_lo_W(words), 8);
                *(UnalignedVector *)(dst + 2 * x + 128) = Q6_Vh_vasl_VhR(Q6_V_hi_W(words), 8);
            } else {
                HVX_Vector lo = *(const UnalignedVector *)(src + 2 * x);
                HVX_Vector hi = *(const UnalignedVector *)(src + 2 * x + 128);
                // Saturating addition preserves the 1022/1023 -> 255 endpoint.
                HVX_Vector bias = Q6_V_vsplat_R(0x00800080);
                lo = Q6_Vuh_vadd_VuhVuh_sat(lo, bias);
                hi = Q6_Vuh_vadd_VuhVuh_sat(hi, bias);
                *(UnalignedVector *)(dst + x) = Q6_Vb_vpacko_VhVh(hi, lo);
            }
        }
    for (; x < width; x++) {
        if (src_format == CDSP_VPP_NV12)
            ((uint16_t *)dst)[x] = (uint16_t)src[x] << 8;
        else
            dst[x] = clamp_int((((const uint16_t *)src)[x] + 128) >> 8, 0, 255);
    }
}
static __attribute__((noinline)) void wide_vertical(const uint8_t *src, int stride,
                                                    int16_t *restrict row, int width,
                                                    const Filter *filter, int format, int hvx) {
    // Q4 keeps 10-bit cubic overshoot within int16_t. Area uses the same
    // precision so conversion to 8 bits happens only after both filter passes.
    int shift = filter->shift - 4, x = 0;
    if (hvx && format == CDSP_VPP_P010 && filter->tap_count == 4 && width >= 64) {
        const uint8_t *lines[4];
        int weights[4];
        for (int k = 0; k < 4; k++) {
            lines[k] = src + filter->indices[k] * stride;
            weights[k] = (uint32_t)(uint16_t)filter->weights[k] * 0x00010001u;
        }
        for (;;) {
            if (x + 64 > width)
                x = width - 64;
            HVX_VectorPair sum = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
#pragma clang loop unroll(full)
            for (int k = 0; k < 4; k++) {
                HVX_Vector samples =
                    Q6_Vuh_vlsr_VuhR(*(const UnalignedVector *)(lines[k] + 2 * x), 6);
                sum = Q6_Ww_vmpyacc_WwVhRh(sum, samples, weights[k]);
            }
            *(UnalignedVector *)(row + x) =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(sum), Q6_V_lo_W(sum), shift);
            x += 64;
            if (x == width)
                break;
        }
    } else if (hvx)
        for (; x + 64 <= width; x += 64) {
            HVX_VectorPair sum = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
            for (int k = 0; k < filter->tap_count; k++) {
                const uint8_t *line = src + filter->indices[k] * stride;
                HVX_Vector samples;
                if (format == CDSP_VPP_P010)
                    samples = Q6_Vuh_vlsr_VuhR(*(const UnalignedVector *)(line + 2 * x), 6);
                else {
                    HVX_Vector bytes;
                    if (x + 128 <= width)
                        bytes = *(const UnalignedVector *)(line + x);
                    else {
                        uint8_t tail[128] __attribute__((aligned(128))) = {0};
                        memcpy(tail, line + x, 64);
                        bytes = *(HVX_Vector *)tail;
                    }
                    samples = Q6_Vh_vasl_VhR(Q6_V_lo_W(Q6_Wuh_vunpack_Vub(bytes)), 2);
                }
                uint32_t weight = (uint16_t)filter->weights[k] * 0x00010001u;
                sum = Q6_Ww_vmpyacc_WwVhRh(sum, samples, weight);
            }
            *(HVX_Vector *)(row + x) =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_V_hi_W(sum), Q6_V_lo_W(sum), shift);
        }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < filter->tap_count; k++) {
            const uint8_t *line = src + filter->indices[k] * stride;
            int sample = format == CDSP_VPP_P010 ? ((const uint16_t *)line)[x] >> 6 : line[x] << 2;
            sum += sample * filter->weights[k];
        }
        row[x] = (sum + (1 << (shift - 1))) >> shift;
    }
}
static void wide_horizontal(ScalerState *state, int plane, uint8_t *dst, const int16_t *row,
                            int hvx) {
    int shift = state->horizontal_filters[plane][0].shift;
    int extra = 4 + (state->dst_format == CDSP_VPP_NV12 ? 2 : 0);
    if (hvx) {
        for (int t = 0; t < state->wide_tile_count[plane]; t++) {
            WideTile *tile = &state->wide_tiles[plane][t];
            HVX_Vector lo = Q6_Vb_vshuff_Vb(*(const UnalignedVector *)(row + tile->src_base));
            HVX_Vector hi = Q6_V_vzero();
            if (tile->spans_two_vectors)
                hi = Q6_Vb_vshuff_Vb(*(const UnalignedVector *)(row + tile->src_base + 64));
            HVX_VectorPair sum = Q6_W_vcombine_VV(Q6_V_vzero(), Q6_V_vzero());
            for (int k = 0; k < tile->tap_count; k++) {
                HVX_Vector samples =
                    gather_bytes(*(HVX_Vector *)tile->indices[k], lo, hi, tile->spans_two_vectors);
                sum = Q6_Ww_vmpyacc_WwVhVh(sum, samples, *(HVX_Vector *)tile->weights[k]);
            }
            // The narrowing shift is four bits wide; discard fractional bits
            // separately without rounding, then round exactly once at output.
            HVX_Vector values =
                Q6_Vh_vasr_VwVwR_rnd_sat(Q6_Vw_vasr_VwR(Q6_V_hi_W(sum), extra),
                                         Q6_Vw_vasr_VwR(Q6_V_lo_W(sum), extra), shift);
            int bytes = tile->components;
            HVX_Vector out;
            if (state->dst_format == CDSP_VPP_P010) {
                values = Q6_Vh_vmax_VhVh(values, Q6_V_vzero());
                values = Q6_Vh_vmin_VhVh(values, Q6_V_vsplat_R(0x03ff03ff));
                out = Q6_Vh_vasl_VhR(values, 6);
                bytes *= 2;
            } else
                out = Q6_Vub_vpack_VhVh_sat(Q6_V_vzero(), values);
            uint8_t *target = dst + tile->dst_offset * (state->dst_format == CDSP_VPP_P010 ? 2 : 1);
            if (bytes == 128)
                *(UnalignedVector *)target = out;
            else {
                uint8_t tail[128] __attribute__((aligned(128)));
                *(HVX_Vector *)tail = out;
                memcpy(target, tail, bytes);
            }
        }
        return;
    }
    int channels = plane ? 2 : 1;
    for (int x = 0; x < state->dst_width; x++) {
        Filter *filter = &state->horizontal_filters[plane][x / channels];
        int sum = 0;
        for (int k = 0; k < filter->tap_count; k++)
            sum += row[filter->indices[k] * channels + x % channels] * filter->weights[k];
        int value = (sum + (1 << (shift + extra - 1))) >> (shift + extra);
        if (state->dst_format == CDSP_VPP_P010)
            ((uint16_t *)dst)[x] = clamp_int(value, 0, 1023) << 6;
        else
            dst[x] = clamp_int(value, 0, 255);
    }
}
static HVX_VectorPair load_10bit_samples(const uint8_t *src, int format) {
    HVX_Vector lo, hi;
    if (format == CDSP_VPP_P010) {
        lo = Q6_Vuh_vlsr_VuhR(*(const UnalignedVector *)src, 6);
        hi = Q6_Vuh_vlsr_VuhR(*(const UnalignedVector *)(src + 128), 6);
    } else {
        HVX_VectorPair words = Q6_Wuh_vunpack_Vub(*(const UnalignedVector *)src);
        lo = Q6_Vh_vasl_VhR(Q6_V_lo_W(words), 2);
        hi = Q6_Vh_vasl_VhR(Q6_V_hi_W(words), 2);
    }
    return Q6_W_vcombine_VV(hi, lo);
}
static void wide_downscale_2x_row(ScalerState *state, const uint8_t *r0, const uint8_t *r1,
                                  uint8_t *dst, int channels, int hvx) {
    int x = 0, src_bytes = state->src_format == CDSP_VPP_P010 ? 2 : 1;
    if (hvx)
        for (; x + 64 <= state->dst_width; x += 64) {
            HVX_VectorPair a = load_10bit_samples(r0 + 2 * x * src_bytes, state->src_format);
            HVX_VectorPair b = load_10bit_samples(r1 + 2 * x * src_bytes, state->src_format);
            HVX_Vector lo = Q6_Vh_vadd_VhVh(Q6_V_lo_W(a), Q6_V_lo_W(b));
            HVX_Vector hi = Q6_Vh_vadd_VhVh(Q6_V_hi_W(a), Q6_V_hi_W(b));
            // Deinterleave whole U/V pairs for chroma, individual words for Y.
            HVX_VectorPair pairs = Q6_W_vdeal_VVR(hi, lo, channels == 2 ? -4 : -2);
            HVX_Vector sum = Q6_Vh_vadd_VhVh(Q6_V_lo_W(pairs), Q6_V_hi_W(pairs));
            if (state->dst_format == CDSP_VPP_P010) {
                sum = Q6_Vuh_vlsr_VuhR(Q6_Vh_vadd_VhVh(sum, Q6_V_vsplat_R(0x00020002)), 2);
                *(UnalignedVector *)(dst + 2 * x) = Q6_Vh_vasl_VhR(sum, 6);
            } else {
                sum = Q6_Vuh_vlsr_VuhR(Q6_Vh_vadd_VhVh(sum, Q6_V_vsplat_R(0x00080008)), 4);
                HVX_Vector out = Q6_Vub_vpack_VhVh_sat(Q6_V_vzero(), sum);
                store_partial_vector(dst + x, out, 64);
            }
        }
    for (; x < state->dst_width; x++) {
        int index = 2 * (x / channels) * channels + x % channels;
        int sum;
        if (state->src_format == CDSP_VPP_P010) {
            const uint16_t *a = (const uint16_t *)r0, *b = (const uint16_t *)r1;
            sum = (a[index] >> 6) + (a[index + channels] >> 6) + (b[index] >> 6) +
                  (b[index + channels] >> 6);
        } else
            sum = (r0[index] + r0[index + channels] + r1[index] + r1[index + channels]) * 4;
        if (state->dst_format == CDSP_VPP_P010)
            ((uint16_t *)dst)[x] = ((sum + 2) >> 2) << 6;
        else
            dst[x] = clamp_int((sum + 8) >> 4, 0, 255);
    }
}
static void process_rows(ScalerState *state, const uint8_t *src, uint8_t *dst, int src_stride,
                         int dst_stride, int plane, int hvx, int worker, int first_row,
                         int end_row) {
    if (state->src_format == CDSP_VPP_NV12 && state->dst_format == CDSP_VPP_NV12) {
        scale_plane(state, src, dst, src_stride, dst_stride, plane, hvx, worker, first_row,
                    end_row);
        return;
    }
    int height = state->dst_height / (plane ? 2 : 1);
    int same_size = state->src_width == state->dst_width && state->src_height == state->dst_height;
    int halve =
        state->src_width == 2 * state->dst_width && state->src_height == 2 * state->dst_height;
    int16_t *row = (same_size || halve)
                       ? 0
                       : (int16_t *)((uint8_t *)state->wide_rows +
                                     worker * cubic_row_stride_bytes(state->src_width) + 128);
    for (int y = first_row; y < end_row; y++) {
        if (same_size) {
            convert_row(src + y * src_stride, dst + (y - first_row) * dst_stride, state->dst_width,
                        state->src_format, state->dst_format, hvx);
            continue;
        }
        if (hvx && src_stride <= 65535) {
            int ahead = y + 2 < height ? y + 2 : y;
            Filter *filter = &state->vertical_filters[plane][ahead];
            int bytes = state->src_width * (state->src_format == CDSP_VPP_P010 ? 2 : 1);
            Q6_l2fetch_AP((void *)(src + filter->indices[0] * src_stride),
                          ((uint64_t)src_stride << 32) | ((uint64_t)bytes << 16) |
                              (filter->indices[filter->tap_count - 1] - filter->indices[0] + 1));
        }
        if (halve) {
            wide_downscale_2x_row(state, src + 2 * y * src_stride, src + (2 * y + 1) * src_stride,
                                  dst + (y - first_row) * dst_stride, plane ? 2 : 1, hvx);
            continue;
        }
        uint64_t start = state->profiling ? HAP_perf_get_time_us() : 0;
        if (hvx && state->src_format == CDSP_VPP_NV12 && state->dst_height == 2 * state->src_height)
            cubic_vertical_2x(src, src_stride, row, state->src_width,
                              &state->vertical_filters[plane][y]);
        else if (state->src_format == CDSP_VPP_NV12 && state->cubic_rows)
            cubic_vertical(src, src_stride, row, state->src_width,
                           &state->vertical_filters[plane][y], hvx);
        else
            wide_vertical(src, src_stride, row, state->src_width,
                          &state->vertical_filters[plane][y], state->src_format, hvx);
        uint64_t middle = state->profiling ? HAP_perf_get_time_us() : 0;
        if (hvx && state->cubic_rows)
            cubic_horizontal(state, plane, dst + (y - first_row) * dst_stride, row);
        else
            wide_horizontal(state, plane, dst + (y - first_row) * dst_stride, row, hvx);
        if (state->profiling) {
            state->profile_us[worker * 4 + plane * 2] += middle - start;
            state->profile_us[worker * 4 + plane * 2 + 1] += HAP_perf_get_time_us() - middle;
        }
    }
}
// Reverse pixels, treating each interleaved U/V pair as one indivisible unit.
static void reverse_row(const uint8_t *src, uint8_t *dst, int pixels, int pixel_bytes, int hvx) {
    int bytes = pixels * pixel_bytes, x = 0;
    if (hvx) {
        HVX_Vector control = Q6_V_vsplat_R((128 - pixel_bytes) * 0x01010101u);
        for (; x + 128 <= bytes; x += 128) {
            HVX_Vector v = *(const UnalignedVector *)(src + bytes - x - 128);
            *(UnalignedVector *)(dst + x) = Q6_V_vdelta_VV(v, control);
        }
    }
    for (; x < bytes; x += pixel_bytes)
        memcpy(dst + x, src + bytes - x - pixel_bytes, pixel_bytes);
}
// A square tile has 128 / pixel_bytes rows and columns. Butterfly stages
// transpose whole pixels (including U/V pairs), so each complete output row
// becomes one 128-byte store. The two work arrays live outside the worker stack.
static void rotate_strip(const uint8_t *restrict src, int src_stride, uint8_t *restrict dst,
                         int dst_stride, int width, int height, int first_row, int rows,
                         int pixel_bytes, int rotation, int hvx, uint8_t *restrict workspace,
                         int tile_rows) {
    if (rotation == CDSP_VPP_ROTATE_180) {
        for (int y = 0; y < rows; y++)
            reverse_row(src + y * src_stride, dst + (height - first_row - y - 1) * dst_stride,
                        width, pixel_bytes, hvx);
        return;
    }
    int clockwise = rotation == CDSP_VPP_ROTATE_90;
    int column = clockwise ? height - first_row - rows : first_row;
    if (hvx) {
        HVX_Vector(*buffers)[ROTATION_MAX_ROWS] = (HVX_Vector(*)[ROTATION_MAX_ROWS])workspace;
        int columns = 128 / pixel_bytes;
        for (int x = 0; x < width; x += columns) {
            int count = width - x < columns ? width - x : columns;
            for (int y = 0; y < tile_rows; y++) {
                buffers[0][y] = Q6_V_vzero();
                if (y < rows) {
                    const uint8_t *line =
                        src + (clockwise ? rows - y - 1 : y) * src_stride + x * pixel_bytes;
                    if (count == columns)
                        buffers[0][y] = *(const UnalignedVector *)line;
                    else
                        memcpy(&buffers[0][y], line, count * pixel_bytes);
                }
            }
            int stage = 0;
            for (int group = 1; group < tile_rows; stage++, group *= 2) {
                HVX_Vector *in = buffers[stage & 1], *out = buffers[(stage + 1) & 1];
                for (int base = 0; base < tile_rows; base += 2 * group)
                    for (int i = 0; i < group; i++) {
                        HVX_VectorPair pair = Q6_W_vshuff_VVR(in[base + i + group], in[base + i],
                                                              -pixel_bytes * group);
                        out[base + 2 * i] = Q6_V_lo_W(pair);
                        out[base + 2 * i + 1] = Q6_V_hi_W(pair);
                    }
            }
            const uint8_t *transposed = (const uint8_t *)buffers[stage & 1];
            for (int i = 0; i < count; i++) {
                int output_row = clockwise ? x + i : width - x - i - 1;
                uint8_t *target = dst + output_row * dst_stride + column * pixel_bytes;
                HVX_Vector values = ((const HVX_Vector *)transposed)[i];
                if (rows == tile_rows)
                    *(UnalignedVector *)target = values;
                else
                    store_partial_vector(target, values, rows * pixel_bytes);
            }
        }
        return;
    }
    for (int x = 0; x < width; x++)
        for (int y = 0; y < rows; y++) {
            int output_row = clockwise ? x : width - x - 1;
            int output_column = clockwise ? height - first_row - y - 1 : first_row + y;
            memcpy(dst + output_row * dst_stride + output_column * pixel_bytes,
                   src + y * src_stride + x * pixel_bytes, pixel_bytes);
        }
}
static void process_plane(ScalerState *state, const uint8_t *src, uint8_t *dst, int src_stride,
                          int dst_stride, int plane, int hvx, int worker, int workers) {
    int channels = plane ? 2 : 1;
    int height = state->dst_height / channels;
    if (!state->rotation) {
        int first = height * worker / workers, end = height * (worker + 1) / workers;
        process_rows(state, src, dst + first * dst_stride, src_stride, dst_stride, plane, hvx,
                     worker, first, end);
        return;
    }
    int pixel_bytes = channels * (state->dst_format == CDSP_VPP_P010 ? 2 : 1);
    int direct = state->src_format == state->dst_format && state->src_width == state->dst_width &&
                 state->src_height == state->dst_height;
    int tile_rows = hvx && (state->rotation & 1) ? 128 / pixel_bytes : 16;
    int strips = (height + tile_rows - 1) / tile_rows;
    for (int block = strips * worker / workers; block < strips * (worker + 1) / workers; block++) {
        int first = block * tile_rows;
        int rows = height - first < tile_rows ? height - first : tile_rows;
        const uint8_t *input = src;
        int stride = src_stride;
        if (!direct) {
            uint8_t *scratch = state->rotation_rows.data +
                               worker * state->rotation_worker_rows * state->rotation_stride;
            stride = state->rotation_stride;
            process_rows(state, src, scratch, src_stride, stride, plane, hvx, worker, first,
                         first + rows);
            input = scratch;
        } else
            input += first * src_stride;
        rotate_strip(input, stride, dst, dst_stride, state->dst_width / channels, height, first,
                     rows, pixel_bytes, state->rotation, hvx,
                     hvx && (state->rotation & 1)
                         ? state->transpose_workspace.data + worker * TRANSPOSE_WORKER_BYTES
                         : 0,
                     tile_rows);
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
            process_plane(state, state->src + state->src_y_offset, state->dst + state->dst_y_offset,
                          state->src_stride, state->dst_stride, 0, 1, worker->worker_id,
                          state->worker_count);
            process_plane(state, state->src + state->src_uv_offset,
                          state->dst + state->dst_uv_offset, state->src_stride, state->dst_stride,
                          1, 1, worker->worker_id, state->worker_count);
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
    return iris_vpp_process(h, src, src_size, dst, dst_size, src_stride, dst_stride, src_y_offset,
                            src_uv_offset, dst_y_offset, dst_uv_offset, worker_count, CDSP_VPP_NV12,
                            CDSP_VPP_NV12, us);
}
int iris_vpp_process(remote_handle64 h, const uint8_t *src, int src_size, uint8_t *dst,
                     int dst_size, int src_stride, int dst_stride, int src_y_offset,
                     int src_uv_offset, int dst_y_offset, int dst_uv_offset, int worker_count,
                     int src_format, int dst_format, uint64_t *us) {
    return iris_vpp_transform(h, src, src_size, dst, dst_size, src_stride, dst_stride, src_y_offset,
                              src_uv_offset, dst_y_offset, dst_uv_offset, worker_count, src_format,
                              dst_format, CDSP_VPP_ROTATE_NONE, us);
}
int iris_vpp_transform(remote_handle64 h, const uint8_t *src, int src_size, uint8_t *dst,
                       int dst_size, int src_stride, int dst_stride, int src_y_offset,
                       int src_uv_offset, int dst_y_offset, int dst_uv_offset, int worker_count,
                       int src_format, int dst_format, int rotation, uint64_t *us) {
    if (rotation < CDSP_VPP_ROTATE_NONE || rotation > CDSP_VPP_ROTATE_270)
        return AEE_EBADPARM;
    ScalerState *state = (ScalerState *)(uintptr_t)h;
    if ((src_format != CDSP_VPP_NV12 && src_format != CDSP_VPP_P010) ||
        (dst_format != CDSP_VPP_NV12 && dst_format != CDSP_VPP_P010))
        return AEE_EBADPARM;
    int src_bytes = src_format == CDSP_VPP_P010 ? 2 : 1;
    int dst_bytes = dst_format == CDSP_VPP_P010 ? 2 : 1;
    int output_width = (rotation & 1) ? state->dst_height : state->dst_width;
    int output_height = (rotation & 1) ? state->dst_width : state->dst_height;
    if (((src_stride | src_y_offset | src_uv_offset) & (src_bytes - 1)) ||
        ((dst_stride | dst_y_offset | dst_uv_offset) & (dst_bytes - 1)))
        return AEE_EBADPARM;
    if (!state->src_width || !src || !dst || src_size < 0 || dst_size < 0 || worker_count < 0 ||
        worker_count > CDSP_VPP_MAX_WORKERS ||
        !plane_fits_buffer(src_size, src_y_offset, src_stride, state->src_width * src_bytes,
                           state->src_height) ||
        !plane_fits_buffer(src_size, src_uv_offset, src_stride, state->src_width * src_bytes,
                           state->src_height / 2) ||
        !plane_fits_buffer(dst_size, dst_y_offset, dst_stride, output_width * dst_bytes,
                           output_height) ||
        !plane_fits_buffer(dst_size, dst_uv_offset, dst_stride, output_width * dst_bytes,
                           output_height / 2))
        return AEE_EBADPARM;
    state->rotation = rotation;
    state->src_format = src_format;
    state->dst_format = dst_format;
    if ((src_format == CDSP_VPP_P010 || dst_format == CDSP_VPP_P010) &&
        (state->src_width != state->dst_width || state->src_height != state->dst_height) &&
        !(state->src_width == 2 * state->dst_width && state->src_height == 2 * state->dst_height)) {
        int e = prepare_wide_filters(state);
        if (e)
            return e;
    }
    int transpose = worker_count && (rotation & 1);
    int workers = worker_count ? worker_count : 1;
    int e = resize_rotation_buffer(&state->transpose_workspace,
                                   transpose ? workers * TRANSPOSE_WORKER_BYTES : 0, transpose);
    if (e)
        return e;
    // Conversion/scaling feeds bounded strips, never a second full frame.
    // Pure same-format rotation can read the mapped source directly.
    state->rotation_worker_rows = transpose ? 128 / dst_bytes : 16;
    state->rotation_stride = (state->dst_width * dst_bytes + 127) & ~127;
    int needs_strip =
        rotation && (src_format != dst_format || state->src_width != state->dst_width ||
                     state->src_height != state->dst_height);
    unsigned strip_bytes =
        needs_strip ? workers * state->rotation_worker_rows * state->rotation_stride : 0;
    e = resize_rotation_buffer(&state->rotation_rows, strip_bytes, transpose);
    if (e)
        return e;
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
    process_plane(state, src + src_y_offset, dst + dst_y_offset, src_stride, dst_stride, 0,
                  worker_count, 0, 1);
    process_plane(state, src + src_uv_offset, dst + dst_uv_offset, src_stride, dst_stride, 1,
                  worker_count, 0, 1);
    if (worker_count)
        qurt_hvx_unlock();
    *us = HAP_perf_get_time_us() - start_us;
    return 0;
}
