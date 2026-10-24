#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Invoke the independent Hexagon compiler from a native Linux Meson build."""
from pathlib import Path
import subprocess
import sys

arch = sys.argv[1]
if arch not in ('v68', 'v69', 'v73', 'v75', 'v79', 'v81'):
    raise SystemExit('Unsupported DSP architecture: ' + arch)
tools, rpc_include_dir, source, output = map(Path, sys.argv[2:])
cc = tools / 'bin/hexagon-clang'
resource = subprocess.check_output([str(cc), '--no-default-config', '-print-resource-dir'], text=True).strip()
flags = ['--no-default-config', '--target=hexagon', '-m' + arch, '-mhvx=' + arch, '-mhvx-length=128B',
         '-O3', '-fshort-enums', '-G0', '-fPIC', '-fno-stack-protector', '-Wall', '-Werror', '-nostdinc',
         '-isystem', resource + '/include', '-isystem', str(tools / 'target/hexagon/include'),
         '-I' + str(rpc_include_dir), '-I' + str(source),
         '-DIRIS_VPP_SKEL_NAME="' + output.name + '"']
objects = []
object_dir = output.parent / arch
object_dir.mkdir(exist_ok=True)
for src in [source / 'iris_vpp_skel.c', source / 'dsp.c']:
    obj = object_dir / (src.stem + '.o')
    subprocess.run([str(cc), *flags, '-c', str(src), '-o', str(obj)], check=True)
    objects.append(str(obj))
subprocess.run([str(cc), '--no-default-config', '-m' + arch, '-shared', '-G0', '-Wl,-Bsymbolic',
                '-Wl,-soname,' + output.name,
                *['-Wl,--wrap=' + name for name in ['malloc', 'calloc', 'free', 'realloc', 'memalign']],
                *objects, '-o', str(output)], check=True)
