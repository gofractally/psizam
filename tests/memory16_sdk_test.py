"""Compile/link SDK markers, including stripping, multiple TUs, and LTO."""
import pathlib
import subprocess
import sys
import tempfile

compiler, header = sys.argv[1:]


def run(args, ok=True):
    result = subprocess.run([compiler, *args], capture_output=True, text=True)
    if ok:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode != 0, "p32 compiler falsely accepted the p16 ABI"
        assert "pointer ABI mismatch" in result.stderr, result.stderr
    return result


def sections(data):
    pos = 8

    def leb():
        nonlocal pos
        result, shift = 0, 0
        while True:
            byte = data[pos]
            pos += 1
            result |= (byte & 127) << shift
            if byte < 128:
                return result
            shift += 7

    while pos < len(data):
        section = data[pos]
        pos += 1
        size = leb()
        end = pos + size
        if section == 0:
            length = leb()
            name = data[pos:pos + length]
            pos += length
            yield name, data[pos:end]
        pos = end


with tempfile.TemporaryDirectory(prefix="psizam-sdk16-") as temp:
    root = pathlib.Path(temp)
    sources = [root / "a.c", root / "b.c"]
    sources[0].write_text("int f(int* p) { return *p; }\n")
    sources[1].write_text("int g(int* p) { return *p + 1; }\n")
    common = ["--target=wasm32-unknown-unknown", "-include", header]
    link = ["-nostdlib", "-Wl,--no-entry,--strip-all,--keep-section=psizam.execution,"
            "--initial-memory=65536,--max-memory=65536,-z,stack-size=8192"]
    for lto in [[], ["-flto"]]:
        output = root / "tagged.wasm"
        run([*common, "-DPSI_MEMORY16_POINTER_BITS=32", *lto,
             *map(str, sources), *link, "-o", str(output)])
        records = [payload for name, payload in sections(output.read_bytes())
                   if name == b"psizam.execution"]
        assert records, "linker discarded the execution profile"
        assert b"".join(records) == bytes([1, 16, 32, 0]) * 2, records

    run([*common, "-c", str(sources[0]), "-o", str(root / "bad.o")], ok=False)
    # When run with the fork's compiler, also test the real p16 ABI path.
    target16 = ["--target=wasm32-unknown-unknown-p16"]
    macros = run([*target16, "-dM", "-E", "-x", "c", str(sources[0])]).stdout
    if "#define __SIZEOF_POINTER__ 2\n" in macros:
        output = root / "p16.wasm"
        run([*target16, "-include", header, str(sources[0]), *link, "-o", str(output)])
        assert (b"psizam.execution", bytes([1, 16, 16, 0])) in list(sections(output.read_bytes()))
        print("SDK emission passed, including the p16 compiler ABI")
    else:
        print("SDK emission/LTO/stripping and ABI rejection passed; compiler has no p16 ABI")
