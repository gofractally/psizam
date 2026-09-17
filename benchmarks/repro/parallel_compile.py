#!/usr/bin/env python3
"""Repeated, serially scheduled parallel AOT compilation of frozen clang.wasm.

Use identical worker budgets for LLVM and Cranelift. Verify every generated
artifact by compiling both frozen translation units and checking object hashes.
Keep original single-worker measurements separate from this later batch.
"""
import argparse
import os
from pathlib import Path
import platform
import random
import subprocess
import time
from run import Run, WT_FLAGS, save, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--threads', type=int, nargs='+', default=[4, 18])
    p.add_argument('--samples', type=int, default=3)
    p.add_argument('--timeout', type=int, default=3600)
    a = p.parse_args()
    if a.samples < 1 or any(t < 1 for t in a.threads):
        p.error('samples and worker counts must be positive')
    r = Run(a.work)
    d = r.work / 'clang'
    output = d / 'parallel-compilation.json'
    if output.exists():
        raise RuntimeError('Refusing to overwrite ' + str(output))
    import json
    inputs = json.loads((d / 'inputs.json').read_text())
    oracle = json.loads((d / 'oracles.json').read_text())['wasm']
    if sha(d / 'clang.wasm') != inputs['clang_wasm_sha256']:
        raise RuntimeError('Compiler Wasm hash mismatch')
    for item in inputs['inputs']:
        if sha(d / item['path']) != item['sha256']:
            raise RuntimeError('Input hash mismatch: ' + item['name'])
    tools = {name: sha(r.work / 'tools' / name)
             for name in ['pzam-compile', 'pzam-run', 'wasmtime']}
    for name, digest in tools.items():
        if digest != r.config['tools'][name]['sha256']:
            raise RuntimeError('Frozen executable hash mismatch: ' + name)
    tasks = [(mode, threads, sample) for sample in range(a.samples)
             for threads in a.threads for mode in ['llvm-o2-pzam', 'cranelift-canonical-aot']]
    random.Random(20260917).shuffle(tasks)
    metadata = {'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'tools': tools, 'threads': a.threads, 'samples': a.samples,
                'compiler_wasm_sha256': inputs['clang_wasm_sha256'],
                'seed': 20260917, 'task_order': tasks,
                'method': 'Serial processes; explicit same worker limits; complete CLI wall time; no excluded warmup.',
                'host': 'Active desktop; no CPU affinity or frequency control; original one-worker batch is historical.'}
    save(d / 'parallel-config.json', metadata)
    rows = []
    for mode, threads, sample in tasks:
        label = f'parallel-clang-{mode}-j{threads}-{sample}'
        dest = d / (label + ('.pzam' if mode == 'llvm-o2-pzam' else '.cwasm'))
        flags = [x.replace('parallel-compilation=n', 'parallel-compilation=y' if threads > 1 else 'parallel-compilation=n') for x in WT_FLAGS]
        flags += ['-W', 'nan-canonicalization=y']
        r.env['RAYON_NUM_THREADS'] = str(threads)
        if mode == 'llvm-o2-pzam':
            cmd = [r.work / 'tools/pzam-compile', '--target=' + ('aarch64' if platform.machine() in ['arm64', 'aarch64'] else 'x86_64'),
                   '--backend=llvm', '--opt-level=2', '-j' + str(threads), d / 'clang.wasm', '-o', dest]
        else:
            cmd = [r.work / 'tools/wasmtime', 'compile', *flags, d / 'clang.wasm', '-o', dest]
        host = {'loadavg': os.getloadavg(), 'top_cpu_processes': subprocess.check_output(
                ['ps', '-Ao', 'pid,pcpu,comm'], text=True)}
        print('START', label, flush=True)
        rec, _, _ = r.command(label, cmd, timeout=a.timeout)
        rec.update(mode=mode, workload='clang.wasm', phase='parallel-compile', sample=sample,
                   compile_threads=threads, environment_overrides={k: r.env[k] for k in ['LC_ALL','TZ','RAYON_NUM_THREADS','OMP_NUM_THREADS']},
                   host_before=host, valid=False, validation=[])
        if rec['returncode'] == 0 and dest.exists():
            rec.update(artifact_sha256=sha(dest), artifact_bytes=dest.stat().st_size)
            for item in inputs['inputs']:
                obj = d / 'result.o'
                obj.unlink(missing_ok=True)
                job = ['-cc1', '-triple', 'wasm32-unknown-wasi', '-emit-obj', '-O2', '-std=c++17',
                       '-x', 'c++-cpp-output', d / item['path'], '-o', obj]
                if mode == 'llvm-o2-pzam':
                    command = [r.work / 'tools/pzam-run', '--dir=' + str(d), dest, *job]
                else:
                    command = [r.work / 'tools/wasmtime', 'run', *flags, '--allow-precompiled', '--dir', d,
                               '-W', 'max-wasm-stack=16777216', dest, *job]
                check, _, _ = r.command(label + '-verify-' + item['name'], command, timeout=180)
                digest = sha(obj) if obj.exists() else None
                check.update(workload=item['name'], phase='parallel-artifact-validation', mode=mode,
                             compile_threads=threads, sample=sample, object_sha256=digest,
                             expected_sha256=oracle[item['name']], valid=check['returncode'] == 0 and digest == oracle[item['name']])
                rec['validation'].append(check)
            rec['valid'] = all(x['valid'] for x in rec['validation'])
        rows.append(rec)
        save(output, rows)
        print('DONE', label, round(rec['wall_s'], 3), 'PASS' if rec['valid'] else 'FAIL', flush=True)
        # Artifacts are host-specific and reproducible; retain hashes and logs,
        # avoiding multiple gigabytes of duplicate compiler artifacts.
        if rec['valid'] and mode == 'llvm-o2-pzam' and threads == max(a.threads):
            # Keep one representative artifact for a separately interleaved
            # application-runtime batch after all compilation has finished.
            dest.replace(d / 'clang.llvm-o2-parallel-pzam')
        else:
            dest.unlink(missing_ok=True)
    if not all(x['valid'] for x in rows):
        raise SystemExit('Some compilation or object-identity checks failed; inspect retained evidence.')


if __name__ == '__main__':
    main()
