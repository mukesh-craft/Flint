// flint_os.h — v0.20: portable OS services for flintc (no LLVM dependency,
// so this TU compiles for every target, including Windows via MinGW/MSVC).
// POSIX Bourne: fork+exec, dlopen, utimes. Windows: CreateProcess, LoadLibrary.
#pragma once

#include <string>
#include <vector>

namespace flintos {

// Run a program with an explicit argv array — NO shell is involved, so no
// argument can be interpreted as shell syntax. Returns the child's exit
// status (0 == success), or -1 on spawn failure.
int spawn(const std::vector<std::string>& args);

// Same, but captures stdout into `out` (trailing whitespace trimmed).
// Returns the exit status, or -1 on spawn failure.
int spawnCapture(const std::vector<std::string>& args, std::string& out);

// Best-effort preload of the Python shared library so JIT code can resolve
// Python symbols. Never fails the build (failures surface later, loudly).
void loadPython();

// Best-effort mtime bump (cache LRU bookkeeping). Failures are harmless.
void touchFile(const std::string& path);

// Create a directory (0755 on POSIX). True when it exists afterwards.
bool makeDir(const std::string& path);

// True when path names something executable (existence on Windows, which has
// no +x bit; access(X_OK) elsewhere).
bool hasExe(const std::string& path);

} // namespace flintos
