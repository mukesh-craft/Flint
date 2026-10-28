# Code Quality & Memory Safety Report

**Target:** `/data/data/com.termux/files/home/tools/cqma-fixture`  
**Files analyzed:** 1 (19 lines of C/C++)  
**Generated:** 2026-10-24 13:07:21  
**Analyzer:** cqma 1.0.0  
**Duration:** 2.2s

## Verdict: **75/100 (grade C)**

1 finding(s): 1 error. Score = 100 − weighted/(lines/1000), weighted = 25, floor 0.

> ⚠️ **1 runtime memory-safety finding(s)** — these are real failures, not style.

## Summary

| Stage | Status | Findings | Time | Notes |
|---|---|---|---|---|
| runtime memory analysis | ✅ ran | 1 | 0.3s | LeakSanitizer unavailable on this platform; leak coverage was not possible |

## 1. Compilation Warnings

No compiler warnings. ✅

## 2. Static Analysis (cppcheck)

No static-analysis defects. ✅

## 3. Runtime Memory Analysis

1 runtime finding(s), 1 memory/UB error(s), 0 leak-related.

| Tool | Kind | Location | Message | Detail |
|---|---|---|---|---|
| asan | heap-buffer-overflow | /data/data/com.termux/files/usr/tmp/opencode/ov.c:2 | AddressSanitizer: heap-buffer-overflow | main at /data/data/com.termux/files/usr/tmp/opencode/ov.c:2 |

## 4. Hotspots (fix these first)

| File | Weighted issues |
|---|---|
| /data/data/com.termux/files/usr/tmp/opencode/ov.c | 25 |

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

