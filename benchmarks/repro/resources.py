#!/usr/bin/env python3
"""Current-source CPU-limit comparison. Separate artifacts for each instrumentation policy.

prepare uses run.py prepare; this script runs gates and measurements in that NEW
work directory. A successful matrix is required before report.py accepts it.
"""
import argparse
import hashlib
import json
import platform
import random
import re
import shutil
import statistics
import subprocess
from pathlib import Path
from run import Run, WORKLOADS, WT_FLAGS, save, sha, clean_error

PROFILES = ['none', 'gas', 'timeout']
MODES = ['jit1', 'jit2', 'llvm-jit', 'jit2-pzam', 'llvm-o2-pzam', 'cranelift-jit', 'cranelift-aot']


def variants(modes, threads):
    return [(m, t, p) for m in modes for t in (threads if m in ['llvm-o2-pzam', 'cranelift-aot'] else [1]) for p in PROFILES]


def key(mode, threads, profile): return f'{mode}/j{threads}/{profile}'


def wt_flags(threads, profile, budget, timeout_ms):
    flags = ['-C', 'compiler=cranelift,cache=n,parallel-compilation='+('y' if threads > 1 else 'n'),
             '-O', WT_FLAGS[-1], '-W', 'nan-canonicalization=y']
    if profile == 'gas': flags += ['-W', f'fuel={budget}']
    if profile == 'timeout': flags += ['-W', f'epoch-interruption=y,timeout={timeout_ms}ms']
    return flags


def artifact(r, name, mode, threads, profile):
    return r.work/'modules'/f'{name}.{mode}.j{threads}.{profile}'


def compile_command(r, name, mode, threads, profile, budget, timeout_ms):
    wasm = r.work/'modules'/(name+'.wasm')
    dest = artifact(r, name, mode, threads, profile)
    if mode == 'cranelift-aot':
        return [r.work/'tools/wasmtime', 'compile', *wt_flags(threads, profile, budget, timeout_ms), wasm, '-o', dest]
    return [r.work/'tools/pzam-compile', '--target='+('aarch64' if platform.machine() in ['arm64', 'aarch64'] else 'x86_64'),
            '--backend='+('jit2' if mode == 'jit2-pzam' else 'llvm'), '--opt-level=2', f'-j{threads}',
            '--gas-metering='+('on' if profile == 'gas' else 'off'), wasm, '-o', dest]


def invocation(r, name, mode, threads, profile, args, budget, timeout_ms):
    tools = r.work/'tools'; wasm = r.work/'modules'/(name+'.wasm')
    opts = [f'--cpu-limit={profile}', f'--gas-budget={budget}', f'--timeout-ms={timeout_ms}']
    if mode in ['jit1', 'jit2', 'llvm-jit']:
        return [tools/'repro-psizam', {'jit1':'jit', 'jit2':'jit2', 'llvm-jit':'llvm'}[mode], *opts, wasm, *args]
    if mode.endswith('pzam'):
        return [tools/'pzam-run', *opts, artifact(r, name, mode, threads, profile), *args]
    return [tools/'wasmtime', 'run', *wt_flags(threads, profile, budget, timeout_ms),
            *(['--allow-precompiled', artifact(r, name, mode, threads, profile)] if mode == 'cranelift-aot' else [wasm]), *args]


def acceptable_limit(rec, stderr, mode, profile):
    if rec['timeout']: return False  # Outer watchdog is never an engine success.
    if mode.startswith('cranelift'):
        expected = 'all fuel consumed' if profile == 'gas' else 'interrupt'
        # Wasmtime 48 CLI deliberately exits 128+SIGABRT for a Wasm trap.
        return rec['returncode'] == 134 and 'wasm trap: '+expected in stderr and 'Command terminated' not in stderr
    return rec['returncode'] == (75 if profile == 'gas' else 76) and 'RESOURCE_LIMIT='+profile in stderr


