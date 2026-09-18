#pragma once
/* profiler.h — thecoolestcompiler profiling runtime (REQ-PR3)
 *
 * Plain C, dependency-free (no C++/ROSE/Clang). Compiled standalone as a
 * static library (see top-level CMakeLists.txt: `profiler` target built
 * from support/profiler.c) and linked into instrumented output produced by
 * Pass 1 (ProfilingInstrumentor, --instrument).
 *
 * Usage (emitted by ProfilingInstrumentor around each PARALLEL_SAFE loop):
 *
 *   profile_loop_start("file.cpp:14");
 *   for (...) { ... }
 *   profile_loop_end("file.cpp:14", trip_count, flops, bytes_read, bytes_written);
 *
 * On normal program exit (via atexit()), all recorded records are written
 * as a JSON array to the file named by the PROFILE_OUTPUT environment
 * variable, or "profile.json" in the current working directory if that
 * variable is unset. Schema:
 *
 *   [
 *     {
 *       "loop_id": "file.cpp:14",
 *       "trip_count": 50000000,
 *       "flops": 150000000,
 *       "bytes_read": 400000000,
 *       "bytes_written": 200000000
 *     }
 *   ]
 *
 * This header/implementation intentionally does not modify the
 * instrumented program's observable output (REQ-PR1) — all bookkeeping is
 * written to the separate profile JSON file only.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Called immediately before a PARALLEL_SAFE loop. loop_id should be a
 * stable string such as "file.cpp:14" (matches LoopNest::location).
 * Currently only ensures the exit handler is registered; timing is not
 * required by REQ-PR3 and is not recorded. */
void profile_loop_start(const char* loop_id);

/* Called immediately after a PARALLEL_SAFE loop finishes. Appends one
 * record to the in-memory log; the full log is flushed to profile.json
 * (or $PROFILE_OUTPUT) once, at program exit. */
void profile_loop_end(const char* loop_id,
                       long trip_count,
                       long flops,
                       long bytes_read,
                       long bytes_written);

#ifdef __cplusplus
}
#endif
