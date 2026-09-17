#!/usr/bin/env python3
"""Time the same source-built clang.wasm compiling frozen C++ translation units.

prepare preprocesses real LLVM translation units using an existing bootstrap
compile_commands.json. Frozen .ii files make subsequent timing independent of
the original source tree and include search paths. artifacts performs one
diagnostic compilation per engine; measure runs repeated precompiled jobs.
"""
import argparse
import json
from pathlib import Path
import platform
import random
import shlex
import shutil
from run import Run, WT_FLAGS, save, sha
CLANG_WT_FLAGS=[*WT_FLAGS,'-W','nan-canonicalization=y']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage',choices=['prepare','artifacts','measure'])
    p.add_argument('--work',required=True,type=Path)
    p.add_argument('--clang-wasm',type=Path)
    p.add_argument('--llvm-compile-commands',type=Path)
    p.add_argument('--samples',type=int,default=5)
    p.add_argument('--timeout',type=int,default=3600)
    p.add_argument('--fallback-compile-threads',type=int,default=1,
                   help='If LLVM compilation timed out, retry with this many workers and the current timeout (0 disables).')
    a=p.parse_args();r=Run(a.work);d=r.work/'clang';d.mkdir(exist_ok=True)
    if a.stage=='prepare':
        if not a.clang_wasm or not a.llvm_compile_commands: p.error('prepare needs both input paths')
        shutil.copy2(a.clang_wasm,d/'clang.wasm')
        database=json.loads(a.llvm_compile_commands.read_text());inputs=[]
        for source in ['SipHash.cpp','APInt.cpp']:
            entry=next(c for c in database if c['file'].endswith('/'+source))
            tokens=shlex.split(entry['command']);cmd=[];i=0
            while i<len(tokens):
                token=tokens[i]
                if token=='-o':i+=2;continue
                if token=='-c' or token.startswith('-flto'):i+=1;continue
                cmd.append(token);i+=1
            dest=d/(source+'.ii')
            cmd+=['-E','-o',str(dest)]
            rec,_,_=r.command('preprocess-'+source,cmd,check=True)
            inputs.append({'name':source,'path':dest.name,'sha256':sha(dest),'bytes':dest.stat().st_size,'preprocess_record':rec})
        save(d/'inputs.json',{'clang_wasm_origin':str(a.clang_wasm.resolve()),'clang_wasm_sha256':sha(d/'clang.wasm'),
             'clang_wasm_bytes':(d/'clang.wasm').stat().st_size,'compile_database_sha256':sha(a.llvm_compile_commands),'inputs':inputs})
    elif a.stage=='artifacts':
        records=[]
        for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']:
            dest=d/('clang.'+('cwasm' if mode=='cranelift-aot' else mode))
            if mode=='cranelift-aot':cmd=[r.work/'tools/wasmtime','compile',*CLANG_WT_FLAGS,d/'clang.wasm','-o',dest]
            else:cmd=[r.work/'tools/pzam-compile','--target='+('aarch64' if platform.machine() in ['arm64','aarch64'] else 'x86_64'),
                     '--backend='+('jit2' if mode=='jit2-pzam' else 'llvm'),'--opt-level=2','-j1',d/'clang.wasm','-o',dest]
            rec,_,_=r.command('large-compile-'+mode,cmd,timeout=a.timeout)
            rec.update(mode=mode,phase='large-compile',valid=rec['returncode']==0,sample=0,compile_threads=1)
            if dest.exists():rec.update(artifact_sha256=sha(dest),artifact_bytes=dest.stat().st_size)
            records.append(rec);save(d/'compilation.json',records)
            print('large compilation',mode,round(rec['wall_s'],3),rec['returncode'],flush=True)
    else:
        inputs=json.loads((d/'inputs.json').read_text())['inputs'];rows=[];baseline={}
        modes=['native','jit2-pzam','llvm-o2-pzam','cranelift-aot']
        compilation=json.loads((d/'compilation.json').read_text())
        allowed={x['mode']:x['valid'] for x in compilation}
        # Retain the censored first trial and retry with a sufficient time limit.
        llvm_attempts=[x for x in compilation if x['mode']=='llvm-o2-pzam']
        if (llvm_attempts and llvm_attempts[-1].get('timeout') and a.fallback_compile_threads>0):
            threads=a.fallback_compile_threads;dest=d/'clang.llvm-o2-pzam'
            rec,_,_=r.command('large-compile-llvm-runtime-preparation',
                [r.work/'tools/pzam-compile','--target='+('aarch64' if platform.machine() in ['arm64','aarch64'] else 'x86_64'),
                 '--backend=llvm','--opt-level=2','-j'+str(threads),d/'clang.wasm','-o',dest],timeout=a.timeout)
            rec.update(mode='llvm-o2-pzam',phase='large-compile',sample=1,compile_threads=threads,
                       role='extended-timeout-retry' if threads==1 else 'runtime-artifact-preparation',
                       valid=rec['returncode']==0 and dest.exists())
            if dest.exists():rec.update(artifact_sha256=sha(dest),artifact_bytes=dest.stat().st_size)
            compilation.append(rec);save(d/'compilation.json',compilation);allowed['llvm-o2-pzam']=rec['valid']
            print('LLVM runtime artifact preparation',threads,'workers',round(rec['wall_s'],3),rec['returncode'],flush=True)
        def command(mode,item):
            src=d/item['path'];dest=d/'result.o'
            flags=['-cc1','-triple','wasm32-unknown-wasi','-emit-obj','-O2','-std=c++17','-x','c++-cpp-output',str(src),'-o',str(dest)]
            if mode=='native':return [r.config['native_cc'],*flags]
            if mode=='cranelift-aot':return [r.work/'tools/wasmtime','run',*CLANG_WT_FLAGS,'--allow-precompiled','--dir',str(d),'-W','max-wasm-stack=16777216',d/'clang.cwasm',*flags]
            return [r.work/'tools/pzam-run','--dir='+str(d),d/('clang.'+mode),*flags]
        for item in inputs:
            rec,_,_=r.command('clang-native-reference-'+item['name'],command('native',item),check=True)
            baseline[item['name']]=sha(d/'result.o')
        # Semantic oracle is output-object identity across Wasm executions. A
        # native compiler version difference is retained separately, never hidden.
        oracle={};failed=set()
        for item in inputs:
            for mode in ['cranelift-aot','llvm-o2-pzam','jit2-pzam']:
                if not allowed.get(mode):failed.add((item['name'],mode));continue
                dest=d/'result.o';dest.unlink(missing_ok=True)
                rec,_,_=r.command('clang-preflight-'+item['name']+'-'+mode,command(mode,item),timeout=180)
                digest=sha(dest) if dest.exists() else None
                if mode=='cranelift-aot' and rec['returncode']==0:oracle[item['name']]=digest
                valid=rec['returncode']==0 and digest is not None and digest==oracle.get(item['name'])
                if not valid:failed.add((item['name'],mode))
                rec.update(workload=item['name'],mode=mode,phase='preflight',valid=valid,object_sha256=digest)
                rows.append(rec);save(d/'measurements.json',rows)
                print('clang preflight',item['name'],mode,'PASS' if valid else 'FAIL',flush=True)
        tasks=[(item,mode,sample) for sample in range(a.samples) for item in inputs for mode in modes if mode=='native' or (item['name'],mode) not in failed]
        random.Random(20260916).shuffle(tasks)
        for item,mode,sample in tasks:
            dest=d/'result.o';dest.unlink(missing_ok=True)
            rec,_,_=r.command(f'clang-run-{item["name"]}-{mode}-{sample}',command(mode,item),timeout=180)
            digest=sha(dest) if dest.exists() else None
            expected=baseline[item['name']] if mode=='native' else oracle.get(item['name'])
            rec.update(workload=item['name'],mode=mode,phase='compile-job',sample=sample,object_sha256=digest,
                       valid=rec['returncode']==0 and digest is not None and digest==expected)
            rows.append(rec);save(d/'measurements.json',rows)
            print('clang',item['name'],mode,sample,round(rec['wall_s'],4),'PASS' if rec['valid'] else 'FAIL',flush=True)
        save(d/'oracles.json',{'native':baseline,'wasm':oracle})

if __name__=='__main__':main()
