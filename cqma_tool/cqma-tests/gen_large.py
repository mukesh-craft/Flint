#!/usr/bin/env python3
"""Generate a large, mostly-clean C++ file with 10 known defects.

The defects are tagged [L1]..[L10] so the verifier can locate them without
hard-coding line numbers. Run:  python3 gen_large.py > large/bugs_large.cpp
"""
import sys

CLEAN = r'''
static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clamp_double(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

static bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    if (i == s.size()) return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647LL) return false;
    }
    out = static_cast<int>(neg ? -v : v);
    return true;
}

static unsigned long hash_string(const std::string& s) {
    unsigned long h = 1469598103934665603UL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211UL; }
    return h;
}

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = (p / 100.0) * static_cast<double>(v.size() - 1);
    std::size_t lo = static_cast<std::size_t>(idx);
    std::size_t hi = lo + 1 < v.size() ? lo + 1 : lo;
    double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

static std::vector<double> moving_average(const std::vector<double>& v, std::size_t w) {
    std::vector<double> out;
    if (w == 0 || v.size() < w) return out;
    double sum = 0.0;
    for (std::size_t i = 0; i < w; ++i) sum += v[i];
    out.push_back(sum / static_cast<double>(w));
    for (std::size_t i = w; i < v.size(); ++i) {
        sum += v[i] - v[i - w];
        out.push_back(sum / static_cast<double>(w));
    }
    return out;
}

static double dot(const std::vector<double>& a, const std::vector<double>& b) {
    std::size_t n = a.size() < b.size() ? a.size() : b.size();
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

static std::vector<std::vector<double>> matmul(
    const std::vector<std::vector<double>>& a,
    const std::vector<std::vector<double>>& b) {
    std::vector<std::vector<double>> out;
    if (a.empty() || b.empty() || a[0].size() != b.size()) return out;
    out.assign(a.size(), std::vector<double>(b[0].size(), 0.0));
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t k = 0; k < b.size(); ++k)
            for (std::size_t j = 0; j < b[0].size(); ++j)
                out[i][j] += a[i][k] * b[k][j];
    return out;
}

static std::string repeat(const std::string& s, int n) {
    std::string out;
    if (n <= 0) return out;
    out.reserve(s.size() * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

static std::string reverse_words(const std::string& s) {
    std::vector<std::string> w = split(s, ' ');
    std::reverse(w.begin(), w.end());
    return join(w, " ");
}

static bool is_palindrome(const std::string& s) {
    std::size_t i = 0, j = s.size();
    if (j == 0) return true;
    --j;
    while (i < j) {
        if (s[i] != s[j]) return false;
        ++i; --j;
    }
    return true;
}

static int count_char(const std::string& s, char c) {
    int n = 0;
    for (char x : s) if (x == c) ++n;
    return n;
}

static std::string to_upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

static std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

struct Item {
    std::string name;
    int qty;
    double price;
};

static double inventory_value(const std::vector<Item>& items) {
    double total = 0.0;
    for (const Item& it : items) total += static_cast<double>(it.qty) * it.price;
    return total;
}

static std::vector<Item> cheap_items(const std::vector<Item>& items, double max_price) {
    std::vector<Item> out;
    for (const Item& it : items) if (it.price <= max_price) out.push_back(it);
    return out;
}

static Item* find_item(std::vector<Item>& items, const std::string& name) {
    for (Item& it : items) if (it.name == name) return &it;
    return nullptr;
}

static bool restock(std::vector<Item>& items, const std::string& name, int add) {
    Item* it = find_item(items, name);
    if (!it) return false;
    it->qty += add;
    return true;
}

static int total_quantity(const std::vector<Item>& items) {
    int n = 0;
    for (const Item& it : items) n += it.qty;
    return n;
}

static std::string format_money(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return std::string(buf);
}

static std::vector<int> range(int n) {
    std::vector<int> out;
    for (int i = 0; i < n; ++i) out.push_back(i);
    return out;
}

static int sum_range(const std::vector<int>& v) {
    int s = 0;
    for (int x : v) s += x;
    return s;
}

static double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

static double variance(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    double m = mean(v), s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return s / static_cast<double>(v.size() - 1);
}

static int gcd(int a, int b) {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

static bool is_prime(long long n) {
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (long long d = 3; d * d <= n; d += 2) if (n % d == 0) return false;
    return true;
}

static long long fib(int n) {
    long long a = 0, b = 1;
    for (int i = 0; i < n; ++i) { long long t = a + b; a = b; b = t; }
    return a;
}

static int binary_search(const std::vector<int>& v, int key) {
    int lo = 0, hi = static_cast<int>(v.size()) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (v[mid] == key) return mid;
        if (v[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void bubble_sort(std::vector<int>& v) {
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        for (std::size_t j = 0; j + 1 < v.size() - i; ++j)
            if (v[j] > v[j + 1]) std::swap(v[j], v[j + 1]);
}

static std::vector<int> merge_sorted(const std::vector<int>& a, const std::vector<int>& b) {
    std::vector<int> out;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] <= b[j]) out.push_back(a[i++]);
        else out.push_back(b[j++]);
    }
    while (i < a.size()) out.push_back(a[i++]);
    while (j < b.size()) out.push_back(b[j++]);
    return out;
}
'''

