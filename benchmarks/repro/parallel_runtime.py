#!/usr/bin/env python3
"""Compare single-worker and batched/parallel AOT code in one fresh batch.

Worker count affects LLVM's compilation strategy, so it must be treated as part
of artifact identity, not merely a scheduling parameter. Execute guests serially.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import re
import time
from run import Run, WORKLOADS, WT_FLAGS, clean_error, save, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--threads', type=int, nargs='+', default=[1, 18])
    p.add_argument('--samples', type=int, default=5)
    p.add_argument('--coremark-samples', type=int, default=3)
    a = p.parse_args(); r = Run(a.work)
    if a.samples < 1 or a.coremark_samples < 1 or any(t < 1 for t in a.threads):
        p.error('samples and worker counts must be positive')
    target = r.work / 'parallel-runtime.json'
    if target.exists(): raise RuntimeError('Refusing to overwrite ' + str(target))
    for name in ['pzam-compile','pzam-run','wasmtime']:
        if sha(r.work/'tools'/name) != r.config['tools'][name]['sha256']:
            raise RuntimeError('Frozen executable hash mismatch: ' + name)
    for w in WORKLOADS:
        name=w['name']+'.wasm'
        if sha(r.work/'modules'/name) != r.config['modules'][name]['sha256']:
            raise RuntimeError('Frozen module hash mismatch: ' + name)
    save(r.work/'parallel-runtime-config.json',{
        'date_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
        'threads':a.threads,'compute_samples':a.samples,'excluded_warmups':1,
        'coremark_performance_samples':a.coremark_samples,'coremark_validation_samples':a.coremark_samples,
        'tools':{name:r.config['tools'][name] for name in ['pzam-compile','pzam-run','wasmtime']},
        'harness_sha256':sha(Path(__file__)),
        'fp_policy':'canonical NaNs; LLVM hardware-deterministic / Cranelift nan-canonicalization=y',
        'method':'Serial processes; fixed-seed randomized orders; same frozen Wasm and tools.',
        'host':'Active desktop; no fixed affinity or frequency; no competing benchmark/build intentionally scheduled.'})
    reference = json.loads((r.work / 'validation.json').read_text())
    result = {'compilation': [], 'validation': [], 'compute': [], 'coremark': [], 'calibration': []}
    profiles = [(mode, n) for n in a.threads for mode in ['llvm-o2-pzam', 'cranelift-canonical-aot']]
    def flags(n):
        return [x.replace('parallel-compilation=n', 'parallel-compilation=y' if n > 1 else 'parallel-compilation=n') for x in WT_FLAGS] + ['-W','nan-canonicalization=y']
    def artifact(w, mode, n):
        return r.work / 'modules' / (w['name'] + f'.parallel-{mode}-j{n}' + ('.pzam' if mode == 'llvm-o2-pzam' else '.cwasm'))
    def command(w, mode, n, args=None):
        args = w['args'] if args is None else args
        if mode == 'llvm-o2-pzam': return [r.work / 'tools/pzam-run', artifact(w, mode, n), *args]
        return [r.work / 'tools/wasmtime', 'run', *flags(n), '--allow-precompiled', artifact(w, mode, n), *args]
    def record(key, rec, **values):
        rec.update(values); result[key].append(rec); save(target, result)
    # Complete CLI creation cost: three samples including first-use costs.
    tasks = [(w,m,n,s) for s in range(3) for w in WORKLOADS for m,n in profiles]
    random.Random(20260918).shuffle(tasks)
    for w, mode, n, sample in tasks:
        r.env['RAYON_NUM_THREADS'] = str(n); dest = artifact(w, mode, n)
        src = r.work / 'modules' / (w['name'] + '.wasm')
        if mode == 'llvm-o2-pzam':
            cmd = [r.work/'tools/pzam-compile', '--target=' + ('aarch64' if platform.machine() in ['arm64','aarch64'] else 'x86_64'),
                   '--backend=llvm', '--opt-level=2', '-j'+str(n), src, '-o', dest]
        else: cmd = [r.work/'tools/wasmtime', 'compile', *flags(n), src, '-o', dest]
        rec, _, _ = r.command(f'parallel-small-compile-{w["name"]}-{mode}-j{n}-{sample}', cmd)
        record('compilation', rec, workload=w['name'], mode=mode, compile_threads=n, sample=sample,
               phase='parallel-small-compile', valid=rec['returncode']==0 and dest.exists(),
               artifact_sha256=sha(dest) if dest.exists() else None,
               artifact_bytes=dest.stat().st_size if dest.exists() else None,
               environment_overrides={'RAYON_NUM_THREADS': str(n), 'OMP_NUM_THREADS':'1'})
    failed = {(r['workload'],r['mode'],r['compile_threads']) for r in result['compilation'] if not r['valid']}
    kernels = [w for w in WORKLOADS if w['kind'] != 'coremark']
    for w in kernels:
        for mode, n in profiles:
            r.env['RAYON_NUM_THREADS'] = str(n)
            args = ['--validate'] if w['kind']=='polybench' else w['args']
            rec, out, err = r.command(f'parallel-small-validate-{w["name"]}-{mode}-j{n}', command(w,mode,n,args))
            value = clean_error(err) if w['kind']=='polybench' else out
            digest = hashlib.sha256(value.encode()).hexdigest()
            valid = rec['returncode']==0 and bool(value.strip()) and digest==reference[w['name']+'/native']['reference_sha256']
            record('validation',rec,workload=w['name'],mode=mode,compile_threads=n,phase='parallel-small-validation',valid=valid,result_sha256=digest)
            if not valid: failed.add((w['name'],mode,n))
    tasks = [(w,m,n,s) for s in range(-1,a.samples) for w in kernels for m,n in profiles if (w['name'],m,n) not in failed]
    random.Random(20260919).shuffle(tasks);tasks.sort(key=lambda x:x[3]>=0)
    for w, mode, n, sample in tasks:
        r.env['RAYON_NUM_THREADS'] = str(n)
        rec,out,err=r.command(f'parallel-small-run-{w["name"]}-{mode}-j{n}-{sample}',command(w,mode,n))
        valid=rec['returncode']==0 and 'guest_s' in rec
        if w['kind']=='polybench':
            try:duration=float(out.strip())
            except ValueError:duration=0
            valid=valid and duration>0
        else:
            duration=rec.get('guest_s',0)
            valid=valid and hashlib.sha256(out.encode()).hexdigest()==reference[w['name']+'/native']['reference_sha256']
        record('compute',rec,workload=w['name'],mode=mode,compile_threads=n,sample=sample,warmup=sample<0,phase='parallel-small-run',
               execution_s=duration,outside_main_s=rec['wall_s']-rec.get('guest_s',0),valid=valid)
    w=next(w for w in WORKLOADS if w['kind']=='coremark');counts={}
    seeds=[('performance',['0','0','0x66']),('validation',['0x3415','0x3415','0x66'])]
    for mode,n in profiles:
        r.env['RAYON_NUM_THREADS']=str(n);durations=[]
        for seed,params in seeds:
            rec,out,err=r.command(f'parallel-coremark-calibrate-{mode}-j{n}-{seed}',command(w,mode,n,[*params,'100000']))
            duration=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out)
            # Short calibration intentionally prints the upstream <10-second
            # error. It is not an accepted score; timed trials below must pass
            # the full CRC and minimum-duration requirements independently.
            valid=rec['returncode']==0 and bool(duration) and float(duration[1])>0
            record('calibration',rec,workload='coremark',mode=mode,compile_threads=n,
                   phase='parallel-coremark-calibration',calibration_only=True,valid=valid)
            if valid:durations.append(float(duration[1]))
        if len(durations)==2:counts[(mode,n)]=math.ceil(100000*14/max(min(durations),.001)/1000)*1000
    tasks=[(m,n,s) for s in range(a.coremark_samples) for m,n in counts]
    random.Random(20260920).shuffle(tasks)
    for mode,n,sample in tasks:
        r.env['RAYON_NUM_THREADS']=str(n)
        for seed,params in seeds:
            rec,out,err=r.command(f'parallel-coremark-{seed}-{mode}-j{n}-{sample}',command(w,mode,n,[*params,str(counts[(mode,n)])]))
            duration=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out);score=re.search(r'Iterations/Sec\s*:\s*([0-9.]+)',out)
            valid=rec['returncode']==0 and 'Correct operation validated' in out and bool(duration and score) and float(duration[1])>=10
            record('coremark',rec,workload='coremark',mode=mode,compile_threads=n,phase=seed,sample=sample,iterations=counts[(mode,n)],
                   execution_s=float(duration[1]) if duration else 0,score=float(score[1]) if score else 0,valid=valid)
            print('CoreMark',mode,n,sample,seed,rec['score'],'PASS' if valid else 'FAIL',flush=True)
    total=sum(len(v) for v in result.values())
    failures=sum(not x['valid'] for rows in result.values() for x in rows)
    print('FINISHED',total,'records,',failures,'failures',flush=True)
    if failures:raise SystemExit(1)


if __name__=='__main__':main()
