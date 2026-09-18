// demo_speedup — a live "before vs after" speedup demo.
//
// Deliberately stays within the tool's *verified-sound* territory: direct
// array-dependence analysis on loops with no function calls and no scalar
// accumulation. (Two real gaps were found and ruled out for this file while
// building it — see CONTEXT.md "Known remaining gaps": (1) interprocedural
// purity proofs for calls are currently unsound to rely on for a demo, since
// a plain scalar global write inside a callee is invisible to the IR and can
// be misclassified safe; (2) scalar reduction patterns like `sum += B[i]`
// are equally invisible and can ALSO be misclassified safe, which would be a
// real data race under `#pragma omp parallel for`. Neither pattern appears
// below — only independent per-element array writes, which the direct
// array-dependence check (test_loops.cpp's recurrence case) has already been
// verified correct on.)
//
// No standard-library #includes (see benchmarks/interprocedural_safe/main.cpp
// for why — this sandboxed toolchain has no Clang resource-dir headers).
// gettimeofday() is declared with a hand-rolled struct matching glibc's
// `struct timeval` layout on 64-bit Linux (two longs) — for the same
// no-#includes reason, and so timing is measured *inside* the program
// (wall clock around just the loop of interest) instead of via shell `time`,
// which mixes in process-startup and page-fault/first-touch allocation cost
// that has nothing to do with the loop itself.
extern "C" int printf(const char* fmt, ...);
struct MyTimeval { long tv_sec; long tv_usec; };
extern "C" int gettimeofday(MyTimeval* tv, void* tz);

double elapsed_seconds(const MyTimeval& start, const MyTimeval& end) {
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_usec - start.tv_usec) * 1e-6;
}

int main() {
    const int N = 40000000;
    double* A = new double[N];
    double* B = new double[N];
    double* C = new double[N];

    // Warm-up / first-touch: force page allocation for all three arrays
    // *before* timing starts, so the timed region measures compute, not the
    // OS lazily backing fresh `new[]` pages on first write.
    for (int i = 0; i < N; ++i) {
        A[i] = 0.0;
        B[i] = 0.0;
        C[i] = 0.0;
    }

    // PARALLEL_SAFE: independent per-element init, no calls.
    for (int i = 0; i < N; ++i) {
        A[i] = (double)(i % 1000) * 0.001;
    }

    MyTimeval t0, t1;
    gettimeofday(&t0, 0);

    // PARALLEL_SAFE: the loop being timed. A degree-16 Horner polynomial —
    // enough ALU work per element (vs. the earlier degree-3 version, which
    // turned out to be memory-bandwidth-bound on this 2-core machine and
    // showed almost no speedup) that this is genuinely compute-bound, where
    // a second core actually helps instead of just contending for the same
    // memory bus.
    for (int i = 0; i < N; ++i) {
        double x = A[i];
        double y = x;
        y = y * x + 1.0;  y = y * x + 2.0;  y = y * x + 3.0;  y = y * x + 4.0;
        y = y * x + 5.0;  y = y * x + 6.0;  y = y * x + 7.0;  y = y * x + 8.0;
        y = y * x + 9.0;  y = y * x + 10.0; y = y * x + 11.0; y = y * x + 12.0;
        y = y * x + 13.0; y = y * x + 14.0; y = y * x + 15.0; y = y * x + 16.0;
        B[i] = y;
    }

    gettimeofday(&t1, 0);

    // SERIAL_ONLY: genuine loop-carried recurrence (same pattern as
    // test_loops.cpp), included so the demo also shows correct REFUSAL.
    C[0] = B[0];
    for (int i = 1; i < N; ++i) {
        C[i] = C[i - 1] * 0.9999 + B[i] * 0.0001;
    }

    // No scalar accumulation — print a few sampled values instead, so
    // sequential vs. parallel output can be compared byte-for-byte without
    // touching the (currently unsound) scalar-reduction blind spot.
    printf("compute_loop_seconds=%.4f\n", elapsed_seconds(t0, t1));
    printf("B[0]=%.6f B[N/2]=%.6f B[N-1]=%.6f C[0]=%.6f C[N/2]=%.6f C[N-1]=%.6f\n",
           B[0], B[N / 2], B[N - 1], C[0], C[N / 2], C[N - 1]);

    delete[] A;
    delete[] B;
    delete[] C;
    return 0;
}
