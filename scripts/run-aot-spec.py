#!/usr/bin/env python3
"""Run scalar/SIMD assertions through the LLVM O2 saved-PZAM test backend.

Build psizam_scalar_aot_o2_tests and psizam_simd_aot_o2_tests with
PSIZAM_ENABLE_AOT_SPEC_TESTS=ON first. The compiler must enable LLVM AOT.
Each suite retains its input Wasm, output PZAM, compiler commands and logs.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-bin', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve()
    if work.exists() and any(work.iterdir()):
        parser.error('--work must be a fresh directory so old artifacts cannot mask failures')
    work.mkdir(parents=True, exist_ok=True)
    compiler = work / 'pzam-compile'
    shutil.copy2(args.compiler.resolve(), compiler)
    manifest = {'backend': 'LLVM AOT', 'optimization': 2, 'pzam_version': 3,
                'compiler_origin': str(args.compiler.resolve()),
                'compiler_sha256': digest(compiler), 'suites': {}, 'complete': False}

    def run(suite):
        binary = args.test_bin.resolve() / f'psizam_{suite}_aot_o2_tests'
        artifacts = work / suite
        artifacts.mkdir()
        env = os.environ.copy()
        env['PSIZAM_AOT_SPEC_COMPILER'] = str(compiler)
        env['PSIZAM_AOT_SPEC_DIR'] = str(artifacts)
        command = [str(binary), '--durations', 'yes']
        record = {'command': command, 'test_binary_sha256': digest(binary)}
        print(f'{suite}: starting LLVM O2 PZAM assertions', flush=True)
        started = time.monotonic()
        with (work / f'{suite}.log').open('w') as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        record.update(returncode=result.returncode, seconds=time.monotonic() - started,
                      wasm_inputs=len(list(artifacts.glob('*.wasm'))),
                      pzam_outputs=len(list(artifacts.glob('*.pzam'))))
        print(f'{suite}: exit {result.returncode}, {record["seconds"]:.1f}s', flush=True)
        return suite, record

    (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    with ThreadPoolExecutor(max_workers=2) as executor:
        manifest['suites'] = dict(executor.map(run, ['scalar', 'simd']))
    manifest['complete'] = all(x['returncode'] == 0 for x in manifest['suites'].values())
    (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return 0 if manifest['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
