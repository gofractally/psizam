#!/usr/bin/env python3
"""Matched Cranelift NaN-canonicalization profile for psizam LLVM AOT."""
import argparse
import hashlib
import json
from pathlib import Path
import random
from run import Run,WORKLOADS,WT_FLAGS,save,sha,clean_error

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True)
    p.add_argument('--samples',type=int,default=5);a=p.parse_args();r=Run(a.work)
    flags=[*WT_FLAGS,'-W','nan-canonicalization=y'];rows=[];compiled=[];validated={}
    reference=json.loads((r.work/'validation.json').read_text())
    workloads=[w for w in WORKLOADS if w['kind']!='coremark']
    def command(w,args=None):return [r.work/'tools/wasmtime','run',*flags,'--allow-precompiled',
                                    r.work/'modules'/(w['name']+'.canonical.cwasm'),*(w['args'] if args is None else args)]
    for w in workloads:
        dest=r.work/'modules'/(w['name']+'.canonical.cwasm')
        for sample in range(3):
            rec,_,_=r.command(f'canonical-compile-{w["name"]}-{sample}',
                 [r.work/'tools/wasmtime','compile',*flags,r.work/'modules'/(w['name']+'.wasm'),'-o',dest],check=True)
            rec.update(mode='cranelift-canonical-aot',phase='compile',workload=w['name'],sample=sample,
                       artifact_bytes=dest.stat().st_size,artifact_sha256=sha(dest));compiled.append(rec)
        args=['--validate'] if w['kind']=='polybench' else w['args']
        rec,out,err=r.command('canonical-validate-'+w['name'],command(w,args))
        content=clean_error(err) if w['kind']=='polybench' else out
        digest=hashlib.sha256(content.encode()).hexdigest()
        rec.update(valid=rec['returncode']==0 and digest==reference[w['name']+'/native']['reference_sha256'],result_sha256=digest)
        validated[w['name']]=rec
        print('canonical validation',w['name'],rec['valid'],flush=True)
    save(r.work/'canonical-compilation.json',compiled);save(r.work/'canonical-validation.json',validated)
    tasks=[(w,sample) for sample in range(-1,a.samples) for w in workloads if validated[w['name']]['valid']]
    random.Random(20260916).shuffle(tasks);tasks.sort(key=lambda x:x[1]>=0)
    for w,sample in tasks:
        rec,out,err=r.command(f'canonical-run-{w["name"]}-{sample}',command(w))
        valid=rec['returncode']==0 and 'guest_s' in rec
        if w['kind']=='polybench':
            try:duration=float(out.strip())
            except ValueError:duration=0
            valid=valid and duration>0
        else:
            duration=rec.get('guest_s',0)
            valid=valid and hashlib.sha256(out.encode()).hexdigest()==reference[w['name']+'/native']['reference_sha256']
        rec.update(mode='cranelift-canonical-aot',workload=w['name'],phase='run',sample=sample,warmup=sample<0,
                   execution_s=duration,valid=valid)
        if 'guest_s' in rec:rec['outside_main_s']=rec['wall_s']-rec['guest_s']
        rows.append(rec);save(r.work/'canonical.json',rows)
        print('canonical',w['name'],sample,round(duration,6),valid,flush=True)

if __name__=='__main__':main()
