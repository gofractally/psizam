# Clang PZAM loading: copy cost and startup share

Measured September 17, 2026 (US Central). Five measured compiler launches and ten container probes per cell, plus one excluded warmup.

Fresh-process, warm-filesystem-cache compiler jobs and a separate container-only probe. No gas/fuel checks or engine timer; memory and stack protections remain enabled.

## Large C++ compilation (LLParser.cpp)

| Host | Engine | Job median | Startup median | Startup share |
|---|---|---:|---:|---:|
| ARM Mac | JIT2 PZAM | 11.921 s | 55.70 ms | 0.47% |
| ARM Mac | LLVM O2 PZAM | 10.117 s | 39.28 ms | 0.39% |
| ARM Mac | Cranelift AOT | 5.768 s | not instrumented | - |
| x86 VPS | JIT2 PZAM | 13.220 s | 115.94 ms | 0.88% |
| x86 VPS | LLVM O2 PZAM | 18.181 s | 115.18 ms | 0.63% |
| x86 VPS | Cranelift AOT | 8.358 s | not instrumented | - |

## Isolated container probe

Owned double validation is the current container path. View single validation borrows metadata/code and copies selected code once into new writable pages. Neither probe variant restores a runtime module, relocates code, or executes it. This is an opportunity estimate, not an integrated loader speedup.

| Host | Engine | Owned/double | View/single | Difference | Probe RSS reduction |
|---|---|---:|---:|---:|---:|
| ARM Mac | JIT2 PZAM | 34.99 ms | 20.19 ms | 14.81 ms | 236.3 MiB |
| ARM Mac | LLVM O2 PZAM | 22.30 ms | 10.02 ms | 12.28 ms | 129.0 MiB |
| x86 VPS | JIT2 PZAM | 103.78 ms | 49.38 ms | 54.39 ms | 160.9 MiB |
| x86 VPS | LLVM O2 PZAM | 87.44 ms | 35.09 ms | 52.35 ms | 179.8 MiB |

Startup is too small to explain the large-job Cranelift gap. PSSZ views remain useful for reducing transient memory and repeated cold-instance creation; cached module reuse amortizes these costs. These measurements do not identify the cause of the remaining execution-time difference. ARM LLVM job times vary substantially between launches; startup does not explain that spread.

## Method and limits

- Counts and sample sizes are recorded per host in summary.json; randomized order after warmups. Ranges are observed min/max, not confidence intervals.
- Compiler outputs must match both the reference raw SHA-256 and the normalized object SHA-256. All copied probe bytes must match the validated PSSZ view.
- Profiling uses the original frozen pzam-run binaries. Startup ends immediately before guest entry; wall time also includes guest execution, process launch and teardown.
- The probe uses Clang -O3, the frozen engine/psio headers, and generic anonymous writable mmap pages. Its times and RSS are not those of an integrated production view loader.
- Existing --trust=full removes only the extra validation pass: pzam_load still validates before decoding.
- No disk-cache flush, native compiler timing, gas/timeout comparison, service dispatch test, or production loader change is included.
- LLParser.cpp: 5,348,473 preprocessed bytes / 145,305 lines; SipHash.cpp is the smaller compiler task.
- Exact commands, binary/input/header hashes, raw profiles and outputs are retained in the two evidence cohorts.

## Reproduce

See benchmarks/repro/README.md, section "PZAM loading diagnostic".

## Hosts

- ARM Mac: Apple M5 Max; macOS-26.3.1-arm64-arm-64bit-Mach-O; wasmtime 48.0.2 (e9f1ea232 2026-09-10).
- x86 VPS: AMD EPYC-Turin virtual CPU (16 vCPUs); Linux-6.17.0-22-generic-x86_64-with-glibc2.42; wasmtime 48.0.2 (e9f1ea232 2026-09-10).
