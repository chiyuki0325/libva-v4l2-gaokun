// SPDX-License-Identifier: MIT
#pragma once
// Private unsigned CDSP ABI subset, checked against SDK 6.7 for
// v68, v69, v73, v75, v79 and v81.
// Firmware provides these functions; no QuRT implementation is linked here.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef unsigned int qurt_thread_t;
typedef struct {
    char name[16];
    uint8_t tcb_partition, stid;
    uint16_t priority;
    uint8_t autostack : 1, group_id : 6, reserved : 1;
    uint8_t bus_priority;
    uint16_t timetest_id;
    uint32_t stack_size;
    void *stack_addr;
    uint16_t detach_state;
} qurt_thread_attr_t;
typedef union {
    uint64_t opaque[2];
} qurt_sem_t;
#define QURT_THREAD_ATTR_PRIORITY_DEFAULT 254
#define QURT_EOK 0
#define QURT_HVX_MODE_128B 1
int qurt_thread_create(qurt_thread_t *, qurt_thread_attr_t *, void (*)(void *), void *);
void qurt_thread_exit(int);
int qurt_thread_join(qurt_thread_t, int *);
void qurt_sem_init_val(qurt_sem_t *, unsigned short);
void qurt_sem_destroy(qurt_sem_t *);
int qurt_sem_down(qurt_sem_t *);
int qurt_sem_add(qurt_sem_t *, unsigned int);
int qurt_hvx_lock(int);
int qurt_hvx_unlock(void);
int qurt_hvx_get_units(void);
static inline int qurt_sem_up(qurt_sem_t *s) {
    return qurt_sem_add(s, 1);
}
static inline void qurt_thread_attr_init(qurt_thread_attr_t *a) {
    memset(a, 0, sizeof(*a));
    a->priority = QURT_THREAD_ATTR_PRIORITY_DEFAULT;
    a->bus_priority = 255;
    a->timetest_id = (uint16_t)-2;
}
static inline void qurt_thread_attr_set_stack_addr(qurt_thread_attr_t *a, void *s) {
    a->stack_addr = s;
}
static inline void qurt_thread_attr_set_stack_size(qurt_thread_attr_t *a, unsigned s) {
    a->stack_size = s;
}
static inline void qurt_thread_attr_set_priority(qurt_thread_attr_t *a, unsigned short p) {
    a->priority = p;
}
static inline void qurt_thread_attr_set_name(qurt_thread_attr_t *a, const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof(a->name))
        n = sizeof(a->name) - 1;
    memcpy(a->name, s, n);
    a->name[n] = 0;
}
static inline void qurt_mem_syncht(void) {
    __asm__ volatile("syncht" ::: "memory");
}
static inline uint64_t HAP_perf_get_time_us(void) {
    uint64_t ticks;
    __asm__ volatile("%0 = c31:30" : "=r"(ticks));
    return ticks * 10 / 192; // 19.2 MHz architectural timer.
}

#define HAP_power_set_HVX 2
#define HAP_power_set_apptype 3
#define HAP_power_set_DCVS_v3 12
#define HAP_power_get_clk_Freq 4
#define HAP_POWER_COMPUTE_CLIENT_CLASS 4
#define HAP_DCVS_V2_PERFORMANCE_MODE 0x10
#define HAP_DCVS_VCORNER_MAX 255
#define HAP_DCVS_LPM_LEVEL1 1
#ifndef TRUE
#define TRUE 1
#endif
typedef struct {
    uint8_t target_corner, min_corner, max_corner;
    uint32_t reserved[3];
} IrisPowerCorners;
typedef struct {
    uint8_t set_dcvs_enable, dcvs_enable;
    uint8_t dcvs_option;
    uint8_t set_latency;
    uint32_t latency;
    uint8_t set_core_params;
    IrisPowerCorners core_params;
    uint8_t set_bus_params;
    IrisPowerCorners bus_params;
    uint8_t set_dcvs_v3_params;
    uint32_t reserved[6];
    uint8_t set_sleep_disable, sleep_disable;
} IrisPowerDcvs;
typedef struct {
    uint8_t type;
    union {
        uint64_t reserved[14];
        uint8_t apptype;
        struct {
            uint8_t power_up;
        } hvx;
        IrisPowerDcvs dcvs_v3;
    };
} HAP_power_request_t;
typedef struct {
    uint8_t type;
    union {
        uint64_t reserved;
        uint32_t clkFreqHz;
    };
} HAP_power_response_t;
int HAP_power_set(void *, HAP_power_request_t *);
int HAP_power_get(void *, HAP_power_response_t *);
int HAP_power_destroy_client(void *);

// Optional compute-resource ABI behind the SDK's HAP_compute_res wrappers.
// Weak imports let older firmware load the skeleton without VTCM support.
typedef struct {
    unsigned long long attributes[20];
} compute_res_attr_t;
int compute_resource_attr_init(compute_res_attr_t *) __attribute__((weak));
int compute_resource_attr_set_vtcm_param(compute_res_attr_t *, unsigned int, unsigned char)
    __attribute__((weak));
unsigned int compute_resource_acquire(compute_res_attr_t *, unsigned int) __attribute__((weak));
void *compute_resource_attr_get_vtcm_ptr(compute_res_attr_t *) __attribute__((weak));
int compute_resource_release(unsigned int) __attribute__((weak));

#include "qurt_abi_checks.h"
