import tempfile
import unittest
from pathlib import Path
from compiler_jobs import object_content
from compiler_job_report import validate_resumption
from resume_compiler_jobs import checked_prefix
from run import save,sha


class ObjectEquivalence(unittest.TestCase):
    def module(self,producer=b'clang',code=b'\x01\x02'):
        custom=b'\x09producers'+producer
        return b'\0asm\x01\0\0\0'+bytes([10,len(code)])+code+bytes([0,len(custom)])+custom

    def test_producer_identity_does_not_change_comparison(self):
        self.assertEqual(object_content(self.module(b'native release')),object_content(self.module(b'WASI fork')))

    def test_generated_code_is_never_ignored(self):
        self.assertNotEqual(object_content(self.module(code=b'\x01\x02')),object_content(self.module(code=b'\x01\x03')))

    def test_rejects_missing_duplicate_and_truncated_sections(self):
        valid=self.module()
        for bad in [b'',valid[:8],valid[:-1],valid+b'\x00\x0a\x09producers',valid[:8]+b'\x00\x80']:
            with self.subTest(data=bad),self.assertRaises(ValueError):object_content(bad)


class CheckpointIntegrity(unittest.TestCase):
    def test_only_intact_prefix_can_resume(self):
        task=('input','native','none',0);next_task=('input','jit2','gas',0)
        row=dict(zip(['workload','mode','profile','sample'],task),valid=True,warmup=False)
        self.assertEqual(checked_prefix([row],[task,next_task]),[next_task])
        for rows in [[row,row],[dict(row,valid=False)],[dict(row,warmup=True)],[dict(row,sample=1)]]:
            with self.subTest(rows=rows),self.assertRaises(ValueError):checked_prefix(rows,[task,next_task])

    def test_report_rejects_changed_prefix_and_missing_conditioning(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);prefix=[dict(wall_s=1.5)]
            save(directory/'prefix.json',prefix)
            save(directory/'pause-state.json',dict(accepted_prefix_sha256=sha(directory/'prefix.json')))
            save(directory/'resumption.json',dict(complete=True,script_sha256='digest',accepted_prefix_rows=1,
                                                 resumed_trials=1,conditioning_runs=2))
            conditioning=[dict(mode=m,warmup=True,phase='resume-conditioning',profile='none') for m in ['native','jit2']]
            save(directory/'resume-conditioning.json',conditioning)
            config=dict(modes=['jit2']);harness={'resume_compiler_jobs.py':'digest'}
            rows=prefix+[dict(wall_s=2,resumption=1)]
            self.assertEqual(validate_resumption(directory,config,harness,rows)[1],conditioning)
            with self.assertRaisesRegex(ValueError,'prefix changed'):
                validate_resumption(directory,config,harness,[dict(wall_s=1.4),rows[1]])
            save(directory/'resume-conditioning.json',conditioning[:1])
            with self.assertRaisesRegex(ValueError,'Incomplete compiler conditioning'):
                validate_resumption(directory,config,harness,rows)


if __name__=='__main__':unittest.main()
