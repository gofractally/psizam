#!/usr/bin/env python3
"""Correctness gates: a crash, timeout, wrong result or missing input is failure."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class Validation:
    def __init__(self, args):
        self.a = args
        args.work.mkdir(parents=True, exist_ok=True)
        self.records = []

    def command(self, name, command, expected=0, timeout=180):
        command = [str(x) for x in command]
        stdout = self.a.work / (name + '.stdout')
        stderr = self.a.work / (name + '.stderr')
        with stdout.open('wb') as out, stderr.open('wb') as err:
            child = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
            timed_out = False
            try:
                code = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                code = 124
        self.records.append(dict(command=command, pid=child.pid, returncode=code,
                                 expected_returncode=expected, timeout=timed_out,
                                 stdout=str(stdout), stderr=str(stderr)))
        (self.a.work / 'commands.json').write_text(json.dumps(self.records, indent=2) + '\n')
        if timed_out or code != expected:
            raise RuntimeError(f'{name}: exit {code}, expected {expected}; timeout={timed_out}\n'
                               + stderr.read_text(errors='replace') + f'\nEvidence: {self.a.work}')

    def fixtures(self):
        manifest = json.loads((HERE / 'compiler-fixtures.json').read_text())
        if not self.a.data or not self.a.data.is_dir():
            raise RuntimeError('Compiler fixtures are required; set PSIZAM_COMPILER_TEST_DATA. See docs/validation.md.')
        for name, checksum in manifest['files'].items():
            path = self.a.data / name
            if not path.is_file() or digest(path) != checksum:
                raise RuntimeError(f'Missing or mismatched compiler fixture: {path}')
        return manifest

    def compile(self, wasm, artifact, timeout):
        artifact.unlink(missing_ok=True)  # Failed compilation cannot reuse old code.
        if self.a.mode.endswith('-api'):
            backend = self.a.mode.split('-')[0] if self.a.mode.startswith(('jit1', 'jit2')) else 'llvm-o2'
            cmd = [self.a.driver, 'compile', backend, wasm, artifact]
        else:
            backend = self.a.mode.split('-')[0] if self.a.mode.startswith(('jit1', 'jit2')) else 'llvm'
            arch = getattr(self.a, 'arch', None) or {'arm64': 'aarch64', 'aarch64': 'aarch64', 'x86_64': 'x86_64', 'AMD64': 'x86_64'}[platform.machine()]
            cmd = [self.a.compiler, '--target=' + arch, '--backend=' + backend, '--opt-level=2', '-j' + str(self.a.compile_jobs)]
            if self.a.mode == 'llvm-softfloat-cli':
                cmd += ['--softfloat']
            cmd += [wasm, '-o', artifact]
        self.command('compile', cmd, timeout=timeout)
        if not artifact.is_file() or not artifact.stat().st_size:
            raise RuntimeError('Compiler reported success without an artifact')

    def fresh(self, wasm, args, dirs=()):
        backend, fp = self.a.mode.rsplit('-', 1)
        return [self.a.driver, 'run', backend, fp, *dirs, wasm, *args]

    def run(self):
        a = self.a
        if a.stage == 'small':
            if a.mode.endswith(('-cli', '-api')):
                artifact = a.work / 'module.pzam'
                self.compile(a.wasm, artifact, 60)
                command = [a.runner, artifact]
            else:
                command = self.fresh(a.wasm, [])
            # Normal-return cases must write their exact marker; an empty
            # successful process is never evidence of guest execution.
            trap = getattr(a, 'trap', None)
            marker = getattr(a, 'return_marker', None)
            self.command('execute', command, expected=1 if trap else 0 if marker else 42)
            if marker and (a.work / 'execute.stdout').read_text() != marker:
                raise RuntimeError('Normal return without the expected guest output')
            if trap:
                diagnostic = (a.work / 'execute.stderr').read_text()
                valid = ('validation driver: ' + trap + '\n' in diagnostic or
                         'psizam error: ' + trap + ' : ' + trap + '\n' in diagnostic)
                if not valid or 'JIT FAULT' in diagnostic:
                    raise RuntimeError('Expected a controlled Wasm trap: ' + trap + '\n' + diagnostic)
            return
        manifest = self.fixtures()
        if a.stage == 'fixtures':
            return
        if a.stage == 'compile-clang':
            self.compile(a.data / 'clang.wasm', a.artifact, 3600)
            (a.artifact.with_suffix('.identity.json')).write_text(json.dumps({
                'mode': a.mode, 'wasm_sha256': digest(a.data / 'clang.wasm'),
                'compiler_sha256': digest(a.compiler), 'driver_sha256': digest(a.driver),
                'artifact_sha256': digest(a.artifact)}, indent=2) + '\n')
            return
        output = a.work / 'result.o'
        output.unlink(missing_ok=True)
        dirs = ['--dir=/fixtures:' + str(a.data), '--dir=/output:' + str(a.work)]
        flags = ['-cc1', '-triple', 'wasm32-unknown-wasi', '-emit-obj', '-O2', '-std=c++17',
                 '-x', 'c++-cpp-output', '/fixtures/' + a.input + '.ii', '-o', '/output/result.o']
        if a.mode.endswith(('-cli', '-api')):
            identity = json.loads(a.artifact.with_suffix('.identity.json').read_text())
            current = {'mode': a.mode, 'wasm_sha256': digest(a.data / 'clang.wasm'),
                       'compiler_sha256': digest(a.compiler), 'driver_sha256': digest(a.driver),
                       'artifact_sha256': digest(a.artifact)}
            if identity != current:
                raise RuntimeError('Stale compiler artifact; rerun its CTest setup fixture')
            command = [a.runner, *dirs, a.artifact, *flags]
        else:
            command = self.fresh(a.data / 'clang.wasm', flags, dirs)
        self.command('compile-input', command)
        expected = manifest['objects'][a.input]
        if not output.is_file() or digest(output) != expected:
            raise RuntimeError(f'Wrong compiler output for {a.input}; expected SHA-256 {expected}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['small', 'fixtures', 'compile-clang', 'clang'])
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--mode')
    parser.add_argument('--arch', choices=['aarch64', 'x86_64'])
    parser.add_argument('--trap', choices=['stack overflow', 'floating point error', 'integer divide by zero'])
    parser.add_argument('--return-marker')
    parser.add_argument('--compile-jobs', type=int, default=1)
    for name in ['driver', 'compiler', 'runner', 'wasm', 'data', 'artifact']:
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--input', choices=['SipHash.cpp', 'APInt.cpp'])
    args = parser.parse_args()
    if args.compile_jobs < 1:
        parser.error('--compile-jobs must be positive')
    args.work = args.work.resolve()
    for name in ['driver', 'compiler', 'runner', 'wasm', 'data', 'artifact']:
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    try:
        Validation(args).run()
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
