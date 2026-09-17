#!/usr/bin/env python3
"""Current-source Clang artifact creation and C++ jobs under all CPU policies.

Use the pinned standard-validation fixtures, identical guest paths, and exact
object hashes. Run stages serially, outside every other timing experiment.
"""
import argparse
import json
from pathlib import Path
import platform
import random
import shutil
from run import Run, HERE, save, sha
from resources import PROFILES, wt_flags

MODES = ['jit1', 'jit2', 'jit2-pzam', 'llvm-o2-pzam', 'cranelift-jit', 'cranelift-aot']
BUDGET = 1_000_000_000_000
TIMEOUT_MS = 120_000


def artifact(r, mode, workers, profile):
    # Page interruption does not change psizam's emitted code. Reuse exactly
    # the unrestricted artifact, and arm its deadline at execution time.
    if mode != 'cranelift-aot' and profile == 'timeout': profile = 'none'
    return r.work/'modules'/f'clang.{mode}.j{workers}.{profile}'


def compilation(r, mode, workers, profile):
    src = r.work/'modules/clang.wasm'; dest = artifact(r, mode, workers, profile)
    if mode == 'cranelift-aot':
        return [r.work/'tools/wasmtime', 'compile', *wt_flags(workers, profile, BUDGET, TIMEOUT_MS), src, '-o', dest]
    return [r.work/'tools/pzam-compile', '--target='+('aarch64' if platform.machine() in ['arm64','aarch64'] else 'x86_64'),
            '--backend='+('jit2' if mode == 'jit2-pzam' else 'llvm'), '--opt-level=2', f'-j{workers}',
            '--gas-metering='+('on' if profile == 'gas' else 'off'), src, '-o', dest]


