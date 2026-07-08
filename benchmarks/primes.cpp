#include <iostream>
#include <vector>
#include <chrono>

int main() {
    const int64_t limit = 10000000;
    std::vector<char> sieve(limit + 1, 0);
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t i = 2; i * i <= limit; i++)
        if (!sieve[i])
            for (int64_t j = i * i; j <= limit; j += i)
                sieve[j] = 1;
    int64_t count = 0;
    for (int64_t i = 2; i <= limit; i++)
        if (!sieve[i]) count++;
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "primes up to 10M: " << count << std::endl;
    std::cout << "time: " << elapsed << " ns" << std::endl;
    return 0;
}
