#!/usr/bin/env python3
"""Run serial, randomized focused call benchmarks. Refuse to overwrite evidence."""
import argparse,hashlib,json,os,pathlib,platform,random,shutil,statistics,subprocess

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--binary',type=pathlib.Path,required=True);p.add_argument('--fixture',type=pathlib.Path,required=True)
 p.add_argument('--aot-dir',type=pathlib.Path,help='Directory with call-none/gas/timeout.pzam from the CLI test build')
 p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--samples',type=int,default=5)
 p.add_argument('--modes',nargs='+',default=['jit1','jit2','llvm','cranelift-checked','cranelift-cached'])
 a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
 def save(name,v):(a.out/name).write_text(json.dumps(v,indent=2)+'\n')
 binary=a.out.resolve()/'call-boundary';fixture=a.out.resolve()/'calls.wasm'
 shutil.copy2(a.binary,binary);shutil.copy2(a.fixture,fixture)
 aot={}
 if a.aot_dir:
  if 'llvm-aot' not in a.modes:a.modes.insert(3,'llvm-aot')
  for policy in ['none','gas','timeout']:
   artifact=a.out.resolve()/('call-'+policy+'.pzam');shutil.copy2(a.aot_dir/artifact.name,artifact);aot[policy]=artifact
 if 'llvm-aot' in a.modes and not aot:raise RuntimeError('llvm-aot requires --aot-dir')
 root=pathlib.Path(__file__).resolve().parents[2]
 manifest={str(f.relative_to(root)):sha(f) for directory in ['include','src'] for f in (root/directory).rglob('*') if f.is_file()}
 revision_file=root/'BENCHMARK_SOURCE_REVISION.json'
 revision=(json.loads(revision_file.read_text())['commit'] if revision_file.exists() else
           subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip())
 save('config.json',dict(host=platform.platform(),machine=platform.machine(),binary_sha256=sha(binary),fixture_sha256=sha(fixture),samples=a.samples,aot_sha256={k:sha(v) for k,v in aot.items()},
      git_head=revision,source_sha256=manifest,
      note='C++ operator new/new[] counts exclude direct malloc, mmap and Wasmtime Rust allocations. Alternate-stack mapping reuse has separate regression tests. No build or other benchmark may run concurrently.'))
 env={k:v for k,v in os.environ.items() if not k.startswith(('PSIZAM_LLVM_','PSIZAM_DUMP_'))}
 rows=[]
 schedule=[(sample,mode,policy) for sample in range(-1,a.samples) for mode in a.modes for policy in ['none','gas','timeout']]
 random.Random(20260917).shuffle(schedule);schedule.sort(key=lambda r:r[0]>=0)
 for sample,mode,policy in schedule:
  cmd=[str(binary),mode,str(aot[policy] if mode=='llvm-aot' else fixture),policy,'bench'];r=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=90)
  label=f'{mode}-{policy}-{sample}';(a.out/(label+'.stdout')).write_text(r.stdout);(a.out/(label+'.stderr')).write_text(r.stderr)
  if r.returncode:raise RuntimeError(f'{label}: {r.returncode}: {r.stderr[-1000:]}')
  row=json.loads(r.stdout);assert row['valid'];row.update(sample=sample,mode=mode,policy=policy,command=cmd)
  rows.append(row);save('measurements.json',rows)
  print(label,flush=True)
 summary={}
 for mode in a.modes:
  for policy in ['none','gas','timeout']:
   cell=[r for r in rows if r['mode']==mode and r['policy']==policy and r['sample']>=0]
   summary[mode+'/'+policy]={k:statistics.median(r[k] for r in cell) for k in cell[0] if k.endswith(('_ns','_allocations'))}
 save('summary.json',summary)
 print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
