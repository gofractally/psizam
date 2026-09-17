#!/usr/bin/env python3
"""Require deterministic JIT1 files and correct execution in another process."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def command(args, expected=0):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=30)
    if result.returncode != expected:
        raise RuntimeError(f'{args}: expected {expected}, got {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['compiler', 'runner', 'fixtures', 'arch']:
        parser.add_argument('--'+name, required=True)
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='jit1-saved-') as directory:
        artifact = Path(directory)/'module.pzam'
        for name in ['integer', 'normal-return', 'scalar-f32', 'scalar-f64',
                     'scalar-conversions', 'simd-f32', 'simd-f64', 'simd-mixed-call']:
            for gas in ['off', 'on']:
                for backtrace in [False, True]:
                    compile = [args.compiler, '--target='+args.arch, '--backend=jit1',
                               '--gas-metering='+gas, Path(args.fixtures)/(name+'.wasm'), '-o', artifact]
                    if backtrace:
                        compile.append('--backtrace')
                    command(compile)
                    first = artifact.read_bytes()
                    artifact.unlink()
                    command(compile)
                    if artifact.read_bytes() != first:
                        raise RuntimeError(f'JIT1 artifact depends on compiler process addresses: {name}, {gas}, {backtrace}')
                    result = command([args.runner, artifact], 0 if name == 'normal-return' else 42)
                    if name == 'normal-return' and result.stdout != 'normal return':
                        raise RuntimeError('Guest did not produce its required output')
                    count += 1
        # Invalid return_call_indirect type index must be a compiler rejection,
        # never an uncaught std::out_of_range or a process crash.
        invalid = Path(directory)/'invalid.wasm'
        invalid.write_bytes(bytes.fromhex('0061736d01000000010401600000030201000404017000000a0901070041001301000b'))
        command([args.compiler, '--target='+args.arch, '--backend=jit1', invalid, '-o', artifact], 1)
    print(f'{count} deterministic JIT1 saved-file checks and malformed-module rejection passed')


if __name__ == '__main__':
    main()
