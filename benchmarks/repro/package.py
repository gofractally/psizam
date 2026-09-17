#!/usr/bin/env python3
"""Package the exact measured source snapshot and raw evidence (no native binaries)."""
import argparse
import json
import shutil
import tarfile
from pathlib import Path
from run import sha,save

def omit_native_artifacts(directory, names):
    """Exclude built artifacts while preserving equally named source folders."""
    return [name for name in names
            if (Path(directory)/name).is_file()
            and (name in {'call-boundary', 'call-runtime'} or name.endswith('.pzam'))]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--comprehensive',action='store_true')
    a=p.parse_args();w=a.work.resolve();out=a.out.resolve();kit=out/'reproduction-kit'
    data=json.loads((w/'resources.json').read_text())
    if not data.get('complete'):raise ValueError('Cannot package an incomplete matrix')
    manifest=json.loads((w/'source-manifest.json').read_text())
    for repo,files in manifest.items():
        for name,digest in files.items():
            if sha(w/'source'/repo/name)!=digest:raise ValueError('Frozen source changed: '+repo+'/'+name)
    if kit.exists():raise ValueError('Refusing to overwrite kit')
    shutil.copytree(w/'source',kit/'source');e=kit/'evidence';e.mkdir()
    for name,digest in data['config'].get('orchestration',{}).items():
        source=w/name
        if sha(source)!=digest:raise ValueError('Orchestration script changed: '+name)
        directory=kit/'orchestration';directory.mkdir(exist_ok=True)
        shutil.copy2(source,directory/name)
    for item in w.iterdir():
        if item.is_file() and item.suffix in ['.json','.jsonl']:shutil.copy2(item,e/item.name)
    for name in ['logs','sources','validation-evidence','platform-config','platform-reference']:
        if (w/name).exists():shutil.copytree(w/name,e/name)
    (e/'modules').mkdir()
    for item in (w/'modules').glob('*.wasm'):
        if a.comprehensive and item.name=='clang.wasm':continue
        shutil.copy2(item,e/'modules'/item.name)
    if a.comprehensive:
        from extended_report import summarize_experiments
        summary=summarize_experiments(w,data)
        ce=e/'coremark';ce.mkdir()
        for item in (w/'coremark').iterdir():
            if item.is_file() and item.suffix in ['.json','.jsonl']:shutil.copy2(item,ce/item.name)
        shutil.copytree(w/'coremark/logs',ce/'logs')
        (ce/'modules').mkdir()
        for item in (w/'coremark/modules').glob('*.wasm'):shutil.copy2(item,ce/'modules'/item.name)
        fixtures=e/'compiler-fixtures';fixtures.mkdir()
        shutil.copy2(w/'modules/clang.wasm',fixtures/'clang.wasm')
        for item in (w/'clang-inputs').glob('*.ii'):shutil.copy2(item,fixtures/item.name)
        harness=kit/'experiment-source';harness.mkdir()
        for item in Path(__file__).parent.iterdir():
            if item.is_file():shutil.copy2(item,harness/item.name)
        directory=summary['service_cohort']['directory']
        shutil.copytree(w/directory,e/directory,ignore=omit_native_artifacts)
        sizes=summary['artifact_sizes'];directory=sizes['jit1_provenance']['directory']
        (e/directory).mkdir()
        shutil.copytree(w/directory/'source',e/directory/'source')
        shutil.copy2(w/directory/'source-manifest.json',e/directory/'source-manifest.json')
        shutil.copytree(w/'release-validation',e/'release-validation')
        size_file=next(w/name for name in ['artifact-sizes.json','artifact-sizes-jit1.json'] if (w/name).exists())
        save(e/'packaged-evidence.json',dict(
            description='Inspected native artifacts omitted; hashes and measured byte counts retained. Verify kit MANIFEST.json before regeneration.',
            size_evidence_sha256=sha(size_file),
            omitted_native_artifacts=sorted({r['artifact'] for r in sizes['rows']})))
        engineering=kit/'engineering';engineering.mkdir()
        for name in ['host-call-performance.md','runtime-release-review.md']:
            shutil.copy2(Path(__file__).resolve().parents[2]/'docs'/name,engineering/name)
    for name in ['summary.json','psizam-cranelift-report.pdf','report.html','charts.json',
                 *sorted(p.name for p in out.glob('*.csv')),*json.loads((out/'charts.json').read_text())]:
        (kit/name).parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(out/name,kit/name)
    if (out/'platform-comparison').exists():
        shutil.copytree(out/'platform-comparison',kit/'platform-comparison',dirs_exist_ok=True)
    shutil.copy2(Path(__file__).parent/('COMPREHENSIVE.md' if a.comprehensive else 'README.md'),kit/'README.md')
    # Report presentation may evolve independently of the frozen measured engine.
    renderer=kit/'report-source';renderer.mkdir()
    for name in ['report.py','visual_report.py','extended_report.py','service_report.py','size_report.py','provenance.py','run.py','compare_platforms.py','sources.lock.json']:
        shutil.copy2(Path(__file__).parent/name,renderer/name)
    (renderer/'README.md').write_text('Requires Python 3.11 or newer, matplotlib and reportlab. From the kit root, verify the kit and regenerate the report with:\n\n```sh\npython3 experiment-source/verify_package.py .\nPYTHONDONTWRITEBYTECODE=1 python3 report-source/report.py --work evidence --out ../regenerated-report'+(' --comprehensive' if a.comprehensive else '')+'\n```\n\nThe source/ directory preserves the compute/compiler snapshot. evidence/public-release/source preserves the measured call build; the JIT1 source location is recorded in artifact-sizes.json. Native artifacts are omitted explicitly; inspected sizes and SHA-256 identities are retained. report-source/ contains the renderer for the included report.\n')
    save(kit/'MANIFEST.json',{str(f.relative_to(kit)):{'sha256':sha(f),'bytes':f.stat().st_size} for f in sorted(kit.rglob('*')) if f.is_file()})
    archive=out/'reproduction-kit.tar.gz'
    with tarfile.open(archive,'w:gz') as tf:tf.add(kit,arcname=kit.name)
    (out/'reproduction-kit.tar.gz.sha256').write_text(sha(archive)+'  '+archive.name+'\n')
    print(archive)
if __name__=='__main__':main()
