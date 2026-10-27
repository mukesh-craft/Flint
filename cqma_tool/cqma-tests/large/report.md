# Code Quality & Memory Safety Report

**Target:** `/data/data/com.termux/files/home/tools/cqma-tests/large`  
**Files analyzed:** 1 (598 lines of C/C++)  
**Generated:** 2026-10-24 18:03:10  
**Analyzer:** cqma 1.0.0 (Rust)  
**Duration:** 23.4s

## Verdict: **0/100 (grade F)**

106 finding(s): 5 error, 53 style, 48 warning. Score = 100 − weighted/(lines/1000), weighted = 245, floor 0.

> Grade ceiling applied: **1 confirmed memory-safety defect(s)** cap the score at 69 (D) — a leak, overflow or use-after-free is not style debt, so code volume cannot dilute it.

> ⚠️ **1 runtime memory-safety finding(s)** — these are real failures, not style.

## Summary

| Stage | Status | Findings | Time | Notes |
|---|---|---|---|---|
| compiler warnings | ✅ ran | 40 | 2.9s |  |
| cppcheck | ✅ ran | 61 | 0.9s |  |
| clang static analyzer | ✅ ran | 4 | 11.7s |  |
| runtime memory analysis | ✅ ran | 1 | 0.1s | cqma leak tracker active: live=9 live_bytes=504 reachable=8 reachable_bytes=376 unreachable=1 unreachable_bytes=128 tracked=34 |

## 1. Compilation Warnings

40 diagnostic(s) across 1 file(s). Flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

### `bugs_large.cpp` — 40

| Line | Severity | Flag | Message |
|---|---|---|---|
| 11 | warning | -Wunused-function | unused function 'clamp_int' |
| 17 | warning | -Wunused-function | unused function 'clamp_double' |
| 23 | warning | -Wunused-function | unused function 'trim' |
| 50 | warning | -Wunused-function | unused function 'parse_int' |
| 66 | warning | -Wunused-function | unused function 'hash_string' |
| 72 | warning | -Wunused-function | unused function 'percentile' |
| 95 | warning | -Wunused-function | unused function 'dot' |
| 115 | warning | -Wunused-function | unused function 'repeat' |
| 129 | warning | -Wunused-function | unused function 'is_palindrome' |
| 140 | warning | -Wunused-function | unused function 'count_char' |
| 153 | warning | -Wunused-function | unused function 'to_lower' |
| 172 | warning | -Wunused-function | unused function 'cheap_items' |
| 196 | warning | -Wunused-function | unused function 'format_money' |
| 221 | warning | -Wunused-function | unused function 'variance' |
| 252 | warning | -Wsign-conversion | implicit conversion changes signedness: 'int' to 'size_type' (aka 'unsigned long') |
| 253 | warning | -Wsign-conversion | implicit conversion changes signedness: 'int' to 'size_type' (aka 'unsigned long') |
| 264 | warning | -Wunused-function | unused function 'merge_sorted' |
| 276 | warning | -Wunused-function | unused function 'replace_all' |
| 300 | warning | -Wunused-function | unused function 'pad_left' |
| 305 | warning | -Wunused-function | unused function 'pad_right' |
| 310 | warning | -Wunused-function | unused function 'hex_encode' |
| 328 | warning | -Wunused-function | unused function 'hex_decode' |
| 340 | warning | -Wunused-function | unused function 'caesar' |
| 349 | warning | -Wunused-function | unused function 'longest_line' |
| 358 | warning | -Wunused-function | unused function 'wrap_text' |
| 370 | warning | -Wunused-function | unused function 'median' |
| 378 | warning | -Wunused-function | unused function 'standard_deviation' |
| 386 | warning | -Wunused-function | unused function 'correlation' |
| 402 | warning | -Wunused-function | unused function 'normalize' |
| 412 | warning | -Wunused-function | unused function 'softmax' |
| 423 | warning | -Wunused-function | unused function 'levenshtein' |
| 437 | warning | -Wunused-function | unused function 'longest_common_prefix' |
| 449 | warning | -Wunused-function | unused function 'starts_with' |
| 453 | warning | -Wunused-function | unused function 'ends_with' |
| 470 | warning | -Wunused-function | unused function 'roman_to_int' |
| 485 | warning | -Wunused-function | unused function 'unused_large_helper' |
| 499 | warning | -Wfortify-source | 'memcpy' will always overflow; destination buffer has size 8, but size argument is 16 |
| 521 | warning | -Wshadow | declaration shadows a local variable |
| 528 | warning | -Wshorten-64-to-32 | implicit conversion loses integer precision: 'size_type' (aka 'unsigned long') to 'int' |
| 535 | warning | -Wsometimes-uninitialized | variable 'v' is used uninitialized whenever 'if' condition is false |

