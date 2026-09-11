int global_counter = 0;
void side_effect_function() { global_counter++; }

int main() {
    const int N = 50000000;
    
    int* A = new int[N];
    int* B = new int[N];
    int* C = new int[N];

    for (int i = 0; i < N; ++i) {
        A[i] = i;
        B[i] = N - i;
    }

    // Strictly Parallelizable
    for (int i = 0; i < N; ++i) {
        C[i] = A[i] + B[i];
    }

    // Non-Parallelizable 
    for (int i = 1; i < N; ++i) {
        A[i] = A[i - 1] + B[i];
    }

    delete[] A;
    delete[] B;
    delete[] C;
    return 0;
}
