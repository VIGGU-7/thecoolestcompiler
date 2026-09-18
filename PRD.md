# Product Requirements Document
## Interprocedural GPU-Offload Parallelizing Compiler

**Project:** thecoolestcompiler  
**Version:** 1.0  
**Date:** 2026-09-18  
**Status:** Planning complete — implementation not started

---

## 1. Problem Statement

Automatic parallelization in compilers is typically limited in two ways:

1. **Intraprocedural scope only.** A hot loop calling a helper function is conservatively treated as non-parallelizable because the compiler does not look inside the callee to determine whether it reads or writes loop-iterated data in a cross-iteration-unsafe way. This leaves a class of parallelizable loops unexploited.

2. **GPU blindness.** When a region is identified as safe to parallelize, the compiler defaults to CPU threading (OpenMP/OpenACC on host) without reasoning about whether the region is actually a good fit for GPGPU execution — leaving data-parallel workloads with high arithmetic intensity and large trip counts on the table.

Most auto-parallelizing compilers do one of the following:
- Skip interprocedural analysis entirely (GCC `-O3 -ftree-parallelize-loops`)
- Perform interprocedural analysis but target only CPU threads
- Provide manual annotation schemes (OpenMP `#pragma omp target`) requiring programmer effort

None perform **whole-program interprocedural dependence analysis** combined with an **automatic GPU-suitability decision**.

---

## 2. Goal

Design and implement a source-to-source compiler framework — built on top of ROSE (LLNL, v2.14.0) — that:

1. Parses real, unmodified sequential C/C++ programs
2. Performs whole-program interprocedural dependence analysis across function boundaries
3. Reasons explicitly about whether each identified parallel region is GPU-profitable vs. CPU-suitable
4. Emits OpenMP 4.5+ `target` offload directives for GPU-profitable regions and `parallel for` for CPU-suitable regions
5. Validates correctness and measures speedup against standard benchmarks

---

## 3. Non-Goals

- Supporting Fortran, Java, or other languages (C/C++ only)
- Replacing a production compiler — this is a research/demonstration framework
- Handling C++ virtual dispatch, function pointers, or exception-based control flow with full precision (conservative treatment acceptable)
- Auto-tuning GPU thread block geometry (fixed `num_teams` / `thread_limit` defaults acceptable)
- Supporting distributed memory (MPI) parallelism

---

## 4. Success Criteria

| Criterion | Metric |
|---|---|
| Interprocedural parallelism found | At least one benchmark where a loop is parallelized that intraprocedural-only analysis would skip (the callee is proven side-effect-free via mod/ref summary) |
| GPU decision quality | GPU offload decision measurably outperforms naive "offload everything safe" policy on a mixed benchmark set (at least one region correctly left on CPU due to low arithmetic intensity) |
| Correctness | Numerical output of transformed programs matches sequential baseline within floating-point tolerance for all benchmark inputs |
| GPU speedup | At least one GPU-suitable benchmark (e.g., Polybench GEMM) shows measurable speedup when compiled with an offload-capable compiler |
| Build | Framework compiles from source with `cmake + make`, all CTests pass |

---

## 5. Architecture Overview