def freeze_source(r):
    manifests={}
    for repo in ['psizam','psio']:
        root=Path(r.config[repo]['path'])
        if (root/'.git').exists():
            files=subprocess.check_output(['git','-C',root,'ls-files','--cached','--others','--exclude-standard','-z']).decode().split('\0')
            # Gitlinks are directories, not files. Retain their actual source
            # contents so an extracted kit can rebuild the specification tests.
            pending=[Path('.')]
            while pending:
                prefix=pending.pop()
                entries=subprocess.check_output(['git','-C',root/prefix,'ls-files','--stage','-z']).decode().split('\0')
                for entry in entries:
                    if not entry.startswith('160000 '):continue
                    child=prefix/entry.split('\t',1)[1]
                    if not (root/child/'.git').exists():
                        raise RuntimeError('Initialize source submodule before freezing: '+str(root/child))
                    nested=subprocess.check_output(['git','-C',root/child,'ls-files','--cached','--others','--exclude-standard','-z']).decode().split('\0')
                    files.extend(str(child/name) for name in nested if name)
                    pending.append(child)
        else:
            files=[str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()]
        dest=r.work/'source'/repo
        manifests[repo]={}
        for name in sorted(set(files)):
            source=root/name
            # This descriptor is generated below. Hashing its incoming copy and
            # then replacing it would invalidate re-frozen archive manifests.
            if not name or name=='BENCHMARK_SOURCE_REVISION.json' or not source.is_file() or '__pycache__' in source.parts:continue
            target=dest/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
            manifests[repo][name]=sha(target)
        save(dest/'BENCHMARK_SOURCE_REVISION.json',{'commit':r.config[repo]['commit'],
             'source':'complete measured working tree, including local changes'})
    save(r.work/'source-manifest.json',manifests)
    return sha(r.work/'source-manifest.json')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', required=True, type=Path)
    p.add_argument('--only', default=','.join(w['name'] for w in WORKLOADS if w['kind'] != 'coremark'))
    p.add_argument('--modes', default=','.join(MODES)); p.add_argument('--threads', default='1,18')
    p.add_argument('--samples', default=5, type=int); p.add_argument('--compile-samples', default=3, type=int)
    p.add_argument('--gas-budget', default=1_000_000_000_000, type=int)
    p.add_argument('--timeout-ms', default=120_000, type=int)
    p.add_argument('--watchdog', default=240, type=int); p.add_argument('--compile-watchdog', default=3600, type=int)
    p.add_argument('--wat2wasm', default='wat2wasm'); p.add_argument('--seed', type=int, default=20260916)
    a = p.parse_args(); r = Run(a.work)
    if (r.work/'resources.json').exists(): p.error('Use a new work directory; refusing to mix measurement states')
    names = a.only.split(','); modes = a.modes.split(','); threads = list(map(int, a.threads.split(',')))
    if set(modes)-set(MODES) or set(names)-{w['name'] for w in WORKLOADS}: p.error('unknown workload or mode')
    if min(a.samples, a.compile_samples, *threads, a.gas_budget, a.timeout_ms) <= 0: p.error('budgets and sample counts must be positive')
    if a.watchdog <= a.timeout_ms/1000: p.error('outer watchdog must exceed the engine deadline')
    matrix = variants(modes, threads)
    result = {'schema':1, 'config':r.config, 'source_manifest_sha256':freeze_source(r),
              'profiles':PROFILES, 'matrix':[key(*x) for x in matrix], 'workloads':names,
              'samples':a.samples, 'compile_samples':a.compile_samples, 'gas_budget':a.gas_budget,
              'timeout_ms':a.timeout_ms, 'outer_watchdog_s':a.watchdog, 'compile_watchdog_s':a.compile_watchdog,
              'seed':a.seed, 'enforcement':[], 'compilation':[], 'validation':[], 'measurements':[], 'complete':False}
    def persist(): save(r.work/'resources.json', result)
    persist()
    # Materialize finite and nonterminating start/function fixtures.
    for name, wat in {
        'limit-loop':'(module (func (export "_start") (loop $L br $L)))',
        'limit-start':'(module (func $f (loop $L br $L)) (start $f) (func (export "_start")))',
        'limit-finite':'(module (func (export "_start") (local $i i32) (loop $L local.get $i i32.const 1 i32.add local.tee $i i32.const 100 i32.lt_u br_if $L)))',
    }.items():
        source=r.work/'modules'/(name+'.wat');source.write_text(wat)
        subprocess.run([a.wat2wasm,str(source),'-o',str(source.with_suffix('.wasm'))],check=True)
    for mode, t, profile in matrix:
        r.env['RAYON_NUM_THREADS']=str(t)
        for name in ['limit-finite', *(['limit-loop','limit-start'] if profile != 'none' else [])]:
            if mode.endswith('pzam') or mode == 'cranelift-aot':
                r.command('gate-compile-'+name+'-'+key(mode,t,profile), compile_command(r,name,mode,t,profile,a.gas_budget,a.timeout_ms),check=True)
            finite = name == 'limit-finite'
            rec, out, err = r.command('gate-'+name+'-'+key(mode,t,profile),
                invocation(r,name,mode,t,profile,[],1_000_000 if finite else 10,1000 if finite else 20),timeout=15)
            good = rec['returncode']==0 if finite else acceptable_limit(rec,err,mode,profile)
            if finite and mode in ['jit1','jit2','llvm-jit']:
                counter = re.search(r'BENCH_GAS_CONSUMED=(\d+)',err)
                good = good and bool(counter) and (int(counter[1]) == 0 if profile != 'gas' else int(counter[1]) > 0)
            rec.update(profile=profile, mode=mode, threads=t, fixture=name, valid=good)
            result['enforcement'].append(rec);persist()
            print('gate',name,key(mode,t,profile),'PASS' if good else 'FAIL',flush=True)
            if not good: raise RuntimeError('Resource enforcement gate failed; timing is prohibited')
    # Compilation finishes before timing begins; all requested trials retained.
    for name in names:
        for mode,t,profile in matrix:
            if not (mode.endswith('pzam') or mode == 'cranelift-aot'): continue
            r.env['RAYON_NUM_THREADS']=str(t)
            for sample in range(a.compile_samples):
                rec,_,_ = r.command(f'artifact-{name}-{key(mode,t,profile)}-{sample}',
                    compile_command(r,name,mode,t,profile,a.gas_budget,a.timeout_ms),timeout=a.compile_watchdog,check=True)
                dest=artifact(r,name,mode,t,profile)
                rec.update(workload=name,mode=mode,threads=t,profile=profile,sample=sample,artifact_sha256=sha(dest),artifact_bytes=dest.stat().st_size)
                result['compilation'].append(rec);persist()
    workloads=[w for w in WORKLOADS if w['name'] in names]; references={}; iterations={}
    # Native output oracle is independent of every engine/policy.
    for w in workloads:
        name=w['name']
        if w['kind']=='coremark': continue
        args=['--validate'] if w['kind']=='polybench' else w['args']
        rec,out,err=r.command('reference-'+name,r.invocation(w,'native',args),timeout=a.watchdog,check=True)
        content=clean_error(err) if w['kind']=='polybench' else out
        if not content.strip(): raise RuntimeError('Empty native oracle')
        references[name]=hashlib.sha256(content.encode()).hexdigest()
        for mode,t,profile in matrix:
            r.env['RAYON_NUM_THREADS']=str(t)
            rec,out,err=r.command('validate-'+name+'-'+key(mode,t,profile),invocation(r,name,mode,t,profile,args,a.gas_budget,a.timeout_ms),timeout=a.watchdog)
            content=clean_error(err) if w['kind']=='polybench' else out
            valid=rec['returncode']==0 and hashlib.sha256(content.encode()).hexdigest()==references[name]
            rec.update(workload=name,mode=mode,threads=t,profile=profile,valid=valid)
            result['validation'].append(rec);persist()
            print('validate',name,key(mode,t,profile),'PASS' if valid else 'FAIL',flush=True)
            if not valid: raise RuntimeError('Output validation failed; timing is prohibited')
    if 'coremark' in names:
        # Calibrate from uninstrumented execution, use identical counts for its
        # limited counterparts and both required seed sets (>=10s each).
        for mode,t,profile in matrix:
            if profile!='none':continue
            r.env['RAYON_NUM_THREADS']=str(t);durations=[]
            for seed,args in [('performance',['0','0','0x66']),('validation',['0x3415','0x3415','0x66'])]:
                rec,out,err=r.command(f'calibrate-{mode}-j{t}-{seed}',invocation(r,'coremark',mode,t,'none',args+['100000'],a.gas_budget,a.timeout_ms),timeout=a.watchdog,check=True)
                duration=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out)
                if not duration: raise RuntimeError('CoreMark calibration missing timer')
                durations.append(float(duration[1]))
            import math
            iterations[(mode,t)]=math.ceil(100000*14/min(durations)/1000)*1000
    tasks=[(w,mode,t,profile,sample) for sample in range(-1,a.samples) for w in workloads for mode,t,profile in matrix]
    random.Random(a.seed).shuffle(tasks);tasks.sort(key=lambda x:x[-1]>=0)
    for w,mode,t,profile,sample in tasks:
        name=w['name'];r.env['RAYON_NUM_THREADS']=str(t)
        seeds=[('performance',w['args'])] if w['kind']!='coremark' else [
            ('performance',['0','0','0x66',str(iterations[(mode,t)])]),
            ('validation',['0x3415','0x3415','0x66',str(iterations[(mode,t)])])]
        for seed,args in seeds:
            rec,out,err=r.command(f'measure-{name}-{key(mode,t,profile)}-{sample}-{seed}',
                invocation(r,name,mode,t,profile,args,a.gas_budget,a.timeout_ms),timeout=a.watchdog)
            valid=rec['returncode']==0 and 'guest_s' in rec
            execution=rec.get('guest_s',0)
            if w['kind']=='polybench':
                try: execution=float(out.strip())
                except ValueError: execution=0
                valid=valid and execution>0
            elif w['kind']=='coremark':
                duration=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out)
                score=re.search(r'Iterations/Sec\s*:\s*([0-9.]+)',out)
                valid=valid and bool(duration and score) and 'Correct operation validated' in out and float(duration[1])>=10
                execution=float(duration[1]) if duration else 0
                rec.update(score=float(score[1]) if score else 0,iterations=iterations[(mode,t)])
            else: valid=valid and hashlib.sha256(out.encode()).hexdigest()==references[name]
            rec.update(workload=name,mode=mode,threads=t,profile=profile,sample=sample,warmup=sample<0,phase=seed,execution_s=execution,valid=valid)
            result['measurements'].append(rec);persist()
            print('sample',sample,name,key(mode,t,profile),round(execution,6),'PASS' if valid else 'FAIL',flush=True)
            if not valid: raise RuntimeError('Invalid sample; report cannot rank this incomplete matrix')
    result['complete']=True;persist()

if __name__=='__main__':main()
