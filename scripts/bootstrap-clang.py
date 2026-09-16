#!/usr/bin/env python3
"""Build LLVM C++ -> Wasm -> PZAM -> LLVM C++ -> Wasm, with recorded inputs.

This is an executable bootstrap gate, not a statement that the gate has passed.
"""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import time

LLVM_REVISION = "5e48a2978cb565ff30b72b7671588fbd0139d55c"


def digest(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ("llvm-source", "wasi-sdk", "engine-bin", "llvm-tblgen", "clang-tblgen", "work"):
        p.add_argument("--" + arg, type=Path, required=True)
    p.add_argument("--target", choices=("aarch64", "x86_64"),
                   default="aarch64" if platform.machine() in ("arm64", "aarch64") else "x86_64")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--aot-jobs", type=int, default=4)
    p.add_argument("--aot-opt-level", type=int, choices=range(4), default=2,
                   help="LLVM PZAM optimization level (default: 2; use 0 for diagnostics)")
    p.add_argument("--through", choices=("wasm", "pzam", "bootstrap"), default="bootstrap")
    p.add_argument("--lto", choices=("OFF", "Thin", "Full"), default="Thin")
    p.add_argument("--reuse-stage0-wasm", type=Path,
                   help="Use a previously recorded source build's bin directory")
    p.add_argument("--reuse-stage0-pzam", type=Path,
                   help="Use previously generated PZAM tools; still run functional probes")
    args = p.parse_args()
    root = Path(__file__).resolve().parent.parent
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = args.llvm_source.resolve()
    sdk = args.wasi_sdk.resolve()
    engine_source = args.engine_bin.resolve()
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    patch = root / "cmake/llvm-wasi/patches/0001-wasi-close-result.patch"
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
    source_diff = subprocess.check_output(
        ["git", "-C", str(source), "diff", "--no-ext-diff", "--binary", "--full-index", "HEAD"], text=True)
    if (revision != LLVM_REVISION or source_diff != patch.read_text() or
            dirty.strip() not in ("M llvm/lib/Support/Unix/Process.inc", "M  llvm/lib/Support/Unix/Process.inc")):
        p.error("LLVM source must be at " + LLVM_REVISION + " with only " + str(patch) +
                " applied. Use a separate checkout/worktree to preserve your original source.")
    manifest = {"llvm_revision": revision, "complete": False, "commands": [], "artifacts": {},
                "llvm_patch": {"path": str(patch), "sha256": digest(patch)},
                "engine_source": str(engine_source),
                "host": {"platform": platform.platform(), "python": sys.version},
                "lto": args.lto, "aot_opt_level": args.aot_opt_level, "tool_inputs": {}}
    # Freeze the engine executables: local development builds must not silently
    # change a runner or compiler halfway through a long bootstrap.
    bins = work / "engine-bin"
    bins.mkdir(exist_ok=True)
    for name in ("pzam-compile", "pzam-run"):
        shutil.copy2(engine_source / name, bins / name)
    for path in (bins / "pzam-compile", bins / "pzam-run", sdk / "bin/clang",
                 sdk / "bin/wasm-ld", sdk / "bin/llvm-ar", sdk / "bin/llvm-ranlib",
                 args.llvm_tblgen.resolve(), args.clang_tblgen.resolve(),
                 Path(shutil.which("cmake")), Path(shutil.which("ninja"))):
        manifest["tool_inputs"][str(path)] = digest(path)
    manifest["recipe_inputs"] = {str(path.relative_to(root)): digest(path)
        for path in sorted((root / "cmake/llvm-wasi").rglob("*")) if path.is_file()}
    for name in ("scripts/bootstrap-clang.py", "scripts/pzam-clang-driver.py"):
        manifest["recipe_inputs"][name] = digest(root / name)
    # Hash every sysroot/resource file, not just a version string.
    resource = Path(subprocess.check_output([str(sdk / "bin/clang"), "-print-resource-dir"], text=True).strip())
    for tree in (sdk / "share/wasi-sysroot", resource):
        h = hashlib.sha256()
        count = 0
        for path in sorted(tree.rglob("*")):
            if path.is_file():
                h.update(str(path.relative_to(tree)).encode() + b"\0" + bytes.fromhex(digest(path)))
                count += 1
        manifest["tool_inputs"][str(tree)] = {"tree_sha256": h.hexdigest(), "files": count}
    env = os.environ.copy()
    # Do not inherit workstation include/library paths or compiler flags. The
    # source tree, SDK and explicit toolchain files define the build inputs.
    removed = ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
               "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
               "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX", "SDKROOT",
               "CCC_OVERRIDE_OPTIONS")
    for key in removed:
        env.pop(key, None)
    env["LC_ALL"] = "C"
    env["TZ"] = "UTC"
    env["SOURCE_DATE_EPOCH"] = subprocess.check_output(
        ["git", "-C", str(source), "show", "-s", "--format=%ct", "HEAD"], text=True).strip()
    manifest["environment_policy"] = {
        "cleared": list(removed),
        "fixed": {key: env[key] for key in ("LC_ALL", "TZ", "SOURCE_DATE_EPOCH")}}
    env["WASI_SDK"] = str(sdk)
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(args.jobs)
    (work / "tmp").mkdir(exist_ok=True)
    env["TMPDIR"] = str(work / "tmp")
    env["CCACHE_DISABLE"] = "1"  # stage 1 must execute the guest compiler

    def save():
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def run(command, name):
        command = list(map(str, command))
        item = {"argv": command, "log": name + ".log", "started": time.time()}
        manifest["commands"].append(item)
        save()
        print(name + ": " + " ".join(command), flush=True)
        with (work / (name + ".log")).open("w") as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        item.update(returncode=result.returncode, seconds=time.time() - item["started"])
        save()
        result.check_returncode()

    def record(path):
        manifest["artifacts"][str(path)] = {"bytes": path.stat().st_size, "sha256": digest(path)}
        save()

    def build(stage, toolchain):
        directory = work / ("llvm-stage" + str(stage))
        run(["cmake", "-S", root / "cmake/llvm-wasi", "-B", directory, "-G", "Ninja",
             "-DWASI_TOOLCHAIN_FILE=" + str(root / "cmake/llvm-wasi" / toolchain),
             "-DLLVM_LOCAL_SRC=" + str(source), "-DLLVM_WASI_BUILD_CLANG=ON",
             "-DLLVM_HOST_TBLGEN=" + str(args.llvm_tblgen.resolve()),
             "-DCLANG_HOST_TBLGEN=" + str(args.clang_tblgen.resolve()),
             "-DLLVM_WASI_LINK_JOBS=1", "-DLLVM_WASI_LTO=" + args.lto,
             "-DLLVM_WASI_USE_MIMALLOC=OFF"], "configure-stage" + str(stage))
        run(["cmake", "--build", directory, "--target", "clang-wasi", "-j", args.jobs],
            "build-stage" + str(stage))
        return directory / "install/bin"

    wasm = args.reuse_stage0_wasm.resolve() if args.reuse_stage0_wasm else build(0, "wasi-llvm-toolchain.cmake")
    for name in ("clang.wasm", "wasm-ld.wasm"):
        record(wasm / name)
    if args.through == "wasm":
        return
    pzam = args.reuse_stage0_pzam.resolve() if args.reuse_stage0_pzam else work / "stage0-pzam"
    pzam.mkdir(exist_ok=True)
    for name in ("clang", "wasm-ld"):
        if not args.reuse_stage0_pzam:
            run([bins / "pzam-compile", "--target=" + args.target, "--backend=llvm", "--opt-level=" + str(args.aot_opt_level), "-j" + str(args.aot_jobs),
                 wasm / (name + ".wasm"), "-o", pzam / (name + ".pzam")], "aot-" + name)
        record(pzam / (name + ".pzam"))
        run([bins / "pzam-run", pzam / (name + ".pzam"), "--version"], "version-" + name)
    drivers = work / "drivers"
    drivers.mkdir(exist_ok=True)
    for name in ("clang", "clang++"):
        shutil.copyfile(root / "scripts/pzam-clang-driver.py", drivers / name)
        (drivers / name).chmod(0o755)
    config = {"runner": str(bins / "pzam-run"), "clang": str(pzam / "clang.pzam"),
              "linker": str(pzam / "wasm-ld.pzam"), "sdk": str(sdk), "resource_dir": str(resource),
              "preopens": [".", str(work), str(source), str(sdk), str(root), "/tmp", str(Path("/tmp").resolve())]}
    (drivers / "bootstrap-driver.json").write_text(json.dumps(config, indent=2) + "\n")
    env["PSIZAM_BOOTSTRAP_DRIVER_DIR"] = str(drivers)
    env["PSIZAM_BOOTSTRAP_CONFIG"] = str(drivers / "bootstrap-driver.json")
    probe = work / "probe.cpp"
    probe.write_text('#include <vector>\n#include <cstdio>\nint main() { std::vector<int> v{19, 23}; std::printf("%d\\n", v[0]+v[1]); }\n')
    run([drivers / "clang++", probe, "-O1", "-o", work / "probe.wasm"], "compile-probe")
    run([bins / "pzam-compile", "--target=" + args.target, "--backend=llvm", "--opt-level=" + str(args.aot_opt_level), work / "probe.wasm",
         "-o", work / "probe.pzam"], "aot-probe")
    run([bins / "pzam-run", work / "probe.pzam"], "run-probe")
    if "42" not in (work / "run-probe.log").read_text().splitlines():
        raise RuntimeError("C++ probe did not print 42")
    if args.through == "pzam":
        return
    final = build(1, "pzam-toolchain.cmake")
    for name in ("clang", "wasm-ld"):
        record(final / (name + ".wasm"))
        run([bins / "pzam-compile", "--target=" + args.target, "--backend=llvm", "--opt-level=" + str(args.aot_opt_level), "-j" + str(args.aot_jobs),
             final / (name + ".wasm"), "-o", work / (name + "-stage1.pzam")], "aot-stage1-" + name)
        run([bins / "pzam-run", work / (name + "-stage1.pzam"), "--version"], "version-stage1-" + name)
    config["clang"] = str(work / "clang-stage1.pzam")
    config["linker"] = str(work / "wasm-ld-stage1.pzam")
    (drivers / "bootstrap-driver.json").write_text(json.dumps(config, indent=2) + "\n")
    run([drivers / "clang++", probe, "-O1", "-o", work / "probe-stage1.wasm"], "compile-probe-stage1")
    run([bins / "pzam-compile", "--target=" + args.target, "--backend=llvm", "--opt-level=" + str(args.aot_opt_level), work / "probe-stage1.wasm",
         "-o", work / "probe-stage1.pzam"], "aot-probe-stage1")
    run([bins / "pzam-run", work / "probe-stage1.pzam"], "run-probe-stage1")
    if "42" not in (work / "run-probe-stage1.log").read_text().splitlines():
        raise RuntimeError("Self-hosted compiler's C++ probe did not print 42")
    manifest["complete"] = True
    save()


if __name__ == "__main__":
    main()
