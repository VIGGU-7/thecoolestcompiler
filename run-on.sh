#!/usr/bin/env bash
# run-on.sh — point the compiler at ANY C/C++ file and show the full story.
#
# Built for the "someone hands you a file and says run it" case:
#   ./run-on.sh somebody_elses_program.cpp
#
# Does, in order: analyze -> show verdicts -> transform -> show the diff ->
# compile both versions -> run both -> prove the output is identical -> time them.
set -uo pipefail
cd "$(dirname "$0")"

if [ $# -lt 1 ]; then
    echo "usage: ./run-on.sh <file.c|file.cpp>"
    exit 1
fi

SRC="$1"
if [ ! -f "$SRC" ]; then
    echo "error: no such file: $SRC"
    exit 1
fi

TR=./build/translator
if [ ! -x "$TR" ]; then
    echo "error: translator not built. Run:  cmake -B build -S . && cmake --build build -j\$(nproc)"
    exit 1
fi

rm -f rose_reconstructed.cpp temp_dummy_file_rose_reconstructed.cpp
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; rm -f rose_reconstructed.cpp temp_dummy_file_rose_reconstructed.cpp' EXIT

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
quiet() { grep -v '^B1\|VxUtilFuncs\|In SageBuilder::buildSourceFile'; }

BASE=$(basename "$SRC")
OUT="$WORK/transformed_$BASE"

bold "=============================================================="
bold " Analyzing: $SRC"
bold "=============================================================="
echo

bold "--- What the compiler extracted ---"
$TR --dump-ir "$SRC" 2>&1 | quiet
echo

bold "--- Which loops are safe to parallelize, and why ---"
$TR --check-parallel "$SRC" 2>&1 | quiet
echo

bold "--- GPU vs CPU decision (roofline model) ---"
$TR --dump-gpu-decision "$SRC" 2>&1 | quiet
echo

bold "--- Transforming ---"
$TR -o "$OUT" "$SRC" 2>&1 | quiet
echo
echo "Directives inserted:"
grep -n '^[[:space:]]*#pragma omp' "$OUT" || echo "  (none — nothing was provably safe to parallelize)"
echo
echo "Diff vs. the original:"
diff "$SRC" "$OUT" | sed 's/^/  /' || true
echo

bold "--- Correctness: does the transformed program still behave identically? ---"
# Pick the right compiler/standard based on extension.
case "$SRC" in
    *.c) CC=gcc; STD="-std=c11" ;;
    *)   CC=g++; STD="-std=c++14" ;;
esac

if ! $CC $STD -O2 "$SRC" -o "$WORK/seq" 2>"$WORK/seq_err"; then
    echo "  Original didn't compile — skipping the run comparison:"
    sed 's/^/    /' "$WORK/seq_err" | head -5
    exit 0
fi
if ! $CC $STD -O2 -fopenmp "$OUT" -o "$WORK/par" 2>"$WORK/par_err"; then
    echo "  Transformed output didn't compile:"
    sed 's/^/    /' "$WORK/par_err" | head -10
    exit 1
fi
echo "  both compiled OK"

SEQ_OUT=$("$WORK/seq" 2>&1)
PAR_OUT=$(OMP_NUM_THREADS=$(nproc) OMP_PROC_BIND=true "$WORK/par" 2>&1)
echo "  original output:    $SEQ_OUT"
echo "  transformed output: $PAR_OUT"

# A program that times itself (…seconds=0.1604) will never match run to run,
# and that difference says nothing about whether the transformation was
# correct. Drop self-reported timing lines before comparing the actual results.
strip_timing() { grep -viE '(seconds|elapsed|time|msec|usec)[ ]*=' || true; }
SEQ_CMP=$(printf '%s\n' "$SEQ_OUT" | strip_timing)
PAR_CMP=$(printf '%s\n' "$PAR_OUT" | strip_timing)

if [ "$SEQ_CMP" = "$PAR_CMP" ]; then
    bold "  -> IDENTICAL — transformation preserved correctness"
else
    bold "  -> MISMATCH — results differ, do not trust this transformation"
    echo "  differing lines:"
    diff <(printf '%s\n' "$SEQ_CMP") <(printf '%s\n' "$PAR_CMP") | sed 's/^/    /'
    exit 1
fi
echo

bold "--- Timing (3 runs each, whole program) ---"
timeit() {  # $1 = binary, rest = env
    local best=""
    for _ in 1 2 3; do
        local s e
        s=$(date +%s.%N)
        env "${@:2}" "$1" > /dev/null 2>&1
        e=$(date +%s.%N)
        local d
        d=$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.4f", b-a}')
        best=$(awk -v x="$d" -v y="${best:-99999}" 'BEGIN{print (x<y)?x:y}')
    done
    echo "$best"
}
SEQ_T=$(timeit "$WORK/seq" IGNORE=1)
PAR_T=$(timeit "$WORK/par" OMP_NUM_THREADS=$(nproc) OMP_PROC_BIND=true)
echo "  original (best of 3):    ${SEQ_T}s"
echo "  transformed (best of 3): ${PAR_T}s   [$(nproc) threads]"

# Below ~50ms the measurement is all process startup and OpenMP thread-pool
# setup, and a ratio computed from that is noise, not a result. Say so rather
# than printing a meaningless number.
awk -v s="$SEQ_T" -v p="$PAR_T" -v n="$(nproc)" 'BEGIN{
    if (s < 0.05) {
        print "";
        print "  Too short to measure meaningfully — at this size the runtime is";
        print "  process startup and thread-pool setup, not the loops. The point";
        print "  here is the analysis and the correctness check above, not timing.";
        print "  For a real speedup number use a workload that runs long enough";
        print "  to dominate that overhead (see benchmarks/demo_speedup/).";
    } else if (p > 0) {
        printf "  speedup: %.2fx   (on %d cores)\n", s/p, n;
    }
}'
