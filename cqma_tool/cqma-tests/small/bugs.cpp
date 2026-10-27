// Deliberately defective C++ used to validate cqma's detectors.
//
// Every defect is marked [Dn]. The executable path is clean except for the
// heap leak, so AddressSanitizer reaches exit and the leak tracker can report
// it; the crashing defects sit behind `argc > 100`, which is false for the
// analyzer's run but still reachable for path-sensitive analysis.

#include <cstring>
#include <string>
#include <vector>

// [D1] unused static function -> cppcheck unusedFunction / -Wunused-function
static int unused_helper(int a) { return a * 2; }

// [D2] null dereference on the false path -> clang-analyzer
static int null_deref_guarded(int flag) {
    int* p = nullptr;
    if (flag) p = new int(3);
    int v = *p;                      // [D2] null dereference when flag == 0
    delete p;
    return v;
}

// [D3] stack buffer overflow in a guarded, never-executed call
static void overflow_guarded() {
    char buf[4];
    std::strcpy(buf, "abcdef");      // [D3] 7 bytes into a 4-byte buffer
}

// [D4] double free in a guarded, never-executed call
static void double_free_guarded() {
    int* p = new int(5);
    delete p;
    delete p;                        // [D4] second delete
}

// [D5] use-after-free in a guarded, never-executed call
static int uaf_guarded() {
    int* p = new int(7);
    int v = *p;
    delete p;
    return *p + v;                   // [D5] read after delete
}

// [D6] shadowed local -> -Wshadow
static int shadowed(int x) {
    int outer = x;
    {
        int x = 3;                   // [D6] shadows the parameter
        return x + outer;
    }
}

// [D7] sign conversion -> -Wsign-conversion
static int count_of(const std::vector<int>& v) {
    int n = v.size();                // [D7] size_t -> int
    return n;
}

// [D8] sometimes-uninitialized -> -Wsometimes-uninitialized
static int uninit_guarded(bool c) {
    int v;
    if (c) v = 1;
    return v;                        // [D8] uninitialized when !c
}

// [D9] executed heap leak -> cqma leak tracker
static void leak_executed() {
    int* p = new int[16];            // [D9] 64 bytes, never deleted
    p[0] = 1;
    p[15] = 2;
}

// [D10] memory leak on an early-return path (static analysis)
static int early_return_leak(bool fail) {
    char* buf = new char[32];
    std::strcpy(buf, "payload");
    if (fail) return -1;             // [D10] buf leaked
    int n = static_cast<int>(std::strlen(buf));
    delete[] buf;
    return n;
}

int main(int argc, char**) {
    leak_executed();

    int total = count_of({1, 2, 3});
    total += shadowed(1);

    if (argc > 100) {                // false in the analyzer run
        total += null_deref_guarded(0);
        overflow_guarded();
        double_free_guarded();
        total += uaf_guarded();
        total += uninit_guarded(false);
        total += early_return_leak(true);
    }

    return 0;
}
