#!/usr/bin/env python3
"""Paired native/Wasm Clang jobs, reusing one validated compiler-artifact cohort."""
import argparse
import datetime
import hashlib
import json
import platform
import random
import shutil
from pathlib import Path

from clang_resources import MODES, artifact
from resources import wt_flags
from run import Run, save, sha


def object_content(data):
    """Remove only Clang's producer identity; retain all code and linking data."""
    if data[:8] != b'\0asm\x01\0\0\0':
        raise ValueError('Expected a wasm32 object')
    def leb(offset):
        value = 0
        for shift in range(0, 35, 7):
            if offset >= len(data): raise ValueError('Truncated section length')
            byte = data[offset]; offset += 1; value |= (byte & 127) << shift
            if byte < 128: return value, offset
        raise ValueError('Invalid section length')
    offset = 8; result = bytearray(data[:8]); producers = 0
    while offset < len(data):
        start = offset; kind = data[offset]; length, body = leb(offset + 1)
        end = body + length
        if end > len(data): raise ValueError('Truncated section')
        name = b''
        if kind == 0:
            size, text = leb(body)
            if text + size > end: raise ValueError('Truncated custom section name')
            name = data[text:text + size]
        if kind == 0 and name == b'producers': producers += 1
        else: result.extend(data[start:end])
        offset = end
    if producers != 1: raise ValueError('Expected exactly one producers section')
    return bytes(result)


def object_record(path):
    data = path.read_bytes()
    return dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data),
                content_sha256=hashlib.sha256(object_content(data)).hexdigest())


def command(r, parent, config, mode, profile, name):
    flags = ['-cc1', '-triple', 'wasm32-unknown-wasi', '-emit-obj', '-O2',
             '-std=c++17', '-x', 'c++-cpp-output']
    inputs = r.work/'inputs'; output = r.work/'output'
    workers = config['runtime_workers'] if mode in ['llvm-o2-pzam', 'cranelift-aot'] else 1
    if mode == 'native':
        return [config['native']['path'], *flags, inputs/(name+'.ii'), '-o', output/'result.o']
    flags += ['/fixtures/'+name+'.ii', '-o', '/output/result.o']
    tools = parent.work/'tools'; budget = config['gas_budget']; timeout = config['timeout_ms']
    if mode.startswith('cranelift'):
        return [tools/'wasmtime', 'run', *wt_flags(workers, profile, budget, timeout),
                '--dir', str(inputs)+'::/fixtures', '--dir', str(output)+'::/output',
                '-W', 'max-wasm-stack=16777216',
                *(['--allow-precompiled', artifact(parent, mode, workers, profile)]
                  if mode == 'cranelift-aot' else [parent.work/'modules/clang.wasm']), *flags]
    dirs = ['--dir=/fixtures:'+str(inputs), '--dir=/output:'+str(output)]
    limits = [f'--cpu-limit={profile}', f'--gas-budget={budget}', f'--timeout-ms={timeout}']
    if mode in ['jit1', 'jit2']:
        return [tools/'repro-psizam', 'jit' if mode == 'jit1' else 'jit2', *limits, *dirs,
                parent.work/'modules/clang.wasm', *flags]
    return [tools/'pzam-run', *limits, *dirs, artifact(parent, mode, workers, profile), *flags]


def invoke(r, parent, config, mode, profile, name, label):
    dest = r.work/'output/result.o'; dest.unlink(missing_ok=True)
    r.env['RAYON_NUM_THREADS'] = str(config['runtime_workers'] if mode in ['llvm-o2-pzam','cranelift-aot'] else 1)
    cmd = command(r, parent, config, mode, profile, name)
    cpu = config['execution_cpu']
    def ticks():
        if cpu is None: return None
        line = next(x for x in Path('/proc/stat').read_text().splitlines() if x.startswith('cpu'+str(cpu)+' '))
        fields = list(map(int, line.split()[1:])); return sum(fields[:8]), fields[7]
    before = ticks()
    if cpu is not None: cmd = ['taskset', '-c', str(cpu), *cmd]
    rec, _, _ = r.command(label, cmd, timeout=config['watchdog_s'])
    if before:
        after = ticks(); total = after[0]-before[0]; steal = after[1]-before[1]
        rec.update(execution_cpu=cpu, cpu_total_ticks=total, cpu_steal_ticks=steal,
                   cpu_steal_percent=100*steal/total if total else None)
    rec.update(mode=mode, profile=profile, workload=name)
    rec['object'] = object_record(dest) if rec['returncode'] == 0 and dest.exists() else None
    return rec


