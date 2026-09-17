#!/usr/bin/env python3
"""Extract only pinned compiler inputs from the retained reproduction archive."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--dest', type=Path, required=True)
    a = p.parse_args()
    manifest = json.loads((Path(__file__).resolve().parents[1] / 'tests/validation/compiler-fixtures.json').read_text())
    expected = manifest['files']
    pending = {}
    with tarfile.open(a.archive) as archive:
        for entry in archive:
            name = Path(entry.name).name
            if name not in expected or not entry.isfile():
                continue
            data = archive.extractfile(entry).read()
            if hashlib.sha256(data).hexdigest() != expected[name]:
                raise RuntimeError('Wrong fixture content: ' + entry.name)
            if name in pending:
                raise RuntimeError('Duplicate fixture: ' + name)
            pending[name] = data
    if set(pending) != set(expected):
        raise RuntimeError('Missing compiler fixtures: ' + ', '.join(set(expected) - set(pending)))
    a.dest.mkdir(parents=True, exist_ok=True)
    for name, data in pending.items():
        (a.dest / name).write_bytes(data)
    print('Verified and staged all three compiler inputs at', a.dest)

if __name__ == '__main__':
    main()
