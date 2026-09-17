# Reproducible psizam / Cranelift CPU-resource benchmark

The reference is **no CPU constraint checking**. Every backend is measured with
no CPU limits, a finite gas/fuel budget, and an armed wall-clock timeout. These
are separate compiled profiles: an unlimited gas budget is not an uninstrumented
baseline. Memory protection and call-stack limits stay enabled.

## Build and run

Build the standalone engine and runner with `build.sh`; it accepts the psizam
checkout, psio checkout, build directory, LLVM prefix and Wasmtime C API directory.
The C API runner is for the separate boundary experiment. The resource matrix
uses the pinned Wasmtime 48.0.2 CLI and LLVM O2.

```sh
benchmarks/repro/build.sh "$PWD" ../psio /tmp/resource-build /opt/homebrew/opt/llvm /path/to/wasmtime-c-api
python3 benchmarks/repro/run.py prepare --work build/resource-run \
  --wasi-sdk /path/to/wasi-sdk --native-cc /path/to/clang \
  --engine-bin /tmp/resource-build/engine/bin \
  --runner /tmp/resource-build/runner/repro-psizam \
  --wasmtime /path/to/wasmtime --psio-source ../psio
python3 benchmarks/repro/resources.py --work build/resource-run \
  --threads 1,18 --samples 5 --compile-samples 3
python3 benchmarks/repro/report.py --work build/resource-run --out outputs/resource-report
```

Use a **new work directory** for each source state. Source snapshots include
local tracked and untracked changes; tool and module hashes accompany raw
commands, logs and samples. `--only` selects comma-separated workloads. The
default covers eight BenchmarkGame/PolyBench integer, scalar-FP and SIMD kernels.
Add `coremark` to the workload list to include both CRC-checked seed sets, with
counts calibrated from unrestricted execution and accepted runs lasting at
least ten seconds. `--compile-watchdog 3600` allows long compiler preparation;
an outer process watchdog never counts as engine enforcement.

## Policy definitions

| Profile | psizam | Wasmtime / Cranelift |
|---|---|---|
| none | No gas code emitted, no timer armed | Fuel and epoch instrumentation disabled |
| gas | Function/loop accounting and finite budget | Fuel instrumentation and finite budget |
| timeout | Timer revokes executable-page access | Epoch interruption and wall deadline |

Gas and fuel units are different cost models. Equal numeric budgets do not mean
equal permitted work. Performance budgets are large enough to complete the
workload; separate deliberately small budgets verify exhaustion. Wall deadlines
measure elapsed time, not scheduled process CPU time.

JIT1, JIT2, fresh LLVM JIT and saved JIT2/LLVM PZAM all use page interruption.
LLVM ORC executable mappings are registered during linking; no polling is
inserted in its timeout code. Code-page timeout tests use exclusive ownership
of one module. Host calls that block indefinitely are outside this experiment.
The optional `timeout-poll` CLI mode is a distinct gas-checkpoint clock policy
and is not used in the default comparison.

The matrix also validates nonterminating module start functions, finite
completion, zero gas accounting when checks are omitted, exact trap types,
and rejection of gas budgets on uninstrumented artifacts. Standard CTest
registers these checks as `execution.resource-limits`. The public compile API
tests verify the `gas_meter` flag and code reuse after page interruption.

## Compilation and timing

All psizam profiles request hardware deterministic FP; Cranelift enables NaN
canonicalization. JIT1/JIT2 ARM floating-point SIMD uses helpers, which is part
of the current implementation being measured. PZAM artifacts have explicit gas
instrumentation metadata, checked by the runner. Compile worker budgets apply
to LLVM AOT and Cranelift AOT; other profiles use one worker. LLVM's multiworker
path also batches functions, so its result combines batching and parallelism.
Guest execution remains serial.

Heavy artifact creation completes before execution samples. Each cell receives
one excluded warmup and randomized measured fresh-process runs. BenchmarkGame
uses the guest main timer; PolyBench uses its kernel timer and printed-array
validation against native output. CoreMark, when selected, uses its internal
timer, both seed sets and CRCs. Raw wall times include setup and teardown.

Reports describe only the selected source snapshot, host, profiles, checks and
measurements. They never merge retained measurements from other source states.
A missing or failing cell blocks ranking rather than silently dropping it.
Bar charts show unrestricted engine speed and each engine's limited execution
time as percentage overhead above its own no-check reference (0%). Only work
budget and time-limit bars are shown; negative values remain visible. Raw samples, medians,
observed ranges, tool identity and source manifests remain available.
Runtime charts and tables use one fixed build configuration per engine: the
lowest compiler worker count measured, normally one. Compiler worker counts
appear in the compilation comparison; all variants remain in the raw evidence.
Runtime samples from different build configurations are not pooled.

`package.py` bundles the frozen measured source and evidence. `replay.sh` builds
new artifacts and reruns enforcement, correctness and timing. Compiler jobs,
full bootstrap and host boundaries are separate experiments; they are not
inferred from this resource-policy report.
