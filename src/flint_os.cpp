// flint_os.cpp — v0.20: portable OS services (see flint_os.h).
// This file intentionally avoids LLVM: `x86_64-w64-mingw32-clang++ -c` must
// stay clean so Windows support is machine-verified, not hand-waved.
#include "flint_os.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>        // _access
#include <direct.h>    // _mkdir
#include <sys/utime.h> // _utime
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dlfcn.h>
#endif

namespace flintos {

#ifdef _WIN32

namespace {
// Quote one argv element per MSVCRT CommandLineToArgvW inverse rules.
void appendQuoted(std::string& cmd, const std::string& arg) {
    bool needQuote = arg.empty();
    for (char c : arg) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'') { needQuote = true; break; }
    }
    if (!needQuote) { cmd += arg; return; }
    cmd += '"';
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { backslashes++; continue; }
        if (c == '"') {
            cmd.append(backslashes * 2 + 1, '\\');
            cmd += '"';
            backslashes = 0;
            continue;
        }
        if (backslashes) { cmd.append(backslashes, '\\'); backslashes = 0; }
        cmd += c;
    }
    if (backslashes) cmd.append(backslashes * 2, '\\');
    cmd += '"';
}
} // namespace

int spawn(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::string cmd;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) cmd += ' ';
        appendQuoted(cmd, args[i]);
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    // Console handles are shared by default: child stdio flows through.
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int spawnCapture(const std::vector<std::string>& args, std::string& out) {
    out.clear();
    if (args.empty()) return -1;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    // Parent must not inherit the read end into the child.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::string cmd;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) cmd += ' ';
        appendQuoted(cmd, args[i]);
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    int ret = -1;
    if (ok) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
            out.append(buf, (size_t)n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(pi.hProcess, &code);
        ret = (int)code;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return ret;
}

void loadPython() {
    // Best-effort: CPython windows DLL names across layouts/versions.
    static const char* names[] = {
        "python314.dll", "python313.dll", "python312.dll", "python311.dll",
        "python310.dll", "python3.dll", "libpython3.14.dll", "libpython3.13.dll",
        nullptr,
    };
    for (int i = 0; names[i]; i++) {
        if (LoadLibraryA(names[i])) return;
    }
}

void touchFile(const std::string& path) {
    _utime(path.c_str(), nullptr);
}

bool makeDir(const std::string& path) {
    if (_mkdir(path.c_str()) == 0) return true;
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool hasExe(const std::string& path) {
    return _access(path.c_str(), 0) == 0;
}

#else // POSIX

int spawn(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: replace image. execvp searches PATH for argv[0].
        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int spawnCapture(const std::vector<std::string>& args, std::string& out) {
    out.clear();
    if (args.empty()) return -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

void loadPython() {
    // Version-agnostic chain with RTLD_NOW (fail fast on missing symbols)
    // instead of LAZY (which would crash later at first call).
    if (!dlopen("libpython3.so", RTLD_NOW | RTLD_GLOBAL))
        if (!dlopen("libpython3.14.so", RTLD_NOW | RTLD_GLOBAL))
            if (!dlopen("libpython3.13.so", RTLD_NOW | RTLD_GLOBAL))
                dlopen("libpython3.12.so", RTLD_NOW | RTLD_GLOBAL);
}

void touchFile(const std::string& path) {
    // Best-effort LRU bump; failure is harmless.
    utimes(path.c_str(), nullptr);
}

bool makeDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) return true;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool hasExe(const std::string& path) {
    return access(path.c_str(), X_OK) == 0;
}

#endif // _WIN32

} // namespace flintos
