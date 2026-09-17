"""Verify the paired native/compiler cohort before using any of its timings."""
import hashlib
import json
import statistics
from collections import defaultdict
from pathlib import Path
from compiler_jobs import object_record
from run import sha


def validate_resumption(directory,config,harness,rows):
    if not (directory/'pause-state.json').exists():return None,[]
    pause=json.loads((directory/'pause-state.json').read_text())
    resumption=json.loads((directory/'resumption.json').read_text())
    if not resumption['complete']:raise ValueError('Incomplete compiler resumption')
    if harness.get('resume_compiler_jobs.py')!=resumption['script_sha256']:
        raise ValueError('Resumption harness identity differs')
    count=resumption['accepted_prefix_rows']
    if not 0<count<len(rows):raise ValueError('Invalid compiler checkpoint size')
    prefix=rows[:count]
    digest=hashlib.sha256((json.dumps(prefix,indent=2)+'\n').encode()).hexdigest()
    if digest!=pause['accepted_prefix_sha256']:raise ValueError('Accepted compiler prefix changed')
    resumed=rows[count:]
    if len(resumed)!=resumption['resumed_trials'] or any(r.get('resumption')!=1 for r in resumed):
        raise ValueError('Incorrect resumed compiler trials')
    conditioning=json.loads((directory/'resume-conditioning.json').read_text())
    if len(conditioning)!=resumption['conditioning_runs'] or len(conditioning)!=1+len(config['modes']):
        raise ValueError('Incomplete compiler conditioning')
    if {r['mode'] for r in conditioning}!={'native',*config['modes']}:
        raise ValueError('Missing compiler conditioning mode')
    if any(not r['warmup'] or r['phase']!='resume-conditioning' or r['profile']!='none' for r in conditioning):
        raise ValueError('Invalid compiler conditioning trial')
    return resumption,conditioning


def summarize(work, main, original):
    work=Path(work); directory=work/'compiler-jobs-native'
    if not (directory/'COMPLETE').exists(): raise ValueError('Incomplete native/compiler comparison')
    config=json.loads((directory/'config.json').read_text())
    harness=json.loads((directory/'measurement-source.json').read_text())
    if harness['compiler_jobs.py']!=config['script_sha256']:raise ValueError('Compiler harness identity differs')
    for name,digest in harness.items():
        if sha(directory/'measurement-source'/name)!=digest:raise ValueError('Compiler harness archive changed')
    if config['source_manifest_sha256'] != main['source_manifest_sha256']:
        raise ValueError('Compiler comparison uses a different engine source')
    for tool,record in main['config']['tools'].items():
        if config['parent_files'].get('tools/'+tool) != record['sha256']:
            raise ValueError('Compiler comparison uses a different engine tool: '+tool)
    if config['parent_files']['modules/clang.wasm'] != original['fixtures']['files']['clang.wasm']:
        raise ValueError('Guest compiler changed')
    comp=json.loads((work/'clang-resource-compilation.json').read_text())
    for name,digest in config['parent_files'].items():
        if not name.startswith('modules/clang.') or name.endswith('.wasm'): continue
        mode,threads,profile=name.split('/')[-1].split('.')[1:]
        accepted=[row for row in comp if (row['mode'],row['threads'],row['profile'])==(mode,int(threads[1:]),profile)]
        if not accepted or digest not in {row['artifact_sha256'] for row in accepted if row['valid']}:
            raise ValueError('Compiler artifact provenance mismatch: '+name)
    for name,record in config['inputs'].items():
        if sha(directory/'inputs'/(name+'.ii')) != record['sha256']: raise ValueError('Compiler input changed')
        expected=original['fixtures']['files'].get(name+'.ii')
        if expected and expected!=record['sha256']: raise ValueError('Original compiler fixture changed')
        for mode in ['native','cranelift-aot']:
            obj=object_record(directory/'objects'/(name+'.'+mode+'.o'))
            if obj['sha256']!=config['objects'][name+'/'+mode] or obj['content_sha256']!=config['object_content'][name]:
                raise ValueError('Native/Wasm object equivalence failed')
    preflight=json.loads((directory/'preflight.json').read_text())
    rows=json.loads((directory/'measurements.json').read_text())
    resumption,conditioning=validate_resumption(directory,config,harness,rows)
    expected={(n,m,p) for n in config['inputs'] for m in ['native',*config['modes']]
              for p in (['none'] if m=='native' else config['profiles'])}
    groups=defaultdict(list);warmups=defaultdict(list)
    for row in [*preflight,*conditioning,*rows]:
        if not row['valid'] or row['returncode']!=0 or row['timeout']: raise ValueError('Compiler job failed')
        if sha(directory/row['stdout'])!=row['stdout_sha256']: raise ValueError('Compiler output log changed')
        obj=row['object'];name=row['workload'];mode=row['mode']
        if obj['content_sha256']!=config['object_content'][name]: raise ValueError('Compiler output content differs')
        if obj['sha256']!=config['objects'][name+('/native' if mode=='native' else '/cranelift-aot')]:
            raise ValueError('Compiler raw object hash differs')
    for row in rows:
        key=(row['workload'],row['mode'],row['profile'])
        (warmups if row['warmup'] else groups)[key].append(row)
    if set(groups)!=expected or set(warmups)!=expected: raise ValueError('Missing compiler cells')
    cells={}
    for key,group in groups.items():
        if len(warmups[key])!=config['warmups'] or len(group)!=config['samples']:
            raise ValueError('Incorrect compiler sample count')
        if {row['sample'] for row in group}!=set(range(config['samples'])):
            raise ValueError('Duplicate compiler sample')
        if any(row['sample']!=-1 for row in warmups[key]):raise ValueError('Invalid warmup sample')
        cell={'samples':len(group)}
        for metric in ['wall_s','peak_rss_bytes']:
            values=[row[metric] for row in group]
            if min(values)<=0: raise ValueError('Invalid timing or memory result')
            cell.update({metric:statistics.median(values),metric+'_min':min(values),metric+'_max':max(values)})
        cells['/'.join(key)]=cell
    for key,cell in cells.items():cell['native_slowdown']=cell['wall_s']/cells[key.split('/')[0]+'/native/none']['wall_s']
    return dict(config=config,cells=cells,trials=len(rows),preflight_checks=len(preflight),resumption=resumption,
                fixture_metadata=json.loads((directory/'fixture-metadata.json').read_text()))
