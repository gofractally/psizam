#!/usr/bin/env python3
"""Untimed sizes of PZAM and Cranelift AOT artifacts; never execute guests."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
from run import sha, save, source_identity
from resources import freeze_source
from types import SimpleNamespace


def elf_sizes(document):
    if len(document) != 1:
        raise ValueError('Expected one ELF file')
    sections = [s['Section'] for s in document[0]['Sections']]
    executable = {s['Index']: s for s in sections if s['Flags']['Value'] & 4}
    if not executable:
        raise ValueError('No executable sections in Cranelift artifact')
    ranges = {}
    functions = 0
    for wrapper in document[0]['Symbols']:
        symbol = wrapper['Symbol']
        section = symbol['Section']['Value']
        if symbol['Type']['Value'] != 2 or section not in executable or symbol['Size'] == 0:
            continue
        start, end = symbol['Value'], symbol['Value'] + symbol['Size']
        limit = executable[section]
        if start < limit['Address'] or end > limit['Address'] + limit['Size']:
            raise ValueError('Function extends beyond its executable section')
        ranges.setdefault(section, []).append((start, end))
        functions += 1
    # Aliases must not count a body twice.
    body_bytes = 0
    for intervals in ranges.values():
        end = -1
        for left, right in sorted(intervals):
            body_bytes += max(0, right - max(left, end))
            end = max(end, right)
    if body_bytes == 0:
        raise ValueError('No sized native functions in Cranelift symbol table')
    return dict(native_payload_bytes=sum(s['Size'] for s in executable.values()),
                entry_range_bytes=body_bytes, functions=functions,
                executable_sections=[dict(name=s['Name']['Name'], bytes=s['Size'],
                    alignment=s['AddressAlignment']) for s in executable.values()])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=pathlib.Path, required=True)
    p.add_argument('--probe', type=pathlib.Path, required=True)
    p.add_argument('--readobj', type=pathlib.Path, required=True)
    p.add_argument('--output', default='artifact-sizes.json')
    p.add_argument('--jit1-directory', default='jit1-saved')
    p.add_argument('--jit1-compiler', type=pathlib.Path, required=True)
    p.add_argument('--jit1-source', type=pathlib.Path, required=True)
    p.add_argument('--psio-source', type=pathlib.Path, required=True)
    a = p.parse_args(); work = a.work.resolve(); output = work/a.output
    if output.exists(): raise ValueError('Refusing to overwrite size evidence')
    rows, commands = [], []
    jit1_work = work/a.jit1_directory
    if jit1_work.exists(): raise ValueError('Use a fresh JIT1 saved-artifact directory')
    jit1_work.mkdir()
    config = {}
    for repo, path in [('psizam', a.jit1_source.resolve()), ('psio', a.psio_source.resolve())]:
        config[repo] = source_identity(path)
    jit1_manifest = freeze_source(SimpleNamespace(work=jit1_work, config=config))
    jit1_provenance = dict(config=config, source_manifest_sha256=jit1_manifest,
        compiler_sha256=sha(a.jit1_compiler), directory=a.jit1_directory)
    machine = json.loads((work/'resources.json').read_text())['config']['machine']
    arch = {'arm64':'aarch64', 'aarch64':'aarch64', 'x86_64':'x86_64'}[machine]
    def run(command):
        command = [str(v) for v in command]
        result = subprocess.run(command, capture_output=True, check=True, timeout=300)
        commands.append(dict(command=command, stdout_sha256=hashlib.sha256(result.stdout).hexdigest()))
        return json.loads(result.stdout)
    # Inspect all worker variants but leave worker counts out of headline size charts.
    pattern = re.compile(r'(.+)\.(jit2-pzam|llvm-o2-pzam|cranelift-aot)\.j(\d+)\.(none|gas|timeout)$')
    for directory in [work, work/'coremark']:
        if not (directory/'modules').exists(): continue
        known = json.loads((directory/'resources.json').read_text())['workloads']
        if directory == work and (directory/'modules/clang.wasm').exists(): known.append('clang')
        scope = str(directory.relative_to(work))
        for artifact in sorted((directory/'modules').iterdir()):
            match = pattern.fullmatch(artifact.name)
            if not match or match[1] not in known: continue
            name, mode, workers, profile = match.groups()
            if mode == 'cranelift-aot':
                values = elf_sizes(run([a.readobj, '--sections', '--symbols', '--elf-output-style=JSON', artifact]))
            else:
                decoded = run([a.probe, 'pzam', artifact])
                if decoded['artifact_bytes'] != artifact.stat().st_size or len(decoded['sections']) != 1:
                    raise ValueError('Expected a single-section benchmark PZAM')
                values = decoded['sections'][0]
                if values['gas_metering'] != (profile == 'gas'):
                    raise ValueError('Incorrect PZAM metering identity')
            size = artifact.stat().st_size
            if not 0 < values['entry_range_bytes'] <= values['native_payload_bytes'] <= size:
                raise ValueError('Inconsistent code-size measurements: '+str(artifact))
            rows.append(dict(workload=name, mode=mode, threads=int(workers), profile=profile,
                scope=scope, artifact=str(artifact.relative_to(work)), artifact_sha256=sha(artifact),
                artifact_bytes=size, wasm_bytes=(directory/'modules'/(name+'.wasm')).stat().st_size,
                **values))
            print('artifact size', name, mode, workers, profile, size, flush=True)
        for name in known:
            wasm = directory/'modules'/(name+'.wasm')
            dest_dir = jit1_work/('main' if scope == '.' else scope)
            dest_dir.mkdir(exist_ok=True)
            for profile in ['none', 'gas']:
                artifact = dest_dir/(name+'.'+profile+'.pzam')
                command = [str(a.jit1_compiler), '--target='+arch, '--backend=jit1', '-j1',
                    '--gas-metering='+('on' if profile == 'gas' else 'off'), str(wasm), '-o', str(artifact)]
                result = subprocess.run(command, capture_output=True, check=True, timeout=3600)
                commands.append(dict(command=command, stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                    stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
                decoded = run([a.probe, 'pzam', artifact])
                if len(decoded['sections']) != 1: raise ValueError('Expected one JIT1 section')
                values = decoded['sections'][0]
                if values['tier'] != 0 or values['gas_metering'] != (profile == 'gas'):
                    raise ValueError('Incorrect JIT1 artifact identity')
                size = artifact.stat().st_size
                if not 0 < values['entry_range_bytes'] <= values['native_payload_bytes'] <= size:
                    raise ValueError('Invalid JIT1 code sizes')
                rows.append(dict(workload=name, mode='jit1', threads=1, profile=profile,
                    scope=scope, wasm_bytes=wasm.stat().st_size, wasm_sha256=sha(wasm),
                    artifact=str(artifact.relative_to(work)), artifact_sha256=sha(artifact),
                    artifact_bytes=size, **values))
                print('JIT1 saved size', name, profile, size, flush=True)
    save(output, dict(schema=2, jit1_provenance=jit1_provenance, probe_sha256=sha(a.probe), readobj_sha256=sha(a.readobj),
        probe_source_sha256=sha(pathlib.Path(__file__).with_suffix('.cpp')),
        script_sha256=sha(__file__), source_manifest_sha256=sha(work/'source-manifest.json'),
        rows=rows, commands=commands,
        definitions={
            'artifact_bytes':'Logical file length; includes native code, metadata, relocations, data and padding.',
            'native_payload_bytes':'PZAM native code blob or ELF executable sections; includes stubs, constants and alignment padding.',
            'entry_range_bytes':'Diagnostic only, not an engine comparison: PZAM sums entry ranges (LLVM entries cover wrappers, not every body); ELF sums the union of sized STT_FUNC ranges. Headline charts use complete native code regions.',
            'jit1':'Actual CLI JIT1 PZAM output for the same Wasm inputs. Writer build and frozen sources are recorded separately from the timed compute cohort. No guest execution.'}))
    print(output)

if __name__ == '__main__': main()
