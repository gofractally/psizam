#!/usr/bin/env python3
"""Reject watchdog kills/crashes; require the engine's exact resource trap."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def checked(cmd, code=0, marker=None):
    p = subprocess.run(list(map(str, cmd)), capture_output=True, text=True, timeout=15)
    if p.returncode != code or (marker and marker not in p.stdout + p.stderr):
        raise RuntimeError(f'{cmd}: expected {code}, {marker!r}; got {p.returncode}\n{p.stdout}\n{p.stderr}')
    return p


def main():
    p = argparse.ArgumentParser()
    for arg in ['runner', 'compiler', 'pzam-run', 'wat2wasm', 'arch', 'llvm']:
        p.add_argument('--'+arg, required=True)
    a = p.parse_args()
    modes = [('jit', False), ('jit1', True), ('jit2', False), ('jit2', True)]
    if a.llvm == 'ON': modes += [('llvm', False), ('llvm', True)]
    count = 0
    with tempfile.TemporaryDirectory(prefix='psizam-resource-') as temp:
        work = Path(temp)
        sources = {
            'finite': Path(__file__).with_name('normal-return.wat').read_text(),
            'loop': '(module (func (export "_start") (loop $L br $L)))',
            'start': '(module (func $init (loop $L br $L)) (start $init) (func (export "_start")))',
            'tail': '(module (func $f (export "_start") return_call $f))',
        }
        for name, text in sources.items():
            (work/(name+'.wat')).write_text(text)
            checked([a.wat2wasm, '--enable-tail-call', work/(name+'.wat'), '-o', work/(name+'.wasm')])
        for mode, saved in modes:
            for profile in ['none', 'gas', 'timeout']:
                policy = profile
                metered = policy in ['gas', 'timeout-poll']
                for name in sources:
                    if profile == 'none' and name != 'finite': continue
                    # This fixture can hit the independent call-depth limit before
                    # a wall deadline; use it to test deterministic gas charging.
                    if name == 'tail' and profile != 'gas': continue
                    module = work/(name+'.wasm')
                    if saved:
                        dest = work/(name+'.'+mode+'.'+profile+'.pzam')
                        checked([a.compiler, '--target='+a.arch, '--backend='+mode, '-j1',
                                 '--gas-metering='+('on' if metered else 'off'), module, '-o', dest])
                        cmd = [a.pzam_run]; module = dest
                    else: cmd = [a.runner, mode]
                    cmd += ['--cpu-limit='+policy, '--gas-budget='+('1000000' if name == 'finite' else '10'),
                            '--timeout-ms='+('1000' if name == 'finite' else '20'), module]
                    if name == 'finite':
                        result = checked(cmd, marker='normal return')
                        if not saved and not metered and 'BENCH_GAS_CONSUMED=0\n' not in result.stderr:
                            raise RuntimeError('unmetered code updated gas counter')
                    else:
                        checked(cmd, 75 if profile == 'gas' else 76, 'RESOURCE_LIMIT='+profile)
                    count += 1
                if saved:
                    # A runtime budget must never be accepted on uninstrumented code.
                    if profile == 'none':
                        checked([a.pzam_run, '--cpu-limit=gas', work/('finite.'+mode+'.none.pzam')], 2,
                                'instrumentation does not match')
                        count += 1
        print(f'{count} resource-limit checks passed')

if __name__ == '__main__': main()
