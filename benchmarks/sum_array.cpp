#include <iostream>
#include <vector>
#include <chrono>

int main() {
    const int64_t n = 10000000;
    std::vector<int64_t> data(n);

    for (int64_t i = 0; i < n; i++)
        data[i] = i * 2;

    auto t0 = std::chrono::steady_clock::now();
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++)
        sum += data[i];
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    std::cout << "sum: " << sum << std::endl;
    std::cout << "time: " << elapsed << " ns" << std::endl;

    return 0;
}
