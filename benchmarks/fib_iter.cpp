#include <iostream>
#include <chrono>
#include <cstdint>

// Iterative fib(45) mirror of benchmarks/fib_iter.fl. The recursive fib.cpp
// is kept as the original workload.
int64_t fib_iter(int64_t n) {
    if (n == 0) return 0;
    int64_t a = 0, b = 1;
    for (int64_t i = 2; i <= n; i++) {
        int64_t next = a + b;
        a = b;
        b = next;
    }
    return b;
}

int main() {
    volatile int64_t n = 45;   // keep the loop from being constant-folded
    auto t0 = std::chrono::steady_clock::now();
    int64_t result = fib_iter(n);
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "fib_iter(45) = " << result << std::endl;
    std::cout << "time: " << elapsed << " ns" << std::endl;
    return 0;
}
