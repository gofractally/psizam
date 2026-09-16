#!/usr/bin/env python3
"""Host process orchestration for a compiler/linker that execute inside PZAM.

Install as clang and clang++ beside bootstrap-driver.json. No host compiler is
used: clang.pzam expands driver jobs and runs cc1; wasm-ld.pzam links the result.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    config_path = Path(os.environ.get(
        "PSIZAM_BOOTSTRAP_CONFIG", str(Path(sys.argv[0]).absolute().parent / "bootstrap-driver.json")))
    cfg = json.loads(config_path.read_text())
    runner = [cfg["runner"]]
    for path in cfg["preopens"]:
        runner.append("--dir=" + path)
    clang = runner + [cfg["clang"]]
    ld = runner + [cfg["linker"]]
    sdk = Path(cfg["sdk"])
    common = ["--target=wasm32-wasip1", "--sysroot=" + str(sdk / "share/wasi-sysroot"),
              "-resource-dir=" + cfg["resource_dir"], "--ld-path=" + str(sdk / "bin/wasm-ld")]
    if Path(sys.argv[0]).name.endswith("++"):
        common += ["--driver-mode=g++"]
    args = sys.argv[1:]
    # Non-linking commands and driver queries run directly inside the guest.
    nonlink = any(a in ("-c", "-S", "-E", "-fsyntax-only", "--version", "-dumpmachine",
                        "-dumpversion", "-###", "--help", "-help") or
                  a.startswith("-print-") for a in args)
    if nonlink:
        return subprocess.call(clang + common + args)

    # WASI cannot fork. Ask the guest driver for its job graph, then dispatch
    # each job ourselves to a guest compiler or guest linker, preserving order.
    plan = subprocess.run(clang + common + ["-###"] + args, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if plan.returncode:
        sys.stdout.write(plan.stdout)
        sys.stderr.write(plan.stderr)
        return plan.returncode
    jobs = []
    for line in plan.stderr.splitlines():
        if not line.lstrip().startswith('"'):
            continue
        job = shlex.split(line)
        if len(job) > 1 and job[1] == "-cc1":
            jobs.append(clang + job[1:])
        elif job and Path(job[0]).name in ("wasm-ld", "wasm-ld.wasm", "wasm-ld.pzam"):
            jobs.append(ld + job[1:])
        else:
            raise RuntimeError("Unsupported guest compiler job: " + line)
    if not jobs:
        sys.stderr.write(plan.stderr)
        raise RuntimeError("Guest compiler produced no executable jobs")
    for job in jobs:
        result = subprocess.call(job)
        if result:
            return result
    return 0


if __name__ == "__main__":
    sys.exit(main())