def verify_inputs(parent, r, config):
    if sha(parent.work/'source-manifest.json') != config['source_manifest_sha256']:
        raise ValueError('Measured engine source identity changed')
    for name, digest in config['parent_files'].items():
        if sha(parent.work/name) != digest: raise ValueError('Compiler tool/artifact changed: '+name)
    if sha(Path(config['native']['path'])) != config['native']['sha256']:
        raise ValueError('Native compiler changed')
    for item in config['native']['libraries']:
        if sha(Path(item['path'])) != item['sha256']: raise ValueError('Native library changed')
    for name, item in config['inputs'].items():
        if sha(r.work/'inputs'/(name+'.ii')) != item['sha256']: raise ValueError('Input changed: '+name)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage', choices=['prepare','measure'])
    p.add_argument('--work', required=True, type=Path, help='Completed comprehensive measurement directory')
    p.add_argument('--native-cc', type=Path)
    p.add_argument('--native-toolchain', type=Path, help='Download provenance JSON')
    p.add_argument('--extra-fixture', type=Path)
    p.add_argument('--fixture-metadata', type=Path)
    p.add_argument('--cpu', type=int)
    p.add_argument('--samples', type=int, default=5)
    a = p.parse_args(); parent = Run(a.work); directory = parent.work/'compiler-jobs-native'
    if a.stage == 'prepare' and directory.exists(): p.error('Refusing to overwrite compiler cohort')
    r = Run(directory)
    if a.stage == 'prepare':
        if not all([a.native_cc, a.native_toolchain, a.extra_fixture, a.fixture_metadata]):
            p.error('prepare requires native compiler, toolchain provenance and extra fixture metadata')
        if a.samples < 1: p.error('Positive sample count required')
        base = json.loads((parent.work/'clang-resource-config.json').read_text())
        if not json.loads((parent.work/'resources.json').read_text())['complete']:
            raise ValueError('Parent matrix incomplete')
        (r.work/'inputs').mkdir(); (r.work/'output').mkdir(); (r.work/'objects').mkdir()
        for path in (parent.work/'clang-inputs').glob('*.ii'): shutil.copy2(path, r.work/'inputs'/path.name)
        shutil.copy2(a.extra_fixture, r.work/'inputs'/a.extra_fixture.name)
        meta = json.loads(a.fixture_metadata.read_text()); save(r.work/'fixture-metadata.json', meta)
        if sha(a.extra_fixture) != meta['preprocessed_sha256']: raise ValueError('Extra fixture hash mismatch')
        native = a.native_cc.resolve(); version, text, _ = r.command('native-version', [native,'--version'], check=True)
        if '22.1.2' not in text: raise ValueError('Native compiler must match Clang 22.1.2')
        libraries = []
        if platform.system() == 'Linux':
            _, text_ldd, _ = r.command('native-libraries', ['ldd',native], check=True)
            for line in text_ldd.splitlines():
                for token in line.split():
                    if token.startswith('/'):
                        path = Path(token).resolve()
                        libraries.append(dict(path=str(path),sha256=sha(path))); break
        inputs = {path.name[:-3]:dict(sha256=sha(path),bytes=path.stat().st_size,
                                    lines=len(path.read_bytes().splitlines())) for path in (r.work/'inputs').glob('*.ii')}
        files = {'modules/clang.wasm':sha(parent.work/'modules/clang.wasm')}
        for name, item in base['tools'].items():
            files['tools/'+name] = item['sha256']
        for mode in ['jit2-pzam','llvm-o2-pzam','cranelift-aot']:
            for profile in ['none','gas','timeout']:
                workers=base['runtime_workers'] if mode!='jit2-pzam' else 1
                path = artifact(parent,mode,workers,profile)
                saved_profile='none' if profile=='timeout' and mode!='cranelift-aot' else profile
                trials=[row for row in json.loads((parent.work/'clang-resource-compilation.json').read_text())
                        if (row['mode'],row['threads'],row['profile'])==(mode,workers,saved_profile)]
                if len(trials)!=base['compile_samples'] or not all(row['valid'] for row in trials):
                    raise ValueError('Incomplete compiler artifact preparation')
                if sha(path) not in {row['artifact_sha256'] for row in trials}:
                    raise ValueError('Compiler artifact differs from the measured cohort')
                files[str(path.relative_to(parent.work))] = sha(path)
        config = dict(schema=1,date_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      source_manifest_sha256=sha(parent.work/'source-manifest.json'), parent_files=files,
                      native=dict(path=str(native),sha256=sha(native),version=text.strip(),libraries=libraries,
                                  acquisition=json.loads(a.native_toolchain.read_text())),
                      inputs=inputs,modes=MODES,profiles=['none','gas','timeout'],samples=a.samples,warmups=1,
                      runtime_workers=base['runtime_workers'],gas_budget=10_000_000_000_000,
                      timeout_ms=600_000,watchdog_s=900,execution_cpu=a.cpu,
                      script_sha256=sha(Path(__file__)),objects={},object_content={},
                      object_comparison='Raw hashes retained; only producers custom section removed for native/Wasm equivalence.')
        save(r.work/'config.json',config); verify_inputs(parent,r,config)
        preflight=[]
        for name in inputs:
            for mode in ['cranelift-aot','native']:
                rec = invoke(r,parent,config,mode,'none',name,'preflight-'+name+'-'+mode)
                obj = rec['object']; expected = base['fixtures']['objects'].get(name)
                valid = rec['returncode']==0 and obj is not None
                if mode == 'cranelift-aot':
                    valid = valid and (expected is None or obj['sha256']==expected)
                    if valid: config['object_content'][name] = obj['content_sha256']
                else: valid = valid and obj['content_sha256']==config['object_content'].get(name)
                rec['valid']=valid;preflight.append(rec);save(r.work/'preflight.json',preflight)
                if not valid: raise RuntimeError('Native/Wasm object parity failed: '+name+'/'+mode)
                config['objects'][name+'/'+mode] = obj['sha256']
                shutil.copy2(r.work/'output/result.o',r.work/'objects'/(name+'.'+mode+'.o'))
                print('preflight',name,mode,round(rec['wall_s'],3),'PASS',flush=True)
        save(r.work/'config.json',config); (r.work/'PREPARED').touch(); return
    config=json.loads((r.work/'config.json').read_text())
    if not (r.work/'PREPARED').exists(): p.error('Prepare and parity checks must complete')
    target=r.work/'measurements.json'
    if target.exists(): p.error('Refusing to overwrite measurements')
    verify_inputs(parent,r,config)
    cells=[(n,m,profile) for n in config['inputs'] for m in ['native',*config['modes']]
           for profile in (['none'] if m=='native' else config['profiles'])]
    tasks=[(*cell,i) for i in range(-1,config['samples']) for cell in cells]
    random.Random(20260918).shuffle(tasks); tasks.sort(key=lambda x:x[-1]>=0)
    rows=[]
    for name,mode,profile,sample in tasks:
        rec=invoke(r,parent,config,mode,profile,name,f'job-{name}-{mode}-{profile}-{sample}')
        obj=rec['object']; oracle=config['objects'][name+('/native' if mode=='native' else '/cranelift-aot')]
        valid=rec['returncode']==0 and obj is not None and obj['sha256']==oracle and obj['content_sha256']==config['object_content'][name]
        rec.update(sample=sample,warmup=sample<0,valid=valid)
        rows.append(rec); save(target,rows)
        print('job',len(rows),'/',len(tasks),name,mode,profile,sample,round(rec['wall_s'],4),'PASS' if valid else 'FAIL',flush=True)
        if not valid: raise RuntimeError('Compiler output failure; no rankings')
    verify_inputs(parent,r,config); (r.work/'COMPLETE').touch()


if __name__ == '__main__': main()