def invocation(r, mode, workers, profile, name):
    inputs = r.work/'clang-inputs'; output = r.work/'clang-output'
    flags = ['-cc1','-triple','wasm32-unknown-wasi','-emit-obj','-O2','-std=c++17',
             '-x','c++-cpp-output','/fixtures/'+name+'.ii','-o','/output/result.o']
    if mode.startswith('cranelift'):
        return [r.work/'tools/wasmtime','run',*wt_flags(workers,profile,BUDGET,TIMEOUT_MS),
                '--dir',str(inputs)+'::/fixtures','--dir',str(output)+'::/output',
                '-W','max-wasm-stack=16777216',
                *(['--allow-precompiled',artifact(r,mode,workers,profile)] if mode=='cranelift-aot'
                  else [r.work/'modules/clang.wasm']),*flags]
    dirs = ['--dir=/fixtures:'+str(inputs),'--dir=/output:'+str(output)]
    limits = [f'--cpu-limit={profile}',f'--gas-budget={BUDGET}',f'--timeout-ms={TIMEOUT_MS}']
    if mode in ['jit1','jit2']:
        return [r.work/'tools/repro-psizam','jit' if mode=='jit1' else 'jit2',*limits,*dirs,r.work/'modules/clang.wasm',*flags]
    return [r.work/'tools/pzam-run',*limits,*dirs,artifact(r,mode,workers,profile),*flags]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage',choices=['prepare','compile','measure'])
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--fixtures',type=Path)
    p.add_argument('--threads',default='4,18')
    p.add_argument('--compile-samples',type=int,default=3)
    p.add_argument('--samples',type=int,default=5)
    p.add_argument('--compile-watchdog',type=int,default=3660)
    a=p.parse_args();r=Run(a.work)
    if min(a.samples,a.compile_samples,a.compile_watchdog)<=0:p.error('positive counts/timeouts required')
    config_path=r.work/'clang-resource-config.json'
    if a.stage=='prepare':
        if config_path.exists():p.error('Clang experiment already prepared')
        if not a.fixtures:p.error('prepare requires --fixtures')
        manifest=json.loads((HERE.parents[1]/'tests/validation/compiler-fixtures.json').read_text())
        inputs=r.work/'clang-inputs';inputs.mkdir()
        (r.work/'clang-output').mkdir()
        for name,digest in manifest['files'].items():
            source=a.fixtures/name
            if sha(source)!=digest:raise ValueError('Fixture hash mismatch: '+name)
            shutil.copy2(source,r.work/'modules'/name if name.endswith('.wasm') else inputs/name)
        workers=[int(x) for x in a.threads.split(',')]
        if not workers or min(workers)<1:p.error('positive worker counts required')
        save(config_path,{'fixtures':manifest,'threads':workers,'runtime_workers':max(workers),
             'samples':a.samples,'compile_samples':a.compile_samples,'warmups':1,
             'gas_budget':BUDGET,'timeout_ms':TIMEOUT_MS,'compile_watchdog':a.compile_watchdog,
             'modes':MODES,'profiles':PROFILES,'script_sha256':sha(Path(__file__)),
             'tools':r.config['tools'],'source_manifest_sha256':sha(r.work/'source-manifest.json'),
             'runtime_scope':'Fresh JIT1/JIT2/Cranelift and saved JIT2/LLVM O2/Cranelift. Whole-module LLVM ORC Clang startup is outside this experiment.'})
        print('Pinned compiler inputs prepared',flush=True);return
    config=json.loads(config_path.read_text())
    for name,record in config['tools'].items():
        if sha(r.work/'tools'/name)!=record['sha256']:raise ValueError('Frozen tool changed: '+name)
    if a.stage=='compile':
        target=r.work/'clang-resource-compilation.json'
        if target.exists():p.error('Refusing to overwrite compilation trials')
        rows=[]
        tasks=[(m,t,policy,i) for m in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']
               for t in ([1] if m=='jit2-pzam' else config['threads'])
               for policy in (PROFILES if m=='cranelift-aot' else ['none','gas'])
               for i in range(config['compile_samples'])]
        random.Random(20260917).shuffle(tasks)
        for mode,workers,profile,sample in tasks:
            r.env['RAYON_NUM_THREADS']=str(workers)
            dest=artifact(r,mode,workers,profile);dest.unlink(missing_ok=True)
            rec,_,_=r.command(f'clang-artifact-{mode}-j{workers}-{profile}-{sample}',
                             compilation(r,mode,workers,profile),timeout=config['compile_watchdog'])
            valid=rec['returncode']==0 and dest.is_file() and dest.stat().st_size>0
            rec.update(mode=mode,threads=workers,profile=profile,sample=sample,valid=valid,
                       artifact_sha256=sha(dest) if valid else None,artifact_bytes=dest.stat().st_size if valid else 0)
            rows.append(rec);save(target,rows)
            print('clang artifact',mode,workers,profile,sample,round(rec['wall_s'],3),'PASS' if valid else 'FAIL',flush=True)
            if not valid:raise RuntimeError('Compiler artifact failure; retained diagnostic, no rankings')
        return
    target=r.work/'clang-resource-measurements.json'
    if target.exists():p.error('Refusing to overwrite compiler job trials')
    comp=json.loads((r.work/'clang-resource-compilation.json').read_text())
    rows=[];accepted=[]
    for mode in MODES:
        workers=config['runtime_workers'] if mode in ['llvm-o2-pzam','cranelift-aot'] else 1
        for profile in PROFILES:
            if mode.endswith('pzam') or mode=='cranelift-aot':
                native_profile='none' if profile=='timeout' and mode!='cranelift-aot' else profile
                trials=[x for x in comp if (x['mode'],x['threads'],x['profile'])==(mode,workers,native_profile)]
                if len(trials)!=config['compile_samples'] or not all(x['valid'] for x in trials):
                    raise ValueError('Incomplete artifact compilation')
                if sha(artifact(r,mode,workers,profile)) not in {x['artifact_sha256'] for x in trials}:
                    raise ValueError('Artifact identity mismatch')
            for name in config['fixtures']['objects']:accepted.append((mode,workers,profile,name))
    tasks=[(*cell,sample) for sample in range(-1,config['samples']) for cell in accepted]
    random.Random(20260917).shuffle(tasks);tasks.sort(key=lambda x:x[-1]>=0)
    for mode,workers,profile,name,sample in tasks:
        output=r.work/'clang-output/result.o';output.unlink(missing_ok=True)
        r.env['RAYON_NUM_THREADS']=str(workers)
        rec,_,_=r.command(f'clang-job-{name}-{mode}-j{workers}-{profile}-{sample}',
                         invocation(r,mode,workers,profile,name),timeout=240)
        digest=sha(output) if output.exists() else None
        valid=rec['returncode']==0 and digest==config['fixtures']['objects'][name]
        rec.update(mode=mode,threads=workers,profile=profile,workload=name,sample=sample,
                   warmup=sample<0,valid=valid,object_sha256=digest,
                   expected_sha256=config['fixtures']['objects'][name])
        rows.append(rec);save(target,rows)
        print('clang job',name,mode,profile,sample,round(rec['wall_s'],4),'PASS' if valid else 'FAIL',flush=True)
        if not valid:raise RuntimeError('Clang object identity failure; no ranking')


if __name__=='__main__':main()
