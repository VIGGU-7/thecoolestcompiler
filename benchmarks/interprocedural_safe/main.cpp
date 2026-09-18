// interprocedural_safe — a loop parallelized only because whole-program
// interprocedural analysis proves the callee has no cross-iteration effect.
//
// scale_by_index() reads and writes only its own parameter (passed by value)
// and a single element of C indexed by the SAME index the caller passes in.
// It touches no globals and no other iteration's data. An intraprocedural-only
// analysis would have to treat the call conservatively (unknown callee) and
// reject the loop; whole-program mod/ref propagation proves it is safe.
// No standard-library #includes: this sandboxed environment only has the
// libclang-18-dev *headers* package, not the clang-18 package that bundles
// Clang's own resource-dir headers (stddef.h etc. that <cstdio> pulls in via
// the system C library) — so any TU that #includes standard headers fails to
// parse under our ClangBridge. test_loops.cpp is include-free for the same
// reason; matching that style here. extern "C" declares just enough of
// printf to produce a checksum without needing <cstdio>.
extern "C" int printf(const char* fmt, ...);

double C[1000];

// Pure w.r.t. cross-iteration state: only ever touches C[idx] for the idx it
// is given, never any other index, and touches no globals.
void scale_by_index(int idx, double value) {
    C[idx] = value * 2.0;
}

int main() {
    const int N = 1000;
    double A[N];
    double B[N];

    for (int i = 0; i < N; ++i) {
        A[i] = i;
        B[i] = N - i;
    }

    // Strictly parallelizable: scale_by_index(i, ...) only ever writes C[i],
    // never any other loop iteration's slot of C.
    for (int i = 0; i < N; ++i) {
        scale_by_index(i, A[i] + B[i]);
    }

    double checksum = 0.0;
    for (int i = 0; i < N; ++i) {
        checksum += C[i];
    }
    printf("checksum=%f\n", checksum);
    return 0;
}
