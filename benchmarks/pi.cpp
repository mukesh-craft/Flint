#include <iostream>
#include <chrono>

int main() {
    const int64_t n = 100000000;
    auto t0 = std::chrono::steady_clock::now();
    double pi = 0.0;
    for (int64_t k = 0; k < n; k++) {
        pi += (k % 2 == 0 ? 1.0 : -1.0) / (2.0 * k + 1.0);
    }
    pi *= 4.0;
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout.precision(10);
    std::cout << "pi = " << pi << std::endl;
    std::cout << "time: " << elapsed << " ns" << std::endl;
    return 0;
}
