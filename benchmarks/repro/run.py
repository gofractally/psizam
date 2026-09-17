#!/usr/bin/env python3
"""Reproducible, correctness-gated, serial psizam/Cranelift benchmarks.

Subcommands use a persistent work directory: prepare, artifacts, validate, measure.
Source files are fetched at immutable revisions and checked against sources.lock.json.
Every process has a timeout; raw commands, output, timing and RSS are retained.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import shutil
import subprocess
import threading
import time
import urllib.request

HERE = Path(__file__).resolve().parent
MODES = ['native', 'jit1', 'jit2', 'llvm-jit', 'jit2-pzam', 'llvm-o2-pzam',
         'cranelift-jit', 'cranelift-aot']
WT_FLAGS = ['-C', 'compiler=cranelift,cache=n,parallel-compilation=n',
            '-O', 'opt-level=2,memory-reservation=4294967296,memory-guard-size=4294967296']
GAME = 'llvm-test-suite/SingleSource/Benchmarks/BenchmarkGame/'
POLY = 'llvm-test-suite/SingleSource/Benchmarks/Polybench/'
WORKLOADS = [
    {'name':'fannkuch','kind':'main','files':[GAME+'fannkuch.c'],'args':[]},
    {'name':'nbody','kind':'main','files':[GAME+'n-body.c'],'args':[]},
    {'name':'nsieve','kind':'main','files':[GAME+'nsieve-bits.c'],'args':[]},
    {'name':'spectral-norm','kind':'main','files':[GAME+'spectral-norm.c'],'args':['2000']},
    {'name':'coremark','kind':'coremark','files':['coremark/'+f for f in
       ['core_main.c','core_list_join.c','core_matrix.c','core_state.c','core_util.c','posix/core_portme.c']],
       'args':['0','0','0x66','100000']},
]
for kernel, directory, sizes in [
    ('gemm','linear-algebra/blas/gemm',['-DNI=512','-DNJ=512','-DNK=512']),
    ('jacobi','stencils/jacobi-2d',['-DN=512','-DTSTEPS=500'])]:
    for simd in [False,True]:
        WORKLOADS.append({'name':kernel+('-simd' if simd else '-scalar'), 'kind':'polybench',
            'files':[POLY+directory+'/'+('gemm' if kernel=='gemm' else 'jacobi-2d')+'.c'],
            'defines':sizes,'simd':simd,'args':[]})

def sha(path):
    with Path(path).open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()

def save(path,data):
    Path(path).write_text(json.dumps(data,indent=2)+'\n')

def source_identity(loc):
    """Keep original commit identity when replaying the supplied source archives."""
    loc=Path(loc).resolve()
    top=subprocess.run(['git','-C',str(loc),'rev-parse','--show-toplevel'],capture_output=True,text=True)
    if top.returncode==0 and Path(top.stdout.strip()).resolve()==loc:
        return {'path':str(loc),'commit':subprocess.check_output(['git','-C',str(loc),'rev-parse','HEAD'],text=True).strip(),
                'diff_sha256':hashlib.sha256(subprocess.check_output(['git','-C',str(loc),'diff','HEAD'])).hexdigest()}
    metadata=loc/'BENCHMARK_SOURCE_REVISION.json'
    if not metadata.exists():raise RuntimeError('Need a Git checkout or benchmark source-archive metadata: '+str(loc))
    record=json.loads(metadata.read_text())
    return {'path':str(loc),'commit':record['commit'],'source_archive_metadata_sha256':sha(metadata),
            'provenance':'Git archive plus the accompanying benchmark harness snapshot'}

class Run:
    def __init__(self,work):
        self.work=Path(work).resolve(); self.work.mkdir(parents=True,exist_ok=True)
        self.logs=self.work/'logs';self.logs.mkdir(exist_ok=True)
        self.env=os.environ.copy()
        for name in ['CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','DYLD_LIBRARY_PATH',
                     'WASMTIME_BACKTRACE_DETAILS','PZAM_RUN_PROFILE']:
            self.env.pop(name,None)
        # Developer diagnostics and pass overrides must not silently alter a
        # reproducible measurement. Opt levels are explicit runner/CLI settings.
        for name in list(self.env):
            if name.startswith(('PSIZAM_LLVM_','PSIZAM_DUMP_')) or name=='PSIZAM_TRACE_WASI':
                self.env.pop(name,None)
        self.env.update(LC_ALL='C',TZ='UTC',RAYON_NUM_THREADS='1',OMP_NUM_THREADS='1')
        self.config=json.loads((self.work/'config.json').read_text()) if (self.work/'config.json').exists() else {}
    def command(self,label,cmd,timeout=180,check=False):
        cmd=[str(x) for x in cmd]
        slug=re.sub('[^a-zA-Z0-9_.-]','_',label)
        stdout=self.logs/(slug+'.stdout');stderr=self.logs/(slug+'.stderr')
        if stdout.exists(): raise RuntimeError('Refusing to overwrite '+str(stdout))
        wrapped=['/usr/bin/time','-l',*cmd] if platform.system()=='Darwin' else ['/usr/bin/time','-v',*cmd]
        start=time.perf_counter_ns()
        timed_out=False
        with stdout.open('wb') as out, stderr.open('wb') as err:
            proc=subprocess.Popen(wrapped,cwd=self.work,env=self.env,stdout=out,stderr=err,start_new_session=True)
            def expire():
                nonlocal timed_out
                import signal
                try: os.killpg(proc.pid,signal.SIGKILL);timed_out=True
                except ProcessLookupError: pass
            watchdog=threading.Timer(timeout,expire);watchdog.start()
            rc=proc.wait();watchdog.cancel()
            if timed_out: rc=124
        wall=(time.perf_counter_ns()-start)/1e9
        output=stdout.read_text(errors='replace');error=stderr.read_text(errors='replace')
        record={'label':label,'command':cmd,'returncode':rc,'timeout':timed_out,'wall_s':wall,
                'stdout':str(stdout.relative_to(self.work)),'stderr':str(stderr.relative_to(self.work)),
                'stdout_sha256':sha(stdout)}
        for token,name in [('TIME','guest_s'),('SETUP','setup_s'),('START','start_s')]:
            match=re.search(r'BENCH_'+token+r'_NS=(\d+)',error)
            if match: record[name]=int(match[1])/1e9
        match=re.search(r'(\d+)\s+maximum resident set size',error)
        if match: record['peak_rss_bytes']=int(match[1])
        match=re.search(r'Maximum resident set size \(kbytes\):\s*(\d+)',error)
        if match: record['peak_rss_bytes']=int(match[1])*1024
        for counter_label,key in [('instructions retired','process_instructions'),('cycles elapsed','process_cycles')]:
            match=re.search(r'(\d+)\s+'+counter_label,error)
            if match:record[key]=int(match[1])
        with (self.work/'commands.jsonl').open('a') as f: f.write(json.dumps(record)+'\n')
        if check and rc: raise RuntimeError(f'{label}: exit {rc}; see {stderr}')
        return record,output,error
    def invocation(self,w,mode,args=None):
        name=w['name']; args=w['args'] if args is None else args
        data=self.work/'modules';tools=self.work/'tools'
        if mode=='native': return [data/(name+'.native'),*args]
        if mode in ['jit1','jit2','llvm-jit']:
            return [tools/'repro-psizam',{'jit1':'jit','jit2':'jit2','llvm-jit':'llvm'}[mode],data/(name+'.wasm'),*args]
        if mode.endswith('pzam'):
            return [tools/'pzam-run',data/(name+'.'+mode),*args]
        if mode=='cranelift-canonical-aot':
            return [tools/'wasmtime','run',*WT_FLAGS,'-W','nan-canonicalization=y',
                    '--allow-precompiled',data/(name+'.canonical.cwasm'),*args]
        return [tools/'wasmtime','run',*WT_FLAGS,
                *(['--allow-precompiled'] if mode=='cranelift-aot' else []),
                data/(name+('.cwasm' if mode=='cranelift-aot' else '.wasm')),*args]

def prepare(r,a):
    if (r.work/'config.json').exists(): raise RuntimeError('prepare requires a new work directory')
    config={'platform':platform.platform(),'machine':platform.machine(),'date_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
            'wasi_sdk':str(a.wasi_sdk.resolve()),'native_cc':str(a.native_cc.resolve()),'engine_bin':str(a.engine_bin.resolve()),
            'threads':1,'modes':MODES,'wasmtime_flags':WT_FLAGS,'source_lock_sha256':sha(HERE/'sources.lock.json'),
            'environment_overrides':{k:r.env[k] for k in ['LC_ALL','TZ','RAYON_NUM_THREADS','OMP_NUM_THREADS']}}
    for repo in ['psizam','psio']:
        loc=HERE.parents[1] if repo=='psizam' else a.psio_source
        config[repo]=source_identity(loc)
    if platform.system()=='Darwin':
        config['hardware']={k:subprocess.check_output(['sysctl','-n',k],text=True).strip() for k in ['machdep.cpu.brand_string','hw.memsize','hw.ncpu']}
    for name,command in [('wasi_compiler',[a.wasi_sdk/'bin/clang','--version']),('native_compiler',[a.native_cc,'--version']),('wasmtime',[a.wasmtime,'--version'])]:
        config[name]=subprocess.check_output([str(x) for x in command],text=True).strip()
    tools=r.work/'tools';tools.mkdir()
    for name,path in [('wasmtime',a.wasmtime),('repro-psizam',a.runner),('pzam-compile',a.engine_bin/'pzam-compile'),('pzam-run',a.engine_bin/'pzam-run')]:
        shutil.copy2(path,tools/name)
    config['tools']={p.name:{'sha256':sha(p),'bytes':p.stat().st_size} for p in tools.iterdir()}
    shutil.copytree(HERE,r.work/'harness',ignore=shutil.ignore_patterns('__pycache__'))
    sources=r.work/'sources'; sources.mkdir()
    for item in json.loads((HERE/'sources.lock.json').read_text()):
        dest=sources/item['project']/item['path'];dest.parent.mkdir(parents=True,exist_ok=True)
        local=a.source_cache/item['project']/item['path'] if a.source_cache else None
        if local and local.exists(): shutil.copy2(local,dest)
        else: urllib.request.urlretrieve(item['url'],dest)
        if sha(dest)!=item['sha256']: raise RuntimeError('Source hash mismatch: '+str(dest))
    modules=r.work/'modules';modules.mkdir()
    save(r.work/'config.json',config);r.config=config
    for w in WORKLOADS:
        for wasm in [False,True]:
            cc=a.wasi_sdk/'bin/clang' if wasm else a.native_cc
            flags=['-O3','-ffp-contract=off','-fno-fast-math','-w']
            if not w.get('simd'): flags+=['-fno-vectorize','-fno-slp-vectorize']
            elif wasm: flags+=['-msimd128']
            flags+=w.get('defines',[])
            if w['kind']=='coremark':
                flags+=['-I'+str(sources/'coremark'),'-I'+str(sources/'coremark/posix'),'-DITERATIONS=0',
                        '-DMULTITHREAD=1','-DTOTAL_DATA_SIZE=2000','-DMEM_LOCATION="heap"',
                        '-DCOMPILER_FLAGS="O3; ffp-contract=off; no vectorization"']
            if w['kind']=='polybench':
                flags+=['-I'+str(sources/POLY/'utilities'),'-DPOLYBENCH_TIME','-DPOLYBENCH_NO_FLUSH_CACHE','-DFMA_DISABLED=1']
                if wasm: flags+=['-D_WASI_EMULATED_PROCESS_CLOCKS']
            tag=w['name']+('.wasm' if wasm else '.native'); objects=[]
            for index,rel in enumerate(w['files']):
                obj=modules/(tag+f'.{index}.o');objects.append(obj)
                r.command('build-'+tag+f'-{index}',[cc,*flags,'-Dmain=benchmark_main','-c',sources/rel,'-o',obj],check=True)
            obj=modules/(tag+'.timer.o');objects.append(obj)
            r.command('build-'+tag+'-timer',[cc,*flags,'-c',HERE/'timed_main.c','-o',obj],check=True)
            linkflags=['-Wl,-z,stack-size=16777216'] if wasm else []
            if wasm and w['kind']=='polybench': linkflags+=['-lwasi-emulated-process-clocks']
            r.command('link-'+tag,[cc,*objects,*linkflags,'-lm','-o',modules/tag],check=True)
        print('built',w['name'],flush=True)
    config['modules']={p.name:{'sha256':sha(p),'bytes':p.stat().st_size} for p in modules.iterdir() if p.suffix in ['.wasm','.native']}
    save(r.work/'config.json',config)

def selected(a):
    return [w for w in WORKLOADS if not a.only or w['name'] in a.only.split(',')]

def artifacts(r,a):
    records=[]
    for w in selected(a):
        for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']:
            dest=r.work/'modules'/(w['name']+('.cwasm' if mode=='cranelift-aot' else '.'+mode))
            wasm=r.work/'modules'/(w['name']+'.wasm')
            for sample in range(a.samples):
                if mode=='cranelift-aot': cmd=[r.work/'tools/wasmtime','compile',*WT_FLAGS,wasm,'-o',dest]
                else: cmd=[r.work/'tools/pzam-compile','--target='+('aarch64' if platform.machine() in ['arm64','aarch64'] else 'x86_64'),
                           '--backend='+('jit2' if mode=='jit2-pzam' else 'llvm'),'--opt-level=2','-j1',wasm,'-o',dest]
                record,_,_=r.command(f'compile-{w["name"]}-{mode}-{sample}',cmd,check=True)
                record.update(workload=w['name'],mode=mode,phase='compile',sample=sample,
                              artifact_bytes=dest.stat().st_size,artifact_sha256=sha(dest))
                records.append(record)
            print('compiled',w['name'],mode,flush=True)
    save(r.work/'compilation.json',records)

def clean_error(error):
    error=re.sub(r'\n?BENCH_(?:TIME|SETUP|START)_NS=\d+\n?', '',error)
    error=re.sub(r'BENCH_GAS_CONSUMED=\d+\n?','',error)
    error=re.sub(r'\[pzam-run\][^\n]*\n','',error)
    # /usr/bin/time starts its statistics on a new line (Darwin), or a tab (Linux).
    return re.split(r'\s+\d+(?:\.\d+)? real\s+\d|\s*Command being timed:',error)[0].strip()

def validate(r,a):
    validations={}
    for w in selected(a):
        if w['kind']=='coremark': continue # Both seed sets are checked in timed CoreMark runs.
        args=['--validate'] if w['kind']=='polybench' else w['args']
        reference=None
        for mode in MODES:
            record,out,err=r.command(f'validate-{w["name"]}-{mode}',r.invocation(w,mode,args),timeout=a.timeout)
            content=clean_error(err) if w['kind']=='polybench' else out
            digest=hashlib.sha256(content.encode()).hexdigest()
            if mode=='native': reference=digest if record['returncode']==0 and content.strip() else None
            valid=record['returncode']==0 and digest==reference and bool(content.strip())
            validations[w['name']+'/'+mode]={'valid':valid,'reference_sha256':reference,'result_sha256':digest,'record':record}
            print('validate',w['name'],mode,'PASS' if valid else 'FAIL',flush=True)
    save(r.work/'validation.json',validations)

def measure(r,a):
    validation=json.loads((r.work/'validation.json').read_text())
    rows=[];workloads=selected(a)
    schedule=[(w,mode,sample) for sample in range(-1,a.samples) for w in workloads if w['kind']!='coremark' for mode in MODES]
    random.Random(a.seed).shuffle(schedule)
    # One excluded warmup for every pair, before randomized measured trials.
    schedule.sort(key=lambda task:task[2]>=0)
    for w,mode,sample in schedule:
        if not validation.get(w['name']+'/'+mode,{}).get('valid'): continue
        record,out,err=r.command(f'run-{w["name"]}-{mode}-{sample}',r.invocation(w,mode),timeout=a.timeout)
        valid=record['returncode']==0 and 'guest_s' in record
        if w['kind']=='polybench':
            try: execution=float(out.strip())
            except ValueError: execution=0
            valid=valid and execution>0
        else:
            execution=record.get('guest_s',0)
            valid=valid and hashlib.sha256(out.encode()).hexdigest()==validation[w['name']+'/'+mode]['reference_sha256']
        record.update(workload=w['name'],mode=mode,phase='run',sample=sample,warmup=sample<0,
                      execution_s=execution,valid=valid)
        if 'guest_s' in record: record['outside_main_s']=record['wall_s']-record['guest_s']
        rows.append(record);save(r.work/'measurements.json',rows)
        print('sample',sample,w['name'],mode,round(execution,6),'PASS' if valid else 'FAIL',flush=True)

def coremark(r,a):
    w=next(w for w in WORKLOADS if w['kind']=='coremark'); rows=[];counts={};failures=[]
    canonical=getattr(a,'canonical_coremark',False)
    prefix='coremark-canonical' if canonical else 'coremark'
    modes=['cranelift-canonical-aot'] if canonical else MODES
    if canonical:
        dest=r.work/'modules/coremark.canonical.cwasm'
        rec,_,_=r.command('coremark-canonical-compile',[r.work/'tools/wasmtime','compile',
             *WT_FLAGS,'-W','nan-canonicalization=y',r.work/'modules/coremark.wasm','-o',dest],check=True)
        rec.update(artifact_bytes=dest.stat().st_size,artifact_sha256=sha(dest))
        save(r.work/'coremark-canonical-compilation.json',[rec])
    for mode in modes:
        rec,out,err=r.command('coremark-calibrate-'+mode,r.invocation(w,mode),timeout=a.timeout)
        duration=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out)
        if rec['returncode'] or not duration:
            rec.update(mode=mode,workload='coremark',phase='calibration',valid=False)
            failures.append(rec);save(r.work/(prefix+'-failures.json'),failures)
            print('coremark calibration FAIL',mode,flush=True);continue
        rec2,out2,_=r.command('coremark-calibrate-validation-'+mode,
                             r.invocation(w,mode,['0x3415','0x3415','0x66','100000']),timeout=a.timeout)
        duration2=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out2)
        if rec2['returncode'] or not duration2:
            rec2.update(mode=mode,workload='coremark',phase='calibration',valid=False)
            failures.append(rec2);save(r.work/(prefix+'-failures.json'),failures);continue
        fastest=min(float(duration[1]),float(duration2[1]))
        counts[mode]=math.ceil(100000*14/max(fastest,.001)/1000)*1000
    save(r.work/(prefix+'-iterations.json'),counts)
    tasks=[(mode,sample) for sample in range(a.samples) for mode in counts]
    random.Random(a.seed).shuffle(tasks)
    for mode,sample in tasks:
        for seed,params in [('performance',['0','0','0x66']),('validation',['0x3415','0x3415','0x66'])]:
            # Both required seed sets run >=10 sec; retain each score separately.
            rec,out,err=r.command(f'coremark-{seed}-{mode}-{sample}',r.invocation(w,mode,[*params,str(counts[mode])]),timeout=a.timeout)
            duration=re.search(r'Total time \(secs\)\s*:\s*([0-9.]+)',out)
            score=re.search(r'Iterations/Sec\s*:\s*([0-9.]+)',out)
            valid=rec['returncode']==0 and 'Correct operation validated' in out and bool(duration and score) and float(duration[1])>=10
            rec.update(workload='coremark',mode=mode,phase=seed,sample=sample,warmup=False,iterations=counts[mode],
                       execution_s=float(duration[1]) if duration else 0,score=float(score[1]) if score else 0,valid=valid)
            rows.append(rec);save(r.work/(prefix+'.json'),rows)
            print('coremark',seed,mode,sample,rec['score'],'PASS' if valid else 'FAIL',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage',choices=['prepare','artifacts','validate','measure','coremark'])
    p.add_argument('--work',type=Path,required=True);p.add_argument('--samples',type=int,default=5)
    p.add_argument('--timeout',type=int,default=180);p.add_argument('--seed',type=int,default=20260916);p.add_argument('--only')
    p.add_argument('--canonical-coremark',action='store_true',help='Run the separate matched-FP Cranelift AOT CoreMark control.')
    for name in ['wasi-sdk','native-cc','engine-bin','runner','wasmtime','source-cache','psio-source']:
        p.add_argument('--'+name,type=Path)
    a=p.parse_args()
    if a.stage=='prepare' and any(getattr(a,k) is None for k in ['wasi_sdk','native_cc','engine_bin','runner','wasmtime','psio_source']):
        p.error('prepare requires tool paths and --psio-source')
    if a.samples<1: p.error('--samples must be positive')
    globals()[a.stage](Run(a.work),a)

if __name__=='__main__': main()
