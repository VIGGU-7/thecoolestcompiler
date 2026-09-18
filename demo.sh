#!/usr/bin/env bash
# demo.sh — hackathon demo script for thecoolestcompiler.
#
# Runs the whole pipeline against real inputs and prints the numbers that
# matter: how many loops were correctly accepted/rejected for parallelism
# and why, a real profile-guided GPU-suitability decision (roofline model),
# and a real, measured wall-clock speedup from the tool's own generated
# OpenMP code, with a correctness check (byte-identical output) alongside it.
#
# Usage: ./demo.sh   (run from the thecoolestcompiler/ directory)
set -euo pipefail
cd "$(dirname "$0")"

TR=./build/translator
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
strip_rose_noise() { grep -v '^B1\|VxUtilFuncs\|In SageBuilder::buildSourceFile'; }

bold "=============================================================="
bold " thecoolestcompiler — interprocedural parallelism + GPU-offload"
bold " decision demo"
bold "=============================================================="
echo

bold "--- 1) Interprocedural parallelism classification ---"
echo "Running --check-parallel on test_loops.cpp (a parallelizable"
echo "vector-add loop next to a genuine loop-carried recurrence):"
echo
$TR --check-parallel test_loops.cpp 2>&1 | strip_rose_noise
echo
echo "Running --check-parallel on benchmarks/interprocedural_unsafe/main.cpp"
echo "(a loop whose callee mutates a global — must be correctly REJECTED):"
echo
$TR --check-parallel benchmarks/interprocedural_unsafe/main.cpp 2>&1 | strip_rose_noise
echo

bold "--- 2) Profile-guided GPU suitability (two-pass roofline model) ---"
echo "Pass 1: --instrument inserts profiling calls around each PARALLEL_SAFE"
echo "loop; the instrumented binary is compiled, run, and writes profile.json"
echo "with REAL measured trip counts / FLOPs / bytes transferred."
echo
$TR --instrument -o "$WORK/instrumented.cpp" test_loops.cpp 2>&1 | strip_rose_noise
g++ -std=c++14 -I support "$WORK/instrumented.cpp" support/profiler.c -o "$WORK/instrumented_bin"
PROFILE_OUTPUT="$WORK/profile.json" "$WORK/instrumented_bin" > /dev/null
echo "profile.json (real measured data):"
cat "$WORK/profile.json"
echo
echo "Pass 2: --profile=profile.json applies the roofline model"
echo "(flop_intensity = flops/(bytes_read+bytes_written) vs. ridge_point ="
echo "gpu_peak_flops/gpu_peak_bw) to decide GPU_PROFITABLE vs CPU_PREFERRED:"
echo
$TR --profile="$WORK/profile.json" --dump-gpu-decision test_loops.cpp 2>&1 | strip_rose_noise
echo

bold "--- 3) Real, measured speedup from the tool's generated OpenMP code ---"
echo "Transforming benchmarks/demo_speedup/main.cpp (compute-bound loop with"
echo "no interprocedural calls or scalar reductions — see the file's header"
echo "comment for why those are deliberately avoided in this demo):"
echo
$TR -o "$WORK/demo_transformed.cpp" benchmarks/demo_speedup/main.cpp 2>&1 | strip_rose_noise
echo
echo "Inserted pragma(s):"
grep -n '^[[:space:]]*#pragma omp' "$WORK/demo_transformed.cpp" || true
echo
g++ -std=c++14 -O3 -march=native benchmarks/demo_speedup/main.cpp -o "$WORK/seq"
g++ -std=c++14 -O3 -march=native -fopenmp "$WORK/demo_transformed.cpp" -o "$WORK/par"

echo "Correctness check — sequential vs. parallel output must be identical:"
SEQ_OUT=$("$WORK/seq")
PAR_OUT=$(OMP_NUM_THREADS=2 OMP_PROC_BIND=true OMP_PLACES=cores "$WORK/par")
echo "  sequential: $SEQ_OUT"
echo "  parallel:   $PAR_OUT"
if [ "$(echo "$SEQ_OUT" | tail -1)" = "$(echo "$PAR_OUT" | tail -1)" ]; then
    echo "  -> IDENTICAL (correctness preserved)"
else
    echo "  -> MISMATCH — do not report a speedup number if this happens"
fi
echo

echo "Timing (5 runs each, compute loop only, via internal wall-clock timer):"
SEQ_TIMES=()
for i in 1 2 3 4 5; do
    T=$("$WORK/seq" | head -1 | sed 's/compute_loop_seconds=//')
    echo "  sequential run $i: ${T}s"
    SEQ_TIMES+=("$T")
done
PAR_TIMES=()
for i in 1 2 3 4 5; do
    T=$(OMP_NUM_THREADS=2 OMP_PROC_BIND=true OMP_PLACES=cores "$WORK/par" | head -1 | sed 's/compute_loop_seconds=//')
    echo "  parallel (2 threads) run $i: ${T}s"
    PAR_TIMES+=("$T")
done
SEQ_AVG=$(printf '%s\n' "${SEQ_TIMES[@]}" | awk '{s+=$1; n++} END {printf "%.4f", s/n}')
PAR_AVG=$(printf '%s\n' "${PAR_TIMES[@]}" | awk '{s+=$1; n++} END {printf "%.4f", s/n}')
SPEEDUP=$(awk -v s="$SEQ_AVG" -v p="$PAR_AVG" 'BEGIN {printf "%.2f", s/p}')
echo
bold "  sequential avg: ${SEQ_AVG}s"
bold "  parallel avg:   ${PAR_AVG}s"
bold "  speedup:        ${SPEEDUP}x  (on $(nproc) cores available in this environment)"
echo
echo "Note: this sandbox has only $(nproc) CPU cores and is a shared/virtualized"
echo "cloud VM, so speedup is capped well below what dedicated multi-core"
echo "hardware would show for the same tool output — re-run this script on"
echo "real hardware for a bigger number."
