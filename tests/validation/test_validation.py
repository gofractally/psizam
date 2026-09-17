import argparse
from pathlib import Path
import sys
import tempfile
import time
import unittest
from execute import Validation

class ValidationHarnessTests(unittest.TestCase):
    def test_normal_return_requires_guest_output(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            driver = root / 'driver'
            driver.write_text('#!' + sys.executable + '\nraise SystemExit(0)\n')
            driver.chmod(0o755)
            args = argparse.Namespace(work=root, stage='small', mode='jit2-fast',
                                      driver=driver, wasm=root / 'input.wasm',
                                      return_marker='normal return')
            with self.assertRaisesRegex(RuntimeError, 'expected guest output'):
                Validation(args).run()

    def test_native_fault_cannot_satisfy_stack_trap(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            driver = root / 'driver'
            args = argparse.Namespace(work=root, stage='small', mode='jit2-fast',
                                      driver=driver, wasm=root / 'input.wasm', trap='stack overflow')
            def write_driver(message):
                driver.write_text('#!' + sys.executable + '\nimport sys\n'
                                  + 'sys.stderr.write(' + repr(message) + ')\nraise SystemExit(1)\n')
                driver.chmod(0o755)
            write_driver('validation driver: jit control-flow corruption or stack overflow\n')
            with self.assertRaisesRegex(RuntimeError, 'controlled Wasm trap'):
                Validation(args).run()
            write_driver('validation driver: stack overflow\n')
            Validation(args).run()

    def test_success_without_guest_result_is_failure(self):
        with tempfile.TemporaryDirectory() as work:
            gate = Validation(argparse.Namespace(work=Path(work)))
            with self.assertRaises(RuntimeError):
                gate.command('missing-result', [sys.executable, '-c', 'pass'], expected=42)
            gate.command('correct-result', [sys.executable, '-c', 'raise SystemExit(42)'], expected=42)

    def test_compiler_cannot_reuse_stale_artifact(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            compiler = root / 'no-op-compiler'
            compiler.write_text('#!' + sys.executable + '\nraise SystemExit(0)\n')
            compiler.chmod(0o755)
            artifact = root / 'old.pzam'
            artifact.write_bytes(b'stale')
            gate = Validation(argparse.Namespace(work=root, mode='jit2-cli', compiler=compiler, compile_jobs=1))
            with self.assertRaisesRegex(RuntimeError, 'without an artifact'):
                gate.compile(root / 'input.wasm', artifact, 10)
            self.assertFalse(artifact.exists())

    def test_timeout_terminates_child_process_group(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            marker = root / 'should-not-exist'
            child = 'import time,pathlib; time.sleep(1); pathlib.Path(' + repr(str(marker)) + ').touch()'
            parent = 'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",' + repr(child) + ']); time.sleep(30)'
            gate = Validation(argparse.Namespace(work=root))
            with self.assertRaisesRegex(RuntimeError, 'timeout=True'):
                gate.command('timeout', [sys.executable, '-c', parent], timeout=.2)
            time.sleep(1.1)
            self.assertFalse(marker.exists())

    def test_missing_fixtures_fail(self):
        with tempfile.TemporaryDirectory() as work:
            gate = Validation(argparse.Namespace(work=Path(work), data=Path(work)))
            with self.assertRaisesRegex(RuntimeError, 'Missing or mismatched'):
                gate.fixtures()

if __name__ == '__main__':
    unittest.main()
