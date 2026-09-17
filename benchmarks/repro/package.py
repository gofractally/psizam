#!/usr/bin/env python3
"""Package the exact measured source snapshot and raw evidence (no native binaries)."""
import argparse
import json
import shutil
import tarfile
from pathlib import Path
from run import sha,save

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();w=a.work.resolve();out=a.out.resolve();kit=out/'reproduction-kit'
    data=json.loads((w/'resources.json').read_text())
    if not data.get('complete'):raise ValueError('Cannot package an incomplete matrix')
    manifest=json.loads((w/'source-manifest.json').read_text())
    for repo,files in manifest.items():
        for name,digest in files.items():
            if sha(w/'source'/repo/name)!=digest:raise ValueError('Frozen source changed: '+repo+'/'+name)
    if kit.exists():raise ValueError('Refusing to overwrite kit')
    shutil.copytree(w/'source',kit/'source');e=kit/'evidence';e.mkdir()
    for item in w.iterdir():
        if item.is_file() and item.suffix in ['.json','.jsonl']:shutil.copy2(item,e/item.name)
    for name in ['logs','sources','validation-evidence']:
        if (w/name).exists():shutil.copytree(w/name,e/name)
    (e/'modules').mkdir()
    for item in (w/'modules').glob('*.wasm'):shutil.copy2(item,e/'modules'/item.name)
    for name in ['summary.json','samples.csv','psizam-cranelift-report.pdf','report.html','charts.json',*json.loads((out/'charts.json').read_text())]:
        shutil.copy2(out/name,kit/name)
    shutil.copy2(w/'source/psizam/benchmarks/repro/README.md',kit/'README.md')
    # Report presentation may evolve independently of the frozen measured engine.
    renderer=kit/'report-source';renderer.mkdir()
    for name in ['report.py','visual_report.py']:
        shutil.copy2(Path(__file__).parent/name,renderer/name)
    (renderer/'README.md').write_text('Requires Python 3, matplotlib and reportlab. From the kit root, regenerate the report with:\n\n```sh\npython3 report-source/report.py --work evidence --out regenerated-report\n```\n\nThe source/ directory preserves the measured engine snapshot; report-source/ contains the renderer for the included report.\n')
    save(kit/'MANIFEST.json',{str(f.relative_to(kit)):{'sha256':sha(f),'bytes':f.stat().st_size} for f in sorted(kit.rglob('*')) if f.is_file()})
    archive=out/'reproduction-kit.tar.gz'
    with tarfile.open(archive,'w:gz') as tf:tf.add(kit,arcname=kit.name)
    (out/'reproduction-kit.tar.gz.sha256').write_text(sha(archive)+'  '+archive.name+'\n')
    print(archive)
if __name__=='__main__':main()
