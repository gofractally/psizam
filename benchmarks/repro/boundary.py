#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import random
import shutil
from run import Run, HERE, save, sha

def main():
    p=argparse.ArgumentParser(description='Measure scalar host boundaries with pinned native runners.')
    p.add_argument('--work',type=Path,required=True);p.add_argument('--runner',type=Path,required=True)
    p.add_argument('--samples',type=int,default=5);a=p.parse_args();r=Run(a.work)
    exe=r.work/'tools/repro-boundary';shutil.copy2(a.runner,exe)
    module=r.work/'modules/boundary.wasm'
    command=[Path(r.config['wasi_sdk'])/'bin/clang','-O3','-nostdlib',HERE/'boundary.c',
             '-Wl,--no-entry','-Wl,--export=guest_increment','-Wl,--export=bounce','-o',module]
    r.command('build-boundary',command,check=True)
    save(r.work/'boundary-config.json',{'runner_sha256':sha(exe),'wasm_sha256':sha(module),
       'source_sha256':sha(HERE/'boundary.cpp'),'guest_source_sha256':sha(HERE/'boundary.c')})
    schedule=[(mode,sample) for sample in range(-1,a.samples) for mode in ['jit1','jit2','llvm-jit','cranelift']]
    random.Random(20260916).shuffle(schedule);schedule.sort(key=lambda x:x[1]>=0)
    rows=[]
    for mode,sample in schedule:
        rec,out,_=r.command(f'boundary-{mode}-{sample}',[exe,mode,module],timeout=90)
        try:values=json.loads(out)
        except ValueError:values={}
        rec.update(values);rec.update(mode=mode,sample=sample,warmup=sample<0,workload='scalar-boundary',
                                      valid=rec['returncode']==0 and values.get('valid',False))
        rows.append(rec);save(r.work/'boundary.json',rows)
        print('boundary',mode,sample,'PASS' if rec['valid'] else 'FAIL',flush=True)

if __name__=='__main__':main()
