#!/usr/bin/env python3
"""Interleaved application timing after all heavy artifact compilation ends."""
import argparse
import json
from pathlib import Path
import random
from run import Run, WT_FLAGS, save, sha


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--llvm-artifact',type=Path)
    p.add_argument('--samples',type=int,default=5)
    a=p.parse_args();r=Run(a.work);d=r.work/'clang'
    target=d/'parallel-jobs.json'
    if target.exists():raise RuntimeError('Refusing to overwrite '+str(target))
    comp=json.loads((d/'parallel-compilation.json').read_text())
    inputs=json.loads((d/'inputs.json').read_text())['inputs']
    oracle=json.loads((d/'oracles.json').read_text())['wasm']
    modes=['llvm-o2-pzam','cranelift-canonical-aot']
    artifacts={m:d/('clang.llvm-o2-parallel-pzam' if m==modes[0] else 'clang.cwasm') for m in modes}
    if a.llvm_artifact:artifacts[modes[0]]=a.llvm_artifact.resolve()
    digests={m:sha(path) for m,path in artifacts.items()}
    for m in modes:
        expected={x['artifact_sha256'] for x in comp if x['mode']==m and x['compile_threads']==18 and x['valid']}
        if expected!={digests[m]}:raise RuntimeError('Artifact does not match measured 18-worker outputs: '+m)
    for name in ['pzam-run','wasmtime']:
        if sha(r.work/'tools'/name)!=r.config['tools'][name]['sha256']:raise RuntimeError('Frozen tool changed: '+name)
    flags=[x.replace('parallel-compilation=n','parallel-compilation=y') for x in WT_FLAGS]+['-W','nan-canonicalization=y']
    r.env['RAYON_NUM_THREADS']='18'
    save(d/'parallel-jobs-config.json',{'artifact_paths':{m:str(path) for m,path in artifacts.items()},
         'artifact_sha256':digests,'compile_threads':18,'samples':a.samples,'excluded_warmups':1,
         'method':'Serial interleaved fresh processes after artifact compilation; per-job output hash check.',
         'environment_overrides':{'RAYON_NUM_THREADS':'18','OMP_NUM_THREADS':'1'}})
    tasks=[(item,m,s) for s in range(-1,a.samples) for item in inputs for m in modes]
    random.Random(20260921).shuffle(tasks);tasks.sort(key=lambda x:x[2]>=0);rows=[]
    for item,mode,sample in tasks:
        obj=d/'result.o';obj.unlink(missing_ok=True)
        args=['-cc1','-triple','wasm32-unknown-wasi','-emit-obj','-O2','-std=c++17','-x','c++-cpp-output',d/item['path'],'-o',obj]
        if mode==modes[0]:cmd=[r.work/'tools/pzam-run','--dir='+str(d),artifacts[mode],*args]
        else:cmd=[r.work/'tools/wasmtime','run',*flags,'--allow-precompiled','--dir',d,'-W','max-wasm-stack=16777216',artifacts[mode],*args]
        rec,_,_=r.command(f'parallel-clang-job-{item["name"]}-{mode}-{sample}',cmd)
        digest=sha(obj) if obj.exists() else None
        rec.update(workload=item['name'],mode=mode,compile_threads=18,phase='parallel-clang-job',sample=sample,warmup=sample<0,
                   object_sha256=digest,expected_sha256=oracle[item['name']],valid=rec['returncode']==0 and digest==oracle[item['name']])
        rows.append(rec);save(target,rows)
        print(item['name'],mode,sample,round(rec['wall_s'],3),'PASS' if rec['valid'] else 'FAIL',flush=True)
    if not all(x['valid'] for x in rows):raise SystemExit(1)


if __name__=='__main__':main()
