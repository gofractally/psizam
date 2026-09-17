"""Checks that code-size and source evidence survive independent replay."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from artifact_sizes import elf_sizes
from resources import freeze_source
from provenance import verified_manifest
from package import omit_native_artifacts


class ArtifactEvidenceTests(unittest.TestCase):
    def test_package_preserves_source_folder_named_like_binary(self):
        with TemporaryDirectory() as temp:
            root=Path(temp)
            (root/'call-boundary').mkdir()
            (root/'call-boundary/CMakeLists.txt').write_text('project(calls)\n')
            (root/'call-runtime').write_bytes(b'executable')
            (root/'test.pzam').write_bytes(b'cache')
            (root/'run.py').write_text('print(42)\n')
            self.assertEqual(set(omit_native_artifacts(root, [p.name for p in root.iterdir()])),
                             {'call-runtime', 'test.pzam'})

    def test_executable_padding_and_aliased_functions(self):
        def section(index, name, flags, size):
            return {'Section':dict(Index=index,Name={'Name':name},Flags={'Value':flags},
                Size=size,Address=0,AddressAlignment=16384)}
        def symbol(section, start, size):
            return {'Symbol':dict(Section={'Value':section},Type={'Value':2},Value=start,Size=size)}
        # Two symbols alias one body, a third overlaps its tail, and a fourth
        # lives in data. Padding belongs to the region but not body ranges.
        result=elf_sizes([dict(Sections=[section(1,'.text',4,16384),section(2,'.data',2,8192)],
            Symbols=[symbol(1,0,100),symbol(1,0,100),symbol(1,80,40),symbol(2,0,500)])])
        self.assertEqual(result['native_payload_bytes'],16384)
        self.assertEqual(result['entry_range_bytes'],120)

    def test_function_cannot_escape_executable_section(self):
        section={'Section':dict(Index=1,Name={'Name':'.text'},Flags={'Value':4},Size=64,Address=0,AddressAlignment=16)}
        symbol={'Symbol':dict(Section={'Value':1},Type={'Value':2},Value=32,Size=64)}
        with self.assertRaisesRegex(ValueError,'extends beyond'):
            elf_sizes([dict(Sections=[section],Symbols=[symbol])])

    def test_refreeze_archive_then_detect_source_corruption(self):
        with TemporaryDirectory() as temp:
            root=Path(temp);config={}
            for repo in ['psizam','psio']:
                source=root/repo;source.mkdir()
                (source/'code.hpp').write_text('int answer = 42;\n')
                (source/'BENCHMARK_SOURCE_REVISION.json').write_text(json.dumps(dict(commit='test',source='input archive')))
                config[repo]=dict(path=str(source),commit='test')
            first=root/'first';first.mkdir()
            digest=freeze_source(SimpleNamespace(work=first,config=config))
            manifest=verified_manifest(first,digest)
            second=root/'second';second.mkdir()
            refrozen=freeze_source(SimpleNamespace(work=second,config={repo:dict(path=str(first/'source'/repo),commit='test') for repo in config}))
            self.assertEqual(verified_manifest(second,refrozen),manifest)
            (second/'source/psizam/code.hpp').write_text('int answer = 43;\n')
            with self.assertRaisesRegex(ValueError,'Frozen source changed'):
                verified_manifest(second,refrozen)


if __name__=='__main__':unittest.main()