## 2. Static Analysis (cppcheck)

61 defect(s). Null-pointer and unused-function checks are included.

### Error (4)

| Location | Check | Message |
|---|---|---|
| bugs_large.cpp:499 | bufferAccessOutOfBounds | Buffer is accessed out of bounds: buf |
| bugs_large.cpp:506 | doubleFree | Memory pointed to by 'p' is freed twice. |
| bugs_large.cpp:514 | deallocret | Returning/dereferencing 'p' after it is deallocated / released |
| bugs_large.cpp:550 | memleak | Memory leak: buf |

### Warning (4)

| Location | Check | Message |
|---|---|---|
| bugs_large.cpp:162 | uninitMemberVarNoCtor | Member variable 'Item::qty' has no initializer. |
| bugs_large.cpp:163 | uninitMemberVarNoCtor | Member variable 'Item::price' has no initializer. |
| bugs_large.cpp:491 | nullPointer | Possible null pointer dereference: p |
| bugs_large.cpp:536 | uninitvar | Uninitialized variable: v |

### Style (53)

| Location | Check | Message |
|---|---|---|
| bugs_large.cpp:11 | unusedFunction | The function 'clamp_int' is never used. |
| bugs_large.cpp:17 | unusedFunction | The function 'clamp_double' is never used. |
| bugs_large.cpp:23 | unusedFunction | The function 'trim' is never used. |
| bugs_large.cpp:50 | unusedFunction | The function 'parse_int' is never used. |
| bugs_large.cpp:66 | unusedFunction | The function 'hash_string' is never used. |
| bugs_large.cpp:72 | unusedFunction | The function 'percentile' is never used. |
| bugs_large.cpp:86 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:95 | unusedFunction | The function 'dot' is never used. |
| bugs_large.cpp:115 | unusedFunction | The function 'repeat' is never used. |
| bugs_large.cpp:129 | unusedFunction | The function 'is_palindrome' is never used. |
| bugs_large.cpp:140 | unusedFunction | The function 'count_char' is never used. |
| bugs_large.cpp:142 | useStlAlgorithm | Consider using std::count_if algorithm instead of a raw loop. |
| bugs_large.cpp:148 | useStlAlgorithm | Consider using std::transform algorithm instead of a raw loop. |
| bugs_large.cpp:153 | unusedFunction | The function 'to_lower' is never used. |
| bugs_large.cpp:155 | useStlAlgorithm | Consider using std::transform algorithm instead of a raw loop. |
| bugs_large.cpp:168 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:172 | unusedFunction | The function 'cheap_items' is never used. |
| bugs_large.cpp:174 | useStlAlgorithm | Consider using std::copy_if algorithm instead of a raw loop. |
| bugs_large.cpp:179 | useStlAlgorithm | Consider using std::find_if algorithm instead of a raw loop. |
| bugs_large.cpp:192 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:196 | unusedFunction | The function 'format_money' is never used. |
| bugs_large.cpp:210 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:217 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:221 | unusedFunction | The function 'variance' is never used. |
| bugs_large.cpp:224 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:264 | unusedFunction | The function 'merge_sorted' is never used. |
| bugs_large.cpp:276 | unusedFunction | The function 'replace_all' is never used. |
| bugs_large.cpp:300 | unusedFunction | The function 'pad_left' is never used. |
| bugs_large.cpp:305 | unusedFunction | The function 'pad_right' is never used. |
| bugs_large.cpp:310 | unusedFunction | The function 'hex_encode' is never used. |
| bugs_large.cpp:328 | unusedFunction | The function 'hex_decode' is never used. |
| bugs_large.cpp:340 | unusedFunction | The function 'caesar' is never used. |
| bugs_large.cpp:349 | unusedFunction | The function 'longest_line' is never used. |
| bugs_large.cpp:358 | unusedFunction | The function 'wrap_text' is never used. |
| bugs_large.cpp:370 | unusedFunction | The function 'median' is never used. |
| bugs_large.cpp:378 | unusedFunction | The function 'standard_deviation' is never used. |
| bugs_large.cpp:382 | useStlAlgorithm | Consider using std::accumulate algorithm instead of a raw loop. |
| bugs_large.cpp:386 | unusedFunction | The function 'correlation' is never used. |
| bugs_large.cpp:402 | unusedFunction | The function 'normalize' is never used. |
| bugs_large.cpp:407 | useStlAlgorithm | Consider using std::fill algorithm instead of a raw loop. |
| bugs_large.cpp:408 | useStlAlgorithm | Consider using std::transform algorithm instead of a raw loop. |
| bugs_large.cpp:412 | unusedFunction | The function 'softmax' is never used. |
| bugs_large.cpp:416 | useStlAlgorithm | Consider using std::max_element algorithm instead of a raw loop. |
| bugs_large.cpp:419 | useStlAlgorithm | Consider using std::transform algorithm instead of a raw loop. |
| bugs_large.cpp:423 | unusedFunction | The function 'levenshtein' is never used. |
| bugs_large.cpp:437 | unusedFunction | The function 'longest_common_prefix' is never used. |
| bugs_large.cpp:449 | unusedFunction | The function 'starts_with' is never used. |
| bugs_large.cpp:453 | unusedFunction | The function 'ends_with' is never used. |
| bugs_large.cpp:470 | unusedFunction | The function 'roman_to_int' is never used. |
| bugs_large.cpp:485 | unusedFunction | The function 'unused_large_helper' is never used. |
| bugs_large.cpp:504 | unusedAllocatedMemory | Variable 'p' is allocated memory that is never used. |
| bugs_large.cpp:519 | variableScope | The scope of the variable 'outer' can be reduced. |
| bugs_large.cpp:521 | shadowArgument | Local variable 'x' shadows outer argument |

