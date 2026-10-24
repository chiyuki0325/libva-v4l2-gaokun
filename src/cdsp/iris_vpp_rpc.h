// SPDX-License-Identifier: MIT
#ifndef IRIS_VPP_RPC_H
#define IRIS_VPP_RPC_H

#include "iris_vpp.h"
#include <AEEStdErr.h>
#include <string.h>

// Wire ABI from iris_vpp.idl. Preserve these IDs, including legacy run/process.
// Scalars are little-endian 32-bit words, except the 64-bit usec output.
// Each sequence length is an element count; remote_arg.buf.nLen is in bytes.
// An inrout sequence has separate input/output lengths and buffer slots.
enum iris_vpp_method {
    IRIS_VPP_OPEN = 0,
    IRIS_VPP_CLOSE = 1,
    IRIS_VPP_POWER = 2,
    IRIS_VPP_CONFIGURE = 3,
    IRIS_VPP_RUN = 4,
    IRIS_VPP_INFO = 5,
    IRIS_VPP_PROFILE = 6,
    IRIS_VPP_PROCESS = 7,
    IRIS_VPP_TRANSFORM = 8,
    IRIS_VPP_OVERLAY = 9,
};

_Static_assert(sizeof(int) == 4 && sizeof(unsigned int) == 4, "RPC requires 32-bit int");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "iris_vpp RPC requires a little-endian target"
#endif

static inline int iris_vpp_valid_buffer(const void *ptr, int count) {
    return count >= 0 && (count == 0 || ptr != NULL);
}

#endif
