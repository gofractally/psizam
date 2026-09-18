#!/usr/bin/env python3
"""Focused PZAM startup diagnostic using a completed compiler_jobs.py cohort."""
import argparse
import datetime
import json
import platform
import random
import re
import shutil
from pathlib import Path

from clang_resources import artifact
from compiler_jobs import invoke, verify_inputs
from run import Run, save, sha


def profile(error):
    phases = {}
    for ms, name in re.findall(r'^\s*([0-9.]+) ms\s+[0-9.]+%\s+(.+)$', error, re.M):
        if name in phases: raise ValueError('Duplicate startup phase: '+name)
        phases[name] = float(ms)
    total = phases.pop('TOTAL (pre-_start)', None)
    if total is None or not phases or abs(sum(phases.values())-total) > 0.3:
        raise ValueError('Incomplete startup profile')
    return dict(startup_ms=total, phases_ms=phases)


class ProfileRun(Run):
    trust = 'none'

    def command(self, label, cmd, **kwargs):
        cmd = list(cmd)
        for i, arg in enumerate(cmd):
            if Path(arg).name == 'pzam-run':
                cmd.insert(i+1, '--trust='+self.trust)
                self.env['PZAM_RUN_PROFILE'] = '1'
                break
        else: self.env.pop('PZAM_RUN_PROFILE', None)
        rec, output, error = super().command(label, cmd, **kwargs)
        if 'PZAM_RUN_PROFILE' in self.env and rec['returncode'] == 0:
            rec.update(profile(error))
            rec['remaining_wall_s'] = rec['wall_s']-rec['startup_ms']/1000
        return rec, output, error


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage', choices=['prepare', 'measure'])
    p.add_argument('--parent', required=True, type=Path)
    p.add_argument('--work', required=True, type=Path)
    p.add_argument('--cxx', type=Path)
    p.add_argument('--boost-include', type=Path, help='Boost include directory (if not on the compiler search path)')
    p.add_argument('--samples', type=int, default=5)
    p.add_argument('--probe-samples', type=int, default=10)
    a = p.parse_args()
    if a.stage == 'prepare' and a.work.exists(): p.error('Work directory must be new')
    parent = Run(a.parent); r = ProfileRun(a.work)
    if a.stage == 'prepare':
        if not a.cxx or min(a.samples, a.probe_samples) < 1: p.error('Need --cxx and positive samples')
        config = json.loads((parent.work/'compiler-jobs-native/config.json').read_text())
        for name in ['inputs', 'output', 'measurement-source']: (r.work/name).mkdir()
        for path in (parent.work/'compiler-jobs-native/inputs').glob('*.ii'):
            shutil.copy2(path, r.work/'inputs'/path.name)
        source = Path(__file__).resolve().parent
        scripts = ['pzam_loading.py','pzam_load_probe.cpp','compiler_jobs.py','clang_resources.py','resources.py','run.py']
        for name in scripts: shutil.copy2(source/name, r.work/'measurement-source'/name)
        config.update(loading_schema=1, date_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      loading_samples=a.samples, probe_samples=a.probe_samples, warmups=1,
                      loading_workloads=['SipHash.cpp', 'LLParser.cpp'],
                      loading_parent=str(parent.work), platform=platform.platform(),
                      measurement_source={name:sha(source/name) for name in scripts},
                      cache_state='Fresh processes, warm filesystem cache; no OS cache flush',
                      probe_scope='Container validation, ownership/selection, anonymous allocation and copy only; no runtime restoration, relocations or execution')
        cxx = a.cxx.resolve()
        rec, version, _ = r.command('probe-compiler-version', [cxx,'--version'], check=True)
        config['probe_compiler'] = dict(path=str(cxx),sha256=sha(cxx),version=version)
        includes = [parent.work/'source/psizam/include', parent.work/'source/psio/cpp/include']
        # Hash every header supplied to the probe, including transitive definitions.
        config['probe_headers'] = {str(path.relative_to(parent.work)):sha(path)
                                   for inc in includes for path in inc.rglob('*') if path.is_file()}
        boost = a.boost_include.resolve() if a.boost_include else Path('/usr/include')
        config['boost_version_header'] = dict(path=str(boost/'boost/version.hpp'),sha256=sha(boost/'boost/version.hpp'))
        cmd = [cxx,'--driver-mode=g++','-std=c++23','-O3','-DNDEBUG','-DPSIO_EXCEPTIONS_ENABLED=1',*[f'-I{x}' for x in includes],
               *([f'-I{boost}'] if a.boost_include else []),
               r.work/'measurement-source/pzam_load_probe.cpp','-o',r.work/'pzam-load-probe']
        r.command('build-probe',cmd,timeout=180,check=True)
        config['probe_sha256'] = sha(r.work/'pzam-load-probe')
        save(r.work/'config.json',config)
        verify_inputs(parent,r,config)
        (r.work/'PREPARED').touch(); print('PREPARED',r.work,flush=True); return
    if not (r.work/'PREPARED').exists(): p.error('prepare first')
    if (r.work/'probe.json').exists() or (r.work/'jobs.json').exists(): p.error('Refusing to overwrite measurements')
    config = json.loads((r.work/'config.json').read_text())
    if str(parent.work) != config['loading_parent']: p.error('Parent mismatch')
    def verify():
        verify_inputs(parent,r,config)
        if sha(r.work/'pzam-load-probe') != config['probe_sha256']: raise ValueError('Probe changed')
        for name, digest in config['measurement_source'].items():
            if sha(r.work/'measurement-source'/name) != digest: raise ValueError('Harness changed')
        for path,digest in config['probe_headers'].items():
            if sha(parent.work/path) != digest: raise ValueError('Probe header changed')
    verify(); rows=[]; identities={}
    variants=['owned-double','owned-single','view-double','view-single']
    tasks=[(mode,variant,sample) for sample in range(-1,config['probe_samples'])
           for mode in ['jit2-pzam','llvm-o2-pzam'] for variant in variants]
    random.Random(2026091801).shuffle(tasks);tasks.sort(key=lambda x:x[-1]>=0)
    for mode,variant,sample in tasks:
        workers=config['runtime_workers'] if mode=='llvm-o2-pzam' else 1
        path=artifact(parent,mode,workers,'none')
        cmd=[r.work/'pzam-load-probe',path,variant]
        if config['execution_cpu'] is not None: cmd=['taskset','-c',str(config['execution_cpu']),*cmd]
        rec,out,_=r.command(f'probe-{mode}-{variant}-{sample}',cmd,check=True)
        value=json.loads(out); rec.update(value,mode=mode,sample=sample,warmup=sample<0)
        identity={key:value[key] for key in ['file_bytes','code_bytes','code_file_offset','sections','functions','relocations','code_fnv1a64']}
        if identity != identities.setdefault(mode,identity): raise ValueError('Probe byte/section mismatch')
        rows.append(rec);save(r.work/'probe.json',rows)
    print('PROBE COMPLETE',len(rows),'PASS',flush=True)
    tasks=[(name,mode,trust,sample) for sample in range(-1,config['loading_samples'])
           for name in config['loading_workloads'] for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']
           for trust in (['none','full'] if mode.endswith('pzam') else ['n/a'])]
    random.Random(2026091802).shuffle(tasks);tasks.sort(key=lambda x:x[-1]>=0);rows=[]
    for name,mode,trust,sample in tasks:
        r.trust=trust
        rec=invoke(r,parent,config,mode,'none',name,f'job-{name}-{mode}-{trust}-{sample}')
        obj=rec['object']
        valid=(rec['returncode']==0 and obj is not None and
               obj['sha256']==config['objects'][name+'/cranelift-aot'] and
               obj['content_sha256']==config['object_content'][name])
        rec.update(trust=trust,sample=sample,warmup=sample<0,valid=valid)
        rows.append(rec);save(r.work/'jobs.json',rows)
        print('job',len(rows),'/',len(tasks),name,mode,trust,sample,round(rec['wall_s'],4),
              'startup_ms',rec.get('startup_ms'),'PASS' if valid else 'FAIL',flush=True)
        if not valid: raise RuntimeError('Compiler object mismatch')
    verify(); (r.work/'COMPLETE').touch(); print('COMPLETE',r.work,flush=True)


if __name__ == '__main__': main()
