#!/usr/bin/env python3
"""Report one complete, current-source CPU-resource matrix; never merge batches."""
import argparse
import csv
import hashlib
import json
import math
import statistics as stats
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

LABELS={'jit1':'JIT1','jit2':'JIT2','llvm-jit':'LLVM JIT O2','jit2-pzam':'JIT2 PZAM',
        'llvm-o2-pzam':'LLVM O2 PZAM','cranelift-jit':'Cranelift fresh','cranelift-aot':'Cranelift AOT'}
def gm(values): return math.exp(stats.mean(math.log(v) for v in values))


def summarize(data):
    if data.get('schema') != 1 or not data.get('complete'):
        raise ValueError('Report requires a completed CPU-resource matrix from one source state')
    if not data['enforcement'] or any(not r['valid'] for r in data['enforcement']+data['validation']+data['measurements']):
        raise ValueError('Failed or missing correctness/enforcement gate')
    grouped=defaultdict(list)
    for row in data['measurements']:
        if not row['warmup'] and row['phase']=='performance':
            grouped[(row['workload'],row['mode'],row['threads'],row['profile'])].append(row)
    cells={}
    for name in data['workloads']:
        cells[name]={}
        for variant in data['matrix']:
            mode,t,profile=variant.split('/');threads=int(t[1:])
            rows=grouped[(name,mode,threads,profile)]
            if len(rows)!=data['samples']: raise ValueError('Missing samples: '+name+'/'+variant)
            values=[r['execution_s'] for r in rows]
            if min(values)<=0: raise ValueError('Nonpositive timing')
            cells[name][variant]={'median_s':stats.median(values),'min_s':min(values),'max_s':max(values),
                                  'wall_s':stats.median(r['wall_s'] for r in rows),'samples':len(rows)}
            cells[name][variant]['peak_rss_mib']=stats.median(r['peak_rss_bytes']/2**20 for r in rows)
            cells[name][variant]['outside_kernel_ms']=stats.median(1000*(r['wall_s']-r['execution_s']) for r in rows)
        for variant,cell in cells[name].items():
            base=variant.rsplit('/',1)[0]+'/none'
            cell['relative_time']=cell['median_s']/cells[name][base]['median_s']
    profiles=list(dict.fromkeys(v.rsplit('/',1)[0] for v in data['matrix']))
    # Show one runtime result per engine, chosen consistently across workloads
    # and limit settings. Keep every compiler variant in the raw summary.
    runtime_by_engine={}
    for variant in profiles:
        mode,workers=variant.split('/j')
        selected=runtime_by_engine.get(mode)
        if selected is None or int(workers)<int(selected.split('/j')[1]):
            runtime_by_engine[mode]=variant
    overhead={v:{p:gm([cells[n][v+'/'+p]['relative_time'] for n in data['workloads']])
                 for p in data['profiles']} for v in profiles}
    return {'config':data['config'],'source_manifest_sha256':data['source_manifest_sha256'],
            'workloads':data['workloads'],'samples':data['samples'],'compile_samples':data['compile_samples'],'profiles':profiles,
            'runtime_profiles':list(runtime_by_engine.values()),
            'cells':cells,'relative_time':overhead,'gas_budget':data['gas_budget'],'timeout_ms':data['timeout_ms'],
            'enforcement_checks':len(data['enforcement']),'validation_checks':len(data['validation']),
            'compilation':data['compilation'],'outer_watchdog_s':data['outer_watchdog_s']}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--comprehensive',action='store_true')
    a=p.parse_args();data=json.loads((a.work/'resources.json').read_text());s=summarize(data)
    if a.comprehensive:
        from extended_report import summarize_experiments
        s['extended']=summarize_experiments(a.work,data)
    a.out.mkdir(parents=True,exist_ok=True)
    baseline=a.work/'platform-reference/arm-summary.json'
    if a.comprehensive and baseline.exists():
        s['platform_reference']={'file':'platform-reference/arm-summary.json',
                                 'sha256':hashlib.sha256(baseline.read_bytes()).hexdigest()}
    (a.out/'summary.json').write_text(json.dumps(s,indent=2)+'\n')
    if 'platform_reference' in s:
        comparison=a.out/'platform-comparison';comparison.mkdir(exist_ok=True)
        shutil.copy2(baseline,comparison/'arm-summary.json')
        subprocess.run([sys.executable,str(Path(__file__).with_name('compare_platforms.py')),
                        '--arm',str(comparison/'arm-summary.json'),'--x86',str(a.out/'summary.json'),
                        '--out',str(comparison)],check=True)
    def write_csv(name,rows):
        fields=sorted(set().union(*(r.keys() for r in rows)))
        with (a.out/name).open('w') as f:
            writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader();writer.writerows(rows)
    write_csv('samples.csv',data['measurements'])
    if a.comprehensive:
        def read(name):return json.loads((a.work/name).read_text())
        write_csv('coremark-samples.csv',read('coremark/resources.json')['measurements']+
                  [dict(r,mode='native',profile='none') for r in read('coremark/native-coremark.json')['measurements']])
        write_csv('compiler-preparation.csv',data['compilation']+
                  [dict(r,workload='clang') for r in read('clang-resource-compilation.json')])
        write_csv('compiler-jobs.csv',read('compiler-jobs-native/measurements.json' if s['extended']['clang'].get('paired') else 'clang-resource-measurements.json'))
        if s['extended']['clang'].get('paired'):
            write_csv('compiler-job-summary.csv',[
                dict(zip(['workload','mode','profile'],key.split('/')),**cell)
                for key,cell in sorted(s['extended']['clang']['jobs'].items())])
        write_csv('host-calls.csv',read('boundary.json'))
        directory=s['extended']['service_cohort']['directory']
        write_csv('service-calls.csv',read(directory+'/service-calls/measurements.json'))
        write_csv('runtime-calls.csv',read(directory+'/runtime-calls/measurements.json'))
        write_csv('artifact-sizes.csv',s['extended']['artifact_sizes']['rows'])
    from visual_report import generate
    generate(s,a.out)

if __name__=='__main__':main()