```
C/C++ Source
     │
     ▼
┌─────────────────────┐
│  Clang Frontend     │  libclang / libtooling
│  (Task 2)           │  RecursiveASTVisitor
└────────┬────────────┘
         │  LoopNest / FunctionIR structs
         ▼
┌─────────────────────┐
│  ROSE AST Builder   │  SageBuilder / SageInterface
│  (Task 3)           │  Reconstructs SgProject
└────────┬────────────┘
         │  SgProject*
         ▼
┌─────────────────────┐
│  Call Graph +       │  ROSE CallGraph API
│  Mod/Ref Propagator │  LocalRWSetGenerator
│  (Task 4)           │  Bottom-up worklist
└────────┬────────────┘
         │  InterproceduralSummary
         ▼
┌─────────────────────┐
│  Dependence Checker │  ROSE DepInfo / StmtDepAnal
│  (Task 5)           │  + callee mod/ref summaries
└────────┬────────────┘
         │  PARALLEL_SAFE / SERIAL_ONLY / REDUCTION_CANDIDATE
         ▼
┌─────────────────────────────────────────────────┐
│              GPU Suitability Decision            │
│                                                 │
│  Pass 1 (--instrument):  Task 6                 │
│    Insert trip/flop/byte counters               │
│    Emit instrumented binary → run → profile.json│
│                                                 │
│  Pass 2 (--profile=...): Task 7                 │
│    Roofline model score per loop                │
│    GPU_PROFITABLE / CPU_PREFERRED               │
└────────┬────────────────────────────────────────┘
         │
         ▼
┌─────────────────────┐
│  OMP Codegen        │  SageBuilder::buildPragmaDeclaration
│  (Task 8)           │  target teams distribute parallel for
│                     │  map(to/from/tofrom) inference
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  ROSE Unparser      │  backend(project)
│                     │  Transformed C++ source
└─────────────────────┘
```

---

## 6. Detailed Requirements

### 6.1 Frontend (Clang Bridge)

**REQ-F1.** The translator must accept unmodified C/C++ source files as input without requiring any source annotations.

**REQ-F2.** The Clang bridge must extract the following per-function information:
- Function name (mangled and demangled)
- Parameter list with types
- All `for` loop nests: depth, trip-bound expression (as a string), array subscript accesses (variable name, index expression, read/write)
- All direct call sites (callee name, argument expressions)

**REQ-F3.** For functions whose definitions are not available (library calls, external symbols), the bridge must record them as opaque callees. The analysis layer will treat them conservatively.

**REQ-F4.** The Clang bridge must produce a `programIR: map<string, FunctionIR>` data structure that is independent of any ROSE types, making it testable without a ROSE installation.

### 6.2 ROSE AST Reconstruction

**REQ-R1.** The ROSE bridge must produce an `SgProject*` that passes `AstTests::runAllTests()` without errors.

**REQ-R2.** The reconstructed AST must preserve enough structural fidelity for ROSE's `CallGraph`, `DepInfo`, and `SageInterface::insertStatementBefore` APIs to operate correctly.

**REQ-R3.** The reconstruction need not preserve comments, macros, or formatting — only semantic structure.

### 6.3 Interprocedural Analysis

**REQ-I1.** Build the full program call graph using `CallGraphBuilder`. Traverse all `SgFunctionDeclaration` nodes in the project.

**REQ-I2.** Compute per-function read/write sets using `LocalRWSetGenerator::generateRWSetsForFunction()`. Classify accesses using `ReadWriteSets::Globality` (LOCALS, PARAMETERS, GLOBALS, etc.) and `ReadWriteSets::VarType`.

**REQ-I3.** Propagate mod/ref sets bottom-up through the call graph using a worklist algorithm. Iterate until fixpoint. Handle cycles (mutual recursion) by treating the entire SCC conservatively as modifying all non-local variables it touches.

**REQ-I4.** Functions without definitions must be treated as `GLOBALS_MODIFIED` unless they appear in a built-in pure-function whitelist (e.g., `sqrt`, `fabs`, `sin`, `cos`, `exp`, `log`, `memcpy` for source-side read-only arguments).

**REQ-I5.** Expose `getModSet(funcName)` and `getRefSet(funcName)` returning sets of `VarAccess` structs.

### 6.4 Parallelism Classification

**REQ-P1.** For each loop nest, run `DepInfoAnal` on all array accesses within the loop body to detect loop-carried dependences (RAW, WAW, WAR across loop iterations in the same array).

**REQ-P2.** For each call site within a loop body, look up the callee's interprocedural mod/ref summary. If any variable in the callee's mod-set overlaps with any variable read by a different iteration of the loop, the loop is `SERIAL_ONLY`.

**REQ-P3.** Classify each loop as one of: `PARALLEL_SAFE`, `REDUCTION_CANDIDATE`, `SERIAL_ONLY`, `UNKNOWN_CONSERVATIVE`.

