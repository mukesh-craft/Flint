#include <iostream>
#include <chrono>

int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    int64_t result = fib(45);
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "fib(45) = " << result << std::endl;
    std::cout << "time: " << elapsed << " ns" << std::endl;
    return 0;
}
