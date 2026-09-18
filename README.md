# thecoolestcompiler

A source-to-source compiler that reads **unmodified sequential C/C++**, proves which loops are
safe to run in parallel — *including across function-call boundaries* — decides whether each one
is actually worth offloading to a GPU, and emits OpenMP directives accordingly.

Built on [ROSE](http://rosecompiler.org) 2.14.0 (LLNL) + Clang/LLVM 18.

```c
// in                                    // out
for (int i = 0; i < N; ++i)              #pragma omp parallel for
    C[i] = A[i] + B[i];                  for (int i = 0; i < N; ++i)
                                             C[i] = A[i] + B[i];

for (int i = 1; i < N; ++i)              // left alone — loop-carried dependence
    A[i] = A[i-1] + B[i];                for (int i = 1; i < N; ++i)
                                             A[i] = A[i-1] + B[i];
```

---

## Why this is not just `-ftree-parallelize-loops`

Most auto-parallelizers do one of these:

- **Skip interprocedural analysis.** A hot loop that calls a helper function is conservatively
  treated as unparallelizable, because the compiler won't look inside the callee to see whether it
  touches loop-iterated data. That leaves real parallelism on the table.
- **Target CPU threads only.** Once a region is proven safe, it gets `#pragma omp parallel for` and
  that's it — no reasoning about whether the region is actually a good GPU fit.

This project does **whole-program interprocedural dependence analysis** *and* makes an explicit,
measured **GPU-vs-CPU profitability decision** per loop, using a roofline model fed by real
profiling data.

---

## Results

### Correctness of classification

Every loop is classified `PARALLEL_SAFE`, `REDUCTION_CANDIDATE`, `SERIAL_ONLY`, or
`UNKNOWN_CONSERVATIVE`, with a human-readable reason attached.

| Input | Loop | Verdict | Why |
|---|---|---|---|
| `test_loops.cpp:17` | `C[i] = A[i] + B[i]` | `PARALLEL_SAFE` | no loop-carried dependence |
| `test_loops.cpp:22` | `A[i] = A[i-1] + B[i]` | `SERIAL_ONLY` | dependence on `A`: offset 0 vs −1 |
| `interprocedural_unsafe:28` | `accumulate(A[i])` | `SERIAL_ONLY` | **callee mutates a global** — found only by looking *inside* the call |

That last row is the whole point: the loop body itself looks perfectly parallel. Only whole-program
mod/ref propagation reveals that `accumulate()` carries order-dependent state in a global.

### Measured speedup

Generated OpenMP code, compiled with `g++ -O3 -march=native -fopenmp` and timed over 5 runs each
(compute loop only, via an in-program wall clock — not shell `time`, which would fold in process
startup and first-touch page-fault cost):

| | Time |
|---|---|
| Sequential baseline | ~0.168 s |
| Generated OpenMP, 2 threads | ~0.149 s |
| **Speedup** | **~1.1–1.3×** (varies run to run) |

Correctness is checked *before* any speedup is reported — sequential and parallel output must be
byte-identical, and they are.

> **On that number, honestly:** this was measured in a sandbox with **2 shared vCPUs** on a
> virtualized cloud VM, so ~1.2× against a 2× theoretical ceiling is about what memory-bandwidth
> contention allows. The same generated output on dedicated multi-core hardware should scale
> considerably better. Run `./demo.sh` on your own machine to see.

### Profile-guided GPU decision

Pass 1 instruments the code; running it emits real measured counters; Pass 2 applies the roofline
model to *those* numbers, not to static guesses:

```json
[{"loop_id": "test_loops.cpp:17", "trip_count": 1, "flops": 6, "bytes_read": 16, "bytes_written": 8}]
```

```
test_loops.cpp:17: CPU_PREFERRED (intensity=0.25 ridge=11.1111 trips=1)
    flop_intensity=0.25  ridge_point=11.1111  trip_count=1  min_gpu_trips=10000
```

Arithmetic intensity of 0.25 FLOP/byte sits far below the ridge point of 11.1 — this loop is
memory-bound, so shipping it to a GPU would lose to PCIe transfer cost. The compiler says so and
keeps it on the CPU.

### Test suite

**11/11 CTest cases pass**, verified from a clean clone (not just a dirty working tree):

```
100% tests passed, 0 tests failed out of 11
```

Covering: smoke, IR extraction, parallelism classification, instrumentation, instrumented-binary
compile + run, profile-guided transform, transformed-output compile, and all three benchmarks.

---

## Setup

### Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| ROSE | 2.14.0 | Expected at `/config/workspace/rose/install` (override with `-DRose_DIR=`) |
| Clang/LLVM | 18.x | Headers + static libs; see below if you can't `apt install` |
| CMake | ≥ 3.15 | |
| GCC | any with `-fopenmp` | Compiles the generated output |
| Boost | 1.83 | Pulled in transitively by ROSE |

### Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Run the demo

```bash
./demo.sh
```

One command, end to end: classification with reasons → two-pass profiling → roofline decision →
compile the generated OpenMP → correctness check → timed speedup.

### If you don't have root (no `apt install clang-18-dev`)

The build is set up to work against a **locally unpacked** toolchain — no root required:

```bash
mkdir -p toolchain && cd toolchain
dpkg-deb -x /path/to/libclang-18-dev_*.deb .
dpkg-deb -x /path/to/llvm-18_*.deb          .
dpkg-deb -x /path/to/llvm-18-dev_*.deb      .
```

`CMakeLists.txt` already searches `../toolchain/usr/lib/llvm-18` alongside the usual system paths.

One wrinkle: `LLVMExports.cmake` references optional plugin files that the `-dev` packages don't
ship, and `find_package(LLVM CONFIG)` hard-fails on any missing imported-target file. Symlink them
to the real shared library (nothing actually links against LTO/gold/Polly here — the stubs just
need to *exist*):

```bash
cd toolchain/usr/lib/llvm-18/lib
for f in libLTO.so.18.1 LLVMgold.so LLVMPolly.so libLLVM.so.1; do
    ln -sf /usr/lib/llvm-18/lib/libLLVM.so.1 $f
done
```

> **Note on C++17:** the project builds at `-std=c++17` even though ROSE predates it. LLVM 18's own
> headers use `std::optional` / `std::is_integral_v` and simply will not parse as C++14. ROSE
> compiles fine as a strict superset; `_GLIBCXX_USE_CXX11_ABI=1` (the std::string ABI ROSE was
> built against) is orthogonal to the language standard and is still set.

---

## System architecture

```
                      C/C++ source (unmodified)
                                 │
              ┌──────────────────▼──────────────────┐
              │  ClangBridge                Task 2  │   clang::RecursiveASTVisitor
              │  Clang AST ──► ProgramIR            │   over a ClangTool
              └──────────────────┬──────────────────┘
                                 │  FunctionIR / LoopNest / ArrayAccess / CallSite
                                 │  (zero ROSE types — independently testable)
              ┌──────────────────▼──────────────────┐
              │  RoseBridge                 Task 3  │   SageBuilder — NOT frontend(),
              │  ProgramIR ──► SgProject            │   this ROSE has no C/C++ frontend
              └──────────────────┬──────────────────┘
                                 │  SgProject*  +  loopMap: "file:line" ──► SgForStatement*
              ┌──────────────────▼──────────────────┐
              │  InterproceduralAnalysis    Task 4  │   CallGraphBuilder + Tarjan SCC
              │  whole-program mod/ref propagation  │   bottom-up worklist to fixpoint
              └──────────────────┬──────────────────┘
                                 │  FunctionSummary: modSet / refSet / modifiesGlobals / isPure
              ┌──────────────────▼──────────────────┐
              │  ParallelismChecker         Task 5  │   affine index analysis
              │  ──► LoopVerdict per loop           │   + callee summaries
              └──────────────────┬──────────────────┘
                                 │  PARALLEL_SAFE │ REDUCTION_CANDIDATE │ SERIAL_ONLY │ UNKNOWN
                    ┌────────────┴────────────┐
                    │                         │
   ┌────────────────▼──────────┐   ┌──────────▼─────────────────┐
   │ ProfilingInstrumentor     │   │ GpuSuitabilityEngine       │
   │ Pass 1      Task 6        │   │ Pass 2            Task 7   │
   │ inject counters ──► run   │──►│ roofline model             │
   │ ──► profile.json          │   │ ──► GPU_PROFITABLE / CPU   │
   └───────────────────────────┘   └──────────┬─────────────────┘
                                              │
              ┌───────────────────────────────▼─────┐
              │  OmpCodegen                 Task 8  │   pragma text + map(to/from/tofrom)
              │  ──► "#pragma omp ..." string       │   inferred from access direction
              └──────────────────┬──────────────────┘
                                 │
              ┌──────────────────▼──────────────────┐
              │  text-level insertion into original │
              │  source  (translator.cpp)           │
              └──────────────────┬──────────────────┘
                                 │
                   transformed, compilable C/C++
```

### The modules

| Module | Lines | Role |
|---|---:|---|
| `ClangBridge` | 529 | Parses real C/C++ via `clang::RecursiveASTVisitor`; extracts loop nests, array subscripts (with read/write direction), call sites, and function signatures into a ROSE-independent `ProgramIR`. |
| `RoseBridge` | 523 | Reconstructs an `SgProject` programmatically with `SageBuilder`, giving the ROSE analysis APIs something to chew on. |
| `InterproceduralAnalysis` | 857 | Builds the call graph, computes per-function read/write sets, and propagates them **bottom-up over Tarjan SCCs** to a fixpoint. Recursive cycles are collapsed and treated conservatively. Known-pure library functions (`sqrt`, `memcpy`, …) short-circuit. |
| `ParallelismChecker` | 551 | Per loop: parses each array index as an affine expression in the induction variable, looks for cross-iteration overlap (`A[i]` written while `A[i-1]` is read ⇒ carried dependence), then consults callee summaries for any call in the body. |
| `ProfilingInstrumentor` + `profiler.c` | 601 | Pass 1. Injects `profile_loop_start/end` around parallel-safe loops; the dependency-free C runtime writes `profile.json` on exit via `atexit()`. |
| `GpuSuitabilityEngine` | 241 | Pass 2. `flop_intensity = flops/(bytes_read+bytes_written)` vs `ridge_point = peak_flops/peak_bw`; GPU only if intensity clears the ridge **and** trip count clears a floor. Static fallback when no profile is supplied. |
| `OmpCodegen` | 257 | Builds the directive text, classifying each array as read-only / write-only / read-write to infer `map(to:)` / `map(from:)` / `map(tofrom:)`. |
| `translator.cpp` | 402 | CLI driver; wires the pipeline and performs the final text-level pragma insertion. |

### Three design decisions worth explaining

**1. Clang is the frontend, not ROSE's EDG.**
The installed ROSE was built with `AM_ROSE_BUILD_C_LANGUAGE_SUPPORT=false` — it has *no source
frontend at all*, only binary analysis. `frontend()` cannot parse a `.cpp` file here. So Clang does
all parsing, and ROSE is used purely for its analysis APIs over an AST we construct ourselves.
A consequence worth knowing: `SgSourceFile::buildAST()` still tries to invoke the absent EDG
frontend and throws, unless `-rose:skip_parser` is in the project's command-line argument list.
That flag is load-bearing, not cosmetic.

**2. Codegen is text-level, not AST-unparse.**
The natural ROSE approach — mutate the AST, call `backend()` — produces output that doesn't
compile, because `RoseBridge` reconstructs loop *bodies* as placeholder declarations rather than
real statements. Rather than pretend otherwise, final codegen inserts the pragma directly above the
loop's line in the **original source text**, which preserves program logic exactly. The AST path is
still available behind `--ast-codegen` for anyone who wants to improve `RoseBridge`'s fidelity.

**3. When the analysis can't prove safety, it refuses.**
A false "safe" verdict in a parallelizing compiler is a miscompile — a data race that silently
corrupts results. A false "unsafe" verdict just leaves performance unclaimed. Every ambiguity
resolves toward `SERIAL_ONLY` / `UNKNOWN_CONSERVATIVE`. During development a fix that improved
precision was found to flip a genuinely-unsafe loop to "safe"; it was **reverted rather than
shipped**, and the reasoning is preserved in `CONTEXT.md` and in the comment block at the top of
`InterproceduralAnalysis::analyze()`.

---

## CLI

```
translator [OPTIONS] <input.cpp>

  --instrument              Pass 1: insert profiling instrumentation
  --profile=<file>          Pass 2: read profile JSON for GPU suitability
  --gpu-peak-flops=<N>      GPU peak FLOP/s in GFLOPS      (default: 10000)
  --gpu-peak-bw=<N>         GPU peak memory bandwidth GB/s (default: 900)
  --min-gpu-trips=<N>       Minimum trip count for offload (default: 10000)
  -o <output.cpp>           Output file            (default: rose_<input>.cpp)
  --dump-ir                 Print extracted Clang IR and exit
  --dump-sage-ast           Print reconstructed ROSE AST summary and exit
  --dump-modref             Print interprocedural mod/ref tables and exit
  --check-parallel          Print loop parallelism classification and exit
  --dump-gpu-decision       Print GPU suitability decisions and exit
  --ast-codegen             Use the ROSE-AST unparse path (see above)
  --help                    Show this message
```

Typical two-pass workflow:

```bash
./build/translator --check-parallel program.cpp          # what's safe, and why
./build/translator --instrument -o prof.cpp program.cpp  # pass 1
g++ -I support prof.cpp support/profiler.c -o prof && ./prof   # → profile.json
./build/translator --profile=profile.json -o out.cpp program.cpp  # pass 2
g++ -O3 -fopenmp out.cpp -o out                          # build the result
```

---

## Known limitations

Documented rather than hidden — see `CONTEXT.md` for the full write-ups.

- **Scalar variables are invisible to the IR.** `ArrayAccess` records array subscripts only, never
  plain scalar reads/writes. Two consequences: a scalar reduction (`sum += A[i]`) can be
  misclassified `PARALLEL_SAFE` — a real race under a bare `parallel for` — and a callee whose only
  side effect is a scalar global write can't be seen. **Avoid both patterns** on input you intend to
  transform until `ClangBridge` is extended. This is the single highest-value next fix.
- **Multi-dimensional arrays aren't parsed correctly.** `A[i][k]` decomposes into two garbled
  entries because array-name extraction falls back to raw source text for any base that isn't a
  plain `DeclRefExpr`. Every loop in `polybench_gemm` therefore lands in `UNKNOWN_CONSERVATIVE`
  (safe, but unhelpful). 1D arrays are unaffected.
- **Mod/ref is whole-variable, not per-element.** A callee that writes only `C[idx]` for the caller's
  own loop index is provably safe but still gets rejected, because the summary only records *that*
  `C` was modified.
- **Macro loop bounds read as non-constant.** `Lexer::getSourceText` returns `"NI"`, not `256`, so
  the static (no-profile) path conservatively picks `CPU_PREFERRED`. The `--profile=` path is
  unaffected — it uses measured trip counts.
- **No GPU was available** in the development environment, so the offload path is validated by the
  decision logic and generated directives, not by measured GPU execution.

---

## Repository layout

```
├── src/                     compiler implementation (8 modules)
├── support/                 profiler.c/h — dependency-free runtime for Pass 1
├── benchmarks/
│   ├── interprocedural_safe/    parallelism across a call boundary
│   ├── interprocedural_unsafe/  must be rejected (callee mutates a global)
│   ├── polybench_gemm/          dense GEMM
│   └── demo_speedup/            compute-bound kernel used for timing
├── tests/CMakeLists.txt     11 CTest cases
├── demo.sh                  one-command walkthrough
├── PRD.md                   requirements & CLI spec this implements
└── CONTEXT.md               status, environment notes, honest gap list
```
