# Code Quality & Memory Safety Report

**Target:** `/data/data/com.termux/files/home/tools/cqma-fixture`  
**Files analyzed:** 1 (19 lines of C/C++)  
**Generated:** 2026-10-24 13:15:13  
**Analyzer:** cqma 1.0.0 (Rust)  
**Duration:** 2.0s

## Verdict: **50/100 (grade F)**

9 finding(s): 2 error, 3 style, 4 warning. Score = 100 − weighted/(lines/1000), weighted = 50, floor 0.

> ⚠️ **1 runtime memory-safety finding(s)** — these are real failures, not style.

## Summary

| Stage | Status | Findings | Time | Notes |
|---|---|---|---|---|
| compiler warnings | ✅ ran | 3 | 0.3s |  |
| cppcheck | ✅ ran | 5 | 0.1s |  |
| runtime memory analysis | ✅ ran | 1 | 0.1s | LeakSanitizer unavailable on this platform; leak coverage was not possible |

## 1. Compilation Warnings

3 diagnostic(s) across 1 file(s). Flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

### `fixture.c` — 3

| Line | Severity | Flag | Message |
|---|---|---|---|
| 3 | warning | -Wunused-function | unused function 'unused_helper' |
| 7 | warning | -Wshadow | declaration shadows a local variable |
| 8 | warning | -Wconstant-conversion | implicit conversion from 'int' to 'unsigned char' changes value from 300 to 44 |

## 2. Static Analysis (cppcheck)

5 defect(s). Null-pointer and unused-function checks are included.

### Error (1)

| Location | Check | Message |
|---|---|---|
| fixture.c:17 | nullPointer | Null pointer dereference: np |

### Warning (1)

| Location | Check | Message |
|---|---|---|
| fixture.c:14 | nullPointerOutOfMemory | If memory allocation fails, then there is a possible null pointer dereference: buf |

### Style (3)

| Location | Check | Message |
|---|---|---|
| fixture.c:3 | unusedFunction | The function 'unused_helper' is never used. |
| fixture.c:5 | unusedFunction | The function 'shadow_demo' is never used. |
| fixture.c:7 | shadowVariable | Local variable 'total' shadows outer variable |

## 3. Runtime Memory Analysis

1 runtime finding(s), 1 memory/UB error(s), 0 leak-related.

| Tool | Kind | Location | Message | Detail |
|---|---|---|---|---|
| asan | heap-buffer-overflow | fixture.c:14 | AddressSanitizer: heap-buffer-overflow | main at /data/data/com.termux/files/home/tools/cqma-fixture/fixture.c:14 |

## 4. Hotspots (fix these first)

| File | Weighted issues |
|---|---|
| fixture.c | 50 |

## 5. Toolchain & Environment

| Tool | Path | Version | Status |
|---|---|---|---|
| C compiler | /data/data/com.termux/files/usr/bin/clang | clang version 21.1.8 | ready |
| C++ compiler | /data/data/com.termux/files/usr/bin/clang++ | clang version 21.1.8 | ready |
| cppcheck | /data/data/com.termux/files/usr/bin/cppcheck | Cppcheck 2.22.0 | ready |
| valgrind | not found | — | stage skipped/fallback |

- Sanitizers: available (ASan works; LeakSanitizer unsupported on this platform)
- Leak detection: not available on this host

## 6. Reproduce

```sh
# compiler diagnostics
/data/data/com.termux/files/usr/bin/clang -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wconversion -std=c11 <file.c>

# static analysis
cppcheck --enable=all --inconclusive --force --std=c11 <files>

# runtime
/data/data/com.termux/files/usr/bin/clang -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined -g -fno-omit-frame-pointer -O1 <file.c> -o app && ./app
```

