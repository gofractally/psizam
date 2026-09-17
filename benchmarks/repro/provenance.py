"""Verify separately recorded source snapshots without pooling build identities."""
import json
from pathlib import Path
from run import sha


def verified_manifest(directory, expected_sha256=None):
    directory = Path(directory)
    path = directory/'source-manifest.json'
    if expected_sha256 is not None and sha(path) != expected_sha256:
        raise ValueError('Source manifest changed: '+str(path))
    manifest = json.loads(path.read_text())
    for repo, files in manifest.items():
        for name, digest in files.items():
            if sha(directory/'source'/repo/name) != digest:
                raise ValueError('Frozen source changed: '+repo+'/'+name)
    return manifest


def service_cohort(work):
    work = Path(work)
    cohort = json.loads((work/'service-cohort.json').read_text())
    directory = work/cohort['directory']
    manifest = verified_manifest(directory, cohort['source_manifest_sha256'])
    return cohort, directory, manifest
