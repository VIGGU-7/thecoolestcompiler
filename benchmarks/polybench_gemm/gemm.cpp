// polybench_gemm — standard dense matrix-multiply accumulate (C = A*B + C),
// the canonical GPU-offload benchmark: high trip count (NI*NJ*NK iterations),
// high arithmetic intensity (2 FLOPs per inner-loop element pair vs. a few
// loads/stores), so it should be classified GPU_PROFITABLE under the
// roofline model rather than left on the CPU.
// No standard-library #includes — see interprocedural_safe/main.cpp for why
// (this sandboxed toolchain has no Clang resource-dir headers available).
extern "C" int printf(const char* fmt, ...);

#define NI 256
#define NJ 256
#define NK 256

double A[NI][NK];
double B[NK][NJ];
double C[NI][NJ];

int main() {
    for (int i = 0; i < NI; ++i) {
        for (int k = 0; k < NK; ++k) {
            A[i][k] = ((double)i * k) / NI;
        }
    }
    for (int k = 0; k < NK; ++k) {
        for (int j = 0; j < NJ; ++j) {
            B[k][j] = ((double)k * j) / NJ;
        }
    }
    for (int i = 0; i < NI; ++i) {
        for (int j = 0; j < NJ; ++j) {
            C[i][j] = 0.0;
        }
    }

    // GPU-profitable: large trip count, high FLOP/byte ratio.
    for (int i = 0; i < NI; ++i) {
        for (int j = 0; j < NJ; ++j) {
            for (int k = 0; k < NK; ++k) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }

    double checksum = 0.0;
    for (int i = 0; i < NI; ++i) {
        for (int j = 0; j < NJ; ++j) {
            checksum += C[i][j];
        }
    }
    printf("checksum=%f\n", checksum);
    return 0;
}
