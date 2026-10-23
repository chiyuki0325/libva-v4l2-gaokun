#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional ABI audit against SDK 6.7; the SDK is not used by the build."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('tools', type=Path, help='Open Access Tools directory')
p.add_argument('sdk', type=Path, help='HexagonSDK6.7 directory')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
cc = a.tools.resolve() / 'bin/hexagon-clang'
sdk = a.sdk.resolve()
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'audit.c'
    source.write_text('''#include <qurt_thread.h>
#include <qurt_sem.h>
#include <qurt_hvx.h>
#include <HAP_power.h>
#include <HAP_compute_res.h>
#include "qurt_abi_checks.h"
_Static_assert(QURT_THREAD_ATTR_TCB_PARTITION_DEFAULT == 0 &&
               QURT_THREAD_ATTR_STID_DEFAULT == 0 && QURT_THREAD_DEFAULT_GROUP_ID == 0 &&
               QURT_THREAD_ATTR_AUTOSTACK_DEFAULT == 0 && QURT_THREAD_ATTR_BUS_PRIO_DEFAULT == 255 &&
               QURT_THREAD_ATTR_TIMETEST_ID_DEFAULT == -2 && QURT_THREAD_ATTR_CREATE_LEGACY == 0,
               "thread initialization defaults");
''')
    for arch in ('v68', 'v69', 'v73', 'v75', 'v79', 'v81'):
        subprocess.run([str(cc), '--no-default-config', '-m' + arch, '-fshort-enums', '-fsyntax-only',
                        '-I' + str(sdk / 'incs'), '-I' + str(sdk / 'incs/stddef'),
                        '-I' + str(sdk / ('rtos/qurt/compute' + arch) / 'include/qurt'),
                        '-I' + str(root / 'src/cdsp'), str(source)], check=True)
        print(f'PASS: {arch} thread, semaphore, power and compute-resource ABI match SDK 6.7', flush=True)