**REQ-P4.** `UNKNOWN_CONSERVATIVE` is applied when: unresolved indirect calls (function pointers), non-affine array index expressions, or unresolvable callee chains are detected. These loops are left untransformed.

**REQ-P5.** `REDUCTION_CANDIDATE` loops must have their reduction variable and operator identified (e.g., `sum += A[i]` → `reduction(+:sum)`).

### 6.5 Pass 1 — Profiling Instrumentation

**REQ-PR1.** Pass 1 is activated by the `--instrument` CLI flag. It does not modify the program's observable output (counters are written to a separate file).

**REQ-PR2.** For each `PARALLEL_SAFE` loop, insert calls to:
- `profile_loop_start(const char* loop_id)` — before the loop
- `profile_loop_end(const char* loop_id, long trip_count, long flops, long bytes_read, long bytes_written)` — after the loop
where `flops`, `bytes_read`, `bytes_written` are static compile-time estimates injected as integer literals.

**REQ-PR3.** The profiler runtime (`support/profiler.c`) must write a `profile.json` file on program exit using `atexit()`. The JSON schema is:
```json
[
  {
    "loop_id": "file.cpp:14",
    "trip_count": 50000000,
    "flops": 150000000,
    "bytes_read": 400000000,
    "bytes_written": 200000000
  }
]
```

**REQ-PR4.** Pass 1 must call `backend(project)` to produce a compilable, self-contained C++ output file that can be compiled with `g++ profiler.c output.cpp -o instrumented`.

### 6.6 GPU Suitability Decision Engine

**REQ-G1.** Pass 2 is activated by `--profile=<path>` pointing to the JSON produced by Pass 1. Without this flag, static fallback estimates are used.

**REQ-G2.** GPU suitability is determined by the roofline model:
- `flop_intensity = flops / (bytes_read + bytes_written)` (FLOP/byte)
- `ridge_point = GPU_PEAK_FLOPS_GFLOPS / GPU_PEAK_BANDWIDTH_GBs`
- If `flop_intensity >= ridge_point` AND `trip_count >= MIN_GPU_TRIP_COUNT` → `GPU_PROFITABLE`
- Otherwise → `CPU_PREFERRED`

**REQ-G3.** Default hardware constants (overridable via CLI):
- `--gpu-peak-flops=10000` (GFLOP/s)
- `--gpu-peak-bw=900` (GB/s)
- `--min-gpu-trips=10000`

**REQ-G4.** Static fallback: loops with non-constant trip bounds are classified `CPU_PREFERRED`. Loops with constant bounds `>= MIN_GPU_TRIP_COUNT` and arithmetic intensity above ridge point are classified `GPU_PROFITABLE`.

### 6.7 OpenMP Code Generation

**REQ-C1.** For `GPU_PROFITABLE` loops, emit:
```cpp
#pragma omp target teams distribute parallel for \
    map(to: <read-only arrays>[0:N]) \
    map(from: <write-only arrays>[0:N]) \
    map(tofrom: <read-write arrays>[0:N])
```

**REQ-C2.** For `REDUCTION_CANDIDATE` loops classified `GPU_PROFITABLE`, add `reduction(<op>:<var>)` to the target pragma.

**REQ-C3.** For `CPU_PREFERRED` loops (that are `PARALLEL_SAFE`), emit `#pragma omp parallel for` with any applicable `reduction` clause.

**REQ-C4.** Array size in map clauses must use the trip-bound expression extracted by the Clang bridge (e.g., `A[0:N]` where `N` is the loop bound variable).

**REQ-C5.** `SERIAL_ONLY` and `UNKNOWN_CONSERVATIVE` loops must be left entirely untransformed.

**REQ-C6.** The output file must compile without errors using `g++ -fopenmp` on a system with an OpenMP-capable compiler.

### 6.8 Validation

**REQ-V1.** Three mandatory benchmark test cases (see Task 9):
1. `interprocedural_safe` — demonstrates parallelism found across function boundary
2. `interprocedural_unsafe` — demonstrates correct rejection due to global write
3. `polybench_gemm` — demonstrates GPU offload path with correctness verification