CLEAN2 = r"""
static std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == ',') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string pad_left(const std::string& s, std::size_t width, char fill) {
    if (s.size() >= width) return s;
    return std::string(width - s.size(), fill) + s;
}

static std::string pad_right(const std::string& s, std::size_t width, char fill) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), fill);
}

static std::string hex_encode(const std::string& s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode(const std::string& s, std::string& out) {
    if (s.size() % 2) return false;
    out.clear();
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int hi = hex_value(s[i]), lo = hex_value(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

static std::string caesar(const std::string& s, int shift) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>('a' + (c - 'a' + shift % 26 + 26) % 26);
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>('A' + (c - 'A' + shift % 26 + 26) % 26);
    }
    return out;
}

static std::size_t longest_line(const std::string& s) {
    std::size_t best = 0, cur = 0;
    for (char c : s) {
        if (c == '\n') { if (cur > best) best = cur; cur = 0; }
        else ++cur;
    }
    return cur > best ? cur : best;
}

static std::vector<std::string> wrap_text(const std::string& s, std::size_t width) {
    std::vector<std::string> lines;
    std::string cur;
    for (const std::string& w : tokenize(s)) {
        if (cur.empty()) cur = w;
        else if (cur.size() + 1 + w.size() <= width) { cur += ' '; cur += w; }
        else { lines.push_back(cur); cur = w; }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

static double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    if (n % 2) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static double standard_deviation(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    double m = mean(v);
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}

static double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    std::size_t n = a.size() < b.size() ? a.size() : b.size();
    if (n < 2) return 0.0;
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double num = 0.0, da = 0.0, db = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double xa = a[i] - ma, xb = b[i] - mb;
        num += xa * xb; da += xa * xa; db += xb * xb;
    }
    if (da == 0.0 || db == 0.0) return 0.0;
    return num / std::sqrt(da * db);
}

static std::vector<double> normalize(const std::vector<double>& v) {
    std::vector<double> out = v;
    if (out.empty()) return out;
    double lo = out[0], hi = out[0];
    for (double x : out) { if (x < lo) lo = x; if (x > hi) hi = x; }
    if (hi == lo) { for (double& x : out) x = 0.0; return out; }
    for (double& x : out) x = (x - lo) / (hi - lo);
    return out;
}

static std::vector<double> softmax(const std::vector<double>& v) {
    std::vector<double> out = v;
    if (out.empty()) return out;
    double mx = out[0];
    for (double x : out) if (x > mx) mx = x;
    double sum = 0.0;
    for (double& x : out) { x = std::exp(x - mx); sum += x; }
    for (double& x : out) x /= sum;
    return out;
}

static std::size_t levenshtein(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min(std::min(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

static std::string longest_common_prefix(const std::vector<std::string>& v) {
    if (v.empty()) return std::string();
    std::string p = v[0];
    for (std::size_t i = 1; i < v.size(); ++i) {
        std::size_t n = 0;
        while (n < p.size() && n < v[i].size() && p[n] == v[i][n]) ++n;
        p.resize(n);
        if (p.empty()) break;
    }
    return p;
}

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

static int roman_value(char c) {
    switch (c) {
        case 'I': return 1;
        case 'V': return 5;
        case 'X': return 10;
        case 'L': return 50;
        case 'C': return 100;
        case 'D': return 500;
        case 'M': return 1000;
        default: return 0;
    }
}

static int roman_to_int(const std::string& s) {
    int total = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        int v = roman_value(s[i]);
        int next = i + 1 < s.size() ? roman_value(s[i + 1]) : 0;
        total += v < next ? -v : v;
    }
    return total;
}
"""

