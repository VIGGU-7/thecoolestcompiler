# tests/CMakeLists.txt
# ---------------------------------------------------------------------------
# CTest entries for thecoolestcompiler
#
# Tests are defined incrementally:
#   T0 — smoke: verify the translator binary exists and exits with --help
#   T1 — dump-ir: run --dump-ir on test_loops.cpp (Task 2+)
#   T2 — check-parallel: run --check-parallel (Task 5+)
#   T3 — instrument pass: run --instrument (Task 6+)
#   T4 — full pipeline: instrument → profile → transform (Task 7+)
#
# Guards around each test block ensure cmake doesn't error when the
# corresponding source files haven't been created yet.
# ---------------------------------------------------------------------------

# Locate the translator binary produced by the top-level CMakeLists.
# During the build tree it lives at <binary_dir>/translator.
# We use the generator expression $<TARGET_FILE:translator> where possible,
# but also keep a plain path fallback for CTest invocations that run after
# cmake --install.
set(_translator "$<TARGET_FILE:translator>")

# ---------------------------------------------------------------------------
# T0: Binary existence / --help smoke test
# ---------------------------------------------------------------------------
# This test is always registered; it will fail gracefully if the translator
# hasn't been built yet (CTest will report NOT_RUN or FAIL with a clear
# message rather than a cmake error).

add_test(
  NAME T0_translator_help
  COMMAND ${_translator} --help
)
set_tests_properties(T0_translator_help PROPERTIES
  LABELS          "smoke"
  TIMEOUT         30
  PASS_REGULAR_EXPRESSION "usage|Usage|USAGE|Options|options|translator"
  # If the binary doesn't exist at test time we get a clear failure message.
  SKIP_RETURN_CODE 77
)

# ---------------------------------------------------------------------------
# T1: --dump-ir on test_loops.cpp  (requires Task 2 ClangBridge)
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_loops.cpp")
  set(_test_input "${CMAKE_CURRENT_SOURCE_DIR}/test_loops.cpp")
else()
  # Fall back to the project-root test_loops.cpp that already exists
  set(_test_input "${CMAKE_CURRENT_SOURCE_DIR}/../test_loops.cpp")
endif()

if(EXISTS "${_test_input}")
  add_test(
    NAME T1_dump_ir
    COMMAND ${_translator} --dump-ir ${_test_input}
  )
  set_tests_properties(T1_dump_ir PROPERTIES
    LABELS   "clang-bridge"
    TIMEOUT  60
    DEPENDS  T0_translator_help
  )
endif()

# ---------------------------------------------------------------------------
# T2: --check-parallel on test_loops.cpp  (requires Task 5)
# ---------------------------------------------------------------------------
if(EXISTS "${_test_input}")
  add_test(
    NAME T2_check_parallel
    COMMAND ${_translator} --check-parallel ${_test_input}
  )
  set_tests_properties(T2_check_parallel PROPERTIES
    LABELS   "parallelism-checker"
    TIMEOUT  120
    DEPENDS  T1_dump_ir
  )
endif()

