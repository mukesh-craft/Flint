#include <iostream>
#include <string>
#include <algorithm>
#include <chrono>

int main() {
    const int64_t n = 10000000;
    std::string s(n, 'a');
    for (int64_t i = 0; i < n; i++) s[i] = 'a' + (i % 26);
    auto t0 = std::chrono::steady_clock::now();
    std::string rev(n, 'a');
    for (int64_t i = 0; i < n; i++)
        rev[i] = s[n - 1 - i];
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "last char: " << rev[n - 1] << std::endl;
    std::cout << "time: " << elapsed << " ns" << std::endl;
    return 0;
}