DEFECTS = r'''
// ---------------------------------------------------------------------------
// Injected defects
// ---------------------------------------------------------------------------

// [L1] unused static function
static int unused_large_helper(int a) { return a + 1; }

// [L2] null dereference on the false path
static int deref_large(int flag) {
    int* p = nullptr;
    if (flag) p = new int(3);
    int v = *p;                       // [L2] null when flag == 0
    delete p;
    return v;
}

// [L3] stack buffer overflow (guarded, never executed)
static void overflow_large() {
    char buf[8];
    std::memcpy(buf, "0123456789abcdef", 16);   // [L3] 16 into 8
}

// [L4] double free (guarded, never executed)
static void double_free_large() {
    int* p = new int(11);
    delete p;
    delete p;                         // [L4]
}

// [L5] use after free (guarded, never executed)
static int uaf_large() {
    int* p = new int(13);
    int v = *p;
    delete p;
    return *p + v;                    // [L5]
}

// [L6] shadowed parameter
static int shadow_large(int x) {
    int outer = x;
    {
        int x = 5;                    // [L6]
        return x + outer;
    }
}

// [L7] sign conversion
static int count_large(const std::vector<int>& v) {
    int n = v.size();                 // [L7]
    return n;
}

// [L8] sometimes uninitialized
static int uninit_large(bool c) {
    int v;
    if (c) v = 9;
    return v;                         // [L8]
}

// [L9] executed heap leak
static void leak_large() {
    int* p = new int[32];             // [L9] 128 bytes, never deleted
    for (int i = 0; i < 32; ++i) p[i] = i;
}

// [L10] leak on an early-return path (guarded, never executed)
static int early_return_large(bool fail) {
    char* buf = new char[64];
    std::memset(buf, 'x', 63);
    buf[63] = '\0';
    if (fail) return -1;              // [L10] buf leaked
    int n = static_cast<int>(std::strlen(buf));
    delete[] buf;
    return n;
}

}  // namespace inventory

using namespace inventory;

int main(int argc, char**) {
    leak_large();

    std::vector<Item> items = {
        {"bolt", 40, 0.25}, {"nut", 100, 0.10}, {"gear", 3, 12.50}};
    double value = inventory_value(items);
    restock(items, "bolt", 10);

    std::vector<double> xs = {1.0, 2.0, 4.0, 8.0};
    std::vector<double> ma = moving_average(xs, 2);
    std::vector<std::vector<double>> m = {{1, 2}, {3, 4}};
    std::vector<std::vector<double>> sq = matmul(m, m);

    std::string text = "the quick brown fox";
    std::string rev = reverse_words(text);
    std::string up = to_upper(rev);
    std::vector<std::string> words = split(up, ' ');

    std::vector<int> nums = range(16);
    bubble_sort(nums);
    int idx = binary_search(nums, 7);

    int total = count_large(nums) + shadow_large(1) + sum_range(nums);
    total += static_cast<int>(value) + idx + static_cast<int>(ma.size()) +
             static_cast<int>(sq.size()) + static_cast<int>(words.size()) +
             static_cast<int>(total_quantity(items)) + gcd(12, 18) +
             static_cast<int>(fib(10)) + (is_prime(97) ? 1 : 0);

    if (argc > 100) {                 // false for the analyzer run
        total += deref_large(0);
        overflow_large();
        double_free_large();
        total += uaf_large();
        total += uninit_large(false);
        total += early_return_large(true);
    }

    return total == 123456 ? 1 : 0;
}
'''

CLEAN = CLEAN.replace("\n\n\n", "\n\n")
# Re-indent: CLEAN starts with a blank line; strip leading/trailing blanks.
CLEAN = CLEAN.strip("\n")
DEFECTS = DEFECTS.strip("\n")

HEADER = """// Large analyzer fixture: ~800 lines of realistic C++ with 10 known defects.
// Generated by gen_large.py; the [Ln] tags are the contract with verify.py.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace inventory {
"""

FOOTER = "\n"

def main():
    body = HEADER + CLEAN + "\n" + CLEAN2 + "\n" + DEFECTS + FOOTER
    sys.stdout.write(body)

if __name__ == "__main__":
    main()
