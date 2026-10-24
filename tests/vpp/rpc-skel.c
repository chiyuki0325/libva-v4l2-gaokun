// SPDX-License-Identifier: MIT
// Link both RPC endpoints on the host, with distinct names for DSP methods.
#define iris_vpp_open impl_open
#define iris_vpp_close impl_close
#define iris_vpp_power impl_power
#define iris_vpp_configure impl_configure
#define iris_vpp_run impl_run
#define iris_vpp_info impl_info
#define iris_vpp_profile impl_profile
#define iris_vpp_process impl_process
#define iris_vpp_transform impl_transform
#define iris_vpp_overlay impl_overlay
#include "../../src/cdsp/iris_vpp_skel.c"
