# CONTEXT — Project Status & Handoff Notes
## Interprocedural GPU-Offload Parallelizing Compiler

**Last updated:** 2026-09-18  
**Current phase:** Planning complete — Task 1 (build system) is next  
**Active branch:** `issue-2` (local only, not yet pushed)

---

## Quick Status

| Task | Description | Status | Notes |
|---|---|---|---|
| 1 | CMakeLists.txt + build scaffold | ✅ Done, verified | Builds against a locally-extracted (no-root) LLVM/Clang 18 toolchain at C++17 (see "Toolchain" below) |
| 2 | Clang bridge (IR extraction) | ✅ Done, verified | `--dump-ir` on `test_loops.cpp` produces correct `ArrayAccess`/`LoopNest`/`FunctionIR` data, including correct `WRITE`/`READ` classification |
| 3 | ROSE AST reconstruction | ✅ Done, verified | Real bug found+fixed: see "RoseBridge crash fixes" below |
| 4 | Call graph + mod/ref propagation | ✅ Done, verified | `--dump-modref` produces real (if occasionally coarse) summaries |
| 5 | Parallelism checker | ✅ Done, verified | `--check-parallel` correctly classifies the `test_loops.cpp` recurrence as `SERIAL_ONLY` and the two other loops `PARALLEL_SAFE`; see "Known analysis-precision limitation" below |
| 6 | Pass 1 instrumentation | ✅ Done, verified | `--instrument` output compiles, runs, and produces a real `profile.json` (text-level transform, high fidelity) |
| 7 | GPU suitability engine | ✅ Done, verified | Roofline math verified against a real profile.json; static fallback (REQ-G4) verified |
| 8 | OMP codegen | ✅ Done, verified | Default codegen path is now **text-level** (`emitTextLevelOmp()` in `translator.cpp`), not the AST-unparse path — see gap #1 below for why. Old AST path kept behind `--ast-codegen`. |
| 9 | Benchmark validation | ✅ Done, verified | 3 benchmarks + reference outputs; **11/11 CTest cases pass**. Plus `benchmarks/demo_speedup/` (new) for a live speedup demo, and `demo.sh` (new) that runs the whole pipeline and prints real numbers in one command. |

