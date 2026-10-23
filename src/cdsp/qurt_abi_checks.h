// SPDX-License-Identifier: MIT
// Also compiled against the official SDK by tests/vpp/check-abi.py.
#include <stddef.h>
#define ABI_SIZE(t, s, a) _Static_assert(sizeof(t) == (s) && _Alignof(t) == (a), #t " ABI")
#define ABI_OFFSET(t, f, o) _Static_assert(offsetof(t, f) == (o), #t "." #f " ABI")
ABI_SIZE(qurt_thread_attr_t, 36, 4);
ABI_OFFSET(qurt_thread_attr_t, stid, 17);
ABI_OFFSET(qurt_thread_attr_t, priority, 18);
ABI_OFFSET(qurt_thread_attr_t, bus_priority, 21);
ABI_OFFSET(qurt_thread_attr_t, timetest_id, 22);
ABI_OFFSET(qurt_thread_attr_t, stack_size, 24);
ABI_OFFSET(qurt_thread_attr_t, stack_addr, 28);
ABI_OFFSET(qurt_thread_attr_t, detach_state, 32);
ABI_SIZE(qurt_sem_t, 16, 8);
ABI_SIZE(compute_res_attr_t, 160, 8);
ABI_OFFSET(compute_res_attr_t, attributes, 0);
#define ABI_FUNCTION(name, type)                                                                   \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&name), type), #name " ABI")
ABI_FUNCTION(compute_resource_attr_init, int (*)(compute_res_attr_t *));
ABI_FUNCTION(compute_resource_attr_set_vtcm_param,
             int (*)(compute_res_attr_t *, unsigned int, unsigned char));
ABI_FUNCTION(compute_resource_acquire, unsigned int (*)(compute_res_attr_t *, unsigned int));
ABI_FUNCTION(compute_resource_attr_get_vtcm_ptr, void *(*)(compute_res_attr_t *));
ABI_FUNCTION(compute_resource_release, int (*)(unsigned int));
#undef ABI_FUNCTION
ABI_SIZE(HAP_power_request_t, 120, 8);
ABI_SIZE(HAP_power_response_t, 16, 8);
ABI_OFFSET(HAP_power_request_t, apptype, 8);
ABI_OFFSET(HAP_power_request_t, hvx.power_up, 8);
ABI_OFFSET(HAP_power_request_t, dcvs_v3, 8);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.set_dcvs_enable, 8);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.dcvs_enable, 9);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.dcvs_option, 10);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.set_latency, 11);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.latency, 12);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.set_core_params, 16);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.core_params, 20);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.core_params.target_corner, 20);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.core_params.min_corner, 21);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.core_params.max_corner, 22);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.set_bus_params, 36);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.bus_params, 40);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.bus_params.target_corner, 40);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.bus_params.min_corner, 41);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.bus_params.max_corner, 42);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.set_dcvs_v3_params, 56);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.set_sleep_disable, 84);
ABI_OFFSET(HAP_power_request_t, dcvs_v3.sleep_disable, 85);
ABI_OFFSET(HAP_power_response_t, clkFreqHz, 8);
_Static_assert(HAP_power_set_HVX == 2 && HAP_power_set_apptype == 3 &&
                   HAP_power_set_DCVS_v3 == 12 && HAP_power_get_clk_Freq == 4 &&
                   HAP_POWER_COMPUTE_CLIENT_CLASS == 4 && HAP_DCVS_V2_PERFORMANCE_MODE == 16 &&
                   HAP_DCVS_VCORNER_MAX == 255 && HAP_DCVS_LPM_LEVEL1 == 1 &&
                   QURT_HVX_MODE_128B == 1 && QURT_THREAD_ATTR_PRIORITY_DEFAULT == 254,
               "firmware constants");
#undef ABI_SIZE
#undef ABI_OFFSET
