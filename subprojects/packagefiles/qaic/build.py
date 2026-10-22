#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the pinned open QAIC with system GHC packages, without Cabal downloads."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

ghc, source, output = sys.argv[1:]
output = Path(output).resolve()
work = output.parent / 'qaic-source'
shutil.copytree(Path(source) / 'src', work, dirs_exist_ok=True)
version = work / 'Compiler/Version.hs'
text = version.read_text()
for name, value in {
    'gitcommit': 'a66a0019fb488abae955bbc9b3a9de3dc15962d0',
    'ghcVersion': subprocess.check_output([ghc, '--numeric-version'], text=True).strip(),
    'cabalVersion': 'built directly with GHC',
    'platform': sys.platform,
}.items():
    text += f'\n{name} :: String\n{name} = {json.dumps(value)}\n'
version.write_text(text)

def run(args, **kw):
    subprocess.run(args, cwd=work, check=True, **kw)

converter = work / 'file2hs-tool'
run([ghc, '-dynamic', '-O2', '-outputdir', 'build/file2hs', '-o', str(converter), 'file2hs/file2hs.hs'])
for name in ['Stub', 'Skel', 'CStub', 'Env', 'Allocator', 'Wstring', 'Slim']:
    base = work / 'Language/SO' / name
    with base.with_suffix('.c').open('rb') as src, base.with_suffix('.hs').open('wb') as dst:
        run([str(converter), 'Language.SO.' + name, 'source'], stdin=src, stdout=dst)
run([ghc, '--make', '-dynamic', '-O2', '-threaded', '-outputdir', 'build/qaic', '-o', str(output), 'Qidlc.hs'])
