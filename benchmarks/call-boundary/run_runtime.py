#!/usr/bin/env python3
"""Measure real scalar runtime bindings serially, retaining exact source identity."""
import argparse
import hashlib
import json
import pathlib
import platform
import random
import shutil
import statistics
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=pathlib.Path, required=True)
    p.add_argument('--fixtures', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--samples', type=int, default=5)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    def sha(f): return hashlib.sha256(f.read_bytes()).hexdigest()
    def save(name, value): (a.out/name).write_text(json.dumps(value, indent=2)+'\n')
    binary = a.out.resolve()/'call-runtime'
    shutil.copy2(a.binary, binary)
    fixtures = a.out.resolve()/'fixtures'
    fixtures.mkdir()
    for name in ['provider.wasm', 'consumer.wasm']:
        shutil.copy2(a.fixtures/name, fixtures/name)
    root = pathlib.Path(__file__).resolve().parents[2]
    sources = {str(f.relative_to(root)):sha(f) for d in ['include', 'src']
               for f in (root/d).rglob('*') if f.is_file()}
    save('config.json', dict(host=platform.platform(), samples=a.samples,
        binary_sha256=sha(binary), source_sha256=sources,
        fixtures={f.name:sha(f) for f in fixtures.iterdir()},
        resource_setting='Gas instrumented; each backend has a 10^12 budget.',
        scope='Host proxy -> consumer -> runtime binding -> provider; scalar canonical ABI. '
              'C++ new/new[] counts only. Binding and compilation are outside timing.'))
    schedule = [(sample, mode, binding) for sample in range(-1, a.samples)
                for mode in ['jit1', 'jit2', 'llvm'] for binding in ['typed', 'dynamic']]
    random.Random(20260917).shuffle(schedule)
    schedule.sort(key=lambda r:r[0]>=0)
    rows = []
    for sample, mode, binding in schedule:
        cmd = [str(binary), str(fixtures), mode, binding]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
        label = f'{mode}-{binding}-{sample}'
        (a.out/(label+'.stdout')).write_text(result.stdout)
        (a.out/(label+'.stderr')).write_text(result.stderr)
        if result.returncode: raise RuntimeError(label+': '+result.stderr)
        row = json.loads(result.stdout)
        if not row['valid'] or not row['consumer_gas'] or not row['provider_gas']:
            raise RuntimeError('Invalid output or missing gas instrumentation: '+label)
        row.update(sample=sample, mode=mode, binding=binding, command=cmd)
        rows.append(row)
        save('measurements.json', rows)
        print(label, flush=True)
    summary = {}
    for mode in ['jit1', 'jit2', 'llvm']:
        for binding in ['typed', 'dynamic']:
            cell = [r for r in rows if r['mode']==mode and r['binding']==binding and r['sample']>=0]
            summary[mode+'/'+binding] = {k:statistics.median(r[k] for r in cell)
                for k in ['ns_per_call', 'cpp_allocations', 'consumer_gas', 'provider_gas']}
    save('summary.json', summary)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__': main()