## 3. Clang Static Analyzer

4 defect(s) from clang's path-sensitive engine (null derefs, leaks, dead stores, uninitialized reads).

| Location | Checker | Message |
|---|---|---|
| bugs_large.cpp:506 | cplusplus.NewDelete | Attempt to free released memory |
| bugs_large.cpp:514 | cplusplus.NewDelete | Use of memory after it is freed |
| bugs_large.cpp:536 | core.uninitialized.UndefReturn | Undefined or garbage value returned to caller |
| bugs_large.cpp:550 | cplusplus.NewDeleteLeaks | Potential leak of memory pointed to by 'buf' |

## 4. Runtime Memory Analysis

1 runtime finding(s), 1 memory/UB error(s), 3 leak-related.

| Tool | Kind | Location | Message | Detail |
|---|---|---|---|---|
| leak-tracker | leak | bugs_large.cpp:541 | 128 byte(s) in 1 allocation(s) still allocated and unreachable at exit | inventory::leak_large() |

## 5. Hotspots (fix these first)

| File | Weighted issues |
|---|---|
| bugs_large.cpp | 245 |

## 6. Toolchain & Environment

| Tool | Path | Version | Status |
|---|---|---|---|
| C compiler | /data/data/com.termux/files/usr/bin/clang | clang version 21.1.8 | ready |
| C++ compiler | /data/data/com.termux/files/usr/bin/clang++ | clang version 21.1.8 | ready |
| cppcheck | /data/data/com.termux/files/usr/bin/cppcheck | Cppcheck 2.22.0 | ready |
| valgrind | not found | — | stage skipped/fallback |

- Sanitizers: available (ASan works; LeakSanitizer unsupported on this platform)
- Leak detection: cqma allocator-hook tracker (ASan builds); LeakSanitizer unavailable

## 7. Reproduce

```sh
# compiler diagnostics
/data/data/com.termux/files/usr/bin/clang -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wconversion -std=c11 <file.c>

# static analysis
cppcheck --enable=all --inconclusive --force --std=c11 <files>

# runtime
/data/data/com.termux/files/usr/bin/clang -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined -g -fno-omit-frame-pointer -O1 <file.c> -o app && ./app
```

