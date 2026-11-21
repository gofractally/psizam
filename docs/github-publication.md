# Standalone GitHub publication

The standalone source repository is
[gofractally/psizam](https://github.com/gofractally/psizam). Its initial `main`
preserves the extracted engine history and the newer standalone work through
`eb5c3296d2d9436a5f15535fc365c6f5b91053d7`, followed by the publication setup.
See [extraction provenance](extraction.md) for the exact published psiserve
subtree and split commit.

## Publication checks on November 21 2026

Native macOS ARM64 checks used Homebrew Clang 22.1.2, WASI SDK's Clang 22.1.0,
and an independently installed psio built from
`ff114b6a8c9265e6b5da5ffb85df1617b2b43514`.

| Check | Result |
| --- | --- |
| Unit executable | 324 cases passed, 2,600 assertions |
| Runtime parity executable | 52 cases passed, 271 assertions |
| Compliance executable | 68 cases passed, 360 assertions |
| Quick CTest gate | 173 checks passed |
| Installed and source consumer examples | Both built and passed |
| Installed runtime CTest gate | Passed, including saved JIT1/JIT2 execution |

These are separate checks with overlapping behavior; their counts are not an
aggregate coverage measure. The publication setup changes package/CI plumbing,
SDK/dependency forwarding, notices and documentation; it does not modify runtime
algorithms. The installed files include the SoftFloat, ucc, Catch2 and wasm3
license notices.

To reproduce the native checks after installing the pinned psio package:

```sh
WASI_SDK=/path/to/wasi-sdk CC=/path/to/clang CXX=/path/to/clang++ \
  bash scripts/verify-independent.sh /path/to/check /path/to/psio-install
ctest --test-dir /path/to/check/build --output-on-failure --no-tests=error \
  -R '^execution\.installed-runtime$'
```

The `Standalone library` workflow performs these checks on fresh Linux x86-64
and macOS ARM64 runners. Its run results, rather than this local record, establish
remote CI outcomes for a particular commit.

## Linux sparse arena allocation

Native Linux x86-64 qualification with Clang 20.1.8 passed 324 unit cases,
52 runtime-parity cases, 77 compliance cases, all 173 quick CTest checks,
both consumer examples and the installed-runtime gate. The fresh build and
the allocator follow-up were run in an isolated workspace on the Linux builder.

Hosted Linux validation exposed a failure mapping the default 16 GiB module/IR
arena. Linux growable arenas now use `MAP_NORESERVE` for their sparse mappings,
with allocation diagnostics that report requested bytes and errno. The arena
capacity and guest linear-memory guards are preserved. CI records the memory
policy and checks a source consumer before starting the full test build.

## Remaining release qualification

Publishing the source and passing package checks do not establish a stable
runtime release. The compiler-fixture archive still needs an accessible HTTPS
location for the separate `Compiler qualification` workflow. Full scalar/SIMD
spec generation, optional LLVM paths, other runtime policy gates and the
psibase application integration require their own qualified results. See
[validation](validation.md), [runtime release review](runtime-release-review.md)
and [release status](release-status.md).
