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
    if (r.work/'boundary.json').exists():raise RuntimeError('Refusing to overwrite boundary measurements')
    exe=r.work/'tools/repro-boundary';shutil.copy2(a.runner,exe)
    manifest=json.loads((r.work/'source-manifest.json').read_text())
    for repo in ['psizam','psio']:
        archived=r.work/'source'/repo
        root=archived if archived.exists() else Path(r.config[repo]['path'])
        for name,digest in manifest[repo].items():
            if repo=='psio' or name.startswith(('include/','src/')):
                if sha(root/name)!=digest:raise RuntimeError('Boundary engine source differs: '+name)
    module=r.work/'modules/boundary.wasm'
    command=[Path(r.config['wasi_sdk'])/'bin/clang','-O3','-nostdlib',HERE/'boundary.c',
             '-Wl,--no-entry','-Wl,--export=guest_increment','-Wl,--export=bounce','-o',module]
    r.command('build-boundary',command,check=True)
    save(r.work/'boundary-config.json',{'runner_sha256':sha(exe),'wasm_sha256':sha(module),
       'source_sha256':sha(HERE/'boundary.cpp'),'guest_source_sha256':sha(HERE/'boundary.c'),
       'engine_source_manifest_sha256':sha(r.work/'source-manifest.json'),
       'profiles':['none','gas','timeout'],'gas_budget':1_000_000_000_000,'timeout_seconds':120,
       'samples':a.samples,'warmups':1})
    schedule=[(mode,sample,profile) for sample in range(-1,a.samples)
              for mode in ['jit1','jit2','llvm-jit','cranelift'] for profile in ['none','gas','timeout']]
    random.Random(20260916).shuffle(schedule);schedule.sort(key=lambda x:x[1]>=0)
    rows=[]
    for mode,sample,profile in schedule:
        rec,out,_=r.command(f'boundary-{mode}-{profile}-{sample}',[exe,mode,module,profile],timeout=180)
        try:values=json.loads(out)
        except ValueError:values={}
        rec.update(values);rec.update(mode=mode,profile=profile,sample=sample,warmup=sample<0,workload='scalar-boundary',
                                      valid=rec['returncode']==0 and values.get('valid',False))
        rows.append(rec);save(r.work/'boundary.json',rows)
        print('boundary',mode,profile,sample,'PASS' if rec['valid'] else 'FAIL',flush=True)
        if not rec['valid']:raise RuntimeError('Boundary correctness failure')

if __name__=='__main__':main()
