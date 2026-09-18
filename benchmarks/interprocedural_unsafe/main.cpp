// interprocedural_unsafe — a loop that must be correctly REJECTED because the
// callee has a cross-iteration side effect (a running total kept in a global).
// A naive "offload everything with an unknown call" policy would wrongly
// parallelize this; whole-program mod/ref propagation must see that
// accumulate() modifies the global `running_total` on every call and mark the
// loop SERIAL_ONLY.
// No standard-library #includes — see interprocedural_safe/main.cpp for why
// (this sandboxed toolchain has no Clang resource-dir headers available).
extern "C" int printf(const char* fmt, ...);

double running_total = 0.0;

// NOT pure: reads and writes a global that persists across calls, so calls
// for different iterations are NOT independent of each other.
void accumulate(double value) {
    running_total += value;
}

int main() {
    const int N = 1000;
    double A[N];
    for (int i = 0; i < N; ++i) {
        A[i] = i;
    }

    // Must be classified SERIAL_ONLY: accumulate() has a loop-carried
    // dependence through running_total (order-dependent read-modify-write).
    for (int i = 0; i < N; ++i) {
        accumulate(A[i]);
    }

    printf("running_total=%f\n", running_total);
    return 0;
}