# ---------------------------------------------------------------------------
# T3: Instrumentation pass on test_loops.cpp  (requires Tasks 5-6)
# ---------------------------------------------------------------------------
if(EXISTS "${_test_input}")
  # Output goes to the binary dir to keep sources clean
  set(_instrumented_out "${CMAKE_CURRENT_BINARY_DIR}/test_loops_instrumented.cpp")

  add_test(
    NAME T3_instrument
    COMMAND ${_translator} --instrument -o ${_instrumented_out} ${_test_input}
  )
  set_tests_properties(T3_instrument PROPERTIES
    LABELS   "instrumentation"
    TIMEOUT  120
    DEPENDS  T2_check_parallel
  )

  # If a profiler.c exists, also verify the instrumented output compiles.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../support/profiler.c")
    add_test(
      NAME T3_compile_instrumented
      COMMAND ${CMAKE_CXX_COMPILER}
                -std=c++14
                -fopenmp
                -I${CMAKE_CURRENT_SOURCE_DIR}/../support
                ${_instrumented_out}
                ${CMAKE_CURRENT_SOURCE_DIR}/../support/profiler.c
                -o ${CMAKE_CURRENT_BINARY_DIR}/test_loops_instrumented
    )
    set_tests_properties(T3_compile_instrumented PROPERTIES
      LABELS   "instrumentation"
      TIMEOUT  60
      DEPENDS  T3_instrument
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# T4: Full pipeline — instrument → run → transform  (requires Tasks 5-8)
# ---------------------------------------------------------------------------
# This heavyweight test is guarded behind the existence of the profiler
# source so it isn't registered until the full pipeline is in place.
if(EXISTS "${_test_input}" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../support/profiler.c")

  set(_profile_json    "${CMAKE_CURRENT_BINARY_DIR}/profile.json")
  set(_final_out       "${CMAKE_CURRENT_BINARY_DIR}/test_loops_transformed.cpp")
  set(_instrumented_bin "${CMAKE_CURRENT_BINARY_DIR}/test_loops_instrumented")

  # Step A: run the instrumented binary to produce profile.json
  add_test(
    NAME T4a_run_instrumented
    COMMAND ${_instrumented_bin}
  )
  set_tests_properties(T4a_run_instrumented PROPERTIES
    LABELS   "full-pipeline"
    TIMEOUT  120
    DEPENDS  T3_compile_instrumented
    ENVIRONMENT "PROFILE_OUTPUT=${_profile_json}"
  )

  # Step B: GPU-suitability transform with profile
  add_test(
    NAME T4b_transform_with_profile
    COMMAND ${_translator}
              --profile=${_profile_json}
              -o ${_final_out}
              ${_test_input}
  )
  set_tests_properties(T4b_transform_with_profile PROPERTIES
    LABELS   "full-pipeline"
    TIMEOUT  120
    DEPENDS  T4a_run_instrumented
  )

  # Step C: compile the transformed output
  add_test(
    NAME T4c_compile_transformed
    COMMAND ${CMAKE_CXX_COMPILER}
              -std=c++14
              -fopenmp
              ${_final_out}
              -o ${CMAKE_CURRENT_BINARY_DIR}/test_loops_transformed
  )
  set_tests_properties(T4c_compile_transformed PROPERTIES
    LABELS   "full-pipeline"
    TIMEOUT  60
    DEPENDS  T4b_transform_with_profile
  )
endif()

# ---------------------------------------------------------------------------
# Benchmark tests (registered here; actual benchmark sources added in Task 9)
# ---------------------------------------------------------------------------

# interprocedural_safe — a loop parallelized via interprocedural analysis
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/interprocedural_safe/main.cpp")
  set(_bench_src "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/interprocedural_safe/main.cpp")
  set(_bench_ref "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/interprocedural_safe/reference_output.txt")
  set(_bench_out "${CMAKE_CURRENT_BINARY_DIR}/interprocedural_safe_out.cpp")

  add_test(
    NAME Bench1_interprocedural_safe_transform
    COMMAND ${_translator} --check-parallel -o ${_bench_out} ${_bench_src}
  )
  set_tests_properties(Bench1_interprocedural_safe_transform PROPERTIES
    LABELS   "benchmark;interprocedural"
    TIMEOUT  300
  )
endif()

# interprocedural_unsafe — a loop that must NOT be parallelized
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/interprocedural_unsafe/main.cpp")
  set(_bench_src "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/interprocedural_unsafe/main.cpp")
  set(_bench_out "${CMAKE_CURRENT_BINARY_DIR}/interprocedural_unsafe_out.cpp")

  add_test(
    NAME Bench2_interprocedural_unsafe_transform
    COMMAND ${_translator} --check-parallel -o ${_bench_out} ${_bench_src}
  )
  set_tests_properties(Bench2_interprocedural_unsafe_transform PROPERTIES
    LABELS   "benchmark;interprocedural"
    TIMEOUT  300
  )
endif()

# polybench_gemm — GPU offload benchmark
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/polybench_gemm/gemm.cpp")
  set(_bench_src "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/polybench_gemm/gemm.cpp")
  set(_bench_out "${CMAKE_CURRENT_BINARY_DIR}/polybench_gemm_out.cpp")

  add_test(
    NAME Bench3_polybench_gemm_transform
    COMMAND ${_translator} --dump-gpu-decision -o ${_bench_out} ${_bench_src}
  )
  set_tests_properties(Bench3_polybench_gemm_transform PROPERTIES
    LABELS   "benchmark;gpu"
    TIMEOUT  600
  )
endif()
