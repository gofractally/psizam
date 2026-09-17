#!/usr/bin/env python3
"""Compare JIT1, JIT2 and Cranelift launching Clang from the same fresh Wasm."""
import argparse
import json
from pathlib import Path
import random
from run import Run,WT_FLAGS,save,sha

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True)
    p.add_argument('--samples',type=int,default=5);a=p.parse_args();r=Run(a.work);d=r.work/'clang'
    inputs=json.loads((d/'inputs.json').read_text())['inputs']
    oracle=json.loads((d/'oracles.json').read_text())['wasm'];rows=[];accepted=[]
    def command(mode,item):
        args=['-cc1','-triple','wasm32-unknown-wasi','-emit-obj','-O2','-std=c++17',
              '-x','c++-cpp-output',d/item['path'],'-o',d/'result.o']
        if mode=='cranelift-jit':return [r.work/'tools/wasmtime','run',*WT_FLAGS,'--dir',d,
              '-W','max-wasm-stack=16777216',d/'clang.wasm',*args]
        return [r.work/'tools/repro-psizam','jit' if mode=='jit1' else 'jit2','--dir='+str(d),d/'clang.wasm',*args]
    def invoke(item,mode,sample,phase):
        dest=d/'result.o';dest.unlink(missing_ok=True)
        rec,_,_=r.command(f'clang-fresh-{phase}-{item["name"]}-{mode}-{sample}',command(mode,item),timeout=180)
        digest=sha(dest) if dest.exists() else None
        rec.update(mode=mode,workload=item['name'],phase=phase,sample=sample,object_sha256=digest,
                   valid=rec['returncode']==0 and digest is not None and digest==oracle.get(item['name']))
        rows.append(rec);save(d/'fresh-measurements.json',rows)
        print('clang fresh',item['name'],mode,phase,sample,round(rec['wall_s'],4),rec['valid'],flush=True)
        return rec['valid']
    for item in inputs:
        for mode in ['jit1','jit2','cranelift-jit']:
            if invoke(item,mode,-1,'preflight'):accepted.append((item,mode))
    tasks=[(item,mode,s) for s in range(a.samples) for item,mode in accepted]
    random.Random(20260916).shuffle(tasks)
    for item,mode,s in tasks:invoke(item,mode,s,'compile-job')

if __name__=='__main__':main()
