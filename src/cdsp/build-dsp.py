#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Invoke the independent Hexagon compiler from a native Linux Meson build."""
from pathlib import Path
import re
import subprocess
import sys

arch = sys.argv[1]
if arch not in ('v68', 'v69', 'v73', 'v75', 'v79', 'v81'):
    raise SystemExit('Unsupported DSP architecture: ' + arch)
tools, rpc_include_dir, generated, source, output = map(Path, sys.argv[2:])
cc = tools / 'bin/hexagon-clang'
resource = subprocess.check_output([str(cc), '--no-default-config', '-print-resource-dir'], text=True).strip()
flags = ['--no-default-config', '--target=hexagon', '-m' + arch, '-mhvx=' + arch, '-mhvx-length=128B',
         '-O3', '-fshort-enums', '-G0', '-fPIC', '-fno-stack-protector', '-Wall', '-Werror', '-nostdinc',
         '-isystem', resource + '/include', '-isystem', str(tools / 'target/hexagon/include'),
         '-I' + str(rpc_include_dir), '-I' + str(generated), '-I' + str(source)]
objects = []
object_dir = output.parent / arch
object_dir.mkdir(exist_ok=True)
# QAIC embeds its default module filename in an exported URI. Keep that
# metadata consistent with this variant, including the string's array size.
skel = object_dir / 'iris_vpp_skel.c'
skel_text, count = re.subn(r'(char iris_vpp_skel_handle_invoke_uri)\[[^\]]*\]',
                         r'\1[]', (generated / skel.name).read_text())
if count != 1 or skel_text.count('libiris_vpp_skel.so') != 1:
    raise SystemExit('Unexpected QAIC skeleton URI declaration')
skel.write_text(skel_text.replace('libiris_vpp_skel.so', output.name))
for src in [skel, source / 'dsp.c']:
    obj = object_dir / (src.stem + '.o')
    subprocess.run([str(cc), *flags, '-c', str(src), '-o', str(obj)], check=True)
    objects.append(str(obj))
subprocess.run([str(cc), '--no-default-config', '-m' + arch, '-shared', '-G0', '-Wl,-Bsymbolic',
                '-Wl,-soname,' + output.name,
                *['-Wl,--wrap=' + name for name in ['malloc', 'calloc', 'free', 'realloc', 'memalign']],
                *objects, '-o', str(output)], check=True)
