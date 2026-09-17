#!/usr/bin/env python3
"""Verify every manifest entry in an extracted kit or a safe temporary extraction."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile
import tempfile


def verify(root):
    root=root.resolve();manifest=json.loads((root/'MANIFEST.json').read_text())
    actual={str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() and p!=root/'MANIFEST.json'}
    if actual!=set(manifest):raise RuntimeError('Manifest file set mismatch')
    links=0
    for name,record in manifest.items():
        p=root/name
        if not p.resolve().is_relative_to(root):raise RuntimeError('Symlink escapes kit: '+name)
        if p.is_symlink():links+=1
        with p.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
        if digest!=record['sha256'] or p.stat().st_size!=record['bytes']:
            raise RuntimeError('Content mismatch: '+name)
    return {'verified':True,'manifest_entries':len(manifest),'internal_symlinks':links}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('path',type=Path);a=parser.parse_args()
    if a.path.is_dir():result=verify(a.path)
    else:
        with tempfile.TemporaryDirectory(prefix='psizam-kit-verify-') as tmp:
            with tarfile.open(a.path) as archive:archive.extractall(tmp,filter='data')
            result=verify(Path(tmp)/'reproduction-kit')
        with a.path.open('rb') as f:result['archive_sha256']=hashlib.file_digest(f,'sha256').hexdigest()
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
