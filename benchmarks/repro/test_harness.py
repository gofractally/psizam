import sys
import tempfile
import time
import unittest
from pathlib import Path
from run import Run

class HarnessTests(unittest.TestCase):
    def test_failures_are_not_successful_timings(self):
        with tempfile.TemporaryDirectory() as work:
            r=Run(work)
            record,_,_=r.command('nonzero',[sys.executable,'-c','raise SystemExit(7)'])
            self.assertEqual(record['returncode'],7)
            with self.assertRaises(RuntimeError):
                r.command('checked',[sys.executable,'-c','raise SystemExit(7)'],check=True)
    def test_timeout_kills_process_group(self):
        with tempfile.TemporaryDirectory() as work:
            record,_,_=Run(work).command('timeout',[sys.executable,'-c','import time; time.sleep(30)'],timeout=.15)
            self.assertTrue(record['timeout'])
            self.assertEqual(record['returncode'],124)
            self.assertLess(record['wall_s'],3)
    def test_phase_times_and_overwrite_protection(self):
        with tempfile.TemporaryDirectory() as work:
            r=Run(work)
            record,_,_=r.command('phase',[sys.executable,'-c','import sys; print("BENCH_TIME_NS=123000000",file=sys.stderr)'])
            self.assertEqual(record['guest_s'],.123)
            with self.assertRaises(RuntimeError):r.command('phase',[sys.executable,'-c','pass'])

if __name__=='__main__':unittest.main()

class ResourceTests(unittest.TestCase):
    def test_process_kill_is_not_resource_enforcement(self):
        from resources import acceptable_limit
        self.assertFalse(acceptable_limit({'returncode':124,'timeout':True},'RESOURCE_LIMIT=timeout','jit1','timeout'))
        self.assertFalse(acceptable_limit({'returncode':-11,'timeout':False},'RESOURCE_LIMIT=gas','jit2','gas'))
        self.assertTrue(acceptable_limit({'returncode':76,'timeout':False},'RESOURCE_LIMIT=timeout','llvm-jit','timeout'))
    def test_none_is_separate_codegen(self):
        from resources import wt_flags,compile_command
        with tempfile.TemporaryDirectory() as work:
            r=Run(work)
            self.assertIn('--gas-metering=off',compile_command(r,'a','llvm-o2-pzam',18,'none',100,1000))
            self.assertIn('--gas-metering=off',compile_command(r,'a','llvm-o2-pzam',18,'timeout',100,1000))
            self.assertIn('--gas-metering=on',compile_command(r,'a','llvm-o2-pzam',18,'gas',100,1000))
        self.assertNotIn('fuel',str(wt_flags(1,'none',100,1000)))
        self.assertNotIn('epoch-interruption',str(wt_flags(1,'none',100,1000)))
    def test_report_rejects_incomplete_or_failed_state(self):
        from report import summarize
        with self.assertRaises(ValueError):summarize({'schema':1,'complete':False})
        with self.assertRaises(ValueError):summarize({'schema':1,'complete':True,'enforcement':[{'valid':False}], 'validation':[], 'measurements':[]})
