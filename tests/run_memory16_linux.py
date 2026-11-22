#!/usr/bin/env python3
"""Qualify a frozen memory16 source on Linux without modifying its checkout.

Uses an existing PSIO checkout and installed tools. New result directories; an
explicitly requested build-cache reuse must match the frozen source location.
No package installation, Git mutation or engine fallback.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def source_identity(source):
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    head = None
    if (source / ".git").exists():
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, env=env, text=True).strip()
        names = subprocess.check_output(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=source, env=env,
        ).split(b"\0")
    else:
        # Exact-source archives need no Git metadata in the build environment.
        excluded = {".git", "target", "build", "__pycache__"}
        names = [os.fsencode(str(p.relative_to(source))) for p in source.rglob("*")
                 if not any(part in excluded for part in p.relative_to(source).parts)]
    files = {}
    for name in sorted(set(names) - {b""}):
        path = source / os.fsdecode(name)
        if path.is_symlink():
            files[os.fsdecode(name)] = "symlink:" + os.readlink(path)
        elif path.is_file():
            files[os.fsdecode(name)] = hashlib.sha256(path.read_bytes()).hexdigest()
    fingerprint = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {"head": head, "source_sha256": fingerprint, "files": files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--psio", type=Path, required=True)
    parser.add_argument("--llvm-dir", type=Path)
    parser.add_argument("--sdk-cc", type=Path)
    parser.add_argument("--wasi-sdk", type=Path)
    parser.add_argument("--wat2wasm", type=Path)
    parser.add_argument("--extra-source", type=Path, action="append", default=[])
    parser.add_argument("--wasmtime-build-id", default="",
                        help="development path source receipt; omit for lock-derived Git identity")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--reuse-build", action="store_true")
    parser.add_argument("--compiler", default="/usr/bin/clang++-20")
    parser.add_argument("--cargo", default=str(Path.home() / ".cargo/bin/cargo"))
    parser.add_argument("--lock", type=Path, default=Path.home() / ".cache/psibase-foundation-qualification.lock")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("use between 1 and 8 bounded jobs")
    source, build, results = (p.resolve() for p in (args.source, args.build, args.results))
    if build == source or source in build.parents or results == source or source in results.parents:
        parser.error("build/results must be separate from frozen source")
    results.mkdir(parents=True, exist_ok=False)
    args.lock.parent.mkdir(parents=True, exist_ok=True)
    record = {"status": "starting", "commands": [], "jobs": args.jobs, "source": str(source)}
    env = dict(os.environ, CARGO_BUILD_JOBS=str(args.jobs), RUSTUP_TOOLCHAIN="1.98.0", GIT_OPTIONAL_LOCKS="0")
    env.pop("PSIZAM_WASMTIME_BUILD_ID", None)
    if args.wasmtime_build_id:
        env["PSIZAM_WASMTIME_BUILD_ID"] = args.wasmtime_build_id
    record["wasmtime_build_id_override"] = args.wasmtime_build_id or None

    def save():
        (results / "qualification.json").write_text(json.dumps(record, indent=2) + "\n")

    def run(name, command, timeout):
        item = {"name": name, "command": command}
        record["commands"].append(item)
        save()
        started = time.monotonic()
        with (results / (name + ".log")).open("w") as log:
            result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        item.update(returncode=result.returncode, elapsed_seconds=time.monotonic() - started)
        save()
        if result.returncode:
            raise RuntimeError(name + " failed; retained log")

    with args.lock.open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            record["status"] = "busy; no build started"
            save()
            return 75
        try:
            record["before"] = source_identity(source)
            record["psio"] = source_identity(args.psio.resolve())
            record["extra_sources_before"] = {str(p.resolve()): source_identity(p.resolve()) for p in args.extra_source}
            if args.wat2wasm:
                tool = args.wat2wasm.resolve()
                record["wat2wasm"] = {"path": str(tool), "sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
                    "version": subprocess.check_output([str(tool), "--version"], text=True).strip()}
            if build.exists() and any(build.iterdir()):
                cache = build / "CMakeCache.txt"
                if not args.reuse_build or not cache.is_file():
                    raise RuntimeError("use a new isolated build directory or explicit --reuse-build")
                expected = "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(source)
                if expected not in cache.read_text().splitlines():
                    raise RuntimeError("build cache belongs to another source directory")
                record["reused_cmake_cache_sha256"] = hashlib.sha256(cache.read_bytes()).hexdigest()
            build.mkdir(parents=True, exist_ok=True)
            configure = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=/usr/bin/clang-20",
                "-DCMAKE_CXX_COMPILER=" + args.compiler, "-DPSIZAM_ENABLE_TESTS=ON",
                "-DPSIZAM_ENABLE_SPEC_TESTS=OFF", "-DPSIZAM_ENABLE_WASMTIME=ON",
                "-DPSIZAM_WASMTIME_BUILD_ID=" + args.wasmtime_build_id,
                "-DPSIZAM_CARGO=" + args.cargo, "-DFETCHCONTENT_SOURCE_DIR_PSIO=" + str(args.psio.resolve()),
                "-DPSIZAM_ENABLE_LLVM=" + ("ON" if args.llvm_dir else "OFF")]
            if args.llvm_dir:
                configure += ["-DLLVM_DIR=" + str(args.llvm_dir.resolve())]
            if args.sdk_cc:
                configure += ["-DPSIZAM_MEMORY16_SDK_CC=" + str(args.sdk_cc.resolve())]
            if args.wasi_sdk:
                configure += ["-DPSIZAM_WASI_SDK=" + str(args.wasi_sdk.resolve())]
            if args.wat2wasm:
                for variable in ("PSIZAM_WAT2WASM", "PSIZAM_CALL_WAT2WASM", "PSIZAM_BACKTRACE_WAT2WASM"):
                    configure += ["-D" + variable + "=" + str(args.wat2wasm.resolve())]
            run("configure", configure, 180)
            targets = ["psizam_memory16_tests", "psizam_wasmtime_memory16_tests",
                "psizam_wasmtime_backend_tests", "psizam_engine_parity_tests", "psizam_pzam_compile_tests",
                "psizam_memory16_cli_artifact", "psizam-wasi", "pzam-compile",
                "psizam_signal_tests", "psizam_thread_stack_tests", "psizam_runtime_release_tests",
                "psizam_jit1_backtrace_tests", "psizam_table_limit_tests", "psizam_byte_stack_tests",
                "psizam_jit_worker_policy_tests", "psizam_regalloc_tests", "psizam_pzam_format_tests",
                "unit_tests", "jit2_tests"]
            if args.llvm_dir:
                targets += ["psizam_private_atomic_tests"]
            run("build", ["cmake", "--build", str(build), "--parallel", str(args.jobs), "--target", *targets], 3600)
            tests = "^(execution\\.memory16|execution\\.wasmtime-memory16|execution\\.wasmtime-backend|execution\\.engine-parity|execution\\.private-atomics|pzam_compile_api|memory16_sdk_emission|memory16_cli|psizam_signal_tests|psizam_thread_stack_tests|runtime_release|execution\\.jit1-backtrace|execution\\.table-growth-policy|execution\\.byte-stack|psizam_jit_worker_policy_tests|execution\\.regalloc|pzam_format)$"
            run("tests", ["ctest", "--test-dir", str(build), "--verbose", "--output-on-failure", "--no-tests=error", "--parallel", "2", "-R", tests], 1800)
            run("unit-tests", [str(build / "bin/unit_tests")], 600)
            run("jit2-tests", [str(build / "bin/jit2_tests")], 600)
            record["status"] = "passed"
        except Exception as error:
            record.update(status="failed", error=str(error))
        finally:
            record["after"] = source_identity(source)
            record["psio_after"] = source_identity(args.psio.resolve())
            record["extra_sources_after"] = {str(p.resolve()): source_identity(p.resolve()) for p in args.extra_source}
            if record.get("before", {}).get("source_sha256") != record["after"]["source_sha256"]:
                record.update(status="failed", error="source changed during qualification")
            if record.get("extra_sources_before") != record["extra_sources_after"]:
                record.update(status="failed", error="dependency source changed during qualification")
            if record.get("psio") != record["psio_after"]:
                record.update(status="failed", error="PSIO source changed during qualification")
            if args.wat2wasm and record.get("wat2wasm", {}).get("sha256") != hashlib.sha256(args.wat2wasm.resolve().read_bytes()).hexdigest():
                record.update(status="failed", error="WAT compiler changed during qualification")
            save()
    return 0 if record["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
