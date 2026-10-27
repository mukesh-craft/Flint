# Code Quality & Memory Safety Report

**Target:** `/data/data/com.termux/files/home/tools/cqma-tests/small`  
**Files analyzed:** 1 (100 lines of C/C++)  
**Generated:** 2026-10-24 18:02:41  
**Analyzer:** cqma 1.0.0 (Rust)  
**Duration:** 9.7s

## Verdict: **0/100 (grade F)**

31 finding(s): 6 error, 10 style, 15 warning. Score = 100 − weighted/(lines/1000), weighted = 113, floor 0.

> Grade ceiling applied: **1 confirmed memory-safety defect(s)** cap the score at 69 (D) — a leak, overflow or use-after-free is not style debt, so code volume cannot dilute it.

> ⚠️ **1 runtime memory-safety finding(s)** — these are real failures, not style.

## Summary

| Stage | Status | Findings | Time | Notes |
|---|---|---|---|---|
| compiler warnings | ✅ ran | 6 | 2.2s |  |
| cppcheck | ✅ ran | 17 | 0.1s |  |
| clang static analyzer | ✅ ran | 7 | 2.1s |  |
| runtime memory analysis | ✅ ran | 1 | 0.2s | cqma leak tracker active: live=9 live_bytes=440 reachable=8 reachable_bytes=376 unreachable=1 unreachable_bytes=64 tracked=10 |

## 1. Compilation Warnings

6 diagnostic(s) across 1 file(s). Flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

### `bugs.cpp` — 6

| Line | Severity | Flag | Message |
|---|---|---|---|
| 13 | warning | -Wunused-function | unused function 'unused_helper' |
| 27 | warning | -Wfortify-source | 'strcpy' will always overflow; destination buffer has size 4, but the source string has length 7 (including NUL byte) |
| 49 | warning | -Wshadow | declaration shadows a local variable |
| 56 | warning | -Wshorten-64-to-32 | implicit conversion loses integer precision: 'size_type' (aka 'unsigned long') to 'int' |
| 63 | warning | -Wsometimes-uninitialized | variable 'v' is used uninitialized whenever 'if' condition is false |
| 87 | warning | -Wunused-but-set-variable | variable 'total' set but not used |

## 2. Static Analysis (cppcheck)

17 defect(s). Null-pointer and unused-function checks are included.

### Error (5)

| Location | Check | Message |
|---|---|---|
| bugs.cpp:27 | bufferAccessOutOfBounds | Buffer is accessed out of bounds: buf |
| bugs.cpp:34 | doubleFree | Memory pointed to by 'p' is freed twice. |
| bugs.cpp:42 | deallocret | Returning/dereferencing 'p' after it is deallocated / released |
| bugs.cpp:72 | memleak | Memory leak: p |
| bugs.cpp:78 | memleak | Memory leak: buf |

### Warning (2)

| Location | Check | Message |
|---|---|---|
| bugs.cpp:19 | nullPointer | Possible null pointer dereference: p |
| bugs.cpp:64 | uninitvar | Uninitialized variable: v |

### Style (10)

| Location | Check | Message |
|---|---|---|
| bugs.cpp:13 | unusedFunction | The function 'unused_helper' is never used. |
| bugs.cpp:32 | unusedAllocatedMemory | Variable 'p' is allocated memory that is never used. |
| bugs.cpp:47 | variableScope | The scope of the variable 'outer' can be reduced. |
| bugs.cpp:49 | shadowArgument | Local variable 'x' shadows outer argument |
| bugs.cpp:87 | unreadVariable | Variable 'total' is assigned a value that is never used. |
| bugs.cpp:88 | unreadVariable | Variable 'total' is assigned a value that is never used. |
| bugs.cpp:91 | unreadVariable | Variable 'total' is assigned a value that is never used. |
| bugs.cpp:94 | unreadVariable | Variable 'total' is assigned a value that is never used. |
| bugs.cpp:95 | unreadVariable | Variable 'total' is assigned a value that is never used. |
| bugs.cpp:96 | unreadVariable | Variable 'total' is assigned a value that is never used. |

## 3. Clang Static Analyzer

7 defect(s) from clang's path-sensitive engine (null derefs, leaks, dead stores, uninitialized reads).

| Location | Checker | Message |
|---|---|---|
| bugs.cpp:19 | core.NullDereference | Dereference of null pointer (loaded from variable 'p') |
| bugs.cpp:34 | cplusplus.NewDelete | Attempt to free released memory |
| bugs.cpp:42 | cplusplus.NewDelete | Use of memory after it is freed |
| bugs.cpp:64 | core.uninitialized.UndefReturn | Undefined or garbage value returned to caller |
| bugs.cpp:72 | cplusplus.NewDeleteLeaks | Potential leak of memory pointed to by 'p' |
| bugs.cpp:78 | cplusplus.NewDeleteLeaks | Potential leak of memory pointed to by 'buf' |
| bugs.cpp:96 | deadcode.DeadStores | Value stored to 'total' is never read |

## 4. Runtime Memory Analysis

1 runtime finding(s), 1 memory/UB error(s), 5 leak-related.

| Tool | Kind | Location | Message | Detail |
|---|---|---|---|---|
| leak-tracker | leak | bugs.cpp:69 | 64 byte(s) in 1 allocation(s) still allocated and unreachable at exit | leak_executed() |

## 5. Hotspots (fix these first)

| File | Weighted issues |
|---|---|
| bugs.cpp | 113 |

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

