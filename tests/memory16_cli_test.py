"""Exercise real CLI admission and inspect saved output with canonical APIs."""
import argparse
import pathlib
import platform
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--wasi", required=True)
parser.add_argument("--compile", required=True)
parser.add_argument("--artifact", required=True)
parser.add_argument("--wasm", required=True)
parser.add_argument("--llvm", action="store_true")
args = parser.parse_args()


def run(command, success=True):
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    print(" ".join(map(str, command)), "=>", result.returncode, flush=True)
    print(result.stdout + result.stderr, end="", flush=True)
    assert (result.returncode == 0) == success, result.stdout + result.stderr


with tempfile.TemporaryDirectory(prefix="psizam-memory16-cli-") as temporary:
    root = pathlib.Path(temporary)
    p16, p32 = 0x00101001, 0x00201001
    tagged = root / "p16.wasm"
    run([args.artifact, "tag", args.wasm, str(p16), str(tagged)])
    for backend in ["interpreter", "jit", "jit2", *(["llvm"] if args.llvm else [])]:
        run([args.wasi, "--backend=" + backend, args.wasm], success=False)
        run([args.wasi, "--backend=" + backend, "--memory16", args.wasm])
        run([args.wasi, "--backend=" + backend, str(tagged)])
        run([args.wasi, "--backend=" + backend, "--memory16", str(tagged)])

    arch = "aarch64" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64"
    for backend, tier in [("jit1", 0), ("jit2", 1), *([("llvm", 3)] if args.llvm else [])]:
        for name, source, flags, profile in [
            ("ordinary", args.wasm, [], 0),
            ("forced", args.wasm, ["--memory16"], p32),
            ("tagged", str(tagged), [], p16),
            ("tagged-forced", str(tagged), ["--memory16"], p16),
        ]:
            output = root / (backend + "-" + name + ".pzam")
            run([args.compile, "--target=" + arch, "--backend=" + backend,
                 "--opt-level=2", "-j3", *flags, "-o", str(output), source])
            run([args.artifact, "check", str(output), str(profile), str(tier)])
