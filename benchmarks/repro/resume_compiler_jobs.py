#!/usr/bin/env python3
"""Resume an intact compiler-job prefix after an external benchmark window.

An unpublished boundary trial is excluded and repeated under a new log label.
Seven extra conditioning runs warm the execution modes; they are not samples.
No paused or suspended child process is used as a valid timing.
"""
import argparse
import datetime
import json
import random
from pathlib import Path
from compiler_jobs import invoke, verify_inputs
from run import Run, save, sha


def tasks(config):
    cells=[(n,m,p) for n in config['inputs'] for m in ['native',*config['modes']]
           for p in (['none'] if m=='native' else config['profiles'])]
    result=[(*cell,i) for i in range(-1,config['samples']) for cell in cells]
    random.Random(20260918).shuffle(result);result.sort(key=lambda x:x[-1]>=0)
    return result


def checked_prefix(rows, planned):
    if len(rows)>len(planned):raise ValueError('Too many prior trials')
    for row,expected in zip(rows,planned):
        actual=tuple(row[k] for k in ['workload','mode','profile','sample'])
        if actual!=expected or not row['valid'] or row['warmup']!=(row['sample']<0):
            raise ValueError('Prior measurements are not an intact valid prefix')
    return planned[len(rows):]


def accepted(row, config):
    obj=row['object'];name=row['workload'];mode=row['mode']
    return (row['returncode']==0 and not row['timeout'] and obj is not None and
            obj['sha256']==config['objects'][name+('/native' if mode=='native' else '/cranelift-aot')] and
            obj['content_sha256']==config['object_content'][name])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True)
    a=p.parse_args();parent=Run(a.work);r=Run(parent.work/'compiler-jobs-native')
    if (r.work/'COMPLETE').exists():p.error('Cohort already complete')
    target=r.work/'measurements.json';config=json.loads((r.work/'config.json').read_text())
    pause=json.loads((r.work/'pause-state.json').read_text())
    if sha(target)!=pause['accepted_prefix_sha256']:raise ValueError('Paused prefix changed')
    if (r.work/'resumption.json').exists():p.error('This checkpoint already has a resumption attempt')
    rows=json.loads(target.read_text());planned=tasks(config);remaining=checked_prefix(rows,planned)
    verify_inputs(parent,r,config)
    archived=r.work/'measurement-source'/Path(__file__).name
    if not archived.exists() or sha(archived)!=sha(Path(__file__)):
        raise ValueError('Archive the resumption script before running it')
    state=dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
               accepted_prefix_rows=len(rows),resumed_trials=len(remaining),conditioning_runs=7,
               script_sha256=sha(Path(__file__)),complete=False,
               description='Two serial measurement segments with unchanged engines and inputs. An unpublished boundary trial is excluded and repeated; seven post-window conditioning runs are excluded.')
    save(r.work/'resumption.json',state)
    conditioning=[];smallest=min(config['inputs'],key=lambda n:config['inputs'][n]['bytes'])
    for mode in ['native',*config['modes']]:
        rec=invoke(r,parent,config,mode,'none',smallest,'resume-conditioning-'+mode)
        rec.update(valid=accepted(rec,config),warmup=True,phase='resume-conditioning')
        conditioning.append(rec);save(r.work/'resume-conditioning.json',conditioning)
        if not rec['valid']:raise RuntimeError('Resume conditioning output failure')
        print('conditioning',mode,'PASS',flush=True)
    for name,mode,profile,sample in remaining:
        rec=invoke(r,parent,config,mode,profile,name,f'resume-job-{name}-{mode}-{profile}-{sample}')
        rec.update(sample=sample,warmup=sample<0,valid=accepted(rec,config),resumption=1)
        rows.append(rec);save(target,rows)
        print('job',len(rows),'/',len(planned),name,mode,profile,sample,round(rec['wall_s'],4),'PASS' if rec['valid'] else 'FAIL',flush=True)
        if not rec['valid']:raise RuntimeError('Compiler output failure; no rankings')
    verify_inputs(parent,r,config)
    state.update(complete=True,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    save(r.work/'resumption.json',state);(r.work/'COMPLETE').touch()


if __name__=='__main__':main()