**Full pipeline is wired and runs end-to-end** (`translator.cpp` main() dispatches all CLI modes from PRD §7). Verified with `ctest --output-on-failure` in `build/`: **11/11 tests pass** (was 10/11 — fixed by switching default codegen to the text-level path, see gap #1's resolution below). Run `./demo.sh` from `thecoolestcompiler/` for a one-command walkthrough with real measured numbers (classification, profiling, roofline decision, and a real compiled+timed speedup with a correctness check).

### Known remaining gaps (honest, not swept under the rug)

1. **RESOLVED — default codegen switched to text-level.** The AST-unparse path (`OmpCodegen::applyPragmas` + ROSE's `backend()`) was producing non-compilable output because RoseBridge only rebuilds loop bodies as placeholder variable declarations, not real statements (documented, original RoseBridge design — not a regression). Fixed by adding `emitTextLevelOmp()` to `translator.cpp`: it inserts `#pragma omp ...` directly above each decided loop's line in the **original source text** (same proven technique as `ProfilingInstrumentor`, just simpler since a pragma only needs the loop's start line, not its end). This is now the default; the old AST path is preserved behind `--ast-codegen` for anyone who wants to continue improving RoseBridge's body-reconstruction fidelity instead. `T4c_compile_transformed` now passes.
2. **A second, more serious interprocedural-analysis bug was found and *deliberately reverted*, not fixed — read this before touching `InterproceduralAnalysis.cpp` again.** `InterproceduralAnalysis` keys summaries two different ways for the same function — ROSE's own local analysis uses ROSE-style keys (`"::normalize_value(double)"`), while `ParallelismChecker` looks summaries up by ClangBridge's plain name (`"normalize_value"`). A fix was written that pre-seeded plain-keyed entries before `computeLocalSummaries()`'s tail sweep could conservative-stamp them as "unknown callee". That fix **worked** for functions ClangBridge can see effects of (array accesses) — but it also flipped functions whose only side effect is a **plain scalar global write** (e.g. `running_total += v;` inside a callee) from correctly-conservative to incorrectly-"pure", because `ArrayAccess` only records array subscripts, never scalar variable reads/writes — so a genuinely order-dependent loop (`benchmarks/interprocedural_unsafe/main.cpp`) got reclassified from `SERIAL_ONLY` to `REDUCTION_CANDIDATE`, which would have applied `#pragma omp parallel for` to code with a real data race. **A false-safe verdict in a parallelizing compiler is a correctness bug, not a missed optimization — it was reverted.** Root cause and a real fix (extending ClangBridge to track scalar/global variable accesses, not just array subscripts, so summaries can be soundly reconciled) is future work; see the revert's comment block at the top of `InterproceduralAnalysis::analyze()` for the full writeup. **The same blind spot also affects scalar reductions** (`sum += A[i]` gets misclassified `PARALLEL_SAFE`, which is also an unsound data race under naive `#pragma omp parallel for` without a `reduction()` clause) — avoid both patterns in any input you expect this tool to safely transform until ClangBridge is extended.
3. **Analysis precision is coarser than the `interprocedural_safe` benchmark assumes** (independent of gap #2 above — this one is a real, still-conservative limitation, not a soundness bug). `InterproceduralAnalysis`'s `modifiesGlobals` flag is all-or-nothing (a function either does or doesn't touch some global), not indexed per-element. So a call like `scale_by_index(i, ...)` — which only ever writes `C[i]` (the same `i` as the caller's own loop index, hence actually safe) — gets conservatively classified `SERIAL_ONLY` rather than the intended `PARALLEL_SAFE`. This is REQ-P4's conservative-by-default behavior working as designed — safe, just not maximally precise.
4. **GPU suitability on `polybench_gemm`** currently reports `CPU_PREFERRED` for all loops under the static (no-profile) path — not because of low arithmetic intensity, but because Clang's `Lexer::getSourceText` on the loop bound returns the macro *name* text (`"NI"`, `"NK"`) rather than the preprocessor-resolved literal (`256`), so `GpuSuitabilityEngine::classifyStatic` sees a non-constant bound and correctly (per REQ-G4) falls back to `CPU_PREFERRED`. The `--profile=`-driven path (Pass 2 proper) would classify correctly since it uses real runtime trip counts, not source text.
5. **2D array indexing is mis-parsed.** `A[i][k]` gets extracted by ClangBridge as two separate, garbled `ArrayAccess` entries (`arrayName="A[i]", index="k"` for the outer subscript, plus `arrayName="A", index="i"` for the inner one) because the array-name extraction falls back to raw source text for any base expression that isn't a plain `DeclRefExpr`/`MemberExpr` — a nested `ArraySubscriptExpr` (i.e. any multi-dimensional array) hits that fallback. This is why every loop in `polybench_gemm` (all 2D arrays) comes back `UNKNOWN_CONSERVATIVE` — the affine parser sees an index like `"k"` relative to induction variable `"i"` and correctly can't make sense of it. 1D arrays are unaffected (verified extensively). Fixing this needs ClangBridge to flatten nested subscripts into one `(baseArrayName, [index0, index1, ...])` representation.

### RoseBridge crash fixes (real bugs found via AddressSanitizer, not hypothetical)

1. **`SgProject` must carry a non-empty `originalCommandLineArgumentList`.** A bare `new SgProject()` (RoseBridge's original code) left this list empty; `SageBuilder::buildSourceFile()` → `buildFile()` → `determineFileType()` then segfaults dereferencing uninitialized `SgFile` state (`SgFile::get_preprocessorDirectivesAndCommentsList()` on a null-ish object). Fix: explicitly set `project->set_originalCommandLineArgumentList({"cc", "-c", "-rose:skip_parser"})` before building the source file.
2. **`-rose:skip_parser` is load-bearing, not optional.** Without it, `SgSourceFile::buildAST()` unconditionally tries to invoke the (absent — this ROSE build has `AM_ROSE_BUILD_C_LANGUAGE_SUPPORT=false`, confirmed via the CMake configure summary) EDG frontend and throws a `frontend_failed` C++ exception. With it, `buildAST()` returns 0 immediately (`if (get_skip_parser()) return 0;` in ROSE's `sage_support.C`), leaving a valid empty `SgGlobal` for `SageBuilder` to populate.
3. **`SageBuilder::buildFunctionCallExp(name, voidType, argList, scope)` segfaults** (`SgFunctionRefExp::get_type()` on a null symbol) when `name` happens to resolve, via scope lookup, to a function this same reconstruction pass already forward-declared with a real signature — i.e. almost any call to a function defined elsewhere in the same program, which is exactly the common case for interprocedural test code. Fix: removed the "call-site stub" statement-building in `buildLoopForNest()` and the function-body-level equivalent entirely (see comments in `RoseBridge.cpp`) — these stub statements were cosmetic only; neither the interprocedural analysis (which runs on ClangBridge's independent `ProgramIR`) nor `OmpCodegen` (which locates loops via `RoseBridge::loopMap` without inspecting body contents) depends on them.

Note: "done, verified" above means actually run against real input and checked, not just "compiles."

---

## Environment Facts

### Repository
```
Path:     /config/workspace/thecoolestcompiler/
Remote:   https://github.com/VIGGU-7/thecoolestcompiler.git
Branches: dashank/issue1 (remote), side (remote), issue-2 (local, current)
```

Existing files:
- `translator.cpp` — skeleton AST traversal using `AstSimpleProcessing`, visits loops/functions/variables and prints them. Calls `frontend(argc, argv)`. **No build system exists yet.**
- `test_loops.cpp` — test input with two loops: one parallelizable vector-add and one serial recurrence (`A[i] = A[i-1] + B[i]`), plus a `side_effect_function` that writes to a global.

### Toolchain (LLVM/Clang 18) — no root available

This environment has no passwordless sudo, so the `libclang-18-dev`, `llvm-18`,
and `llvm-18-dev` `.deb` files sitting in `/config/workspace/*.deb` cannot be
`apt install`ed. Workaround: extracted them directly with `dpkg-deb -x` (no
root needed) into `/config/workspace/toolchain/`:
```
dpkg-deb -x /config/workspace/libclang-18-dev_*.deb /config/workspace/toolchain
dpkg-deb -x /config/workspace/llvm-18_*.deb          /config/workspace/toolchain
dpkg-deb -x /config/workspace/llvm-18-dev_*.deb      /config/workspace/toolchain
```
Headers land at `/config/workspace/toolchain/usr/lib/llvm-18/include`, static
Clang/LLVM libs at `/config/workspace/toolchain/usr/lib/llvm-18/lib`.
`CMakeLists.txt` already has this path added to `CMAKE_PREFIX_PATH` and every
manual `find_library`/`find_path` HINTS list.

`find_package(LLVM CONFIG)` initially failed because `LLVMExports.cmake`
references three optional plugin/runtime files that aren't part of the `-dev`
packages (`libLTO.so.18.1`, `LLVMgold.so`, `LLVMPolly.so`) and `libLLVM.so.1`
itself (the monolithic runtime .so, part of the separately-installed
`libllvm18` system package, not the `-dev` packages). Fixed by symlinking
those four names inside `/config/workspace/toolchain/usr/lib/llvm-18/lib/` to
the real system-installed `/usr/lib/llvm-18/lib/libLLVM.so.1` — we never
actually link against LTO/gold/Polly, so the stub just needs to exist to
satisfy CMake's imported-target file check. If this build tree is ever
recreated from scratch, redo those four symlinks or `find_package(LLVM
CONFIG)` will fail at configure time with a "file does not exist" error.

### ROSE Installation
```
Path:      /config/workspace/rose/install/
Version:   2.14.0
Library:   /config/workspace/rose/install/lib/librose.so.2.14.0
Headers:   /config/workspace/rose/install/include/rose/
```

**Critical constraint: `ROSE_BUILD_CXX_LANGUAGE_SUPPORT` is `#undef`** — the installed ROSE does not include the EDG C++ frontend. `frontend()` cannot parse `.cpp` files at runtime. This is the central architectural constraint.

**Confirmed via CMake configure output:** `AM_ROSE_BUILD_C_LANGUAGE_SUPPORT = false` as well — this is not just "no C++ frontend", it's no source-language frontend at all (`C/C++: OFF` in the `find_package(Rose)` summary). Only `AM_ROSE_BUILD_BINARY_ANALYSIS_SUPPORT = true`. Confirms Decision 1 (Clang frontend + SageBuilder reconstruction, never call `frontend()`) is not optional — it's the only path.

Available in this ROSE build (confirmed by header inspection):
- `CallGraph.h` — `CallGraphBuilder`, `CallTargetSet`
- `LocalRWSetGenerator.h` — per-function read/write sets with Globality/VarType/AccessType
- `ReadWriteSetRecords.h` — `ReadWriteSets::Globality`, `VarType`, `AccessType` enums
- `DefUseAnalysis.h` — def-use chains over filtered CFG
- `OmpAttribute.h` — full OpenMP 4.x construct enum including `e_target`, `e_target_data`, `e_map_to/from/tofrom`
- `omp_lowering.h` — `OmpSupport::lower_omp()`, `transOmpTargetParallel()`
- `DepInfo.h` / `DepInfoAnal.h` / `StmtDepAnal.h` — loop dependence analysis
- `sageBuilder.h` — AST construction
- `sageInterface.h` — AST manipulation (insertStatementBefore, etc.)
- `nlohmann/json.hpp` — JSON support (bundled)

ROSE build config flags (from `rose-config.cfg`):
```
ROSE_CXX        = g++
ROSE_CPPFLAGS   = -I/config/workspace/rose/install/include/rose -pthread -I/usr/include
ROSE_CXXFLAGS   = -D_GLIBCXX_USE_CXX11_ABI=1 -Ddisc_union=union
ROSE_LDFLAGS    = -L/config/workspace/rose/install/lib -lrose -pthread
                  -lboost_chrono -lboost_date_time -lboost_filesystem
                  -lboost_iostreams -lboost_program_options -lboost_random
                  -lboost_regex -lboost_system -lboost_wave -lboost_thread
                  -lboost_serialization
```

CMake integration: `find_package(Rose REQUIRED)` works via `/config/workspace/rose/install/lib/cmake/`. Target is `Rose::rose`.

### ROSE Source Tree
```
Path:   /config/workspace/rose/source/
```
Available for reference only — not rebuilt. Useful for reading tutorial examples at `rose/source/tutorial/` (especially `buildCallGraph.C`, `defuseAnalysis.C`, `staticCFG.C`, `interproceduralCFG.C`, `loopOptimization.C`).

---

## Key Architectural Decisions

### Decision 1: Clang as C++ Frontend (not ROSE EDG)

**Why:** ROSE 2.14.0 installed without EDG (`ROSE_BUILD_CXX_LANGUAGE_SUPPORT = false`). `frontend()` will fail on `.cpp` files at runtime.

**Approach:** Use `clang::RecursiveASTVisitor` (libclang / libtooling) to parse C/C++ files and extract a thin in-memory IR (`LoopNest`, `FunctionIR`, `ArrayAccess` structs). This IR is then fed into ROSE's `SageBuilder` to reconstruct an `SgProject` for analysis. ROSE is used only for its analysis APIs (CallGraph, DepInfo, mod/ref) and its unparser/codegen path.

**Implication:** The Clang IR extraction and ROSE AST reconstruction are separate, testable layers. `ClangBridge` has no ROSE dependency; `RoseBridge` has no Clang dependency.

### Decision 2: Full Call-Graph Transitive Mod/Ref Propagation

**Why:** Single-level inlining would miss multi-hop call chains. The core differentiator of this project is finding parallelism that intraprocedural analysis misses.

**Approach:** Build the full program call graph with `CallGraphBuilder`. Use `LocalRWSetGenerator` for per-function read/write sets. Propagate bottom-up using a worklist until fixpoint. SCCs treated conservatively (union of all member mod/ref sets).

**Implication:** Analysis cost is O(E × k) where E = call graph edges and k = iterations to fixpoint. For programs with large call graphs, this may be slow — acceptable for a research tool.

### Decision 3: Two-Pass Profile-Guided GPU Suitability

**Why:** Static trip-count estimation alone is too imprecise for the GPU decision. Roofline model requires actual measured trip counts and memory access volumes.

**Approach:**
- **Pass 1 (`--instrument`):** Insert `profile_loop_start/end` calls around each `PARALLEL_SAFE` loop. Emit instrumented binary. User runs it to produce `profile.json`.
- **Pass 2 (`--profile=...`):** Read profile, compute `flop_intensity = flops / bytes_transferred`, compare to `ridge_point = gpu_peak_flops / gpu_peak_bw`. Classify as `GPU_PROFITABLE` or `CPU_PREFERRED`.
- **Static fallback:** When no `--profile` is given, use statically estimated values.

**Implication:** The user workflow requires two invocations. This is documented in the CLI interface. The profiler runtime (`support/profiler.c`) is a small self-contained C file with no external dependencies.

### Decision 4: OpenMP 4.5+ Target Offload (not CUDA/HIP directly)

**Why:** OpenMP target offload is portable across GPU vendors and is the direction stated in the project goal. ROSE has `OmpAttribute` support for `e_target`, `e_map_*` constructs.

**Approach:** Use `SageBuilder::buildPragmaDeclaration` to insert the full pragma string rather than constructing `OmpAttribute` nodes directly. This is simpler and produces correct, compilable output. `lower_omp()` is available if we need to lower to runtime calls instead.

**Data mapping:** Inferred from the loop's array access classification:
- Arrays that are only read inside the loop → `map(to:...)`
- Arrays that are only written → `map(from:...)`
- Arrays that are both read and written → `map(tofrom:...)`

---

## What Needs to Happen Next (Task 1 in Detail)

The immediate next step is creating the build system. Here is exactly what needs to be done:

**File: `thecoolestcompiler/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.15)
project(thecoolestcompiler CXX C)

set(CMAKE_CXX_STANDARD 14)

# Find ROSE
set(Rose_DIR /config/workspace/rose/install/lib/cmake)
find_package(Rose REQUIRED)

# Find Clang/LLVM (for libclang / libtooling)
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

# Compiler flags from ROSE
add_compile_options(-D_GLIBCXX_USE_CXX11_ABI=1 -Ddisc_union=union)

# Main translator executable
add_executable(translator
    src/translator.cpp
    src/ClangBridge.cpp
    src/RoseBridge.cpp
    src/InterproceduralAnalysis.cpp
    src/ParallelismChecker.cpp
    src/ProfilingInstrumentor.cpp
    src/GpuSuitabilityEngine.cpp
    src/OmpCodegen.cpp
)

target_include_directories(translator PRIVATE
    src/
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_link_libraries(translator PRIVATE
    Rose::rose
    clangTooling
    clangFrontend
    clangAST
    clangBasic
    clangLex
    LLVM
)

# Profiler runtime (compiled as C, not C++)
add_library(profiler STATIC support/profiler.c)

# Tests
enable_testing()
add_subdirectory(tests)
```

**Verification:** `cmake -B build -S . && cmake --build build` should produce `./build/translator`.

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Clang libtooling API version incompatibility | Medium | Pin to system Clang version; use `clang_getCursorSpelling` (stable C API) for initial bridge, upgrade to C++ AST later |
| ROSE SageBuilder produces AST that fails consistency checks | Medium | Start with minimal AST (only loop + array accesses), add complexity incrementally; always run `AstTests::runAllTests()` after each addition |
| LocalRWSetGenerator requires fully resolved types | Medium | Test on simple C programs first before moving to templates/lambdas |
| Roofline model oversimplification | Low | The model is a known approximation; document its limits in the evaluation |
| `backend(project)` unparser produces uncompilable code | Low | The unparser is well-tested for programs built with SageBuilder; compare against tutorial examples |

---

## Reference Examples in ROSE Source

These files in `/config/workspace/rose/source/tutorial/` are useful reading before starting each task:

| File | Relevant to |
|---|---|
| `buildCallGraph.C` | Task 4 — CallGraph API usage |
| `defuseAnalysis.C` | Task 4/5 — DefUse API |
| `interproceduralCFG.C` | Task 4 — interprocedural CFG |
| `loopOptimization.C` | Task 5 — loop analysis |
| `loopRecognition.C` | Task 5 — loop nest structure |
| `addComments.C` | Task 8 — inserting text before statements |
| `identityTranslator.C` | Task 1/8 — minimal ROSE translator pattern |
| `instrumentationExample.C` | Task 6 — inserting function calls into AST |
| `staticCFG.C` | Task 5 — building CFGs |

---

## Conversation History Summary

Planning session on 2026-09-18 (~40 min). The following decisions were made interactively:

1. **Frontend:** Option b — Use Clang as C++ frontend shim (not EDG rebuild)
2. **Interprocedural depth:** Option b — Full call-graph transitive closure mod/ref propagation
3. **GPU suitability:** Option b — Two-pass profile-guided with static fallback
4. **PGO workflow:** Option a — Two separate translator invocations (instrument → run → transform)

The plan was reviewed and accepted before this CONTEXT.md was written.

---

## Files Changed in This Session

| File | Action |
|---|---|
| `/config/workspace/thecoolestcompiler/PRD.md` | Created — full product requirements |
| `/config/workspace/thecoolestcompiler/CONTEXT.md` | Created — this file |
| `/config/workspace/thecoolestcompiler/translator.cpp` | Unchanged — existing skeleton |
| `/config/workspace/thecoolestcompiler/test_loops.cpp` | Unchanged — existing test input |
