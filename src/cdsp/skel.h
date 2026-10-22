// SPDX-License-Identifier: MIT
#pragma once
#include <remote.h>

int cdsp_vpp_open_skel(remote_handle64 *handle);
int cdsp_vpp_select_workers(int *workers);