**REQ-V2.** For each benchmark, a CTest entry must: transform → compile → run → diff output against sequential reference.

**REQ-V3.** Numerical correctness tolerance: relative error `< 1e-6` for floating-point results.

---

## 7. CLI Interface

```
./translator [OPTIONS] <input.cpp>

Options:
  --instrument              Pass 1: insert profiling instrumentation
  --profile=<file>          Pass 2: read profile JSON for GPU suitability
  --gpu-peak-flops=<N>      GPU peak FLOP/s in GFLOPS (default: 10000)
  --gpu-peak-bw=<N>         GPU peak memory bandwidth in GB/s (default: 900)
  --min-gpu-trips=<N>       Minimum trip count for GPU offload (default: 10000)
  -o <output.cpp>           Output file (default: rose_<input>.cpp)
  --dump-ir                 Print extracted Clang IR to stdout and exit
  --dump-sage-ast           Print ROSE AST as DOT and exit
  --dump-modref             Print interprocedural mod/ref tables and exit
  --check-parallel          Print loop parallelism classification and exit
  --dump-gpu-decision       Print GPU suitability decisions and exit
  --help                    Show this message
```

---

## 8. Task List

| # | Task | Description | Status |
|---|---|---|---|
| 1 | Build system | CMakeLists.txt, link ROSE + Clang, smoke test | Not started |
| 2 | Clang bridge | Clang AST → LoopNest/FunctionIR IR extraction | Not started |
| 3 | ROSE AST builder | IR → SgProject reconstruction via SageBuilder | Not started |
| 4 | Interprocedural analysis | CallGraph + mod/ref bottom-up propagation | Not started |
| 5 | Parallelism checker | DepInfo + summary → PARALLEL_SAFE/SERIAL_ONLY | Not started |
| 6 | Pass 1 instrumentation | Trip/flop/byte counters + profiler runtime | Not started |
| 7 | GPU suitability engine | Roofline model decision, profile JSON reader | Not started |
| 8 | OMP codegen | target/parallel for pragma emission | Not started |
| 9 | Benchmark validation | 3 benchmarks, CTest, correctness + speedup | Not started |

---

## 9. Dependencies

| Dependency | Version | Role |
|---|---|---|
| ROSE | 2.14.0 (installed) | Analysis APIs, AST builder, unparser |
| Clang/LLVM | System (14+) | C/C++ frontend (libclang, libtooling) |
| Boost | 1.83.0 (via ROSE) | Graph, filesystem, program_options |
| CMake | 3.15+ | Build system |
| nlohmann/json | (bundled with ROSE) | Profile JSON read/write |
| GCC / Clang OpenMP | Any with `-fopenmp` | Compile and run transformed output |

---

## 10. Repository Layout (Target)

```
thecoolestcompiler/
├── CMakeLists.txt
├── PRD.md                          ← this file
├── CONTEXT.md                      ← current status + handoff notes
├── src/
│   ├── translator.cpp              ← main() pipeline driver
│   ├── ClangBridge.h
│   ├── ClangBridge.cpp
│   ├── RoseBridge.h
│   ├── RoseBridge.cpp
│   ├── InterproceduralAnalysis.h
│   ├── InterproceduralAnalysis.cpp
│   ├── ParallelismChecker.h
│   ├── ParallelismChecker.cpp
│   ├── ProfilingInstrumentor.h
│   ├── ProfilingInstrumentor.cpp
│   ├── GpuSuitabilityEngine.h
│   ├── GpuSuitabilityEngine.cpp
│   ├── OmpCodegen.h
│   └── OmpCodegen.cpp
├── support/
│   ├── profiler.h
│   └── profiler.c
├── tests/
│   ├── CMakeLists.txt
│   └── test_loops.cpp
└── benchmarks/
    ├── interprocedural_safe/
    ├── interprocedural_unsafe/
    └── polybench_gemm/
```
