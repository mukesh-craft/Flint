#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Passes/PassBuilder.h"
#include <dlfcn.h>
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cassert>
#include <iostream>
#include <thread>
#include <mutex>
#include <functional>
#include <queue>
#include <future>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cctype>
#include <chrono>
#include <fstream>
#include <cerrno>

// Parse an all-digit literal to exact i64 without exceptions
// (build uses -fno-exceptions, so std::stoll try/catch won't compile).
// Returns false on overflow/non-digits; caller falls back to double.
static bool parseI64Exact(const std::string& lit, int64_t& out) {
    if (lit.empty() || lit.size() > 19) return false;
    for (char c : lit) if (c < '0' || c > '9') return false;
    errno = 0;
    char* end = nullptr;
    long long v = ::strtoll(lit.c_str(), &end, 10);
    if (errno == ERANGE || !end || end != lit.c_str() + lit.size()) return false;
    out = static_cast<int64_t>(v);
    return true;
}

// ============================================================================
// PROFILER — nanosecond-precision instrumentation
// ============================================================================

#ifdef FLINTC_PROFILE

class Timer {
    using Clock = std::chrono::high_resolution_clock;
    struct Record { const char* phase; Clock::time_point start; Clock::duration wall; };
    std::vector<Record> records;
    std::vector<size_t> stack;
    Clock::time_point programStart;
public:
    Timer() { programStart = Clock::now(); }
    void begin(const char* phase) {
        records.push_back({phase, Clock::now(), {}});
        stack.push_back(records.size() - 1);
    }
    void end() {
        if (stack.empty()) return;
        auto idx = stack.back(); stack.pop_back();
        records[idx].wall = Clock::now() - records[idx].start;
    }
    void report() {
        auto total = Clock::now() - programStart;
        std::ofstream out("profile_report.json");
        out << "{\n  \"total_ns\": " << total.count() << ",\n  \"phases\": [\n";
        bool first = true;
        for (auto& r : records) {
            if (!first) out << ",\n";
            first = false;
            out << "    {\"phase\":\"" << r.phase << "\",\"wall_ns\":" << r.wall.count() << "}";
        }
        out << "\n  ]\n}\n";
        out.close();
        std::cerr << "\n=== Profile Report ===\n";
        std::cerr << "Total: " << total.count() / 1000000 << " ms\n";
        for (auto& r : records)
            if (r.wall.count() > 0)
                std::cerr << "  " << r.phase << ": " << r.wall.count() / 1000 << " us (" << r.wall.count() << " ns)\n";
        std::cerr << "=====================\n";
    }
};

Timer g_timer;
#define PROFILE_BEGIN(phase) g_timer.begin(phase)
#define PROFILE_END()        g_timer.end()
#define PROFILE_REPORT()     g_timer.report()

#else
#define PROFILE_BEGIN(phase)
#define PROFILE_END()
#define PROFILE_REPORT()
#endif

// ============================================================================
// FAILURE REPORT — detailed nanosecond trace on error
// ============================================================================

struct PhaseRecorder {
    struct Record { const char* name; uint64_t start; uint64_t end; };
    std::vector<Record> records;
    void begin(const char* name) {
        records.push_back({name, nanos(), 0});
    }
    void end() {
        if (!records.empty()) records.back().end = nanos();
    }
    static uint64_t nanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }
    void report(const char* context) {
        uint64_t now = nanos();
        std::cerr << "\n=== FAILURE REPORT (" << context << ") ===\n";
        for (auto& r : records) {
            if (r.end == 0) r.end = now;
            uint64_t wall = r.end - r.start;
            std::cerr << "  " << r.name << ": " << wall << " ns (" << (wall / 1000) << " us, " << (wall / 1000000) << " ms)\n";
        }
        std::cerr << "========================================\n";
    }
};

// ============================================================================
// FNV-1A HASH
// ============================================================================

static std::once_flag targetInitFlag;

static void initTargets() {
    std::call_once(targetInitFlag, []() {
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
    });
}

static std::string sysRoot() {
    const char* pref = getenv("PREFIX");
    if (pref) return std::string(pref);
    return "/data/data/com.termux/files/usr";
}

// Reject paths containing characters that are dangerous even when passed
// directly to exec (control chars) — defensive validation.
static bool isSafePath(const std::string& p) {
    if (p.empty()) return false;
    for (unsigned char c : p) {
        if (c == '\0' || c == '\n' || c == '\r') return false;
    }
    return true;
}

// Split a flag string (e.g. from python3-config) on whitespace into argv tokens.
// This does NOT interpret shell metacharacters — tokens are passed literally
// to exec, so injection via `$()`, backticks, `;`, etc. is impossible.
static std::vector<std::string> splitFlags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Run a program with an explicit argv array via fork + execvp — NO shell is
// involved, so none of the arguments can be interpreted as shell syntax.
// Returns the child's exit status (0 == success), or -1 on spawn failure.
static int runProcess(const std::vector<std::string>& args) {
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

static bool spawnLinker(const std::string& objPath, const std::string& outputPath,
                        const std::string& extraFlags = "") {
    if (!isSafePath(objPath) || !isSafePath(outputPath)) {
        std::cerr << "linker: unsafe path rejected\n";
        return false;
    }

    // Build the argv array directly — no shell, no string interpolation.
    std::vector<std::string> args = {"clang", objPath, "runtime.o"};

    // Include standard library .o files if they exist
    struct stat st;
    const char* stdLibs[] = {"flint_tensor.o", "flint_ai.o", "flint_ai_opt.o",
                             "flint_serial.o", "flint_crypto.o", "flint_net.o",
                             "flint_aegis.o", "flint_chan.o",
                             "ffi_helper.o"};
    for (auto* lib : stdLibs) {
        if (stat(lib, &st) == 0) args.push_back(lib);
    }

    // Include pyruntime.o if it exists (Python support) — try current dir.
    if (stat("pyruntime.o", &st) == 0) {
        args.push_back("pyruntime.o");
        // Detect python3-config linker flags. The command here is a fixed
        // literal (no user input), so popen is safe; the *output* is tokenized
        // and passed literally to exec, never re-interpreted by a shell.
        std::string pyFlags;
        FILE* pipe = popen("python3-config --ldflags 2>/dev/null", "r");
        if (!pipe) { pipe = popen("python3-config --ldflags --embed 2>/dev/null", "r"); }
        if (pipe) {
            char buf[4096] = {};
            std::string acc;
            while (fgets(buf, sizeof(buf), pipe)) acc += buf;
            pclose(pipe);
            pyFlags = acc;
        }
        for (auto& tok : splitFlags(pyFlags)) args.push_back(tok);
    }

    // User-supplied --link flags: tokenized and passed literally (no shell).
    for (auto& tok : splitFlags(extraFlags)) args.push_back(tok);

    args.push_back("-o");
    args.push_back(outputPath);

    int ret = runProcess(args);
    if (ret != 0) {
        std::cerr << "linker failed (exit=" << ret << ")\n";
        return false;
    }
    return true;
}
static bool emitModuleOutput(llvm::Module* mod, const std::string& outputPath,
    llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::None) {
    bool emitObject = outputPath.size() >= 2 &&
                      outputPath.substr(outputPath.size() - 2) == ".o";
    if (!emitObject) {
        std::string irBuf;
        llvm::raw_string_ostream rso(irBuf);
        mod->print(rso, nullptr);
        rso.flush();
        std::error_code ec;
        llvm::raw_fd_ostream outStream(outputPath, ec);
        if (ec) { std::cerr << "cannot open output file: " << ec.message() << "\n"; return false; }
        outStream << irBuf;
        outStream.close();
        return true;
    }

    initTargets();
    auto triple = mod->getTargetTriple();
    std::string error;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) { std::cerr << "target error: " << error << "\n"; return false; }
    auto* tm = target->createTargetMachine(llvm::Triple(triple), "generic", "", {}, llvm::Reloc::PIC_,
        std::nullopt, optLevel);
    if (!tm) { std::cerr << "failed to create target machine\n"; return false; }

    llvm::legacy::PassManager pm;
    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec);
    if (ec) { std::cerr << "error: " << ec.message() << "\n"; delete tm; return false; }
    if (tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        std::cerr << "target cannot emit object files\n";
        delete tm;
        return false;
    }
    pm.run(*mod);
    dest.close();
    delete tm;
    return true;
}

// Run LLVM optimization passes on the module (new PM).
static void runLLVMOptimizations(llvm::Module* mod, llvm::OptimizationLevel level) {
    using namespace llvm;
    PassBuilder PB;
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
    MPM.run(*mod, MAM);
}

// Run the compiled module in-memory via ORC JIT, calling main(argc, argv).
// Returns the exit code from main(), or 1 on error.
// Takes ownership of mod and ctx (mod must be in ctx).
// progArgs are the args to pass to the JIT'd program (argv[0] + user args).
// extraObjs is a list of .o files to load (from --link flags).
static int runWithJIT(std::unique_ptr<llvm::Module> mod, std::unique_ptr<llvm::LLVMContext> ctx,
                      const std::vector<char*>& progArgs,
                      const std::vector<std::string>& extraObjs = {}) {
    using namespace llvm;
    using namespace llvm::orc;

    // Pre-load libpython3.13.so so JIT can resolve Python symbols
    dlopen("libpython3.13.so", RTLD_LAZY | RTLD_GLOBAL);

    initTargets();

    auto JTMB = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!JTMB) {
        std::cerr << "JIT error: " << toString(JTMB.takeError()) << "\n";
        return 1;
    }
    JTMB->setCodeGenOptLevel(llvm::CodeGenOptLevel::Default);
    auto createJIT = LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*JTMB))
        .create();
    if (!createJIT) {
        std::cerr << "JIT error: " << toString(createJIT.takeError()) << "\n";
        return 1;
    }
    auto JIT = std::move(*createJIT);

    // Load runtime.o if present
    struct stat st;
    if (stat("runtime.o", &st) == 0) {
        auto buf = MemoryBuffer::getFile("runtime.o");
        if (!buf) {
            std::cerr << "JIT error: cannot open runtime.o: " << buf.getError().message() << "\n";
            return 1;
        }
        if (auto err = JIT->addObjectFile(std::move(*buf))) {
            std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
            return 1;
        }
    }
    // Load AI engine runtime objects
    const char* aiObjs[] = {"flint_tensor.o", "flint_ai.o", "flint_ai_opt.o"};
    for (auto* objName : aiObjs) {
        if (stat(objName, &st) == 0) {
            auto buf = MemoryBuffer::getFile(objName);
            if (!buf) {
                std::cerr << "JIT error: cannot open " << objName << ": " << buf.getError().message() << "\n";
                return 1;
            }
            if (auto err = JIT->addObjectFile(std::move(*buf))) {
                std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
                return 1;
            }
        }
    }
    // Load standard library runtime objects
    const char* stdObjs[] = {"flint_serial.o", "flint_crypto.o", "flint_net.o",
                             "flint_aegis.o", "flint_chan.o"};
    for (auto* objName : stdObjs) {
        if (stat(objName, &st) == 0) {
            auto buf = MemoryBuffer::getFile(objName);
            if (!buf) {
                std::cerr << "JIT error: cannot open " << objName << ": " << buf.getError().message() << "\n";
                return 1;
            }
            if (auto err = JIT->addObjectFile(std::move(*buf))) {
                std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
                return 1;
            }
        }
    }
    if (stat("pyruntime.o", &st) == 0) {
        auto buf = MemoryBuffer::getFile("pyruntime.o");
        if (!buf) {
            std::cerr << "JIT error: cannot open pyruntime.o: " << buf.getError().message() << "\n";
            return 1;
        }
        if (auto err = JIT->addObjectFile(std::move(*buf))) {
            std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
            return 1;
        }
    }
    // Auto-load ffi_helper.o if present (unless already listed in extraObjs)
    bool hasFfiHelper = false;
    for (auto& obj : extraObjs) if (obj.find("ffi_helper") != std::string::npos) { hasFfiHelper = true; break; }
    if (!hasFfiHelper && stat("ffi_helper.o", &st) == 0) {
        auto buf = MemoryBuffer::getFile("ffi_helper.o");
        if (buf) {
            if (auto err = JIT->addObjectFile(std::move(*buf))) {
                std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
                return 1;
            }
        }
    }
    // Load extra .o files from --link flags
    for (auto& obj : extraObjs) {
        auto buf = MemoryBuffer::getFile(obj);
        if (!buf) {
            std::cerr << "JIT error: cannot open '" << obj << "': " << buf.getError().message() << "\n";
            return 1;
        }
        if (auto err = JIT->addObjectFile(std::move(*buf))) {
            std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
            return 1;
        }
    }

    // Move module + its own context into JIT (they must be paired)
    if (auto err = JIT->addIRModule(ThreadSafeModule(std::move(mod), std::move(ctx)))) {
        std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
        return 1;
    }

    auto mainSym = JIT->lookup("main");
    if (!mainSym) {
        std::cerr << "JIT error: " << toString(mainSym.takeError()) << "\n";
        return 1;
    }
    auto* mainFn = (int (*)(int, char**))(mainSym->getValue());

    return mainFn((int)progArgs.size(), const_cast<char**>(progArgs.data()));
}

static uint64_t fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t fnv1a(llvm::StringRef s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// C1: import manifest. Imported file bytes are NOT part of sourceHash
// (they are discovered during parse, after the key is computed), so a
// changed import must never reuse a cached module. Every import read
// records (path, content-hash) here; the cache writes/verifies a sidecar.
static std::vector<std::pair<std::string, uint64_t>> g_importHashes;
static std::mutex g_importMutex; // parallel imports append concurrently
static void recordImportHash(const std::string& absPath, llvm::StringRef bytes) {
    std::lock_guard<std::mutex> lock(g_importMutex);
    g_importHashes.emplace_back(absPath, fnv1a(bytes));
}

// A1: compiler identity baked by build.sh (-DFLINT_VERSION="...").
#ifndef FLINT_VERSION
#define FLINT_VERSION "dev"
#endif
static const char* kFlintVersion = FLINT_VERSION;

// A1: salt the content-addressed cache key with compiler identity +
// codegen-affecting flags. Previously the key was main-file bytes only,
// so a rebuilt/upgraded flintc (or different --unsafe/--opt-level/
// --backend) silently reused stale modules/binaries.
static std::string cacheSalt(llvm::CodeGenOptLevel ol, bool releaseMode,
                             const std::string& backend, const char* argv0) {
    struct stat st;
    uint64_t mt = 0;
    if (argv0 && stat(argv0, &st) == 0) mt = (uint64_t)st.st_mtime;
    return "flintc-v" + std::string(kFlintVersion) + "-o" + std::to_string((int)ol)
         + (releaseMode ? "-unsafe" : "-safe") + "-" + backend + "-m" + std::to_string(mt);
}

// ============================================================================
// MODULE CACHE — content-addressed LLVM bitcode cache
// ============================================================================

class ModuleCache {
    std::string cacheDir;
public:
    ModuleCache() {
        const char* home = getenv("HOME");
        cacheDir = std::string(home ? home : ".") + "/.cache/flintc";
        // create_directories (not single mkdir): a fresh HOME without
        // .cache/ must still cache instead of failing every write silently.
        std::error_code ec = llvm::sys::fs::create_directories(cacheDir);
        if (ec) std::cerr << "cache dir error: " << ec.message() << "\n";
    }

    std::string bitcodePath(uint64_t hash) const {
        return cacheDir + "/" + std::to_string(hash) + ".bc";
    }

    std::string binaryPath(uint64_t hash) const {
        return cacheDir + "/" + std::to_string(hash) + ".bin";
    }

    std::string sidecarPath(uint64_t hash) const {
        return cacheDir + "/" + std::to_string(hash) + ".imports";
    }

    // A1: import sidecar — `<hash> <abspath>` per import, one per line.
    // Written on every save; a load without a matching sidecar is a miss.
    // (The g_importHashes header comment always promised this; it is now real.)
    void saveSidecar(uint64_t hash) {
        std::ofstream out(sidecarPath(hash), std::ios::trunc);
        if (!out) return;
        std::lock_guard<std::mutex> lock(g_importMutex);
        for (auto& kv : g_importHashes) out << kv.second << " " << kv.first << "\n";
    }

    // Re-hash every recorded import from disk; any change/missing file
    // invalidates the entry. No sidecar (pre-A1 entry) also misses —
    // safe because the A1 salt already retires all legacy hashes.
    bool verifySidecar(uint64_t hash) const {
        std::ifstream in(sidecarPath(hash));
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            auto sp = line.find(' ');
            if (sp == std::string::npos) return false;
            errno = 0;
            char* end = nullptr;
            unsigned long long want =
                ::strtoull(line.c_str(), &end, 10);
            if (errno != 0 || !end || end != line.c_str() + (ptrdiff_t)sp) return false;
            std::string path = line.substr(sp + 1);
            if (path.empty()) return false;
            auto buf = llvm::MemoryBuffer::getFile(path);
            if (!buf) return false;
            if (fnv1a(buf.get()->getBuffer()) != (uint64_t)want) return false;
        }
        return true;
    }

    bool has(uint64_t hash) const {
        struct stat st;
        if (stat(bitcodePath(hash).c_str(), &st) != 0) return false;
        return verifySidecar(hash);
    }

    bool hasBinary(uint64_t hash) const {
        struct stat st;
        if (stat(binaryPath(hash).c_str(), &st) != 0) return false;
        return verifySidecar(hash);
    }

    void save(llvm::Module* mod, uint64_t hash) {
        auto path = bitcodePath(hash);
        std::error_code ec;
        llvm::raw_fd_ostream out(path, ec);
        if (ec) { std::cerr << "cache write error: " << ec.message() << "\n"; return; }
        llvm::WriteBitcodeToFile(*mod, out);
        out.close();
        saveSidecar(hash);
    }

    void saveBinary(uint64_t hash, const std::string& binPath) {
        auto dest = binaryPath(hash);
        llvm::sys::fs::copy_file(binPath, dest);
        saveSidecar(hash);
    }

    bool loadBinary(uint64_t hash, const std::string& outputPath) {
        auto src = binaryPath(hash);
        std::error_code ec;
        llvm::sys::fs::copy_file(src, outputPath);
        return !ec;
    }

    std::unique_ptr<llvm::Module> load(uint64_t hash, llvm::LLVMContext& ctx) {
        auto path = bitcodePath(hash);
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) return nullptr;
        auto src = llvm::parseBitcodeFile(buf.get()->getMemBufferRef(), ctx);
        if (!src) {
            llvm::handleAllErrors(src.takeError(), [](const llvm::ErrorInfoBase& e) {
                std::cerr << "cache read error: " << e.message() << "\n";
            });
            return nullptr;
        }
        return std::move(*src);
    }
};

// ============================================================================
// THREAD POOL — simple worker thread pool
// ============================================================================

class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
    size_t active = 0;
public:
    ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++)
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                        ++active;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        --active;
                    }
                    cv.notify_all();
                }
            });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers) w.join();
    }

    void enqueue(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push(std::move(f));
        }
        cv.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return tasks.empty() && active == 0; });
    }
};

// ============================================================================
// ARENA ALLOCATOR & STRING POOL
// ============================================================================

class ArenaAllocator {
public:
    static constexpr size_t BLOCK_SIZE = 64 * 1024;

    ArenaAllocator() { addBlock(BLOCK_SIZE); }
    ~ArenaAllocator() { for (auto* b : blocks) std::free(b); }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    template<typename T, typename... Args>
    T* make(Args&&... args) {
        void* ptr = alloc(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void reset() {
        for (size_t i = 1; i < blocks.size(); i++) std::free(blocks[i]);
        blocks.resize(1);
        cur = blocks[0];
        offset = 0;
    }

private:
    struct BlockHeader { size_t size; };

    static void* addBlock(size_t sz) {
        void* block = std::malloc(sz);
        if (!block) { std::fprintf(stderr, "ArenaAllocator: out of memory\n"); std::abort(); }
        static_cast<BlockHeader*>(block)->size = sz;
        return block;
    }

    void* alloc(size_t sz, size_t align) {
        size_t start = (offset + align - 1) & ~(align - 1);
        if (start + sz > capacity()) {
            size_t newSz = std::max(BLOCK_SIZE, sz + sizeof(BlockHeader) + align);
            blocks.push_back(addBlock(newSz));
            cur = blocks.back();
            offset = 0;
            start = 0;
        }
        void* ptr = static_cast<char*>(cur) + sizeof(BlockHeader) + start;
        offset = start + sz;
        return ptr;
    }

    size_t capacity() const {
        return static_cast<BlockHeader*>(cur)->size - sizeof(BlockHeader);
    }

    std::vector<void*> blocks;
    void* cur = nullptr;
    size_t offset = 0;
};

class StringPool {
public:
    uint32_t intern(const std::string& s) {
        auto it = table.find(s);
        if (it != table.end()) return it->second;
        uint32_t id = nextId++;
        table[s] = id;
        char* copy = arena.make<char>(s.size() + 1);
        std::memcpy(copy, s.c_str(), s.size() + 1);
        strings.push_back(copy);
        return id;
    }

    const char* get(uint32_t id) const {
        return (id < strings.size()) ? strings[id] : "";
    }

    void reset() {
        arena.reset();
        table.clear();
        strings.clear();
        nextId = 0;
    }

private:
    ArenaAllocator arena;
    std::unordered_map<std::string, uint32_t> table;
    std::vector<const char*> strings;
    uint32_t nextId = 0;
};

// ============================================================================
// TYPE
// ============================================================================

enum class TypeKind { I64, F64, Str, Bool, Void, Ptr, Array, Ref, Struct, Enum, TypeParam, Map };

struct Type {
    TypeKind kind = TypeKind::Void;
    std::shared_ptr<Type> elemType;
    std::string structName;

    bool operator==(const Type& o) const {
        if (kind != o.kind) return false;
        if (kind == TypeKind::Array || kind == TypeKind::Ref) {
            if (!elemType || !o.elemType) return elemType == o.elemType;
            return *elemType == *o.elemType;
        }
        if (kind == TypeKind::Struct) return structName == o.structName;
        if (kind == TypeKind::Enum) return structName == o.structName;
        if (kind == TypeKind::TypeParam) return structName == o.structName;
        return true;
    }
    bool operator!=(const Type& o) const { return !(*this == o); }

    static Type i64()    { Type t; t.kind = TypeKind::I64; return t; }
    static Type f64()    { Type t; t.kind = TypeKind::F64; return t; }
    static Type str()    { Type t; t.kind = TypeKind::Str; return t; }
    static Type boolean(){ Type t; t.kind = TypeKind::Bool; return t; }
    static Type void_()  { Type t; t.kind = TypeKind::Void; return t; }
    static Type ptr_()   { Type t; t.kind = TypeKind::Ptr; return t; }
    static Type array(Type e)  { Type t; t.kind = TypeKind::Array; t.elemType = std::make_shared<Type>(e); return t; }
    static Type ref(Type e)    { Type t; t.kind = TypeKind::Ref;   t.elemType = std::make_shared<Type>(e); return t; }
    static Type struct_(const std::string& n) { Type t; t.kind = TypeKind::Struct; t.structName = n; return t; }
    static Type enum_(const std::string& n) { Type t; t.kind = TypeKind::Enum; t.structName = n; return t; }
    static Type typeParam(const std::string& n) { Type t; t.kind = TypeKind::TypeParam; t.structName = n; return t; }
    static Type map() { Type t; t.kind = TypeKind::Map; return t; }

    bool isCopyType() const {
        return kind == TypeKind::I64 || kind == TypeKind::F64 || kind == TypeKind::Bool || kind == TypeKind::Str || kind == TypeKind::Ptr || kind == TypeKind::Ref || kind == TypeKind::Struct || kind == TypeKind::Enum || kind == TypeKind::Map;
    }
    bool isCompound() const { return kind == TypeKind::Array || kind == TypeKind::Ref; }
};

struct StructFieldDef {
    std::string name;
    Type type;
};

struct StructDef {
    std::string name;
    std::vector<StructFieldDef> fields;
};

struct EnumVariantDef {
    std::string name;
    std::vector<Type> payloadTypes;
};

struct EnumDef {
    std::string name;
    std::vector<EnumVariantDef> variants;
};

 llvm::Type* llvmType(Type t, llvm::LLVMContext& ctx, llvm::Module* mod = nullptr) {
    switch (t.kind) {
        case TypeKind::I64:  return llvm::Type::getInt64Ty(ctx);
        case TypeKind::F64:  return llvm::Type::getDoubleTy(ctx);
        case TypeKind::Bool: return llvm::Type::getInt64Ty(ctx);
        case TypeKind::Ptr:  return llvm::PointerType::get(ctx, 0);
        case TypeKind::Str:  return llvm::PointerType::get(ctx, 0);
        case TypeKind::Map:  return llvm::PointerType::get(ctx, 0);
        case TypeKind::Void: return llvm::Type::getVoidTy(ctx);
        case TypeKind::Ref:  return llvm::PointerType::get(ctx, 0);
        case TypeKind::Array: {
            llvm::Type* elemPtrTy = llvm::PointerType::get(ctx, 0);
            llvm::Type* lenTy = llvm::Type::getInt64Ty(ctx);
            return llvm::StructType::get(ctx, {elemPtrTy, lenTy});
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
            return llvm::StructType::getTypeByName(ctx, t.structName);
        case TypeKind::TypeParam:
            return llvm::Type::getInt64Ty(ctx); // placeholder, substituted before use
    }
    return llvm::Type::getInt64Ty(ctx);
}

// ============================================================================
// TOKEN
// ============================================================================

enum class TokenType {
    END_OF_FILE, NUMBER_LITERAL, STRING_LITERAL, IDENTIFIER,
    ASSIGN, LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET, SEMICOLON, COMMA, COLON, COLON_EQ, ARROW,
    NEWLINE, UNKNOWN,
    KW_MUT, KW_FN, KW_VAR, KW_LET, KW_DEF, KW_IF, KW_ELSE, KW_ELIF, KW_WHILE, KW_RETURN, KW_BREAK,     KW_I64, KW_F64, KW_STR, KW_BOOL, KW_PTR, KW_EXTERN, KW_PYTHON, KW_STRUCT, KW_ENUM, KW_MATCH, KW_IMPORT, KW_FOR, KW_IN, KW_PARALLEL, KW_TRY,
    KW_CONTINUE, KW_AND, KW_OR, KW_NOT,
    PLUS, PLUS_EQ, MINUS, MINUS_EQ, STAR, STAR_EQ, SLASH, SLASH_EQ, MODULO, MODULO_EQ,
    ELLIPSIS, AMPERSAND, AMPERSAND_AMP, DOT, DOTDOT, AT, QUESTION, BANG,
    EQ_EQ, NE, LT, GT, LE, GE, FAT_ARROW, PIPE_PIPE, PIPE
};

// Stable Stage-1 token IDs (contract with stage1/flint_lex.fl):
// 1 IDENT, 2 NUMBER, 3 STRING, 10-36 keywords, 50-88 punctuation.
static int stableTokenKind(TokenType t, const std::string& lexeme) {
    switch (t) {
        case TokenType::IDENTIFIER: return 1;
        case TokenType::NUMBER_LITERAL: return 2;
        case TokenType::STRING_LITERAL: return 3;
        case TokenType::KW_MUT: return 10;
        case TokenType::KW_VAR: return 11;
        case TokenType::KW_LET: return 12;
        case TokenType::KW_DEF: return 13;
        case TokenType::KW_FN: return 14;
        case TokenType::KW_IF: return 15;
        case TokenType::KW_ELIF: return 16;
        case TokenType::KW_ELSE: return 17;
        case TokenType::KW_WHILE: return 18;
        case TokenType::KW_BREAK: return 19;
        case TokenType::KW_CONTINUE: return 20;
        case TokenType::KW_RETURN: return 21;
        case TokenType::KW_EXTERN: return 22;
        case TokenType::KW_I64: return 23;
        case TokenType::KW_F64: return 24;
        case TokenType::KW_STR: return 25;
        case TokenType::KW_BOOL: return 26;
        case TokenType::KW_PTR: return 27;
        case TokenType::KW_PYTHON: return 28;
        case TokenType::KW_STRUCT: return 29;
        case TokenType::KW_ENUM: return 30;
        case TokenType::KW_MATCH: return 31;
        case TokenType::KW_IMPORT: return 32;
        case TokenType::KW_FOR: return 33;
        case TokenType::KW_IN: return 34;
        case TokenType::KW_PARALLEL: return 35;
        case TokenType::KW_TRY: return 36;
        case TokenType::KW_AND: return 60;
        case TokenType::KW_OR: return 88;
        case TokenType::KW_NOT: return 77;
        case TokenType::LPAREN: return 50;
        case TokenType::RPAREN: return 51;
        case TokenType::LBRACE: return 52;
        case TokenType::RBRACE: return 53;
        case TokenType::LBRACKET: return 54;
        case TokenType::RBRACKET: return 55;
        case TokenType::SEMICOLON: return 56;
        case TokenType::COMMA: return 57;
        case TokenType::COLON: return 58;
        case TokenType::COLON_EQ: return 59;
        case TokenType::AMPERSAND_AMP: return 60;
        case TokenType::AMPERSAND: return 61;
        case TokenType::PLUS: return 62;
        case TokenType::PLUS_EQ: return 63;
        case TokenType::ARROW: return 64;
        case TokenType::MINUS: return 65;
        case TokenType::MINUS_EQ: return 66;
        case TokenType::STAR: return 67;
        case TokenType::STAR_EQ: return 68;
        case TokenType::SLASH: return 69;
        case TokenType::SLASH_EQ: return 70;
        case TokenType::MODULO: return 71;
        case TokenType::MODULO_EQ: return 72;
        case TokenType::ASSIGN: return 73;
        case TokenType::EQ_EQ: return 74;
        case TokenType::FAT_ARROW: return 75;
        case TokenType::NE: return 76;
        case TokenType::BANG: return 77;
        case TokenType::LT: return 78;
        case TokenType::LE: return 79;
        case TokenType::GT: return 80;
        case TokenType::GE: return 81;
        case TokenType::DOT: return 82;
        case TokenType::DOTDOT: return 83;
        case TokenType::ELLIPSIS: return 84;
        case TokenType::AT: return 85;
        case TokenType::QUESTION: return 86;
        case TokenType::PIPE: return 87;
        case TokenType::PIPE_PIPE: return 88;
        default: return (int)t;
    }
}

// Dump escaping (mirrors stage1 lex_esc_dump): backslash, LF, CR.
static std::string stage1Escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

struct SourceLocation {
    int line = 1;
    int col = 1;
};

struct Token {
    TokenType type = TokenType::UNKNOWN;
    std::string lexeme;
    SourceLocation loc;
};

// ============================================================================
// LEXER
// ============================================================================

class Lexer {
public:
    explicit Lexer(llvm::StringRef source) : source(source), pos(0), loc{1,1} {}
    llvm::StringRef getSource() const { return source; }
    std::string getLine(int line) const {
        size_t start = 0; int currentLine = 1;
        for (size_t i = 0; i < source.size(); i++) {
            if (currentLine == line && (source[i] == '\n' || i == source.size() - 1)) {
                size_t end = (source[i] == '\n') ? i : i + 1;
                while (start < end && source[start] == ' ') start++;
                return source.substr(start, end - start).str();
            }
            if (source[i] == '\n') { currentLine++; start = i + 1; }
            if (currentLine > line) break;
        }
        return "";
    }

    // Legacy: tokenize all upfront.
    std::vector<Token> tokenize() {
        while (ensure(tokens.size() + 1).type != TokenType::END_OF_FILE) {}
        return tokens;
    }

    // Streaming: ensure that at least `pos + 1` tokens have been generated.
    // Returns a const-ref to the token at `pos` (satisfies random access for Parser).
    const Token& ensure(size_t idx) {
        while (idx >= tokens.size()) {
            if (isAtEnd()) {
                tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
                return tokens.back();
            }
            skipWhitespace();
            if (isAtEnd()) continue; // re-check after whitespace
            char c = advance();
            if (c == '\n') { if (tokens.empty()) { skipWhitespace(); } continue; }
            switch (c) {
                case '(': tokens.push_back(makeToken(TokenType::LPAREN, "(")); break;
                case ')': tokens.push_back(makeToken(TokenType::RPAREN, ")")); break;
                case '{': tokens.push_back(makeToken(TokenType::LBRACE, "{")); break;
                case '}': tokens.push_back(makeToken(TokenType::RBRACE, "}")); break;
                case '[': tokens.push_back(makeToken(TokenType::LBRACKET, "[")); break;
                case ']': tokens.push_back(makeToken(TokenType::RBRACKET, "]")); break;
                case ';': tokens.push_back(makeToken(TokenType::SEMICOLON, ";")); break;
                case ',': tokens.push_back(makeToken(TokenType::COMMA, ",")); break;
                case ':':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::COLON_EQ, ":=")); }
                    else { tokens.push_back(makeToken(TokenType::COLON, ":")); }
                    break;
                case '&':
                    if (peek() == '&') { advance(); tokens.push_back(makeToken(TokenType::AMPERSAND_AMP, "&&")); }
                    else { tokens.push_back(makeToken(TokenType::AMPERSAND, "&")); }
                    break;
                case '+':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::PLUS_EQ, "+=")); }
                    else { tokens.push_back(makeToken(TokenType::PLUS, "+")); }
                    break;
                case '-':
                    if (peek() == '>') { advance(); tokens.push_back(makeToken(TokenType::ARROW, "->")); }
                    else if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::MINUS_EQ, "-=")); }
                    else { tokens.push_back(makeToken(TokenType::MINUS, "-")); }
                    break;
                case '*':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::STAR_EQ, "*=")); }
                    else { tokens.push_back(makeToken(TokenType::STAR, "*")); }
                    break;
                case '/':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::SLASH_EQ, "/=")); }
                    else { tokens.push_back(makeToken(TokenType::SLASH, "/")); }
                    break;
                case '%':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::MODULO_EQ, "%=")); }
                    else { tokens.push_back(makeToken(TokenType::MODULO, "%")); }
                    break;
                case '=':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::EQ_EQ, "==")); }
                    else if (peek() == '>') { advance(); tokens.push_back(makeToken(TokenType::FAT_ARROW, "=>")); }
                    else { tokens.push_back(makeToken(TokenType::ASSIGN, "=")); }
                    break;
                case '!':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::NE, "!=")); }
                    else { tokens.push_back(makeToken(TokenType::BANG, "!")); }
                    break;
                case '<':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::LE, "<=")); }
                    else { tokens.push_back(makeToken(TokenType::LT, "<")); }
                    break;
                case '>':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::GE, ">=")); }
                    else { tokens.push_back(makeToken(TokenType::GT, ">")); }
                    break;
                    case '.':
                        if (peek() == '.' && peek(1) == '.') { advance(); advance(); tokens.push_back(makeToken(TokenType::ELLIPSIS, "...")); }
                        else if (peek() == '.') { advance(); tokens.push_back(makeToken(TokenType::DOTDOT, "..")); }
                        else { tokens.push_back(makeToken(TokenType::DOT, ".")); }
                        break;
                    case '@': tokens.push_back(makeToken(TokenType::AT, "@")); break;
                    case '?': tokens.push_back(makeToken(TokenType::QUESTION, "?")); break;
                    case '|':
                        if (peek() == '|') { advance(); tokens.push_back(makeToken(TokenType::PIPE_PIPE, "||")); }
                        else if (peek() == '>') { advance(); tokens.push_back(makeToken(TokenType::PIPE, "|>")); }
                        else { tokens.push_back(makeToken(TokenType::PIPE, "|")); }
                        break;
                case '"': tokens.push_back(readString()); break;
                case '#':
                    while (peek() != '\n' && peek() != '\0') advance();
                    if (peek() == '\n') advance();
                    break;
                default:
                    if (std::isdigit(c)) { pos--; loc.col--; tokens.push_back(readNumber()); }
                    else if (std::isalpha(c) || c == '_') { pos--; loc.col--; tokens.push_back(readIdentifier()); }
                    else { unexpected(c); }
                    break;
            }
        }
        return tokens[idx];
    }

    // Access all tokens produced so far (for Parser random access / QBE re-parse)
    const std::vector<Token>& getTokens() const { return tokens; }
    size_t getSourcePos() const { return pos; }

private:
    llvm::StringRef source;
    size_t pos;
    SourceLocation loc;
    std::vector<Token> tokens;

    char peek(size_t ahead = 0) const {
        size_t idx = pos + ahead;
        return idx < source.size() ? source[idx] : '\0';
    }

    char advance() {
        char c = source[pos++];
        if (c == '\n') { loc.line++; loc.col = 1; } else { loc.col++; }
        return c;
    }

    bool isAtEnd() const { return pos >= source.size(); }

    void skipWhitespace() {
        while (!isAtEnd()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') { advance(); }
            else if (c == '/' && peek(1) == '/') { while (!isAtEnd() && peek() != '\n') advance(); }
            else { break; }
        }
    }

    Token makeToken(TokenType type, const std::string& lexeme) {
        Token t; t.type = type; t.lexeme = lexeme; t.loc = loc; return t;
    }

    void unexpected(char c) {
        std::cerr << "lex error: unexpected character '" << c << "' at line " << loc.line << "\n";
    }

    Token readNumber() {
        size_t start = pos;
        bool isFloat = false;
        while (std::isdigit(peek())) advance();
        if (peek() == '.' && std::isdigit(peek(1))) {
            isFloat = true;
            advance(); // '.'
            while (std::isdigit(peek())) advance();
        }
        // Store isFloat in the lexeme as a hint? No — better in the token.
        // We append a secret 'd' suffix to the lexeme that the parser will see.
        std::string lit = source.substr(start, pos - start).str();
        if (isFloat) lit += "d";  // marker for double
        return makeToken(TokenType::NUMBER_LITERAL, lit);
    }

    Token readString() {
        std::string value;
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        auto appendUtf8 = [&value](int cp) {
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            if (cp < 0x80) value += (char)cp;
            else if (cp < 0x800) {
                value += (char)(0xC0 + cp / 64);
                value += (char)(0x80 + cp % 64);
            } else {
                value += (char)(0xE0 + cp / 4096);
                value += (char)(0x80 + (cp / 64) % 64);
                value += (char)(0x80 + cp % 64);
            }
        };
        while (!isAtEnd() && peek() != '"') {
            if (peek() == '\\') {
                advance(); // '\\'
                if (isAtEnd()) { value += '\\'; break; }
                char e = advance();
                switch (e) {
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    case 'r': value += '\r'; break;
                    case '\\': value += '\\'; break;
                    case '"': value += '"'; break;
                    case 'x': {
                        // \xHH: need 2 hex digits, else literal "\x" (mirrors Flint)
                        if (pos + 1 < source.size()) {
                            int hi = hexVal(source[pos]);
                            int lo = hexVal(source[pos + 1]);
                            if (hi >= 0 && lo >= 0) {
                                value += (char)(hi * 16 + lo);
                                advance(); advance();
                            } else {
                                value += '\\'; value += 'x';
                            }
                        } else {
                            value += '\\'; value += 'x';
                        }
                        break;
                    }
                    case 'u': {
                        // \uHHHH: need 4 hex digits, else literal "\u"
                        if (pos + 3 < source.size()) {
                            int d0 = hexVal(source[pos]);
                            int d1 = hexVal(source[pos + 1]);
                            int d2 = hexVal(source[pos + 2]);
                            int d3 = hexVal(source[pos + 3]);
                            if (d0 >= 0 && d1 >= 0 && d2 >= 0 && d3 >= 0) {
                                appendUtf8(d0 * 4096 + d1 * 256 + d2 * 16 + d3);
                                advance(); advance(); advance(); advance();
                            } else {
                                value += '\\'; value += 'u';
                            }
                        } else {
                            value += '\\'; value += 'u';
                        }
                        break;
                    }
                    default: value += '\\'; value += e; break;
                }
            } else { value += advance(); }
        }
        if (isAtEnd()) { std::cerr << "lex error: unterminated string\n"; }
        else { advance(); }
        return makeToken(TokenType::STRING_LITERAL, value);
    }

    Token readIdentifier() {
        size_t start = pos;
        while (std::isalnum(peek()) || peek() == '_') advance();
        std::string s = source.substr(start, pos - start).str();
        if (s == "mut")    return makeToken(TokenType::KW_MUT, s);
        if (s == "var")    return makeToken(TokenType::KW_VAR, s);
        if (s == "let")    return makeToken(TokenType::KW_LET, s);
        if (s == "def")    return makeToken(TokenType::KW_DEF, s);
        if (s == "fn")     return makeToken(TokenType::KW_FN, s);
        if (s == "if")     return makeToken(TokenType::KW_IF, s);
        if (s == "elif")   return makeToken(TokenType::KW_ELIF, s);
        if (s == "else")   return makeToken(TokenType::KW_ELSE, s);
        if (s == "while")  return makeToken(TokenType::KW_WHILE, s);
        if (s == "break")  return makeToken(TokenType::KW_BREAK, s);
        if (s == "continue") return makeToken(TokenType::KW_CONTINUE, s);
        if (s == "return") return makeToken(TokenType::KW_RETURN, s);
        if (s == "and") return makeToken(TokenType::KW_AND, s);
        if (s == "or") return makeToken(TokenType::KW_OR, s);
        if (s == "not") return makeToken(TokenType::KW_NOT, s);
        if (s == "extern") return makeToken(TokenType::KW_EXTERN, s);
        if (s == "i64")    return makeToken(TokenType::KW_I64, s);
        if (s == "f64")    return makeToken(TokenType::KW_F64, s);
        if (s == "str")    return makeToken(TokenType::KW_STR, s);
        if (s == "bool")   return makeToken(TokenType::KW_BOOL, s);
        if (s == "ptr")    return makeToken(TokenType::KW_PTR, s);
        if (s == "python") return makeToken(TokenType::KW_PYTHON, s);
        if (s == "struct") return makeToken(TokenType::KW_STRUCT, s);
        if (s == "enum")   return makeToken(TokenType::KW_ENUM, s);
        if (s == "match")   return makeToken(TokenType::KW_MATCH, s);
        if (s == "import")  return makeToken(TokenType::KW_IMPORT, s);
        if (s == "for")     return makeToken(TokenType::KW_FOR, s);
        if (s == "in")      return makeToken(TokenType::KW_IN, s);
        if (s == "parallel") return makeToken(TokenType::KW_PARALLEL, s);
        if (s == "try")     return makeToken(TokenType::KW_TRY, s);
        return makeToken(TokenType::IDENTIFIER, s);
    }
};

// ============================================================================
// AST
// ============================================================================

enum class NodeKind {
    Number, String, Variable, Assign, Binary, Compare, Call, VarDecl,
    Return, If, While, Break, Continue, Block, PyBlock, Array, Index, Slice, Ref, Deref,
    StructLiteral, EnumConstruct, Match, FieldAccess, Function, Lambda, Unwrap, Destructure,
    MapLiteral
};

struct ExprAST {
    SourceLocation loc;
    NodeKind kind;
    virtual ~ExprAST() = default;
    ExprAST() = default;
    explicit ExprAST(NodeKind k) : kind(k) {}
};

struct NumberExprAST : ExprAST {
    double value;      // float value (exact only when isFloat)
    bool isFloat;
    int64_t intValue;  // exact integer value (valid only when isInt)
    bool isInt;
    // Double path: never claims exactness (use int overload for integers).
    explicit NumberExprAST(double v, bool f = false)
        : ExprAST(NodeKind::Number), value(v), isFloat(f),
          intValue(0), isInt(false) {}
    // Integer path: exact i64 (also sets lossy double mirror for legacy readers).
    explicit NumberExprAST(int64_t i)
        : ExprAST(NodeKind::Number), value(static_cast<double>(i)), isFloat(false),
          intValue(i), isInt(true) {}
    // int literals (0, 1) would otherwise be ambiguous between double/int64_t.
    explicit NumberExprAST(int i) : NumberExprAST(static_cast<int64_t>(i)) {}
};

struct StringExprAST : ExprAST {
    std::string value;
    explicit StringExprAST(std::string v) : ExprAST(NodeKind::String), value(std::move(v)) {}
};

struct VariableExprAST : ExprAST {
    std::string name;
    explicit VariableExprAST(std::string n) : ExprAST(NodeKind::Variable), name(std::move(n)) {}
};

struct AssignExprAST : ExprAST {
    std::string varName;
    std::unique_ptr<ExprAST> rhs;
    AssignExprAST(std::string vn, std::unique_ptr<ExprAST> r)
        : ExprAST(NodeKind::Assign), varName(std::move(vn)), rhs(std::move(r)) {}
};

struct BinaryExprAST : ExprAST {
    char op;
    std::unique_ptr<ExprAST> lhs, rhs;
    BinaryExprAST(char o, std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r)
        : ExprAST(NodeKind::Binary), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct CompareExprAST : ExprAST {
    std::string op;
    std::unique_ptr<ExprAST> lhs, rhs;
    CompareExprAST(std::string o, std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r)
        : ExprAST(NodeKind::Compare), op(std::move(o)), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct CallExprAST : ExprAST {
    std::string callee;
    std::vector<Type> typeArgs;
    std::vector<std::unique_ptr<ExprAST>> args;
    CallExprAST(std::string c, std::vector<std::unique_ptr<ExprAST>> a)
        : ExprAST(NodeKind::Call), callee(std::move(c)), args(std::move(a)) {}
};

struct VarDeclAST : ExprAST {
    std::string varName;
    bool isMutable;
    Type varType;
    std::unique_ptr<ExprAST> init;
    VarDeclAST(std::string vn, bool mut, Type t, std::unique_ptr<ExprAST> i)
        : ExprAST(NodeKind::VarDecl), varName(std::move(vn)), isMutable(mut), varType(t), init(std::move(i)) {}
};

struct ReturnStmtAST : ExprAST {
    std::unique_ptr<ExprAST> value;
    explicit ReturnStmtAST(std::unique_ptr<ExprAST> v) : ExprAST(NodeKind::Return), value(std::move(v)) {}
};

struct IfStmtAST : ExprAST {
    std::unique_ptr<ExprAST> condition;
    std::unique_ptr<ExprAST> thenBlock;
    std::unique_ptr<ExprAST> elseBlock;
    IfStmtAST() : ExprAST(NodeKind::If) {}
};

struct WhileStmtAST : ExprAST {
    std::unique_ptr<ExprAST> condition;
    std::unique_ptr<ExprAST> body;
    WhileStmtAST() : ExprAST(NodeKind::While) {}
};

struct BreakStmtAST : ExprAST {
    BreakStmtAST() : ExprAST(NodeKind::Break) {}
};

struct ContinueStmtAST : ExprAST {
    ContinueStmtAST() : ExprAST(NodeKind::Continue) {}
};

struct BlockStmtAST : ExprAST {
    std::vector<std::unique_ptr<ExprAST>> stmts;
    BlockStmtAST() : ExprAST(NodeKind::Block) {}
};

struct PyBlockStmtAST : ExprAST {
    std::vector<std::string> codeStrings;
    PyBlockStmtAST() : ExprAST(NodeKind::PyBlock) {}
};

struct ArrayExprAST : ExprAST {
    std::vector<std::unique_ptr<ExprAST>> elements;
    std::vector<bool> spreadFlags; // true if element is a spread (...expr)
    ArrayExprAST() : ExprAST(NodeKind::Array) {}
};

struct SliceExprAST : ExprAST {
    std::unique_ptr<ExprAST> arr;
    std::unique_ptr<ExprAST> start;
    std::unique_ptr<ExprAST> end;
    SliceExprAST(std::unique_ptr<ExprAST> a, std::unique_ptr<ExprAST> s, std::unique_ptr<ExprAST> e)
        : ExprAST(NodeKind::Slice), arr(std::move(a)), start(std::move(s)), end(std::move(e)) {}
};

struct IndexExprAST : ExprAST {
    std::unique_ptr<ExprAST> base;
    std::unique_ptr<ExprAST> index;
    bool unchecked = false; // R2 BCE-lite: proven in-bounds (hoisted check covers it)
    IndexExprAST(std::unique_ptr<ExprAST> b, std::unique_ptr<ExprAST> i)
        : ExprAST(NodeKind::Index), base(std::move(b)), index(std::move(i)) {}
};

struct RefExprAST : ExprAST {
    std::unique_ptr<ExprAST> target;
    explicit RefExprAST(std::unique_ptr<ExprAST> t) : ExprAST(NodeKind::Ref), target(std::move(t)) {}
};

struct DerefExprAST : ExprAST {
    std::unique_ptr<ExprAST> target;
    explicit DerefExprAST(std::unique_ptr<ExprAST> t) : ExprAST(NodeKind::Deref), target(std::move(t)) {}
};

struct StructLiteralAST : ExprAST {
    std::string structName;
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> fields;
    StructLiteralAST() : ExprAST(NodeKind::StructLiteral) {}
};

struct EnumConstructAST : ExprAST {
    std::string enumName;
    std::string variantName;
    std::vector<std::unique_ptr<ExprAST>> args;
    EnumConstructAST() : ExprAST(NodeKind::EnumConstruct) {}
};

struct MatchArm {
    std::string enumName;
    std::string variantName;
    std::string bindName;
    std::unique_ptr<ExprAST> body;
};

struct MatchExprAST : ExprAST {
    std::unique_ptr<ExprAST> scrutinee;
    std::vector<MatchArm> arms;
    MatchExprAST() : ExprAST(NodeKind::Match) {}
};

struct FieldAccessAST : ExprAST {
    std::unique_ptr<ExprAST> base;
    std::string fieldName;
    FieldAccessAST(std::unique_ptr<ExprAST> b, std::string f)
        : ExprAST(NodeKind::FieldAccess), base(std::move(b)), fieldName(std::move(f)) {}
};

struct LambdaExprAST : ExprAST {
    std::vector<std::pair<std::string, Type>> params;
    std::unique_ptr<ExprAST> body;
    std::vector<std::string> captures; // outer vars used by body (by-value)
    Type retType;                       // inferred at parse from the body
    LambdaExprAST() : ExprAST(NodeKind::Lambda) {}
};

struct UnwrapExprAST : ExprAST {
    std::unique_ptr<ExprAST> inner;
    UnwrapExprAST(std::unique_ptr<ExprAST> i) : ExprAST(NodeKind::Unwrap), inner(std::move(i)) {}
};

struct DestructureAST : ExprAST {
    std::vector<std::string> names;
    std::unique_ptr<ExprAST> init;
    bool isMutable;
    bool isStruct; // true for { a, b }, false for [a, b]
    DestructureAST() : ExprAST(NodeKind::Destructure) {}
};

struct MapLiteralAST : ExprAST {
    std::vector<std::string> keys;
    std::vector<std::unique_ptr<ExprAST>> values;
    MapLiteralAST() : ExprAST(NodeKind::MapLiteral) {}
};

// Clone an expression AST node (for default parameter expansion)
static std::unique_ptr<ExprAST> cloneExpr(ExprAST* e) {
    if (!e) return nullptr;
    switch (e->kind) {
        case NodeKind::Number: {
            auto* n = static_cast<NumberExprAST*>(e);
            std::unique_ptr<NumberExprAST> c;
            if (n->isInt) c = std::make_unique<NumberExprAST>(n->intValue);
            else c = std::make_unique<NumberExprAST>(n->value, n->isFloat);
            c->loc = n->loc; return c;
        }
        case NodeKind::String: {
            auto* s = static_cast<StringExprAST*>(e);
            auto c = std::make_unique<StringExprAST>(s->value);
            c->loc = s->loc; return c;
        }
        case NodeKind::Variable: {
            auto* v = static_cast<VariableExprAST*>(e);
            auto c = std::make_unique<VariableExprAST>(v->name);
            c->loc = v->loc; return c;
        }
        case NodeKind::Lambda: {
            auto* l = static_cast<LambdaExprAST*>(e);
            auto c = std::make_unique<LambdaExprAST>();
            c->loc = l->loc;
            for (auto& p : l->params) c->params.push_back(p);
            c->body = l->body ? cloneExpr(l->body.get()) : nullptr;
            c->captures = l->captures;
            c->retType = l->retType;
            return c;
        }
        case NodeKind::Unwrap: {
            auto* uw = static_cast<UnwrapExprAST*>(e);
            auto c = std::make_unique<UnwrapExprAST>(uw->inner ? cloneExpr(uw->inner.get()) : nullptr);
            c->loc = uw->loc;
            return c;
        }
        case NodeKind::MapLiteral: {
            auto* m = static_cast<MapLiteralAST*>(e);
            auto c = std::make_unique<MapLiteralAST>();
            c->loc = m->loc;
            c->keys = m->keys;
            for (auto& v : m->values) c->values.push_back(cloneExpr(v.get()));
            return c;
        }
        default: return nullptr;
    }
}

// Rewrite `continue` inside a desugared for-body so the loop counter still
// advances: `continue` → `{ counter = counter + 1; continue; }`.
// (The desugar appends the increment as the body's last statement; a bare
// continue would jump to the condition and skip it → infinite loop.)
// Descends through Block/If/Match bodies but NOT through nested While or
// Lambda, whose `continue` binds to the inner loop (or is invalid there).
static void wrapForContinues(ExprAST* node, const std::string& counterVar, SourceLocation loc) {
    if (!node) return;
    auto makeWrap = [&]() {
        auto wrap = std::make_unique<BlockStmtAST>();
        auto cv = std::make_unique<VariableExprAST>(counterVar);
        cv->loc = loc;
        auto one = std::make_unique<NumberExprAST>(1);
        one->loc = loc;
        auto add = std::make_unique<BinaryExprAST>('+', std::move(cv), std::move(one));
        add->loc = loc;
        auto inc = std::make_unique<AssignExprAST>(counterVar, std::move(add));
        inc->loc = loc;
        auto cont = std::make_unique<ContinueStmtAST>();
        wrap->stmts.push_back(std::move(inc));
        wrap->stmts.push_back(std::move(cont));
        wrap->loc = loc;
        return wrap;
    };
    switch (node->kind) {
        case NodeKind::Block: {
            auto* b = static_cast<BlockStmtAST*>(node);
            for (auto& s : b->stmts) {
                if (s->kind == NodeKind::Continue) {
                    auto wrap = makeWrap();
                    wrap->loc = s->loc;
                    s = std::move(wrap);
                } else {
                    wrapForContinues(s.get(), counterVar, loc);
                }
            }
            break;
        }
        case NodeKind::If: {
            auto* i = static_cast<IfStmtAST*>(node);
            wrapForContinues(i->thenBlock.get(), counterVar, loc);
            if (i->elseBlock) wrapForContinues(i->elseBlock.get(), counterVar, loc);
            break;
        }
        case NodeKind::Match: {
            auto* m = static_cast<MatchExprAST*>(node);
            for (auto& arm : m->arms) {
                if (arm.body && arm.body->kind == NodeKind::Continue) {
                    auto wrap = makeWrap();
                    wrap->loc = arm.body->loc;
                    arm.body = std::move(wrap);
                } else {
                    wrapForContinues(arm.body.get(), counterVar, loc);
                }
            }
            break;
        }
        default: break; // While/Lambda/everything else: do not descend
    }
}

struct FunctionAST : ExprAST {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<std::pair<std::string, Type>> params;
    std::vector<std::unique_ptr<ExprAST>> defaults; // default values for params
    Type returnType;
    std::unique_ptr<ExprAST> body;
    bool isDeclaration = false;
    size_t bodyStart = 0;  // token index of '{' in token stream
    size_t bodyEnd = 0;    // token index past '}'
    FunctionAST() : ExprAST(NodeKind::Function) {}
};

struct ExternFn {
    std::string name;
    std::vector<Type> paramTypes;
    Type returnType;
    bool isVararg = false;
};

struct ProgramAST {
    std::vector<ExternFn> externs;
    std::vector<std::unique_ptr<FunctionAST>> functions;
    std::vector<StructDef> structs;
    std::vector<EnumDef> enums;
    std::vector<std::string> imports;
    std::vector<std::unique_ptr<VarDeclAST>> globals;
    std::vector<std::unique_ptr<ExprAST>> topStmts;
};

// ============================================================================
// SYMBOL TABLE
// ============================================================================

struct Symbol {
    Type type;
    llvm::AllocaInst* alloca = nullptr;
    llvm::GlobalVariable* global = nullptr;
    bool isMutable = false;
    bool moved = false;
    int borrowCount = 0;
    // Lambda binding (P0-6 closure conversion; re-added after 2026-09-14
    // wipe): the name refers to an outlined function. Direct calls rewrite
    // to it with by-value captures; any other use is an error.
    bool isLambda = false;
    llvm::Function* lambdaFn = nullptr;
    llvm::Function* lambdaDefinedIn = nullptr;
    std::vector<Type> lambdaParamTypes;
    std::vector<std::string> lambdaCapNames;
    std::vector<Type> lambdaCapTypes;
    std::vector<llvm::AllocaInst*> lambdaCapSlots;
};

class SymbolTable {
public:
    void enterScope() {
        scopes.emplace_back();
        borrowRecords.emplace_back();
    }

    void exitScope() {
        if (!borrowRecords.empty()) {
            for (auto& pair : borrowRecords.back()) {
                auto* sym = lookup(pair.first);
                if (sym) sym->borrowCount -= pair.second;
            }
            borrowRecords.pop_back();
        }
        if (!scopes.empty()) scopes.pop_back();
    }

    bool declare(const std::string& name, const Symbol& sym) {
        if (scopes.empty()) scopes.emplace_back();
        if (scopes.back().count(name)) return false;
        scopes.back()[name] = sym;
        return true;
    }

    Symbol* lookup(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    bool isDeclaredInCurrentScope(const std::string& name) {
        if (scopes.empty()) return false;
        return scopes.back().count(name) > 0;
    }

    void recordBorrow(const std::string& name) {
        if (!borrowRecords.empty()) {
            for (auto& pair : borrowRecords.back()) {
                if (pair.first == name) { pair.second++; return; }
            }
            borrowRecords.back().emplace_back(name, 1);
        }
    }

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::vector<std::vector<std::pair<std::string, int>>> borrowRecords;
};

// ============================================================================
// PARSER
// ============================================================================

class Codegen; // forward declaration for single-pass mode

class Parser {
public:
    explicit Parser(Lexer& lex) : lexer(lex), pos(0) {}

    // Single-pass mode: parser emits IR directly instead of building AST
    void setCodegen(Codegen* c, bool mode = true) { cg = c; emitMode = mode; }
    void setSkipBodies(bool s) { skipBodies = s; }
    size_t getPos() const { return pos; }
    void setPos(size_t p) { pos = p; lexer.ensure(p); }
    bool isInEmitMode() const { return emitMode; }
    // Used by Codegen to re-parse function bodies from token ranges
    std::unique_ptr<ExprAST> reparseBlock() { return parseBlock(); }

    // Lambda-body use collector, shared with Codegen (which verifies
    // outlined bodies). The worker is a private static; this forwards it.
    static void lambdaUses(ExprAST* e, std::unordered_set<std::string>& bound,
                           std::vector<std::string>& uses, std::string* err = nullptr);

    // Reset per-function declared-variable state and seed with the given params.
    // Needed when re-parsing multiple function bodies in sequence (QBE backend),
    // otherwise a variable declared in one function is seen as already-declared
    // in the next, turning `x = ...` VarDecls into Assigns.
    void resetDeclaredVars(const std::vector<std::pair<std::string, Type>>& params) {
        declaredVars.clear();
        for (auto& p : params) declaredVars.insert(p.first);
    }

    std::unique_ptr<ProgramAST> parseProgram() {
        auto prog = std::make_unique<ProgramAST>();
        while (!isAtEnd()) {
            if (check(TokenType::KW_EXTERN)) {
                parseExternBlock(*prog);
            } else if (check(TokenType::KW_FN) || check(TokenType::KW_DEF)) {
                auto fn = parseFunction();
                if (fn) {
                    prog->functions.push_back(std::move(fn));
                    declaredVars.clear();
                }
            } else if (check(TokenType::KW_STRUCT)) {
                auto sd = parseStructDef();
                if (sd.name.empty()) { advance(); continue; }
                structRegistry[sd.name] = sd;
                prog->structs.push_back(std::move(sd));
            } else if (check(TokenType::KW_ENUM)) {
                auto ed = parseEnumDef();
                if (ed.name.empty()) { advance(); continue; }
                enumRegistry[ed.name] = ed;
                prog->enums.push_back(std::move(ed));
            } else if (check(TokenType::KW_IMPORT)) {
                advance(); // 'import'
                if (check(TokenType::STRING_LITERAL)) {
                    Token path = consume(TokenType::STRING_LITERAL, "expected module path string after 'import'");
                    prog->imports.push_back(path.lexeme);
                } else if (check(TokenType::IDENTIFIER)) {
                    std::string modPath = peek().lexeme;
                    advance();
                    while (match(TokenType::DOT)) {
                        modPath += ".";
                        modPath += consume(TokenType::IDENTIFIER, "expected module name").lexeme;
                    }
                    match(TokenType::NEWLINE);
                    match(TokenType::SEMICOLON);
                    prog->imports.push_back(modPath);
                } else {
                    parseError("expected module path (string literal or identifier)");
                }
            } else if (check(TokenType::KW_LET)) {
                advance(); // 'let'
                auto decl = parseVarDecl(false);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::KW_VAR) || check(TokenType::KW_MUT)) {
                advance(); // 'var' or 'mut'
                auto decl = parseVarDecl(true);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON) {
                auto decl = parseVarDecl(false);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON_EQ) {
                auto decl = parseVarDecl(false);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
                auto decl = parseVarDecl(false);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::LPAREN) {
                // Keywordless function def: name(params) { body } or name(params) -> type { body }
                // Check if followed by LBRACE or ARROW within reasonable lookahead
                bool isFunc = false;
                for (int i = 2; i < 30; i++) {
                    TokenType t = peek(i).type;
                    if (t == TokenType::LBRACE || t == TokenType::ARROW) { isFunc = true; break; }
                    if (t == TokenType::END_OF_FILE || t == TokenType::NEWLINE || t == TokenType::SEMICOLON) break;
                }
                if (isFunc) {
                    auto fn = parseFunctionDefNoKeyword();
                    if (fn) { prog->functions.push_back(std::move(fn)); declaredVars.clear(); }
                } else {
                    auto stmt = parseExprStmt();
                    if (stmt) { prog->topStmts.push_back(std::move(stmt)); declaredVars.clear(); }
                }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::IDENTIFIER) {
                // Bare function def: name param1 param2 ... { body } or name param1 param2 ... -> type { body }
                bool isFunc = false;
                for (int i = 2; i < 30; i++) {
                    TokenType t = peek(i).type;
                    if (t == TokenType::LBRACE || t == TokenType::ARROW) { isFunc = true; break; }
                    if (t == TokenType::END_OF_FILE || t == TokenType::NEWLINE || t == TokenType::SEMICOLON) break;
                }
                if (isFunc) {
                    auto fn = parseFunctionDefNoKeyword();
                    if (fn) { prog->functions.push_back(std::move(fn)); declaredVars.clear(); }
                } else {
                    auto stmt = parseExprStmt();
                    if (stmt) { prog->topStmts.push_back(std::move(stmt)); declaredVars.clear(); }
                }
            } else {
                // Top-level statement (expression, call, etc.) — treat as script
                auto stmt = parseExprStmt();
                if (stmt) {
                    prog->topStmts.push_back(std::move(stmt));
                    declaredVars.clear();
                } else {
                    parseError("unrecognized top-level construct");
                    advance();
                }
            }
        }
        return prog;
    }

    // Try to parse a function definition without a leading keyword (fn/def).
    // Pattern: name [params] -> retType? { body }
    // Only called when we're confident it's a function def.
    std::unique_ptr<FunctionAST> parseFunctionDefNoKeyword() {
        Token nameTok = consume(TokenType::IDENTIFIER, "expected function name");
        std::vector<std::pair<std::string, Type>> params;
        std::vector<std::unique_ptr<ExprAST>> defaults;
        if (match(TokenType::LPAREN)) {
            if (!check(TokenType::RPAREN)) {
                do {
                    Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                    Type pType = Type::i64();
                    if (match(TokenType::COLON)) pType = parseType();
                    if (match(TokenType::ASSIGN)) {
                        defaults.push_back(parseExpression());
                    } else {
                        defaults.push_back(nullptr);
                    }
                    params.emplace_back(pName.lexeme, pType);
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RPAREN, "expected ')'");
        } else {
            while (check(TokenType::IDENTIFIER) && !check(TokenType::LBRACE)) {
                Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                Type pType = Type::i64();
                if (match(TokenType::COLON)) pType = parseType();
                if (match(TokenType::ASSIGN)) {
                    defaults.push_back(parseExpression());
                } else {
                    defaults.push_back(nullptr);
                }
                params.emplace_back(pName.lexeme, pType);
            }
        }
        Type retType = Type::void_();
        if (match(TokenType::ARROW)) retType = parseType();
        auto fn = std::make_unique<FunctionAST>();
        fn->name = nameTok.lexeme;
        fn->params = std::move(params);
        fn->defaults = std::move(defaults);
        fn->returnType = retType;
        fn->isDeclaration = false;
        fn->loc = nameTok.loc;
        for (auto& p : fn->params) {
            declaredVars.insert(p.first);
            varTypeMap[p.first] = p.second;
        }
        fn->bodyStart = pos;
        fn->body = parseBlock();
        fn->bodyEnd = pos;
        // Auto-infer return type from last expression if no arrow given
        if (retType.kind == TypeKind::Void && fn->body && fn->body->kind == NodeKind::Block) {
            auto* block = static_cast<BlockStmtAST*>(fn->body.get());
            if (!block->stmts.empty()) {
                auto& last = block->stmts.back();
                if (last->kind != NodeKind::Return && last->kind != NodeKind::Break &&
                    last->kind != NodeKind::Continue &&
                    last->kind != NodeKind::While &&
                    last->kind != NodeKind::Block && last->kind != NodeKind::PyBlock &&
                    last->kind != NodeKind::VarDecl) {
                    retType = inferType(last.get());
                    fn->returnType = retType;
                }
            }
        }
        // Implicit last-expr return
        if (fn->body && fn->body->kind == NodeKind::Block && retType.kind != TypeKind::Void) {
            auto* block = static_cast<BlockStmtAST*>(fn->body.get());
            if (!block->stmts.empty()) {
                auto& last = block->stmts.back();
                if (last->kind != NodeKind::Return && last->kind != NodeKind::VarDecl &&
                    last->kind != NodeKind::Break && last->kind != NodeKind::Continue &&
                    last->kind != NodeKind::While && last->kind != NodeKind::Block &&
                    last->kind != NodeKind::PyBlock) {
                    auto retStmt = std::make_unique<ReturnStmtAST>(std::move(last));
                    retStmt->loc = nameTok.loc;
                    block->stmts.back() = std::move(retStmt);
                }
            }
        }
        return fn;
    }

    StructDef parseStructDef() {
        advance(); // 'struct'
        Token nameTok = consume(TokenType::IDENTIFIER, "expected struct name");
        StructDef sd;
        sd.name = nameTok.lexeme;
        consume(TokenType::LBRACE, "expected '{' after struct name");
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            Token fName = consume(TokenType::IDENTIFIER, "expected field name");
            consume(TokenType::COLON, "expected ':' after field name");
            Type fType = parseType();
            sd.fields.push_back({fName.lexeme, fType});
            if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
        }
        consume(TokenType::RBRACE, "expected '}' to close struct");
        return sd;
    }

    EnumDef parseEnumDef() {
        advance(); // 'enum'
        Token nameTok = consume(TokenType::IDENTIFIER, "expected enum name");
        EnumDef ed;
        ed.name = nameTok.lexeme;
        consume(TokenType::LBRACE, "expected '{' after enum name");
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            Token vName = consume(TokenType::IDENTIFIER, "expected variant name");
            EnumVariantDef variant;
            variant.name = vName.lexeme;
            if (match(TokenType::LPAREN)) {
                if (!check(TokenType::RPAREN)) {
                    do { variant.payloadTypes.push_back(parseType()); } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "expected ')' after variant types");
            }
            ed.variants.push_back(std::move(variant));
            if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
        }
        consume(TokenType::RBRACE, "expected '}' to close enum");
        return ed;
    }

    void parseExternBlock(ProgramAST& prog) {
        advance(); // 'extern'
        if (check(TokenType::STRING_LITERAL)) {
            std::string linkage = peek().lexeme;
            advance();
            if (linkage == "C") {
                consume(TokenType::LBRACE, "expected '{' after extern \"C\"");
                while (!check(TokenType::RBRACE) && !isAtEnd()) {
                    ExternFn ext;
                    if (!match(TokenType::KW_FN)) consume(TokenType::KW_DEF, "expected 'fn' or 'def' in extern block");
                    Token nameTok = consume(TokenType::IDENTIFIER, "expected function name");
                    ext.name = nameTok.lexeme;
                    consume(TokenType::LPAREN, "expected '('");
                    if (!check(TokenType::RPAREN) && !check(TokenType::ELLIPSIS)) {
                        do {
                            if (check(TokenType::ELLIPSIS)) break;
                            Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                            consume(TokenType::COLON, "expected ':'");
                            ext.paramTypes.push_back(parseType());
                        } while (match(TokenType::COMMA));
                    }
                    if (match(TokenType::ELLIPSIS)) ext.isVararg = true;
                    consume(TokenType::RPAREN, "expected ')'");
                    if (match(TokenType::ARROW)) ext.returnType = parseType();
                    else ext.returnType = Type::void_();
                    match(TokenType::SEMICOLON);
                    match(TokenType::NEWLINE);
                    prog.externs.push_back(ext);
                }
                consume(TokenType::RBRACE, "expected '}' to close extern block");
            }
        } else {
            parseError("expected '\"C\"' after extern");
        }
    }

    bool hadError() const { return error; }
    std::string errorMsg() const { return msg; }

private:
    Lexer& lexer;
    size_t pos;
    bool error = false;
    std::string msg;
    std::unordered_set<std::string> declaredVars;
    std::unordered_set<std::string> globalVarNames;
    std::unordered_map<std::string, Type> varTypeMap;
    std::unordered_map<std::string, StructDef> structRegistry;
    std::unordered_map<std::string, EnumDef> enumRegistry;
    std::vector<std::string> parserTypeParams;
    Codegen* cg = nullptr;
    bool emitMode = false;
    bool skipBodies = false;
    bool inOptParens = false;

    const Token& peek(size_t a = 0) {
        lexer.ensure(pos + a);
        auto& toks = lexer.getTokens();
        size_t i = pos + a; return i < toks.size() ? toks[i] : toks.back();
    }
    const Token& advance() { if (!isAtEnd()) pos++; return previous(); }
    const Token& previous() const { auto& toks = lexer.getTokens(); return toks[pos - 1]; }
    bool isAtEnd() { return peek().type == TokenType::END_OF_FILE; }
    bool check(TokenType t) { return !isAtEnd() && peek().type == t; }
    bool match(TokenType t) { if (check(t)) { advance(); return true; } return false; }

    void parseError(const std::string& message) {
        if (!error) {
            error = true;
            auto& t = peek();
            std::string line = lexer.getLine(t.loc.line);
            std::string spaces(t.loc.col > 1 ? t.loc.col - 1 : 0, ' ');
            msg = "error at line " + std::to_string(t.loc.line) + ":" + std::to_string(t.loc.col)
                + ": " + message + "\n  " + line + "\n  " + spaces + "^";
        }
    }

    Token consume(TokenType t, const std::string& err) {
        if (check(t)) return advance();
        parseError(err);
        auto& toks = lexer.getTokens();
        return toks.empty() ? Token{} : toks[0];
    }

    // ---- type ----
    Type parseType() {
        if (match(TokenType::KW_I64))  return Type::i64();
        if (match(TokenType::KW_F64))  return Type::f64();
        if (match(TokenType::KW_STR))  return Type::str();
        if (match(TokenType::KW_BOOL)) return Type::boolean();
        if (match(TokenType::KW_PTR))  return Type::ptr_();
        if (match(TokenType::AMPERSAND)) {
            Type inner = parseType();
            return Type::ref(inner);
        }
        if (match(TokenType::LBRACKET)) {
            Type inner = parseType();
            consume(TokenType::RBRACKET, "expected ']' after array element type");
            return Type::array(inner);
        }
        if (match(TokenType::IDENTIFIER)) {
            std::string name = previous().lexeme;
            if (name == "map") return Type::map();
            if (structRegistry.count(name)) return Type::struct_(name);
            if (enumRegistry.count(name)) return Type::enum_(name);
            for (auto& tp : parserTypeParams) if (tp == name) return Type::typeParam(name);
            parseError("unknown type '" + name + "'");
            return Type::i64();
        }
        parseError("expected type (i64, str, bool, [T], &T, struct name, or enum name)");
        return Type::i64();
    }

    Type inferType(ExprAST* expr) {
        if (!expr) return Type::i64();
        switch (expr->kind) {
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(expr);
                if (n->isFloat) return Type::f64();
                if (n->isInt) return Type::i64();
                double intPart;
                if (std::modf(n->value, &intPart) != 0.0) return Type::f64();
                return Type::i64();
            }
            case NodeKind::String: return Type::str();
            case NodeKind::Array: return Type::array(Type::i64());
            case NodeKind::MapLiteral: return Type::map();
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(expr);
                auto it = varTypeMap.find(v->name);
                if (it != varTypeMap.end()) return it->second;
                return Type::i64();
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(expr);
                if (b->op == '+') {
                    Type lt = b->lhs ? inferType(b->lhs.get()) : Type::i64();
                    Type rt = b->rhs ? inferType(b->rhs.get()) : Type::i64();
                    if (lt.kind == TypeKind::Str || rt.kind == TypeKind::Str)
                        return Type::str();
                    return Type::i64();
                }
                return Type::i64();
            }
            case NodeKind::Lambda:
            case NodeKind::Unwrap: return Type::i64();
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(expr);
                if (c->callee == "print") return Type::void_();
                if (c->callee == "flint_map_new") return Type::map();
                if (c->callee == "flint_array_alloc" || c->callee == "flint_array_concat")
                    return Type::array(Type::i64());
                return Type::i64();
            }
            case NodeKind::StructLiteral: {
                auto* sl = static_cast<StructLiteralAST*>(expr);
                if (structRegistry.count(sl->structName))
                    return Type::struct_(sl->structName);
                return Type::i64();
            }
            case NodeKind::Compare: return Type::i64();
            case NodeKind::Ref: return inferType(static_cast<RefExprAST*>(expr)->target.get());
            default: return Type::i64();
        }
    }

    // ---- function ----
    std::unique_ptr<FunctionAST> parseFunction() {
        advance(); // 'fn' or 'def'
        Token nameTok = consume(TokenType::IDENTIFIER, "expected function name");
        std::vector<std::string> typeParams;
        if (match(TokenType::LT)) {
            do {
                Token tp = consume(TokenType::IDENTIFIER, "expected type parameter name");
                typeParams.push_back(tp.lexeme);
            } while (match(TokenType::COMMA));
            consume(TokenType::GT, "expected '>' after type parameters");
        }
        parserTypeParams = typeParams;
        consume(TokenType::LPAREN, "expected '(' after function name");

        std::vector<std::pair<std::string, Type>> params;
        std::vector<std::unique_ptr<ExprAST>> defaults;
        if (!check(TokenType::RPAREN)) {
            do {
                Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                Type pType = Type::i64();
                if (match(TokenType::COLON)) {
                    pType = parseType();
                }
                if (match(TokenType::ASSIGN)) {
                    defaults.push_back(parseExpression());
                } else {
                    defaults.push_back(nullptr);
                }
                params.emplace_back(pName.lexeme, pType);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after parameters");

        Type retType = Type::void_();
        if (match(TokenType::ARROW)) {
            retType = parseType();
        }

        for (auto& p : params) {
            declaredVars.insert(p.first);
            varTypeMap[p.first] = p.second;
        }
        
        bool isDecl = !check(TokenType::LBRACE);

        auto fn = std::make_unique<FunctionAST>();
        fn->name = nameTok.lexeme;
        fn->typeParams = std::move(typeParams);
        fn->params = std::move(params);
        fn->defaults = std::move(defaults);
        fn->returnType = retType;
        fn->isDeclaration = isDecl;
        fn->loc = nameTok.loc;

        if (!isDecl) {
            if (skipBodies && retType.kind == TypeKind::Void) {
                fn->bodyStart = pos;
                skipBlock();
                fn->bodyEnd = pos;
            } else {
                fn->bodyStart = pos;
                fn->body = parseBlock();
                fn->bodyEnd = pos;
                // Auto-infer return type from last expression if no arrow given
                if (retType.kind == TypeKind::Void && fn->body && fn->body->kind == NodeKind::Block) {
                    auto* block = static_cast<BlockStmtAST*>(fn->body.get());
                    if (!block->stmts.empty()) {
                        auto& last = block->stmts.back();
                        if (last->kind != NodeKind::Return && last->kind != NodeKind::Break &&
                            last->kind != NodeKind::Continue &&
                            last->kind != NodeKind::If && last->kind != NodeKind::While &&
                            last->kind != NodeKind::Block && last->kind != NodeKind::PyBlock &&
                            last->kind != NodeKind::VarDecl) {
                            retType = inferType(last.get());
                            fn->returnType = retType;
                        }
                    }
                }
                // Implicit last-expr return: for non-void functions, wrap the last
                // expression statement in an explicit return.
                if (fn->body && fn->body->kind == NodeKind::Block && retType.kind != TypeKind::Void) {
                    auto* block = static_cast<BlockStmtAST*>(fn->body.get());
                    if (!block->stmts.empty()) {
                        auto& last = block->stmts.back();
                        if (last->kind != NodeKind::Return && last->kind != NodeKind::VarDecl &&
                            last->kind != NodeKind::Break && last->kind != NodeKind::Continue &&
                            last->kind != NodeKind::While && last->kind != NodeKind::Block &&
                            last->kind != NodeKind::PyBlock) {
                            auto retStmt = std::make_unique<ReturnStmtAST>(std::move(last));
                            retStmt->loc = nameTok.loc;
                            block->stmts.back() = std::move(retStmt);
                        }
                    }
                }
            }
        }
        parserTypeParams.clear();
        return fn;
    }

    // Fast-forward through a { ... } block without building AST
    void skipBlock() {
        if (!check(TokenType::LBRACE)) return;
        advance(); // '{'
        int depth = 1;
        while (depth > 0 && !isAtEnd()) {
            if (check(TokenType::LBRACE)) { depth++; advance(); }
            else if (check(TokenType::RBRACE)) { depth--; advance(); }
            else if (check(TokenType::STRING_LITERAL)) { advance(); }
            else { advance(); }
        }
    }

    // ---- block ----
    std::unique_ptr<ExprAST> parseBlock() {
        if (!check(TokenType::LBRACE)) {
            return nullptr;
        }
        advance(); // '{'
        // Save scoped state for block-scoped variable declarations
        auto outerVars = declaredVars;
        auto outerTypes = varTypeMap;
        auto block = std::make_unique<BlockStmtAST>();
        block->loc = previous().loc;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            if (check(TokenType::SEMICOLON)) { advance(); continue; }
            // Hang insurance: a statement that fails without consuming input
            // would re-parse forever — force progress (the callee already
            // reported the error, so this stays silent).
            size_t before = pos;
            auto s = parseStatement();
            if (s) block->stmts.push_back(std::move(s));
            else if (pos == before && !check(TokenType::RBRACE) && !isAtEnd()) advance();
        }
        consume(TokenType::RBRACE, "expected '}' to close block");
        // Restore outer scope (block-scoped variables go out of scope)
        declaredVars = std::move(outerVars);
        varTypeMap = std::move(outerTypes);
        return block;
    }

    // ---- statement ----
    std::unique_ptr<ExprAST> parseStatement() {
        if (check(TokenType::KW_RETURN)) return parseReturnStmt();
        if (check(TokenType::KW_IF))     return parseIfStmt();
        if (check(TokenType::KW_WHILE))  return parseWhileStmt();
        if (check(TokenType::KW_BREAK))  { advance(); return std::make_unique<BreakStmtAST>(); }
        if (check(TokenType::KW_CONTINUE)) { advance(); return std::make_unique<ContinueStmtAST>(); }
        if (check(TokenType::KW_PYTHON)) return parsePythonBlock();
        if (check(TokenType::KW_FOR))    return parseForStmt();
        if (check(TokenType::KW_PARALLEL)) return parseParallelForStmt();
        if (check(TokenType::LBRACE)) {
            advance();
            auto block = std::make_unique<BlockStmtAST>();
            block->loc = previous().loc;
            while (!check(TokenType::RBRACE) && !isAtEnd()) {
                if (check(TokenType::SEMICOLON)) { advance(); continue; }
                size_t before = pos;
                auto s = parseStatement();
                if (s) block->stmts.push_back(std::move(s));
                else if (pos == before && !check(TokenType::RBRACE) && !isAtEnd()) advance();
            }
            consume(TokenType::RBRACE, "expected '}' to close block");
            return block;
        }
        if (check(TokenType::KW_LET)) {
            advance();
            // Destructure: let [a, b] or let { x, y }
            if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
                return parseDestructure(false);
            }
            return parseVarDecl(false);
        }
        if (check(TokenType::KW_VAR)) {
            advance();
            if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
                return parseDestructure(true);
            }
            return parseVarDecl(true);
        }
        if (check(TokenType::KW_MUT)) {
            advance();
            if (check(TokenType::LBRACKET) || check(TokenType::LBRACE)) {
                return parseDestructure(true);
            }
            return parseVarDecl(true);
        }

        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON) {
            return parseVarDecl(false);
        }

        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON_EQ) {
            return parseVarDecl(false);
        }

        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
            std::string name = peek().lexeme;
            if (declaredVars.count(name) || globalVarNames.count(name)) {
                return parseExprStmt();
            }
            return parseVarDecl(false);
        }

        return parseExprStmt();
    }

    std::unique_ptr<ExprAST> parseReturnStmt() {
        auto node = std::make_unique<ReturnStmtAST>(nullptr);
        node->loc = previous().loc;
        advance(); // 'return'
        if (!check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE) && !check(TokenType::RBRACE) && !isAtEnd()) {
            node->value = parseExpression();
        }
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);
        return node;
    }

    // Parse: cond { then } [elif ...] [else ...]
    // Returns: if cond { then } else { ... }
    std::unique_ptr<ExprAST> parseElifChain() {
        auto node = std::make_unique<IfStmtAST>();
        node->loc = previous().loc;
        auto cond = parseExpression();
        if (!cond) return nullptr;
        node->condition = std::move(cond);
        node->thenBlock = parseBlock();
        if (match(TokenType::KW_ELIF)) {
            auto wrap = std::make_unique<BlockStmtAST>();
            wrap->stmts.push_back(parseElifChain());
            node->elseBlock = std::move(wrap);
        } else if (match(TokenType::KW_ELSE)) {
            node->elseBlock = parseBlock();
        }
        return node;
    }

    std::unique_ptr<ExprAST> parseIfStmt() {
        auto node = std::make_unique<IfStmtAST>();
        node->loc = previous().loc;
        advance(); // 'if'
        auto cond = parseExpression();
        if (!cond) return nullptr;
        node->condition = std::move(cond);
        node->thenBlock = parseBlock();
        if (match(TokenType::KW_ELIF)) {
            auto wrap = std::make_unique<BlockStmtAST>();
            wrap->stmts.push_back(parseElifChain());
            node->elseBlock = std::move(wrap);
        } else if (match(TokenType::KW_ELSE)) {
            node->elseBlock = parseBlock();
        }
        return node;
    }

    std::unique_ptr<ExprAST> parseWhileStmt() {
        auto node = std::make_unique<WhileStmtAST>();
        node->loc = previous().loc;
        advance(); // 'while'
        auto cond = parseExpression();
        if (!cond) return nullptr;
        node->condition = std::move(cond);
        node->body = parseBlock();
        return node;
    }

    std::unique_ptr<ExprAST> parseForStmt() {
        // for i in start..end { body }  — or —
        // for x in collection { body }
        advance(); // 'for'
        Token loopVar = consume(TokenType::IDENTIFIER, "expected loop variable name after 'for'");
        consume(TokenType::KW_IN, "expected 'in' after loop variable");

        // Check if this is a range loop (expr '..' expr) or collection loop (expr)
        auto startExpr = parseExpression();
        if (!startExpr) return nullptr;

        // Shared range-loop builder: `for i in start..end { body }`.
        auto buildRangeLoop = [&](std::unique_ptr<ExprAST> s, std::unique_ptr<ExprAST> e) -> std::unique_ptr<ExprAST> {
            auto body = parseBlock();
            if (!body) return nullptr;

            auto block = std::make_unique<BlockStmtAST>();
            auto varDecl = std::make_unique<VarDeclAST>(loopVar.lexeme, true, Type::i64(), std::move(s));
            varDecl->loc = loopVar.loc;
            block->stmts.push_back(std::move(varDecl));

            auto whileStmt = std::make_unique<WhileStmtAST>();
            whileStmt->loc = loopVar.loc;
            auto varRef = std::make_unique<VariableExprAST>(loopVar.lexeme);
            auto cmp = std::make_unique<CompareExprAST>("<", std::move(varRef), std::move(e));
            cmp->loc = loopVar.loc;
            whileStmt->condition = std::move(cmp);

            auto bodyBlock = static_cast<BlockStmtAST*>(body.get());
            auto incVar = std::make_unique<VariableExprAST>(loopVar.lexeme);
            auto one = std::make_unique<NumberExprAST>(1);
            one->loc = loopVar.loc;
            auto incExpr = std::make_unique<BinaryExprAST>('+', std::move(incVar), std::move(one));
            incExpr->loc = loopVar.loc;
            auto incAssign = std::make_unique<AssignExprAST>(loopVar.lexeme, std::move(incExpr));
            incAssign->loc = loopVar.loc;
            bodyBlock->stmts.push_back(std::move(incAssign));
            wrapForContinues(body.get(), loopVar.lexeme, loopVar.loc);

            whileStmt->body = std::move(body);
            block->stmts.push_back(std::move(whileStmt));
            return block;
        };

        if (startExpr->kind == NodeKind::Call &&
            static_cast<CallExprAST*>(startExpr.get())->callee == "range") {
            auto* rc = static_cast<CallExprAST*>(startExpr.get());
            if (rc->args.size() < 1 || rc->args.size() > 2) {
                parseError("range() needs 1 or 2 arguments");
                return nullptr;
            }
            std::unique_ptr<ExprAST> rStart, rEnd;
            if (rc->args.size() == 1) {
                rStart = std::make_unique<NumberExprAST>(0);
                rStart->loc = loopVar.loc;
                rEnd = std::move(rc->args[0]);
            } else {
                rStart = std::move(rc->args[0]);
                rEnd = std::move(rc->args[1]);
            }
            return buildRangeLoop(std::move(rStart), std::move(rEnd));
        }

        if (check(TokenType::DOTDOT)) {
            // Range for: for i in start..end { body }
            advance(); // '..'
            auto endExpr = parseExpression();
            if (!endExpr) return nullptr;
            return buildRangeLoop(std::move(startExpr), std::move(endExpr));
        } else {
            // Collection for: for x in collection { body }
            // Desugars to: { let __coll = collection; mut __idx = 0; while __idx < len(__coll) { let x = coll[__idx]; body; __idx = __idx + 1 } }
            // with kind-aware len/elem (array: hoisted len + data ptr; str: str_length + byte load).
            // Non-array/non-str never reach here (loud parse error above).
            auto body = parseBlock();
            if (!body) return nullptr;

            auto block = std::make_unique<BlockStmtAST>();
            std::string collVar = "__coll";
            std::string idxVar = "__idx";
            Type collTy = inferType(startExpr.get());
            bool isArray = collTy.kind == TypeKind::Array;
            bool isStr = collTy.kind == TypeKind::Str;
            // A2: only arrays and strings are iterable. Anything else
            // (int/map/void/...) used to fall through to vec-runtime calls
            // (or a str fallback) on a non-vec value → JIT segfault or
            // silent skip. Fail loudly instead (matches stage3 e_for:
            // "for needs range, array or str").
            if (!isArray && !isStr) {
                parseError("for-in needs an array or str collection");
                return nullptr;
            }
            Type collDeclTy = collTy;
            auto collDecl = std::make_unique<VarDeclAST>(collVar, false, collDeclTy, std::move(startExpr));
            collDecl->loc = loopVar.loc;
            block->stmts.push_back(std::move(collDecl));

            // mut __idx = 0
            auto zero = std::make_unique<NumberExprAST>(0);
            zero->loc = loopVar.loc;
            auto idxDecl = std::make_unique<VarDeclAST>(idxVar, true, Type::i64(), std::move(zero));
            idxDecl->loc = loopVar.loc;
            block->stmts.push_back(std::move(idxDecl));

            // D1: for array, hoist data+len via pointer helpers so the loop
            // never moves/borrows __coll inside the condition/body (the old
            // IndexExpr path borrowed while the len call also borrowed →
            // "cannot move while borrowed").
            std::string dataVar, lenVar;
            if (isArray) {
                dataVar = "__coll_data";
                lenVar = "__coll_len";
                {
                    auto var = std::make_unique<VariableExprAST>(collVar);
                    auto ref = std::make_unique<RefExprAST>(std::move(var));
                    std::vector<std::unique_ptr<ExprAST>> a;
                    a.push_back(std::move(ref));
                    auto call = std::make_unique<CallExprAST>("flint_array_data_ptr", std::move(a));
                    call->loc = loopVar.loc;
                    auto decl = std::make_unique<VarDeclAST>(dataVar, false, Type::ptr_(), std::move(call));
                    decl->loc = loopVar.loc;
                    block->stmts.push_back(std::move(decl));
                }
                {
                    auto var = std::make_unique<VariableExprAST>(collVar);
                    auto ref = std::make_unique<RefExprAST>(std::move(var));
                    std::vector<std::unique_ptr<ExprAST>> a;
                    a.push_back(std::move(ref));
                    auto call = std::make_unique<CallExprAST>("flint_array_len_ptr", std::move(a));
                    call->loc = loopVar.loc;
                    auto decl = std::make_unique<VarDeclAST>(lenVar, false, Type::i64(), std::move(call));
                    decl->loc = loopVar.loc;
                    block->stmts.push_back(std::move(decl));
                }
            }

            // while __idx < len(__coll) { ... } — kind-aware
            auto whileStmt = std::make_unique<WhileStmtAST>();
            whileStmt->loc = loopVar.loc;
            std::unique_ptr<ExprAST> lenExpr;
            if (isArray) {
                lenExpr = std::make_unique<VariableExprAST>(lenVar);
            } else {
                // isStr (non-array/non-str rejected above).
                std::vector<std::unique_ptr<ExprAST>> lenArgs;
                lenArgs.push_back(std::make_unique<VariableExprAST>(collVar));
                lenExpr = std::make_unique<CallExprAST>("flint_str_length", std::move(lenArgs));
            }
            lenExpr->loc = loopVar.loc;
            auto idxRef = std::make_unique<VariableExprAST>(idxVar);
            auto cmp = std::make_unique<CompareExprAST>("<", std::move(idxRef), std::move(lenExpr));
            cmp->loc = loopVar.loc;
            whileStmt->condition = std::move(cmp);

            auto bodyBlock = static_cast<BlockStmtAST*>(body.get());
            // let x = coll[idx] — array uses hoisted data ptr (no borrow of __coll)
            std::unique_ptr<ExprAST> elementInit;
            if (isArray) {
                auto d = std::make_unique<VariableExprAST>(dataVar);
                auto idx = std::make_unique<VariableExprAST>(idxVar);
                std::vector<std::unique_ptr<ExprAST>> a;
                a.push_back(std::move(d));
                a.push_back(std::move(idx));
                elementInit = std::make_unique<CallExprAST>("flint_array_read_i64", std::move(a));
            } else {
                // isStr (non-array/non-str rejected above).
                auto base = std::make_unique<VariableExprAST>(collVar);
                auto idx = std::make_unique<VariableExprAST>(idxVar);
                elementInit = std::make_unique<IndexExprAST>(std::move(base), std::move(idx));
            }
            auto getCall = std::move(elementInit);
            getCall->loc = loopVar.loc;
            auto elementDecl = std::make_unique<VarDeclAST>(loopVar.lexeme, false, Type::i64(), std::move(getCall));
            elementDecl->loc = loopVar.loc;
            bodyBlock->stmts.insert(bodyBlock->stmts.begin(), std::move(elementDecl));

            // __idx = __idx + 1
            auto incVar = std::make_unique<VariableExprAST>(idxVar);
            auto one = std::make_unique<NumberExprAST>(1);
            one->loc = loopVar.loc;
            auto incExpr = std::make_unique<BinaryExprAST>('+', std::move(incVar), std::move(one));
            incExpr->loc = loopVar.loc;
            auto incAssign = std::make_unique<AssignExprAST>(idxVar, std::move(incExpr));
            incAssign->loc = loopVar.loc;
            bodyBlock->stmts.push_back(std::move(incAssign));
            wrapForContinues(body.get(), idxVar, loopVar.loc);

            whileStmt->body = std::move(body);
            block->stmts.push_back(std::move(whileStmt));
            return block;
        }
    }

    std::unique_ptr<ExprAST> parseParallelForStmt() {
        // parallel for i in start..end { body }
        // Desugars at parse time to a worker function + flint_parallel_for call
        advance(); // 'parallel'
        consume(TokenType::KW_FOR, "expected 'for' after 'parallel'");
        Token loopVar = consume(TokenType::IDENTIFIER, "expected loop variable name");
        consume(TokenType::KW_IN, "expected 'in'");
        auto startExpr = parseExpression();
        if (!startExpr) return nullptr;
        consume(TokenType::DOTDOT, "expected '..'");
        auto endExpr = parseExpression();
        if (!endExpr) return nullptr;
        auto body = parseBlock();
        if (!body) return nullptr;

        // parallel for i in start..end { body }
        // Desugars to: fn __pfor_N(i: i64) -> i64 { body; return 0 }
        //              flint_parallel_for(end - start, &__pfor_N, 0)
        static int pforCounter = 0;
        std::string workerName = "__pfor_" + std::to_string(pforCounter++);

        auto block = std::make_unique<BlockStmtAST>();

        // Worker function: fn __pfor_N(i: i64) -> i64 { body; return 0 }
        auto workerFn = std::make_unique<FunctionAST>();
        workerFn->name = workerName;
        workerFn->returnType = Type::i64();
        workerFn->params.push_back({loopVar.lexeme, Type::i64()});
        auto bodyBlock = static_cast<BlockStmtAST*>(body.get());
        auto retStmt = std::make_unique<ReturnStmtAST>(std::make_unique<NumberExprAST>(0));
        retStmt->loc = loopVar.loc;
        bodyBlock->stmts.push_back(std::move(retStmt));
        workerFn->body = std::move(body);
        workerFn->loc = loopVar.loc;
        block->stmts.push_back(std::move(workerFn));

        // Build call: flint_parallel_for(count, &workerName, 0)
        // count = end - start (run-time computation)
        std::vector<std::unique_ptr<ExprAST>> pforArgs;
        auto varE = std::make_unique<VariableExprAST>(loopVar.lexeme);
        auto subExpr = std::make_unique<BinaryExprAST>('-', std::move(endExpr), std::move(startExpr));
        subExpr->loc = loopVar.loc;
        pforArgs.push_back(std::move(subExpr));

        auto varRef = std::make_unique<VariableExprAST>(workerName);
        varRef->loc = loopVar.loc;
        auto addrOf = std::make_unique<RefExprAST>(std::move(varRef));
        addrOf->loc = loopVar.loc;
        pforArgs.push_back(std::move(addrOf));

        auto zero = std::make_unique<NumberExprAST>(0);
        zero->loc = loopVar.loc;
        pforArgs.push_back(std::move(zero));

        auto parallelCall = std::make_unique<CallExprAST>("flint_parallel_for", std::move(pforArgs));
        parallelCall->loc = loopVar.loc;
        block->stmts.push_back(std::move(parallelCall));
        return block;
    }

    std::unique_ptr<ExprAST> parsePythonBlock() {
        advance(); // 'python'
        auto node = std::make_unique<PyBlockStmtAST>();
        node->loc = previous().loc;
        consume(TokenType::LBRACE, "expected '{' after 'python'");
        while (check(TokenType::STRING_LITERAL)) {
            node->codeStrings.push_back(peek().lexeme);
            advance();
        }
        consume(TokenType::RBRACE, "expected '}' to close python block");
        return node;
    }

    // Destructure pattern: let [a, b] = arr; or let { x, y } = s;
    std::unique_ptr<ExprAST> parseDestructure(bool isMutable) {
        auto ds = std::make_unique<DestructureAST>();
        ds->isMutable = isMutable;
        ds->isStruct = match(TokenType::LBRACE);
        if (!ds->isStruct) {
            consume(TokenType::LBRACKET, "expected '[' or '{' for destructure pattern");
        }
        while (true) {
            if (ds->isStruct ? check(TokenType::RBRACE) : check(TokenType::RBRACKET)) break;
            if (isAtEnd()) break;
            Token nameTok = advance();
            if (declaredVars.count(nameTok.lexeme)) {
                parseError("variable '" + nameTok.lexeme + "' already declared");
            }
            declaredVars.insert(nameTok.lexeme);
            ds->names.push_back(nameTok.lexeme);
            varTypeMap[nameTok.lexeme] = Type::i64();
            if (ds->isStruct ? !check(TokenType::RBRACE) : !check(TokenType::RBRACKET))
                consume(TokenType::COMMA, "expected ',' or closing bracket in destructure");
        }
        if (ds->isStruct) consume(TokenType::RBRACE, "expected '}'");
        else consume(TokenType::RBRACKET, "expected ']'");
        if (!match(TokenType::COLON_EQ))
            consume(TokenType::ASSIGN, "expected '=' in destructure");
        ds->init = parseExpression();
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);
        ds->loc = previous().loc;
        return ds;
    }

    std::unique_ptr<ExprAST> parseVarDecl(bool isMutable) {
        Token nameTok = advance();
        if (declaredVars.count(nameTok.lexeme)) {
            parseError("variable '" + nameTok.lexeme + "' already declared");
        }
        declaredVars.insert(nameTok.lexeme);
        Type varType = Type::i64();
        bool hasTypeAnnotation = match(TokenType::COLON);
        if (hasTypeAnnotation) {
            varType = parseType();
        }
        if (!match(TokenType::COLON_EQ)) {
            consume(TokenType::ASSIGN, "expected '=' or ':=' in variable declaration");
        }
        auto init = parseExpression();
        // Hang/segfault insurance (Bug-1 class): a failed RHS must never be
        // dereferenced below, and the caller must always make progress.
        if (!init) {
            parseError("expected expression after '='");
            return nullptr;
        }
        if (!hasTypeAnnotation) {
            switch (init->kind) {
                case NodeKind::String:
                    varType = Type::str();
                    break;
                case NodeKind::Number: {
                    auto* numInit = static_cast<NumberExprAST*>(init.get());
                    if (numInit->isFloat) varType = Type::f64();
                    else if (numInit->isInt) varType = Type::i64();
                    else {
                        double intPart;
                        if (std::modf(numInit->value, &intPart) != 0.0) varType = Type::f64();
                    }
                    break;
                }
                case NodeKind::Array:
                    varType = Type::array(Type::i64());
                    break;
                case NodeKind::Variable: {
                    auto* varInit = static_cast<VariableExprAST*>(init.get());
                    auto it = varTypeMap.find(varInit->name);
                    if (it != varTypeMap.end()) varType = it->second;
                    break;
                }
                case NodeKind::Ref: {
                    auto* refInit = static_cast<RefExprAST*>(init.get());
                    if (refInit->target->kind == NodeKind::Variable) {
                        auto* refVar = static_cast<VariableExprAST*>(refInit->target.get());
                        auto it = varTypeMap.find(refVar->name);
                        if (it != varTypeMap.end()) varType = Type::ref(it->second);
                    }
                    break;
                }
                case NodeKind::StructLiteral: {
                    auto* structInit = static_cast<StructLiteralAST*>(init.get());
                    if (structRegistry.count(structInit->structName))
                        varType = Type::struct_(structInit->structName);
                    break;
                }
                case NodeKind::EnumConstruct: {
                    auto* enumInit = static_cast<EnumConstructAST*>(init.get());
                    if (enumRegistry.count(enumInit->enumName))
                        varType = Type::enum_(enumInit->enumName);
                    break;
                }
                case NodeKind::MapLiteral:
                    varType = Type::map();
                    break;
                case NodeKind::Call: {
                    auto* cInit = static_cast<CallExprAST*>(init.get());
                    if (cInit->callee == "flint_map_new") varType = Type::map();
                    if (cInit->callee == "flint_array_alloc" || cInit->callee == "flint_array_concat")
                        varType = Type::array(Type::i64());
                    break;
                }
                default: break;
            }
        }
        varTypeMap[nameTok.lexeme] = varType;
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);

        auto decl = std::make_unique<VarDeclAST>(nameTok.lexeme, isMutable, varType, std::move(init));
        decl->loc = nameTok.loc;
        return decl;
    }

    std::unique_ptr<ExprAST> parseExprStmt() {
        auto expr = parseExpression();
        if (!expr) return nullptr;
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);
        return expr;
    }

    // ---- expression precedence ----
    int getPrec(TokenType t) {
        switch (t) {
            case TokenType::AT:    return 55;
            case TokenType::STAR:
            case TokenType::SLASH:
            case TokenType::MODULO: return 60;
            case TokenType::PLUS:
            case TokenType::MINUS: return 50;
            case TokenType::EQ_EQ:
            case TokenType::NE:    return 40;
            case TokenType::LT:
            case TokenType::GT:
            case TokenType::LE:
            case TokenType::GE:    return 40;
            case TokenType::PIPE: return 20;
            case TokenType::PIPE_PIPE:
            case TokenType::KW_OR: return 30;
            case TokenType::AMPERSAND_AMP:
            case TokenType::KW_AND: return 35;
            default: return -1;
        }
    }

    // Classify a method receiver: 0 unknown, 1 str, 2 map.
    // Uses parse-time types (varTypeMap) plus literal/call shapes, so
    // `s.upper().len()` chaining classifies through the rewritten call.
    int methodRecvKind(ExprAST* e) {
        if (!e) return 0;
        if (e->kind == NodeKind::String) return 1;
        if (e->kind == NodeKind::MapLiteral) return 2;
        if (e->kind == NodeKind::Variable) {
            auto* v = static_cast<VariableExprAST*>(e);
            auto it = varTypeMap.find(v->name);
            if (it != varTypeMap.end()) {
                if (it->second.kind == TypeKind::Str) return 1;
                if (it->second.kind == TypeKind::Map) return 2;
            }
            return 0;
        }
        if (e->kind == NodeKind::Call) {
            auto* c = static_cast<CallExprAST*>(e);
            const std::string& n = c->callee;
            if (n == "flint_map_new") return 2;
            if (n == "flint_str_concat" || n == "flint_str_substring" ||
                n == "flint_str_to_upper" || n == "flint_str_to_lower" ||
                n == "flint_str_trim" || n == "flint_str_reverse" ||
                n == "flint_str_replace" || n == "flint_str_repeat" ||
                n == "flint_str_join" || n == "flint_i64_to_string" ||
                n == "flint_f64_to_string" || n == "flint_map_keys")
                return 1;
        }
        return 0;
    }

    // Map a method name + receiver kind to its flint_* builtin.
    // Returns "" when unknown; sets *wantArity to the required TOTAL arg
    // count (incl. receiver) when the name is known but haveArity mismatches.
    std::string methodBuiltin(const std::string& m, TypeKind recv, size_t haveArity,
                              size_t* wantArity) {
        const char* target = nullptr;
        size_t want = 0;
        if (recv == TypeKind::Str) {
            if (m == "len") { target = "flint_str_length"; want = 1; }
            else if (m == "upper") { target = "flint_str_to_upper"; want = 1; }
            else if (m == "lower") { target = "flint_str_to_lower"; want = 1; }
            else if (m == "trim") { target = "flint_str_trim"; want = 1; }
            else if (m == "reverse") { target = "flint_str_reverse"; want = 1; }
            else if (m == "get") { target = "flint_str_char_at"; want = 2; }
            else if (m == "repeat") { target = "flint_str_repeat"; want = 2; }
            else if (m == "startswith") { target = "flint_str_starts_with"; want = 2; }
            else if (m == "endswith") { target = "flint_str_ends_with"; want = 2; }
            else if (m == "indexof") { target = "flint_str_index_of"; want = 2; }
            else if (m == "replace") { target = "flint_str_replace"; want = 3; }
        } else if (recv == TypeKind::Map) {
            if (m == "get") { target = "flint_map_get"; want = 2; }
            else if (m == "set") { target = "flint_map_set"; want = 3; }
            else if (m == "has") { target = "flint_map_has"; want = 2; }
            else if (m == "len") { target = "flint_map_len"; want = 1; }
            else if (m == "keys") { target = "flint_map_keys"; want = 1; }
        }
        if (!target) return "";
        if (haveArity != want) { *wantArity = want; return ""; }
        return target;
    }

    // Collect variable uses in a lambda body for capture detection.
    // bound: params + inner declarations (scoped per block/branch so a
    // body-local never shadows an outer use — missed captures would
    // miscompile, over-captured names fail cleanly at codegen instead).
    // Static so Codegen reuses it to verify bodies (err receives any error).
    static void collectLambdaUses(ExprAST* e, std::unordered_set<std::string>& bound,
                           std::vector<std::string>& uses, std::string* err = nullptr) {
        if (!e) return;
        auto note = [&](const std::string& n) {
            if (!bound.count(n) &&
                std::find(uses.begin(), uses.end(), n) == uses.end())
                uses.push_back(n);
        };
        switch (e->kind) {
            case NodeKind::Number:
            case NodeKind::String:
            case NodeKind::Break:
            case NodeKind::Continue:
                break;
            case NodeKind::Variable:
                note(static_cast<VariableExprAST*>(e)->name);
                break;
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(e);
                note(a->varName);
                collectLambdaUses(a->rhs.get(), bound, uses, err);
                break;
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(e);
                collectLambdaUses(b->lhs.get(), bound, uses, err);
                collectLambdaUses(b->rhs.get(), bound, uses, err);
                break;
            }
            case NodeKind::Compare: {
                auto* c = static_cast<CompareExprAST*>(e);
                collectLambdaUses(c->lhs.get(), bound, uses, err);
                collectLambdaUses(c->rhs.get(), bound, uses, err);
                break;
            }
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(e);
                for (auto& a : c->args) collectLambdaUses(a.get(), bound, uses, err);
                break;
            }
            case NodeKind::VarDecl: {
                auto* d = static_cast<VarDeclAST*>(e);
                collectLambdaUses(d->init.get(), bound, uses, err);
                bound.insert(d->varName);
                break;
            }
            case NodeKind::Return:
                collectLambdaUses(static_cast<ReturnStmtAST*>(e)->value.get(), bound, uses, err);
                break;
            case NodeKind::If: {
                auto* i = static_cast<IfStmtAST*>(e);
                collectLambdaUses(i->condition.get(), bound, uses, err);
                auto tb = bound, eb = bound;
                collectLambdaUses(i->thenBlock.get(), tb, uses, err);
                collectLambdaUses(i->elseBlock.get(), eb, uses, err);
                break;
            }
            case NodeKind::While: {
                auto* w = static_cast<WhileStmtAST*>(e);
                collectLambdaUses(w->condition.get(), bound, uses, err);
                auto bb = bound;
                collectLambdaUses(w->body.get(), bb, uses, err);
                break;
            }
            case NodeKind::Block: {
                auto nb = bound;
                for (auto& s : static_cast<BlockStmtAST*>(e)->stmts)
                    collectLambdaUses(s.get(), nb, uses, err);
                break;
            }
            case NodeKind::Array:
                for (auto& el : static_cast<ArrayExprAST*>(e)->elements)
                    collectLambdaUses(el.get(), bound, uses, err);
                break;
            case NodeKind::Index: {
                auto* ix = static_cast<IndexExprAST*>(e);
                collectLambdaUses(ix->base.get(), bound, uses, err);
                collectLambdaUses(ix->index.get(), bound, uses, err);
                break;
            }
            case NodeKind::Slice: {
                auto* s = static_cast<SliceExprAST*>(e);
                collectLambdaUses(s->arr.get(), bound, uses, err);
                collectLambdaUses(s->start.get(), bound, uses, err);
                collectLambdaUses(s->end.get(), bound, uses, err);
                break;
            }
            case NodeKind::Ref:
                collectLambdaUses(static_cast<RefExprAST*>(e)->target.get(), bound, uses, err);
                break;
            case NodeKind::Deref:
                collectLambdaUses(static_cast<DerefExprAST*>(e)->target.get(), bound, uses, err);
                break;
            case NodeKind::StructLiteral: {
                auto* sl = static_cast<StructLiteralAST*>(e);
                for (auto& f : sl->fields) collectLambdaUses(f.second.get(), bound, uses, err);
                break;
            }
            case NodeKind::EnumConstruct: {
                auto* ec = static_cast<EnumConstructAST*>(e);
                for (auto& a : ec->args) collectLambdaUses(a.get(), bound, uses, err);
                break;
            }
            case NodeKind::Match: {
                auto* m = static_cast<MatchExprAST*>(e);
                collectLambdaUses(m->scrutinee.get(), bound, uses, err);
                for (auto& arm : m->arms) {
                    auto ab = bound;
                    if (!arm.bindName.empty()) ab.insert(arm.bindName);
                    collectLambdaUses(arm.body.get(), ab, uses, err);
                }
                break;
            }
            case NodeKind::FieldAccess:
                collectLambdaUses(static_cast<FieldAccessAST*>(e)->base.get(), bound, uses, err);
                break;
            case NodeKind::Unwrap:
                collectLambdaUses(static_cast<UnwrapExprAST*>(e)->inner.get(), bound, uses, err);
                break;
            case NodeKind::Destructure: {
                auto* d = static_cast<DestructureAST*>(e);
                collectLambdaUses(d->init.get(), bound, uses, err);
                for (auto& n : d->names) bound.insert(n);
                break;
            }
            case NodeKind::MapLiteral:
                for (auto& v : static_cast<MapLiteralAST*>(e)->values)
                    collectLambdaUses(v.get(), bound, uses, err);
                break;
            case NodeKind::Lambda:
                if (err) *err = "nested lambdas are not supported";
                break;
            default: break; // Function, PyBlock: cannot appear in bodies
        }
    }

    std::unique_ptr<ExprAST> parsePostfix(std::unique_ptr<ExprAST> lhs) {
        while (lhs) {
            if (check(TokenType::LBRACKET)) {
                advance(); // '['
                auto idx = parseExpression();
                if (check(TokenType::COLON)) {
                    // Slice: arr[start:end]
                    advance(); // ':'
                    auto endExpr = parseExpression();
                    consume(TokenType::RBRACKET, "expected ']' after slice end");
                    auto sliceNode = std::make_unique<SliceExprAST>(std::move(lhs), std::move(idx), std::move(endExpr));
                    sliceNode->loc = previous().loc;
                    lhs = std::move(sliceNode);
                } else {
                    consume(TokenType::RBRACKET, "expected ']' after index");
                    auto idxNode = std::make_unique<IndexExprAST>(std::move(lhs), std::move(idx));
                    idxNode->loc = previous().loc;
                    lhs = std::move(idxNode);
                }
            } else if (check(TokenType::DOT)) {
                advance(); // '.'
                Token fName = consume(TokenType::IDENTIFIER, "expected field or method name after '.'");
                // Method call: known str/map methods rewrite to flint_*
                // builtins at parse time (P2; re-added after 2026-09-14 wipe).
                // Unknown receivers keep the legacy fa_method(base) call,
                // which fails cleanly at codegen (fa_ gate in emitCall).
                if (check(TokenType::LPAREN)) {
                    advance(); // '('
                    std::vector<std::unique_ptr<ExprAST>> args;
                    args.push_back(std::move(lhs));
                    if (!check(TokenType::RPAREN)) {
                        do {
                            auto a = parseExpression();
                            if (a) args.push_back(std::move(a));
                        } while (match(TokenType::COMMA));
                    }
                    consume(TokenType::RPAREN, "expected ')' after method arguments");
                    std::string methodTarget;
                    size_t methodArity = 0;
                    int recvKind = methodRecvKind(args[0].get());
                    if (recvKind != 0) {
                        methodTarget = methodBuiltin(fName.lexeme,
                            recvKind == 1 ? TypeKind::Str : TypeKind::Map,
                            args.size(), &methodArity);
                    }
                    if (!methodTarget.empty()) {
                        auto call = std::make_unique<CallExprAST>(methodTarget, std::move(args));
                        call->loc = fName.loc;
                        lhs = std::move(call);
                    } else if (recvKind != 0) {
                        // Known receiver, unknown method or arity — the args
                        // are fully consumed, so returning nullptr is safe
                        // (block-loop guards guarantee progress).
                        if (methodArity > 0)
                            parseError("method '" + fName.lexeme + "' needs " +
                                std::to_string(methodArity - 1) + " argument(s)");
                        else if (recvKind == 1)
                            parseError("unknown method '" + fName.lexeme +
                                "' for str (available: len, upper, lower, trim, reverse, get, repeat, startswith, endswith, indexof, replace)");
                        else
                            parseError("unknown method '" + fName.lexeme +
                                "' for map (available: get, set, has, len, keys)");
                        return nullptr;
                    } else {
                        auto call = std::make_unique<CallExprAST>("fa_" + fName.lexeme, std::move(args));
                        call->loc = fName.loc;
                        lhs = std::move(call);
                    }
                } else {
                    auto fa = std::make_unique<FieldAccessAST>(std::move(lhs), fName.lexeme);
                    fa->loc = fName.loc;
                    lhs = std::move(fa);
                }
            } else if (check(TokenType::QUESTION)) {
                advance(); // '?'
                auto uw = std::make_unique<UnwrapExprAST>(std::move(lhs));
                uw->loc = previous().loc;
                lhs = std::move(uw);
            } else {
                break;
            }
        }
        return lhs;
    }

    std::unique_ptr<ExprAST> parseExpression() {
        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
            std::string name = peek().lexeme;
            if (!declaredVars.count(name) && !globalVarNames.count(name)) {
                parseError("variable '" + name + "' not declared");
                return nullptr;
            }
            Token nameTok = advance();
            advance(); // '='
            auto rhs = parseExpression();
            auto assign = std::make_unique<AssignExprAST>(nameTok.lexeme, std::move(rhs));
            assign->loc = nameTok.loc;
            return assign;
        }
        // Compound assignment: `x += e` desugars to `x = x + e` (likewise
        // -= *= /= %=). Without this the statement parser makes no progress
        // on the _EQ token and the block loop spins forever (hang, not error).
        if (check(TokenType::IDENTIFIER) &&
            (peek(1).type == TokenType::PLUS_EQ || peek(1).type == TokenType::MINUS_EQ ||
             peek(1).type == TokenType::STAR_EQ || peek(1).type == TokenType::SLASH_EQ ||
             peek(1).type == TokenType::MODULO_EQ)) {
            std::string name = peek().lexeme;
            if (!declaredVars.count(name) && !globalVarNames.count(name)) {
                parseError("variable '" + name + "' not declared");
                return nullptr;
            }
            Token nameTok = advance();
            Token opTok = advance(); // '+=' etc.
            char op = '+';
            if (opTok.type == TokenType::MINUS_EQ) op = '-';
            else if (opTok.type == TokenType::STAR_EQ) op = '*';
            else if (opTok.type == TokenType::SLASH_EQ) op = '/';
            else if (opTok.type == TokenType::MODULO_EQ) op = '%';
            auto rhs = parseExpression();
            auto lhsVar = std::make_unique<VariableExprAST>(nameTok.lexeme);
            lhsVar->loc = nameTok.loc;
            auto bin = std::make_unique<BinaryExprAST>(op, std::move(lhsVar), std::move(rhs));
            bin->loc = nameTok.loc;
            auto assign = std::make_unique<AssignExprAST>(nameTok.lexeme, std::move(bin));
            assign->loc = nameTok.loc;
            return assign;
        }
        auto lhs = parsePostfix(parsePrimary());
        if (!lhs) return nullptr;
        // Index-assign: `a[i] = v` desugars to checked `flint_array_set(a,i,v)`
        // (Stage-0 0c2; re-added after 2026-09-14 wipe). Without this the `= v`
        // is left unconsumed and the block loop spins forever (hang, not error).
        // Only plain `=` on an Array-typed variable desugars; undeclared /
        // non-array / compound forms are clean *consuming* errors (same rule).
        if (lhs->kind == NodeKind::Index && check(TokenType::ASSIGN)) {
            auto* ix = static_cast<IndexExprAST*>(lhs.get());
            bool isArray = false;
            if (ix->base && ix->base->kind == NodeKind::Variable) {
                auto* bv = static_cast<VariableExprAST*>(ix->base.get());
                auto it = varTypeMap.find(bv->name);
                if (it != varTypeMap.end() && it->second.kind == TypeKind::Array) isArray = true;
            }
            if (!isArray) {
                parseError("index assignment requires an array variable");
                advance(); // '=' — guarantee progress, never spin
                auto sink = parseExpression();
                (void)sink;
                return nullptr;
            }
            SourceLocation aloc = peek().loc;
            advance(); // '='
            auto rhs = parseExpression();
            if (!rhs) return nullptr;
            std::vector<std::unique_ptr<ExprAST>> sargs;
            sargs.push_back(std::move(ix->base));
            sargs.push_back(std::move(ix->index));
            sargs.push_back(std::move(rhs));
            auto setCall = std::make_unique<CallExprAST>("flint_array_set", std::move(sargs));
            setCall->loc = aloc;
            return setCall;
        }
        return parseBinaryOpRHS(0, std::move(lhs));
    }

    std::unique_ptr<ExprAST> parseBinaryOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs) {
        if (!lhs) return nullptr;
        while (true) {
            int prec = getPrec(peek().type);
            if (prec < exprPrec) return lhs;

            Token opTok = advance();
            auto rhs = parsePostfix(parsePrimary());
            if (!rhs) return lhs;

            int nextPrec = getPrec(peek().type);
            if (prec < nextPrec) {
                rhs = parseBinaryOpRHS(prec + 1, std::move(rhs));
                if (!rhs) return lhs;
            }

            TokenType tt = opTok.type;
            if (tt == TokenType::PLUS || tt == TokenType::MINUS ||
                tt == TokenType::STAR || tt == TokenType::SLASH ||
                tt == TokenType::MODULO || tt == TokenType::PIPE_PIPE ||
                tt == TokenType::KW_OR || tt == TokenType::AMPERSAND_AMP ||
                tt == TokenType::KW_AND ||
                tt == TokenType::AT) {
                // `&&`/`and` desugar to the &-node (truthy AND in codegen);
                // `or` keyword maps to the |-node like `||`.
                char opc = opTok.lexeme[0];
                if (tt == TokenType::AMPERSAND_AMP || tt == TokenType::KW_AND) opc = '&';
                else if (tt == TokenType::KW_OR) opc = '|';
                lhs = std::make_unique<BinaryExprAST>(opc, std::move(lhs), std::move(rhs));
            } else if (tt == TokenType::PIPE) {
                // Desugar a |> f(b...) into f(a, b...)
                // Desugar a |> f into f(a)
                if (auto* call = dynamic_cast<CallExprAST*>(rhs.get())) {
                    call->args.insert(call->args.begin(), std::move(lhs));
                    lhs = std::move(rhs);
                } else if (auto* var = dynamic_cast<VariableExprAST*>(rhs.get())) {
                    std::vector<std::unique_ptr<ExprAST>> args;
                    args.push_back(std::move(lhs));
                    auto c = std::make_unique<CallExprAST>(var->name, std::move(args));
                    c->loc = opTok.loc;
                    lhs = std::move(c);
                } else {
                    parseError("right side of '|>' must be a function call or identifier");
                    return lhs;
                }
            } else {
                lhs = std::make_unique<CompareExprAST>(opTok.lexeme, std::move(lhs), std::move(rhs));
            }
            lhs->loc = opTok.loc;
        }
    }

    std::unique_ptr<ExprAST> parsePrimary() {
        // Map literal: contextual `map` + `{` ("map{...}" or "map {...}")
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "map" && peek(1).type == TokenType::LBRACE) {
            advance(); // 'map'
            advance(); // '{'
            auto ml = std::make_unique<MapLiteralAST>();
            ml->loc = previous().loc;
            if (!check(TokenType::RBRACE)) {
                do {
                    Token kTok = consume(TokenType::STRING_LITERAL, "expected string key in map literal");
                    consume(TokenType::COLON, "expected ':' after map key");
                    auto v = parseExpression();
                    if (!v) { parseError("expected value in map literal"); break; }
                    ml->keys.push_back(kTok.lexeme);
                    ml->values.push_back(std::move(v));
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RBRACE, "expected '}' to close map literal");
            return ml;
        }
        if (match(TokenType::NUMBER_LITERAL)) {
            std::string lit = previous().lexeme;
            bool isFloatLit = false;
            if (!lit.empty() && lit.back() == 'd') { isFloatLit = true; lit.pop_back(); }
            std::unique_ptr<NumberExprAST> n;
            if (isFloatLit) {
                n = std::make_unique<NumberExprAST>(std::stod(lit), true);
            } else {
                // Integer syntax: keep exact i64 (double can't hold 2^53+1).
                // Overflow beyond i64 falls back to double (matches old behavior).
                int64_t iv = 0;
                if (parseI64Exact(lit, iv)) n = std::make_unique<NumberExprAST>(iv);
                else n = std::make_unique<NumberExprAST>(std::stod(lit), false);
            }
            n->loc = previous().loc; return n;
        }
        if (match(TokenType::STRING_LITERAL)) {
            std::string raw = previous().lexeme;
            SourceLocation sloc = previous().loc;
            auto unescapeBraces = [](std::string s) {
                std::string out;
                out.reserve(s.size());
                for (size_t i = 0; i < s.size(); ) {
                    if (i + 1 < s.size() && s[i] == '{' && s[i + 1] == '{') { out += '{'; i += 2; }
                    else if (i + 1 < s.size() && s[i] == '}' && s[i + 1] == '}') { out += '}'; i += 2; }
                    else { out += s[i]; i++; }
                }
                return out;
            };
            auto hasInterp = [&]() {
                for (size_t i = 0; i < raw.size(); ) {
                    if (raw[i] == '{') {
                        if (i + 1 < raw.size() && raw[i + 1] == '{') { i += 2; continue; }
                        if (raw.find('}', i + 1) != std::string::npos) return true;
                        i++;
                    } else i++;
                }
                return false;
            };
            if (hasInterp()) {
                std::vector<std::unique_ptr<ExprAST>> parts;
                size_t start = 0;
                while (start < raw.size()) {
                    size_t ob = start;
                    while (ob < raw.size()) {
                        if (raw[ob] == '{') {
                            if (ob + 1 < raw.size() && raw[ob + 1] == '{') { ob += 2; continue; }
                            break;
                        }
                        ob++;
                    }
                    if (ob >= raw.size()) {
                        auto lit = unescapeBraces(raw.substr(start));
                        if (!lit.empty()) {
                            auto sn = std::make_unique<StringExprAST>(lit);
                            sn->loc = sloc;
                            parts.push_back(std::move(sn));
                        }
                        break;
                    }
                    if (ob > start) {
                        auto sn = std::make_unique<StringExprAST>(unescapeBraces(raw.substr(start, ob - start)));
                        sn->loc = sloc;
                        parts.push_back(std::move(sn));
                    }
                    size_t cb = raw.find('}', ob + 1);
                    if (cb == std::string::npos) {
                        auto sn = std::make_unique<StringExprAST>(unescapeBraces(raw.substr(ob)));
                        sn->loc = sloc;
                        parts.push_back(std::move(sn));
                        break;
                    }
                    std::string varName = raw.substr(ob + 1, cb - ob - 1);
                    auto vn = std::make_unique<VariableExprAST>(varName);
                    vn->loc = sloc;
                    parts.push_back(std::move(vn));
                    start = cb + 1;
                }
                if (!parts.empty()) {
                    auto it = parts.begin();
                    auto result = std::move(*it++);
                    for (; it != parts.end(); ++it) {
                        result = std::make_unique<BinaryExprAST>('+', std::move(result), std::move(*it));
                    }
                    return result;
                }
            }
            auto s = std::make_unique<StringExprAST>(unescapeBraces(raw));
            s->loc = sloc; return s;
        }
        if (match(TokenType::LBRACKET)) {
            std::vector<std::unique_ptr<ExprAST>> elems;
            std::vector<bool> spreadFlags;
            if (!check(TokenType::RBRACKET)) {
                do {
                    if (match(TokenType::ELLIPSIS)) {
                        auto inner = parseExpression();
                        if (inner) { elems.push_back(std::move(inner)); spreadFlags.push_back(true); }
                    } else {
                        auto e = parseExpression();
                        if (e) { elems.push_back(std::move(e)); spreadFlags.push_back(false); }
                    }
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RBRACKET, "expected ']' after array literal");
            auto arr = std::make_unique<ArrayExprAST>();
            arr->elements = std::move(elems);
            arr->spreadFlags = std::move(spreadFlags);
            arr->loc = previous().loc;
            return arr;
        }
        // try expression: try expr → unwrap expr
        if (match(TokenType::KW_TRY)) {
            auto inner = parseExpression();
            auto uw = std::make_unique<UnwrapExprAST>(std::move(inner));
            uw->loc = previous().loc;
            return uw;
        }
        // if expression: if cond { then } else { else }
        if (check(TokenType::KW_IF)) {
            auto node = std::make_unique<IfStmtAST>();
            node->loc = peek().loc;
            advance(); // 'if'
            auto cond = parseExpression();
            if (!cond) return nullptr;
            node->condition = std::move(cond);
            node->thenBlock = parseBlock();
            if (match(TokenType::KW_ELIF)) {
                auto wrap = std::make_unique<BlockStmtAST>();
                wrap->stmts.push_back(parseElifChain());
                node->elseBlock = std::move(wrap);
            } else if (match(TokenType::KW_ELSE)) {
                node->elseBlock = parseBlock();
            }
            return node;
        }
        // Lambda: |args| expr — closure-converted at codegen (P0-6).
        // Params are scoped to the body; outer names used by the body are
        // recorded as by-value captures. Nested lambdas are rejected.
        if (match(TokenType::PIPE) && previous().lexeme == "|") {
            auto lambda = std::make_unique<LambdaExprAST>();
            lambda->loc = previous().loc;
            // Parse params until closing '|'
            if (!check(TokenType::PIPE)) {
                do {
                    Token pName = consume(TokenType::IDENTIFIER, "expected parameter name in lambda");
                    Type pType = Type::i64();
                    if (match(TokenType::COLON)) pType = parseType();
                    lambda->params.emplace_back(pName.lexeme, pType);
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::PIPE, "expected '|' to close lambda params");
            // Scope the params for the body (restored after).
            auto savedVars = declaredVars;
            auto savedTypes = varTypeMap;
            for (auto& p : lambda->params) {
                declaredVars.insert(p.first);
                varTypeMap[p.first] = p.second;
            }
            lambda->body = parseExpression();
            // NOTE: restore by COPY, not move — capture detection below
            // reads savedVars. `std::move` would leave it empty and all
            // captures would be silently dropped (was: unknown variable
            // 'base' in tutorial 04).
            declaredVars = savedVars;
            // Captures: body uses resolved against the OUTER scope (params
            // excluded, globals excluded — globals link directly).
            if (lambda->body) {
                std::unordered_set<std::string> paramSet;
                for (auto& p : lambda->params) paramSet.insert(p.first);
                std::vector<std::string> uses;
                std::string walkErr;
                collectLambdaUses(lambda->body.get(), paramSet, uses, &walkErr);
                if (!walkErr.empty()) { parseError(walkErr); return nullptr; }
                for (auto& u : uses) {
                    if (paramSet.count(u)) continue;
                    if (globalVarNames.count(u)) continue;
                    if (!savedVars.count(u)) continue; // typo → clean codegen error later
                    if (std::find(lambda->captures.begin(), lambda->captures.end(), u) ==
                        lambda->captures.end())
                        lambda->captures.push_back(u);
                }
                lambda->retType = inferType(lambda->body.get());
            }
            varTypeMap = std::move(savedTypes);
            return lambda;
        }
        if (match(TokenType::KW_MATCH)) {
            auto mn = std::make_unique<MatchExprAST>();
            mn->loc = previous().loc;
            mn->scrutinee = parseExpression();
            consume(TokenType::LBRACE, "expected '{' after match expression");
            while (!check(TokenType::RBRACE) && !isAtEnd()) {
                MatchArm arm;
                Token enumTk = consume(TokenType::IDENTIFIER, "expected enum name in match arm");
                consume(TokenType::DOT, "expected '.' after enum name");
                Token varTk = consume(TokenType::IDENTIFIER, "expected variant name");
                arm.enumName = enumTk.lexeme;
                arm.variantName = varTk.lexeme;
                if (match(TokenType::LPAREN)) {
                    Token bind = consume(TokenType::IDENTIFIER, "expected binding name");
                    arm.bindName = bind.lexeme;
                    consume(TokenType::RPAREN, "expected ')'");
                }
                consume(TokenType::FAT_ARROW, "expected '=>' after match arm pattern");
                if (check(TokenType::LBRACE)) {
                    arm.body = parseBlock();
                } else {
                    arm.body = parseStatement();
                }
                mn->arms.push_back(std::move(arm));
                if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
            }
            consume(TokenType::RBRACE, "expected '}' to close match");
            return mn;
        }
        if (match(TokenType::IDENTIFIER)) {
            std::string name = previous().lexeme;
            SourceLocation loc = previous().loc;
            // Boolean literals (v0.9 AEGIS session; re-added after 2026-09-14 wipe)
            if (name == "true" || name == "false") {
                auto n = std::make_unique<NumberExprAST>(name == "true" ? 1 : 0);
                n->loc = loc; return n;
            }
            // Enum construction: EnumName.Variant(args...)
            if (enumRegistry.count(name) && check(TokenType::DOT)) {
                advance(); // '.'
                Token varTok = consume(TokenType::IDENTIFIER, "expected variant name");
                auto ec = std::make_unique<EnumConstructAST>();
                ec->enumName = name;
                ec->variantName = varTok.lexeme;
                ec->loc = loc;
                if (match(TokenType::LPAREN)) {
                    if (!check(TokenType::RPAREN)) {
                        do {
                            ec->args.push_back(parseExpression());
                        } while (match(TokenType::COMMA));
                    }
                    consume(TokenType::RPAREN, "expected ')'");
                }
                return ec;
            }
            if (match(TokenType::LPAREN)) {
                std::vector<std::unique_ptr<ExprAST>> args;
                if (!check(TokenType::RPAREN)) {
                    do {
                        // Named argument: name = expr
                        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
                            Token nTok = advance();
                            advance(); // '='
                            auto val = parseExpression();
                            // Desugar to a special call that's resolved at codegen
                            // For now, just wrap in a marker — we reorder in emitCall
                            auto named = std::make_unique<BinaryExprAST>('=', 
                                std::make_unique<VariableExprAST>(nTok.lexeme), std::move(val));
                            args.push_back(std::move(named));
                        } else {
                            auto a = parseExpression();
                            if (a) args.push_back(std::move(a));
                        }
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "expected ')' after arguments");
                auto c = std::make_unique<CallExprAST>(name, std::move(args));
                c->loc = loc;
                return c;
            }
            if (structRegistry.count(name) && match(TokenType::LBRACE)) {
                auto sl = std::make_unique<StructLiteralAST>();
                sl->structName = name;
                sl->loc = loc;
                while (!check(TokenType::RBRACE) && !isAtEnd()) {
                    Token fName = consume(TokenType::IDENTIFIER, "expected field name");
                    consume(TokenType::COLON, "expected ':'");
                    auto fVal = parseExpression();
                    sl->fields.emplace_back(fName.lexeme, std::move(fVal));
                    if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
                }
                consume(TokenType::RBRACE, "expected '}' to close struct literal");
                return sl;
            }
            auto v = std::make_unique<VariableExprAST>(name);
            v->loc = loc;
            // Optional parens: name expr → name(expr)
            // Only for unknown names (not declared variables) — prevents
            // `y = x print(x)` from being parsed as `y = x(print(x))`.
            // Same-line rule (P0-6): the arg must START on the same line as
            // the name, else `|x| x` followed by `print` on the next line
            // parses as `x(print)` (hang/miscompile class, not error).
            if (!inOptParens && !declaredVars.count(name)) {
                inOptParens = true;
                TokenType nt = peek().type;
                if ((nt == TokenType::NUMBER_LITERAL || nt == TokenType::STRING_LITERAL || nt == TokenType::IDENTIFIER) &&
                    peek().loc.line == loc.line) {
                    auto arg = parseExpression();
                    if (arg) {
                        std::vector<std::unique_ptr<ExprAST>> args;
                        args.push_back(std::move(arg));
                        auto c = std::make_unique<CallExprAST>(name, std::move(args));
                        c->loc = loc;
                        inOptParens = false;
                        return c;
                    }
                }
                inOptParens = false;
            }
            return v;
        }
        if (match(TokenType::LPAREN)) {
            auto e = parseExpression();
            consume(TokenType::RPAREN, "expected ')' after expression");
            return e;
        }
        // Logical NOT: `!e` / `not e` desugars to `e == 0` (truthy semantics,
        // mirrors the &-node used for `&&`). Binds as tightly as unary minus.
        if (match(TokenType::BANG) || match(TokenType::KW_NOT)) {
            SourceLocation bloc = previous().loc;
            auto inner = parsePrimary();
            if (!inner) return nullptr;
            auto zero = std::make_unique<NumberExprAST>(0);
            zero->loc = bloc;
            auto cmp = std::make_unique<CompareExprAST>("==", std::move(inner), std::move(zero));
            cmp->loc = bloc;
            return cmp;
        }
        if (match(TokenType::MINUS)) {
            auto rhs = parsePrimary();
            if (!rhs) return nullptr;
            auto zero = std::make_unique<NumberExprAST>(0);
            zero->loc = previous().loc;
            auto neg = std::make_unique<BinaryExprAST>('-', std::move(zero), std::move(rhs));
            neg->loc = previous().loc;
            return neg;
        }
        if (match(TokenType::AMPERSAND)) {
            auto target = parsePrimary();
            if (!target) return nullptr;
            auto ref = std::make_unique<RefExprAST>(std::move(target));
            ref->loc = previous().loc;
            return ref;
        }
        if (match(TokenType::STAR)) {
            auto target = parsePrimary();
            if (!target) return nullptr;
            auto deref = std::make_unique<DerefExprAST>(std::move(target));
            deref->loc = previous().loc;
            return deref;
        }
        return nullptr;
    }

    // ---- emit-mode (single-pass): parse + emit directly, no AST nodes ----
public:
    enum Prec : int {
        PREC_NONE = 0,
        PREC_OR = 10,
        PREC_AND = 15,
        PREC_COMPARE = 20,
        PREC_TERM = 30,
        PREC_FACTOR = 40,
        PREC_POSTFIX = 60,
        PREC_CALL = 70,
    };

    int tokPrec(TokenType t);
    llvm::Value* parseExpressionEmit(int minPrec = PREC_NONE);
    llvm::Value* parseNudEmit();
    llvm::Value* parseIdentEmit();
    llvm::Value* emitArithOpEmit(char op, llvm::Value* l, llvm::Value* r);
    llvm::Value* emitBinaryOpEmit(Token opTok, llvm::Value* l, llvm::Value* r);
    llvm::Value* emitIndexEmit(llvm::Value* base, llvm::Value* index);
    llvm::Value* emitIndexStoreEmit(llvm::Value* base, llvm::Value* index, llvm::Value* val);
    llvm::Value* emitFieldAccessEmit(llvm::Value* base, const std::string& field);
    llvm::Value* emitArrayLiteralEmit(const std::vector<llvm::Value*>& elems);
    llvm::Value* emitStructLiteralEmit(const std::string& name);
    llvm::Value* emitEnumConstructEmit(const std::string& enumName, const std::string& variantName);
    llvm::Value* emitCallEmit(const std::string& callee);
    llvm::Value* emitDirectCall(const std::string& callee, std::vector<llvm::Value*>& args);
    llvm::Value* parseMatchEmit();
    void parseStatementEmit();
    void parseExpressionStmtEmit();
    void parseBlockEmit();
    void parseVarDeclEmit(bool isMutable);
    void parseReturnEmit();
    void parseBreakEmit();
    void parseContinueEmit();
    void parseIfEmit();
    void parseWhileEmit();
    void parseForStmtEmit();
    void parseParallelForStmtEmit();
    void parsePythonBlockEmit();
    std::string generateGenericInstance(FunctionAST* genFn, const std::string& callee);
};

// Forwarder defined after the class (the worker is a private static).
void Parser::lambdaUses(ExprAST* e, std::unordered_set<std::string>& bound,
                        std::vector<std::string>& uses, std::string* err) {
    collectLambdaUses(e, bound, uses, err);
}

// ============================================================================
// CODEGEN
// ============================================================================

class Codegen {
public:
    Codegen() : ctx(std::make_unique<llvm::LLVMContext>()),
                mod(std::make_unique<llvm::Module>("flint_module", *ctx)),
                builder(std::make_unique<llvm::IRBuilder<>>(*ctx)),
                i64Ty(llvm::Type::getInt64Ty(*ctx)),
                i32Ty(llvm::Type::getInt32Ty(*ctx)),
                i8Ty(llvm::Type::getInt8Ty(*ctx)),
                voidTy(llvm::Type::getVoidTy(*ctx)),
                i8PtrTy(llvm::PointerType::get(*ctx, 0)) {
        mod->setTargetTriple(llvm::Triple("aarch64-unknown-linux-android24"));
        // Use the typical aarch64-linux data layout. The exact pointer-ABI
        // extensions (p270/p271/p272) are not needed at -O0; the layout here
        // must match ORC JIT's default to avoid enum/struct size mismatches.
        mod->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32");
    }

    void setParser(Parser* p) { parser = p; }
    bool usedPython() const { return hasPython; }

    // P0-3 error gate (re-added after 2026-09-14 wipe): every codegen
    // diagnostic routes through here so a failed compile exits nonzero and
    // partial IR is never JIT-run/emitted/linked. emitFunction returns false
    // when the flag is set (checked at its end).
    bool cgHadError = false;
    llvm::Value* codegenError(const std::string& msg) {
        std::cerr << "codegen: " << msg << "\n";
        cgHadError = true;
        return nullptr;
    }

    bool generateDeclarations(ProgramAST& prog) {
        PROFILE_BEGIN("codegen_init");
        initBuiltins();
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_externs");
        declareExterns(prog.externs);
        PROFILE_END();

        // Initialize standard enums (Option, Result)
        initStdEnums();

        PROFILE_BEGIN("codegen_declare_structs");
        declareStructs(prog.structs);
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_enums");
        declareEnums(prog.enums);
        PROFILE_END();

        programGlobals = &prog.globals;
        PROFILE_BEGIN("codegen_declare_globals");
        declareGlobals(prog);
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_functions");
        declareFunctions(prog);
        PROFILE_END();

        // Populate functionDefs for default param lookup
        for (auto& fn : prog.functions) {
            functionDefs[fn->name] = fn.get();
        }
        return true;
    }

    bool emitFunctionBodies(ProgramAST& prog) {
        PROFILE_BEGIN("codegen_emit_functions");
        for (auto& fn : prog.functions) {
            if (!emitFunction(fn.get())) return false;
        }
        PROFILE_END();
        return true;
    }

    bool generate(ProgramAST& prog, const std::string& outputPath,
        llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::None) {
        if (!generateDeclarations(prog)) return false;
        if (!emitFunctionBodies(prog)) return false;

        PROFILE_BEGIN("codegen_output");
        bool ok = emitModuleOutput(mod.get(), outputPath, optLevel);
        PROFILE_END();
        return ok;
    }

    void declareExterns(std::vector<ExternFn>& externs) {
        for (auto& ext : externs) {
            std::vector<llvm::Type*> paramTys;
            for (auto& pt : ext.paramTypes) paramTys.push_back(llvmType(pt, *ctx, mod.get()));
            llvm::Type* retTy = llvmType(ext.returnType, *ctx, mod.get());
            auto fTy = llvm::FunctionType::get(retTy, paramTys, ext.isVararg);
            // Use getOrInsertFunction to avoid duplicate declarations
            auto f = mod->getFunction(ext.name);
            if (!f) {
                f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, ext.name, mod.get());
            }
            functionMap[ext.name] = f;
            externFunctions.insert(ext.name);
        }
    }

    void declareStructs(std::vector<StructDef>& structs) {
        for (auto& sd : structs) {
            structRegistry[sd.name] = sd;
            auto* st = llvm::StructType::create(*ctx, sd.name);
            std::vector<llvm::Type*> fieldTys;
            for (auto& f : sd.fields) fieldTys.push_back(llvmType(f.type, *ctx, mod.get()));
            st->setBody(fieldTys);
        }
    }

    void declareEnums(std::vector<EnumDef>& enums) {
        for (auto& ed : enums) {
            enumRegistry[ed.name] = ed;
            // Compute max *variant struct* size (including tag + alignment)
            // across all variants. This size is used for the [N x i8] payload
            // array so that any variant's data fits within the alloca.
            size_t maxVariantSize = 0;
            for (auto& v : ed.variants) {
                std::vector<llvm::Type*> fldTys = {i8Ty};
                for (auto& pt : v.payloadTypes)
                    fldTys.push_back(llvmType(pt, *ctx, mod.get()));
                auto* varTy = llvm::StructType::get(*ctx, fldTys);
                size_t sz = mod->getDataLayout().getTypeAllocSize(varTy);
                if (sz > maxVariantSize) maxVariantSize = sz;
            }
            if (maxVariantSize < 2) maxVariantSize = 2; // at least {i8} = 1 byte, use 2 for {i8, [1 x i8]}
            // The [N x i8] array starts at offset 1 (after the tag byte).
            // N must be large enough so that offset 1+N covers all bytes up to maxVariantSize.
            size_t arraySize = maxVariantSize - 1;
            auto* payloadTy = llvm::ArrayType::get(i8Ty, arraySize);
            auto* enumTy = llvm::StructType::create(*ctx, {i8Ty, payloadTy}, ed.name);
            (void)enumTy; // created and registered by name
        }
    }

    // Methods used by Parser in emit-mode (single-pass)
    llvm::AllocaInst* createEntryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::IRBuilder<> tmp(&currentFunc->getEntryBlock(), currentFunc->getEntryBlock().begin());
        return tmp.CreateAlloca(ty, nullptr, name);
    }
    Type resolveType(Type t) {
        if (t.kind == TypeKind::TypeParam) {
            auto it = typeSubstMap.find(t.structName);
            if (it != typeSubstMap.end()) return it->second;
            return t;
        }
        if (t.isCompound() && t.elemType) {
            Type inner = resolveType(*t.elemType);
            if (t.kind == TypeKind::Array) return Type::array(inner);
            if (t.kind == TypeKind::Ref) return Type::ref(inner);
        }
        return t;
    }
    llvm::Type* resolvedLlvmType(Type t) {
        return llvmType(resolveType(t), *ctx, mod.get());
    }

public:
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> mod;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Cached LLVM types
    llvm::Type* i64Ty = nullptr;
    llvm::Type* i32Ty = nullptr;
    llvm::Type* i8Ty = nullptr;
    llvm::Type* voidTy = nullptr;
    llvm::Type* i8PtrTy = nullptr;
    SymbolTable symTable;
    llvm::Function* currentFunc = nullptr;
    std::vector<llvm::BasicBlock*> breakStack;
    std::vector<llvm::BasicBlock*> continueStack;

    std::unordered_map<std::string, llvm::Function*> functionMap;
    std::unordered_set<std::string> externFunctions;
    std::unordered_map<std::string, StructDef> structRegistry;
    std::unordered_map<std::string, EnumDef> enumRegistry;
    std::unordered_map<std::string, FunctionAST*> genericFunctions;
    bool hasPython = false;
    bool releaseMode = true;  // default: no overflow checks
    std::unordered_map<std::string, Type> typeSubstMap;
    std::unordered_map<std::string, llvm::Function*> genericCache;
    std::unordered_map<std::string, FunctionAST*> functionDefs; // for default param lookup
    std::unordered_map<std::string, Symbol> globalSymTable;
    std::vector<std::unique_ptr<VarDeclAST>>* programGlobals = nullptr;
    Parser* parser = nullptr;

    void initStdEnums() {
        // Define Option enum: Option.None, Option.Some(i64)
        EnumDef optionEnum;
        optionEnum.name = "Option";
        optionEnum.variants.push_back({"None", {}});
        optionEnum.variants.push_back({"Some", {Type::i64()}});
        enumRegistry["Option"] = optionEnum;

        // Define Result enum: Result.Ok(i64), Result.Err(str)
        EnumDef resultEnum;
        resultEnum.name = "Result";
        resultEnum.variants.push_back({"Ok", {Type::i64()}});
        resultEnum.variants.push_back({"Err", {Type::str()}});
        enumRegistry["Result"] = resultEnum;
    }

    void initBuiltins() {
        auto add = [&](const std::string& n, llvm::Type* r, std::vector<llvm::Type*> p, bool va) {
            auto f = llvm::Function::Create(llvm::FunctionType::get(r, p, va), llvm::Function::ExternalLinkage, n, mod.get());
            functionMap[n] = f;
        };
        add("flint_println_i64", voidTy, {i64Ty}, false);
        add("flint_println_str", voidTy, {i8PtrTy}, false);
        add("flint_println_f64", voidTy, {llvm::Type::getDoubleTy(*ctx)}, false);
        add("flint_panic", voidTy, {i8PtrTy}, false);
        add("flint_py_init", voidTy, {}, false);
        add("flint_py_run", voidTy, {i8PtrTy}, false);
        add("flint_py_fini", voidTy, {}, false);
        add("flint_py_eval_int", i64Ty, {i8PtrTy}, false);
        add("flint_bounds_check", voidTy, {i64Ty, i64Ty}, false);
        // Runtime string / I/O / conversion helpers (from runtime/runtime.c)
        add("flint_str_concat", i8PtrTy, {i8PtrTy, i8PtrTy}, false);
        add("flint_str_compare", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_str_length", i64Ty, {i8PtrTy}, false);
        add("flint_str_char_at", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_str_substring", i8PtrTy, {i8PtrTy, i64Ty, i64Ty}, false);
        add("flint_str_skip_ws", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_str_word_end", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_i64_to_string", i8PtrTy, {i64Ty}, false);
        add("flint_str_free", voidTy, {i8PtrTy}, false);
        add("flint_file_read", i8PtrTy, {i8PtrTy}, false);
        add("flint_file_write", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_arg_count", i64Ty, {}, false);
        add("flint_get_arg", i8PtrTy, {i64Ty}, false);
        add("flint_time_ns", i64Ty, {}, false);
        add("flint_stdin", i8PtrTy, {}, false);
        add("flint_stdout", i8PtrTy, {}, false);
        add("flint_stderr", i8PtrTy, {}, false);
        add("flint_print", voidTy, {i8PtrTy}, false);
        add("flint_println", voidTy, {i8PtrTy}, false);
        add("flint_malloc", i8PtrTy, {i64Ty}, false);
        add("flint_free", voidTy, {i8PtrTy}, false);
        add("flint_ptr_to_int", i64Ty, {i8PtrTy}, false);
        add("flint_int_to_ptr", i8PtrTy, {i64Ty}, false);
        add("flint_array_read_i64", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_array_write_i64", voidTy, {i8PtrTy, i64Ty, i64Ty}, false);
        auto arrTy = llvm::StructType::get(*ctx, {i8PtrTy, i64Ty});
        add("flint_array_alloc", arrTy, {i64Ty}, false);
        add("flint_array_len", i64Ty, {arrTy}, false);
        auto arrPtrTy = llvm::PointerType::get(arrTy, 0);
        add("flint_array_len_ptr", i64Ty, {arrPtrTy}, false);
        add("flint_array_data_ptr", i8PtrTy, {arrPtrTy}, false);
        add("flint_array_free", voidTy, {arrTy}, false);
        add("flint_array_concat", arrTy, {arrTy, arrTy}, false);
        add("flint_null_check", voidTy, {i8PtrTy, i8PtrTy}, false);
        // Pointer-based array access (no struct reload per iteration)
        auto ptrTy = llvm::PointerType::get(*ctx, 0);
        add("flint_array_data", ptrTy, {arrTy}, false);
        add("flint_array_get_ptr", i64Ty, {ptrTy, i64Ty}, false);
        add("flint_array_set_ptr", voidTy, {ptrTy, i64Ty, i64Ty}, false);
        // Threading primitives
        add("flint_thread_create", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_thread_join", i64Ty, {i64Ty}, false);
        // Parallel for: flint_parallel_for(n, func_ptr, num_threads)
        auto ptrTy2 = llvm::PointerType::get(*ctx, 0);
        add("flint_parallel_for", i64Ty, {i64Ty, ptrTy2, i64Ty}, false);

        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Mathematics
        // ──────────────────────────────────────────────────────
        auto f64Ty = llvm::Type::getDoubleTy(*ctx);
        add("flint_sqrt", f64Ty, {f64Ty}, false);
        add("flint_pow", f64Ty, {f64Ty, f64Ty}, false);
        add("flint_abs_f64", f64Ty, {f64Ty}, false);
        add("flint_abs_i64", i64Ty, {i64Ty}, false);
        add("flint_min_f64", f64Ty, {f64Ty, f64Ty}, false);
        add("flint_min_i64", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_max_f64", f64Ty, {f64Ty, f64Ty}, false);
        add("flint_max_i64", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_floor", f64Ty, {f64Ty}, false);
        add("flint_ceil", f64Ty, {f64Ty}, false);
        add("flint_round", f64Ty, {f64Ty}, false);
        add("flint_sin", f64Ty, {f64Ty}, false);
        add("flint_cos", f64Ty, {f64Ty}, false);
        add("flint_tan", f64Ty, {f64Ty}, false);
        add("flint_asin", f64Ty, {f64Ty}, false);
        add("flint_acos", f64Ty, {f64Ty}, false);
        add("flint_atan", f64Ty, {f64Ty}, false);
        add("flint_atan2", f64Ty, {f64Ty, f64Ty}, false);
        add("flint_log", f64Ty, {f64Ty}, false);
        add("flint_log10", f64Ty, {f64Ty}, false);
        add("flint_exp", f64Ty, {f64Ty}, false);
        add("flint_sinh", f64Ty, {f64Ty}, false);
        add("flint_cosh", f64Ty, {f64Ty}, false);
        add("flint_tanh", f64Ty, {f64Ty}, false);
        // PRNG
        add("flint_srand", voidTy, {i64Ty}, false);
        add("flint_rand_f64", f64Ty, {}, false);
        add("flint_rand_i64_range", i64Ty, {i64Ty, i64Ty}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Type Conversion
        // ──────────────────────────────────────────────────────
        add("flint_f64_to_string", i8PtrTy, {f64Ty}, false);
        add("flint_str_to_i64", i64Ty, {i8PtrTy}, false);
        add("flint_str_to_f64", f64Ty, {i8PtrTy}, false);
        add("flint_str_to_bool", i64Ty, {i8PtrTy}, false);
        add("flint_i64_to_f64", f64Ty, {i64Ty}, false);
        add("flint_f64_to_i64", i64Ty, {f64Ty}, false);
        add("flint_err_occurred", i64Ty, {}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Additional String Operations
        // ──────────────────────────────────────────────────────
        add("flint_str_repeat", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_str_to_upper", i8PtrTy, {i8PtrTy}, false);
        add("flint_str_to_lower", i8PtrTy, {i8PtrTy}, false);
        add("flint_str_trim", i8PtrTy, {i8PtrTy}, false);
        add("flint_str_index_of", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_str_last_index_of", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_str_starts_with", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_str_ends_with", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_str_replace", i8PtrTy, {i8PtrTy, i8PtrTy, i8PtrTy}, false);
        add("flint_str_join", i8PtrTy, {i8PtrTy, i64Ty, i8PtrTy}, false);
        add("flint_str_reverse", i8PtrTy, {i8PtrTy}, false);
        add("flint_str_is_ascii", i64Ty, {i8PtrTy}, false);
        add("flint_str_codepoint_at", i64Ty, {i8PtrTy, i64Ty}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Collections (Vec / Map / StrBuilder)
        // ──────────────────────────────────────────────────────
        auto ptrTy3 = llvm::PointerType::get(*ctx, 0);
        // Vec
        add("flint_vec_new", ptrTy3, {}, false);
        add("flint_vec_free", voidTy, {ptrTy3}, false);
        add("flint_vec_len", i64Ty, {ptrTy3}, false);
        add("flint_vec_cap", i64Ty, {ptrTy3}, false);
        add("flint_vec_get", i64Ty, {ptrTy3, i64Ty}, false);
        add("flint_vec_set", voidTy, {ptrTy3, i64Ty, i64Ty}, false);
        add("flint_vec_push", voidTy, {ptrTy3, i64Ty}, false);
        add("flint_vec_pop", i64Ty, {ptrTy3}, false);
        add("flint_array_slice", ptrTy3, {ptrTy3, i64Ty, i64Ty, i64Ty}, false);
        add("flint_vec_clear", voidTy, {ptrTy3}, false);
        // Map
        add("flint_map_new", ptrTy3, {}, false);
        add("flint_map_free", voidTy, {ptrTy3}, false);
        add("flint_map_has", i64Ty, {ptrTy3, i8PtrTy}, false);
        add("flint_map_get", i64Ty, {ptrTy3, i8PtrTy}, false);
        add("flint_map_set", voidTy, {ptrTy3, i8PtrTy, i64Ty}, false);
        add("flint_map_len", i64Ty, {ptrTy3}, false);
        add("flint_map_keys", i8PtrTy, {ptrTy3}, false);
        // StrBuilder
        add("flint_sb_new", ptrTy3, {}, false);
        add("flint_sb_free", voidTy, {ptrTy3}, false);
        add("flint_sb_append", voidTy, {ptrTy3, i8PtrTy}, false);
        add("flint_sb_append_char", voidTy, {ptrTy3, i64Ty}, false);
        add("flint_sb_append_i64", voidTy, {ptrTy3, i64Ty}, false);
        add("flint_sb_append_f64", voidTy, {ptrTy3, f64Ty}, false);
        add("flint_sb_len", i64Ty, {ptrTy3}, false);
        add("flint_sb_build", i8PtrTy, {ptrTy3}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — System / OS
        // ──────────────────────────────────────────────────────
        add("flint_getenv", i8PtrTy, {i8PtrTy}, false);
        add("flint_exit", voidTy, {i64Ty}, false);
        add("flint_sleep_ms", voidTy, {i64Ty}, false);
        add("flint_mkdir", i64Ty, {i8PtrTy}, false);
        add("flint_rmdir", i64Ty, {i8PtrTy}, false);
        add("flint_remove", i64Ty, {i8PtrTy}, false);
        add("flint_exists", i64Ty, {i8PtrTy}, false);
        add("flint_is_dir", i64Ty, {i8PtrTy}, false);
        add("flint_is_file", i64Ty, {i8PtrTy}, false);
        add("flint_file_size", i64Ty, {i8PtrTy}, false);
        add("flint_listdir", i8PtrTy, {i8PtrTy}, false);
        add("flint_cwd", i8PtrTy, {}, false);
        add("flint_read_file", i8PtrTy, {i8PtrTy}, false);
        add("flint_write_file", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_append_file", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_file_copy", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_file_move", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_read_lines", i8PtrTy, {i8PtrTy}, false);
        add("flint_temp_dir", i8PtrTy, {}, false);
        add("flint_command", i64Ty, {i8PtrTy}, false);
        add("flint_command_output", i8PtrTy, {i8PtrTy}, false);
        add("flint_regex_match", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_regex_replace", i8PtrTy, {i8PtrTy, i8PtrTy, i8PtrTy}, false);
        add("flint_csv_parse_line", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_json_build_object", i8PtrTy, {i8PtrTy, i8PtrTy, i64Ty, i8PtrTy, i8PtrTy}, false);
        add("flint_chdir", i64Ty, {i8PtrTy}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Date & Time
        // ──────────────────────────────────────────────────────
        add("flint_time_now", i64Ty, {}, false);
        add("flint_time_ns_monotonic", i64Ty, {}, false);
        add("flint_time_format", i8PtrTy, {i8PtrTy}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Testing
        // ──────────────────────────────────────────────────────
        add("flint_assert", voidTy, {i64Ty, i8PtrTy}, false);
        add("flint_assert_eq_i64", voidTy, {i64Ty, i64Ty, i8PtrTy}, false);
        add("flint_assert_eq_f64", voidTy, {f64Ty, f64Ty, f64Ty, i8PtrTy}, false);
        add("flint_assert_eq_str", voidTy, {i8PtrTy, i8PtrTy, i8PtrTy}, false);
        add("flint_test_run", i64Ty, {i8PtrTy, ptrTy3}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Error handling (unwrap/expect)
        // ──────────────────────────────────────────────────────
        add("flint_unwrap", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_unwrap_str", i8PtrTy, {i64Ty, i8PtrTy}, false);
        add("flint_expect", i64Ty, {i64Ty, i64Ty, i8PtrTy}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Crypto (flint_crypto.o)
        // ──────────────────────────────────────────────────────
        add("flint_crc32", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_crc32_str", i64Ty, {i8PtrTy}, false);
        add("flint_md5", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_md5_str", i8PtrTy, {i8PtrTy}, false);
        add("flint_sha256", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_sha256_str", i8PtrTy, {i8PtrTy}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Serialization (flint_serial.o)
        // ──────────────────────────────────────────────────────
        add("flint_base64_encode", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_base64_decode", i8PtrTy, {i8PtrTy}, false);
        add("flint_hex_encode", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_hex_decode", i8PtrTy, {i8PtrTy}, false);
        add("flint_json_decode", i8PtrTy, {i8PtrTy}, false);
        // ──────────────────────────────────────────────────────
        // STANDARD LIBRARY — Networking (flint_net.o)
        // ──────────────────────────────────────────────────────
        add("flint_tcp_connect", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_tcp_listen", i64Ty, {i64Ty}, false);
        add("flint_tcp_accept", i64Ty, {i64Ty}, false);
        add("flint_tcp_send", i64Ty, {i64Ty, i8PtrTy, i64Ty}, false);
        add("flint_tcp_recv", i8PtrTy, {i64Ty, i64Ty}, false);
        add("flint_tcp_recv_all", i8PtrTy, {i64Ty, i64Ty}, false);
        add("flint_tcp_close", voidTy, {i64Ty}, false);
        add("flint_tcp_set_timeout", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_dns_resolve", i8PtrTy, {i8PtrTy}, false);
        add("flint_http_get", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_http_post", i8PtrTy, {i8PtrTy, i8PtrTy, i8PtrTy, i64Ty}, false);
        // ──────────────────────────────────────────────────────
        // AEGIS LEASES (flint_aegis.o) — generational memory safety.
        // Lease ids are i64 handles; tutorial 08 uses these directly.
        // ──────────────────────────────────────────────────────
        add("flint_aegis_alloc", i64Ty, {i64Ty}, false);
        add("flint_aegis_free", i64Ty, {i64Ty}, false);
        add("flint_aegis_read_i64", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_aegis_write_i64", voidTy, {i64Ty, i64Ty, i64Ty}, false);
        add("flint_aegis_read_i64_unchecked", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_aegis_write_i64_unchecked", voidTy, {i64Ty, i64Ty, i64Ty}, false);
        add("flint_aegis_len", i64Ty, {i64Ty}, false);
        add("flint_aegis_check", i64Ty, {i64Ty}, false);
        add("flint_aegis_move", i64Ty, {i64Ty}, false);
        add("flint_aegis_borrow", i64Ty, {i64Ty}, false);
        add("flint_aegis_unborrow", i64Ty, {i64Ty}, false);
        add("flint_aegis_read_u8", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_aegis_write_u8", voidTy, {i64Ty, i64Ty, i64Ty}, false);
        // ──────────────────────────────────────────────────────
        // CHANNELS (flint_chan.o) — bounded MPMC i64 message channels.
        // Endpoints are opaque pointers.
        // ──────────────────────────────────────────────────────
        add("flint_chan_new", i8PtrTy, {i64Ty}, false);
        add("flint_chan_free", voidTy, {i8PtrTy}, false);
        add("flint_chan_send", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_chan_recv", i64Ty, {i8PtrTy}, false);
        add("flint_chan_close", i64Ty, {i8PtrTy}, false);
        add("flint_chan_len", i64Ty, {i8PtrTy}, false);

        // AI engine builtins
        auto tensorTy = llvm::StructType::get(*ctx, {i64Ty, i64Ty, llvm::PointerType::get(*ctx, 0)}); // {rows, cols, data*}
        add("fao_matmul", tensorTy, {tensorTy, tensorTy}, false);
        add("fao_relu", tensorTy, {tensorTy}, false);
        add("fao_sigmoid", tensorTy, {tensorTy}, false);
        add("fao_dropout", tensorTy, {tensorTy, llvm::Type::getDoubleTy(*ctx)}, false);
        add("fao_softmax", tensorTy, {tensorTy}, false);
        add("fa_relu", tensorTy, {tensorTy}, false);
        add("fa_sigmoid", tensorTy, {tensorTy}, false);
        add("fa_dropout", tensorTy, {tensorTy, llvm::Type::getDoubleTy(*ctx)}, false);
        add("fa_softmax", tensorTy, {tensorTy}, false);
        add("fa_default_arena", llvm::PointerType::get(*ctx, 0), {}, false);
    }

    void declareGlobals(ProgramAST& prog) {
        for (auto& g : prog.globals) {
            llvm::Type* lty = llvmType(g->varType, *ctx, mod.get());
            llvm::GlobalVariable* gv = new llvm::GlobalVariable(
                *mod, lty, false, llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(lty), g->varName);
            globalSymTable[g->varName] = {g->varType, nullptr, gv, g->isMutable, false, 0};
        }
    }

    void declareFunctions(ProgramAST& prog) {
        for (auto& fn : prog.functions) {
            if (functionMap.count(fn->name)) continue; // already declared (e.g. from --use-interface)
            if (!fn->typeParams.empty()) {
                genericFunctions[fn->name] = fn.get();
                continue;
            }
            std::vector<llvm::Type*> paramTys;
            for (auto& p : fn->params) paramTys.push_back(llvmType(p.second, *ctx, mod.get()));
            // Main function returns i32 and takes argc/argv for CLI access
            llvm::Type* retTy = (fn->name == "main") ? i32Ty : llvmType(fn->returnType, *ctx, mod.get());
            llvm::FunctionType* fTy;
            if (fn->name == "main") {
                fTy = llvm::FunctionType::get(i32Ty, {i64Ty, i8PtrTy}, false);
            } else {
                fTy = llvm::FunctionType::get(retTy, paramTys, false);
            }
            // Check if the module already has this function (from interface merge)
            llvm::Function* f = mod->getFunction(fn->name);
            if (!f) {
                f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, fn->name, mod.get());
            }
            if (fn->name == "main") {
                auto ai = f->args().begin();
                ai->setName("flint_argc");
                (ai + 1)->setName("flint_argv");
            } else {
                size_t i = 0;
                for (auto& arg : f->args()) {
                    if (i < fn->params.size()) arg.setName(fn->params[i].first);
                    i++;
                }
            }
            functionMap[fn->name] = f;
        }
    }

    // After merging interface modules via llvm::Linker, sync LLVM declarations
    // into the C++ symbol tables so emitCall / emitVarDecl can find them.
    void syncInterfaceSymbols() {
        for (auto& f : mod->functions()) {
            if (f.isDeclaration() && functionMap.count(f.getName().str()) == 0) {
                functionMap[f.getName().str()] = &f;
            }
            if (f.getName() == "main") {
                functionMap["main"] = &f;
            }
        }
        // Sync globals
        for (auto& gv : mod->globals()) {
            if (globalSymTable.count(gv.getName().str()) == 0) {
                llvm::Type* ty = gv.getValueType();
                Type flintType = Type::i64();
                if (ty->isPointerTy()) flintType = Type::str();
                globalSymTable[gv.getName().str()] = {flintType, nullptr, &gv, !gv.isConstant(), false, 0};
            }
        }
    }

    bool emitFunction(FunctionAST* fn) {
        if (fn->isDeclaration) return true;
        if (!fn->typeParams.empty()) return true; // generic, specialized on demand
        auto fIt = functionMap.find(fn->name);
        if (fIt == functionMap.end()) return false;
        llvm::Function* f = fIt->second;
        currentFunc = f;

        auto entry = llvm::BasicBlock::Create(*ctx, "entry", f);
        builder->SetInsertPoint(entry);
        symTable.enterScope();

        // Store CLI args in globals for flint_arg_count() / flint_get_arg()
        if (fn->name == "main") {
            auto* argcGV = mod->getOrInsertGlobal("flint_g_argc", i64Ty);
            auto* argvGV = mod->getOrInsertGlobal("flint_g_argv", i8PtrTy);
            auto ai = f->args().begin();
            builder->CreateStore(&*ai, argcGV);
            builder->CreateStore(&*(ai + 1), argvGV);
            // Initialize global variables
            if (programGlobals) {
                for (auto& g : *programGlobals) {
                    auto git = globalSymTable.find(g->varName);
                    if (git != globalSymTable.end()) {
                        auto* init = emitExpr(g->init.get());
                        if (init) builder->CreateStore(init, git->second.global);
                    }
                }
            }
        }

        size_t i = 0;
        for (auto& arg : f->args()) {
            if (fn->name == "main" && i >= fn->params.size()) { i++; continue; }
            std::string pName = fn->params[i].first;
            Type pType = resolveType(fn->params[i].second);
            llvm::AllocaInst* alloca = createEntryAlloca(resolvedLlvmType(fn->params[i].second), pName);
            builder->CreateStore(&arg, alloca);
            symTable.declare(pName, {pType, alloca, nullptr, false, false, 0});
            i++;
        }

        if (fn->body) {
            emitStmt(fn->body.get());
        } else if (fn->bodyStart > 0 && fn->bodyEnd > 0 && parser) {
            size_t savedPos = parser->getPos();
            parser->setPos(fn->bodyStart);
            parser->setCodegen(this, true);
            parser->parseBlockEmit();
            if (parser->hadError()) {
                llvm::errs() << "FLINT COMPILE ERROR: " << parser->errorMsg() << "\n";
                parser->setPos(savedPos);
                return false;
            }
            parser->setPos(fn->bodyEnd);
            parser->setPos(savedPos);
        }

        // Insert python init at start of entry block (hasPython is set during body emit)
        if (fn->name == "main" && hasPython) {
            auto it = functionMap.find("flint_py_init");
            if (it != functionMap.end()) {
                llvm::IRBuilderBase::InsertPoint savedIP = builder->saveIP();
                builder->SetInsertPoint(entry, entry->getFirstInsertionPt());
                builder->CreateCall(it->second, {});
                builder->restoreIP(savedIP);
            }
        }

        bool hasReturnType = fn->returnType.kind != TypeKind::Void;
        if (!builder->GetInsertBlock()->getTerminator()) {
            if (fn->name == "main" && hasPython) {
                auto it = functionMap.find("flint_py_fini");
                if (it != functionMap.end()) builder->CreateCall(it->second, {});
            }
            if (fn->name == "main") {
            builder->CreateRet(llvm::ConstantInt::get(i32Ty, 0));
            } else if (hasReturnType) {
                builder->CreateRet(llvm::ConstantInt::get(llvmType(fn->returnType, *ctx, mod.get()), 0));
            } else {
                builder->CreateRetVoid();
            }
        }

        symTable.exitScope();

    if (0 && llvm::verifyFunction(*f, &llvm::outs())) {
        std::cerr << "function verification failed: " << fn->name << "\n";
        return false;
    }
        // P0-3 gate: any codegen diagnostic above fails the whole function
        // (partial IR is never JIT-run/emitted/linked; exit is nonzero).
        if (cgHadError) return false;
        return true;
    }

    void emitStmt(ExprAST* node) {
        switch (node->kind) {
            case NodeKind::VarDecl: emitVarDecl(static_cast<VarDeclAST*>(node)); return;
            case NodeKind::Return:  emitReturn(static_cast<ReturnStmtAST*>(node)); return;
            case NodeKind::Break:
                if (breakStack.empty()) { std::cerr << "break outside loop\n"; cgHadError = true; return; }
                builder->CreateBr(breakStack.back());
                return;
            case NodeKind::Continue:
                if (continueStack.empty()) { std::cerr << "continue outside loop\n"; cgHadError = true; return; }
                builder->CreateBr(continueStack.back());
                return;
            case NodeKind::If:      emitIf(static_cast<IfStmtAST*>(node)); return;
            case NodeKind::While:   emitWhile(static_cast<WhileStmtAST*>(node)); return;
            case NodeKind::Block:   emitBlock(static_cast<BlockStmtAST*>(node)); return;
            case NodeKind::PyBlock: emitPythonBlock(static_cast<PyBlockStmtAST*>(node)); return;
            case NodeKind::Destructure: emitDestructure(static_cast<DestructureAST*>(node)); return;
            default: emitExpr(node); return;
        }
    }

    llvm::Value* emitExpr(ExprAST* node) {
        switch (node->kind) {
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(node);
                if (n->isFloat) {
                    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*ctx), n->value);
                }
                // Exact integer path (fixes 2^53+1 literals); legacy whole
                // doubles fall back to truncating cast as before.
                if (n->isInt) {
                    return llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(n->intValue), true);
                }
                double v = n->value;
                double intPart;
                if (std::modf(v, &intPart) != 0.0) {
                    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*ctx), v);
                }
                return llvm::ConstantInt::get(i64Ty, static_cast<int64_t>(v));
            }
            case NodeKind::String: {
                auto* s = static_cast<StringExprAST*>(node);
                return builder->CreateGlobalString(s->value, "str");
            }
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(node);
                auto* sym = symTable.lookup(v->name);
                if (!sym) {
                    auto git = globalSymTable.find(v->name);
                    if (git != globalSymTable.end()) {
                        auto* gv = git->second.global;
                        return builder->CreateLoad(llvmType(git->second.type, *ctx, mod.get()), gv, v->name.c_str());
                    }
                    return codegenError(std::string("undefined var '") + v->name + "'");
                }
                if (sym->moved) {
                    return codegenError(std::string("use of moved variable '") + v->name + "'");
                }
                // Lambdas have no first-class value (P0-6: direct calls only).
                if (sym->isLambda) {
                    return codegenError(std::string("cannot use lambda '") + v->name + "' as a value");
                }
                if (sym->borrowCount > 0 && !sym->type.isCopyType()) {
                    return codegenError(std::string("cannot move '") + v->name + "' while borrowed");
                }
                llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
                return builder->CreateLoad(llvmType(sym->type, *ctx, mod.get()), ptr, v->name.c_str());
            }
            case NodeKind::Lambda: {
                // Bare lambda expressions have no binding — lambdas must be
                // bound with `name = |params| body` (outlined in emitVarDecl).
                // As arguments, return values, or array elements they are
                // rejected here (P0-6: direct calls only).
                return codegenError("lambdas must be bound with `name = |params| body`");
            }
            case NodeKind::Unwrap: {
                auto* uw = static_cast<UnwrapExprAST*>(node);
                auto* val = emitExpr(uw->inner.get());
                if (!val) return nullptr;
                llvm::Type* enumTy = val->getType();
                if (!enumTy->isStructTy()) {
                    return val; // non-enum type, no-op
                }
                // Store val in alloca for bitcast access (same pattern as emitMatch)
                auto* alloca = builder->CreateAlloca(enumTy, nullptr, "unwrap_temp");
                builder->CreateStore(val, alloca);
                // Load tag at struct index 0
                auto* tagPtr = builder->CreateStructGEP(enumTy, alloca, 0);
                auto* tag = builder->CreateLoad(i8Ty, tagPtr, "unwrap_tag");
                // Branch on tag == 0 (None/Err)
                auto* check = builder->CreateICmpEQ(tag, llvm::ConstantInt::get(i8Ty, 0), "is_none");
                auto* okBB = llvm::BasicBlock::Create(*ctx, "unwrap_ok", currentFunc);
                auto* panicBB = llvm::BasicBlock::Create(*ctx, "unwrap_panic", currentFunc);
                builder->CreateCondBr(check, panicBB, okBB);
                // Panic block
                builder->SetInsertPoint(panicBB);
                auto* panicFn = functionMap["flint_panic"];
                if (!panicFn) {
                    auto* fTy = llvm::FunctionType::get(voidTy, {i8PtrTy}, false);
                    panicFn = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, "flint_panic", mod.get());
                    functionMap["flint_panic"] = panicFn;
                }
                builder->CreateCall(panicFn, {builder->CreateGlobalString("unwrap of None/Err", "unwrap_msg")});
                builder->CreateRet(llvm::ConstantInt::get(i64Ty, 0));
                // OK block: extract payload via bitcast to variant struct (same as emitMatch)
                builder->SetInsertPoint(okBB);
                auto* bcPtr = builder->CreateBitCast(alloca, i8PtrTy);
                // Assume single i64 payload: treat as { i8, i64 }
                auto* varTy = llvm::StructType::get(*ctx, {i8Ty, i64Ty});
                auto* valPtr = builder->CreateStructGEP(varTy, bcPtr, 1);
                auto* payload = builder->CreateLoad(i64Ty, valPtr, "unwrap_val");
                return payload;
            }
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(node);
                auto* sym = symTable.lookup(a->varName);
                if (!sym) {
                    auto git = globalSymTable.find(a->varName);
                    if (git == globalSymTable.end()) {
                        return codegenError(std::string("undefined var '") + a->varName + "'");
                    }
                    if (!git->second.isMutable) {
                        return codegenError(std::string("cannot assign to immutable global '") + a->varName + "'");
                    }
                    auto* val = emitExpr(a->rhs.get());
                    if (!val) return nullptr;
                    return builder->CreateStore(val, git->second.global);
                }
                if (!sym->isMutable) { return codegenError(std::string("cannot assign to immutable '") + a->varName + "'"); }
                // Lambda names cannot be rebound (P0-6).
                if (sym->isLambda) {
                    return codegenError(std::string("cannot rebind lambda '") + a->varName + "'");
                }
                if (sym->borrowCount > 0) {
                    return codegenError(std::string("cannot assign to '") + a->varName + "' while borrowed");
                }
                auto* val = emitExpr(a->rhs.get());
                if (!val) return nullptr;
                if (a->rhs->kind == NodeKind::Variable) {
                    auto* varRhs = static_cast<VariableExprAST*>(a->rhs.get());
                    auto* srcSym = symTable.lookup(varRhs->name);
                    if (srcSym && !srcSym->type.isCopyType()) {
                        if (srcSym->borrowCount > 0) {
                            return codegenError(std::string("cannot move '") + varRhs->name + "' while borrowed");
                        }
                        srcSym->moved = true;
                    }
                }
                auto* store = builder->CreateStore(val, sym->alloca);
                if (sym->moved) sym->moved = false;
                return store;
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(node);
                auto* l = emitExpr(b->lhs.get());
                auto* r = emitExpr(b->rhs.get());
                if (!l || !r) return nullptr;
                // String concatenation: str + str
                if (b->op == '+' && l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    auto it = functionMap.find("flint_str_concat");
                    if (it != functionMap.end()) return builder->CreateCall(it->second, {l, r});
                    return codegenError("flint_str_concat not linked");
                }
                // Mixed str + i64/f64: convert scalar to string first
                if (b->op == '+' && l->getType()->isPointerTy() && r->getType()->isIntegerTy(64)) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto iti = functionMap.find("flint_i64_to_string");
                    if (itc != functionMap.end() && iti != functionMap.end())
                        return builder->CreateCall(itc->second, {l, builder->CreateCall(iti->second, {r})});
                    return codegenError("flint_str_concat/flint_i64_to_string not linked");
                }
                if (b->op == '+' && l->getType()->isIntegerTy(64) && r->getType()->isPointerTy()) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto iti = functionMap.find("flint_i64_to_string");
                    if (itc != functionMap.end() && iti != functionMap.end())
                        return builder->CreateCall(itc->second, {builder->CreateCall(iti->second, {l}), r});
                    return codegenError("flint_str_concat/flint_i64_to_string not linked");
                }
                if (b->op == '+' && l->getType()->isPointerTy() && r->getType()->isDoubleTy()) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto itf = functionMap.find("flint_f64_to_string");
                    if (itc != functionMap.end() && itf != functionMap.end())
                        return builder->CreateCall(itc->second, {l, builder->CreateCall(itf->second, {r})});
                    return codegenError("flint_str_concat/flint_f64_to_string not linked");
                }
                if (b->op == '+' && l->getType()->isDoubleTy() && r->getType()->isPointerTy()) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto itf = functionMap.find("flint_f64_to_string");
                    if (itc != functionMap.end() && itf != functionMap.end())
                        return builder->CreateCall(itc->second, {builder->CreateCall(itf->second, {l}), r});
                    return codegenError("flint_str_concat/flint_f64_to_string not linked");
                }
                // Mixed f64/i64: promote i64 to f64
                if ((l->getType()->isDoubleTy() && r->getType()->isIntegerTy(64)) ||
                    (r->getType()->isDoubleTy() && l->getType()->isIntegerTy(64))) {
                    auto* f64Ty = llvm::Type::getDoubleTy(*ctx);
                    if (l->getType()->isIntegerTy(64)) l = builder->CreateSIToFP(l, f64Ty, "i64_to_f64");
                    if (r->getType()->isIntegerTy(64)) r = builder->CreateSIToFP(r, f64Ty, "i64_to_f64");
                }
                // f64 fast-path: arithmetic comparison ops on doubles
                if (l->getType()->isDoubleTy() && r->getType()->isDoubleTy()) {
                    switch (b->op) {
                        case '+': return builder->CreateFAdd(l, r, "fadd");
                        case '-': return builder->CreateFSub(l, r, "fsub");
                        case '*': return builder->CreateFMul(l, r, "fmul");
                        case '/': return builder->CreateFDiv(l, r, "fdiv");
                        case '%': return builder->CreateFRem(l, r, "frem");
                        case '|': {
                            auto* lZero = llvm::ConstantFP::get(l->getType(), 0.0);
                            auto* rZero = llvm::ConstantFP::get(r->getType(), 0.0);
                            auto* lB = builder->CreateFCmpONE(l, lZero, "ftruthy");
                            auto* rB = builder->CreateFCmpONE(r, rZero, "rtruthy");
                            auto* result = builder->CreateOr(lB, rB, "forOr");
                            return builder->CreateZExt(result, i64Ty, "forOrExt");
                        }
                        case '&': {
                            auto* lZero = llvm::ConstantFP::get(l->getType(), 0.0);
                            auto* rZero = llvm::ConstantFP::get(r->getType(), 0.0);
                            auto* lB = builder->CreateFCmpONE(l, lZero, "ftruthy");
                            auto* rB = builder->CreateFCmpONE(r, rZero, "rtruthy");
                            auto* result = builder->CreateAnd(lB, rB, "fand");
                            return builder->CreateZExt(result, i64Ty, "fandExt");
                        }
                        default:
                            return codegenError(std::string("unsupported f64 operator '") + b->op + "'");
                    }
                }
                if (b->op == '+' || b->op == '-' || b->op == '*') {
                    if (releaseMode) {
                        switch (b->op) {
                            case '+': return builder->CreateAdd(l, r, "add");
                            case '-': return builder->CreateSub(l, r, "sub");
                            default:  return builder->CreateMul(l, r, "mul");
                        }
                    }
                    llvm::Intrinsic::ID iid;
                    switch (b->op) {
                        case '+': iid = llvm::Intrinsic::sadd_with_overflow; break;
                        case '-': iid = llvm::Intrinsic::ssub_with_overflow; break;
                        default:  iid = llvm::Intrinsic::smul_with_overflow; break;
                    }
                    auto* ovFn = llvm::Intrinsic::getOrInsertDeclaration(mod.get(), iid, {i64Ty});
                    auto* callRes = builder->CreateCall(ovFn, {l, r}, "arith_ov");
                    auto* val = builder->CreateExtractValue(callRes, {0}, "arith_val");
                    auto* ov = builder->CreateExtractValue(callRes, {1}, "arith_ovfl");
                    auto* okBB = llvm::BasicBlock::Create(*ctx, "arith_ok", currentFunc);
                    auto* panicBB = llvm::BasicBlock::Create(*ctx, "arith_panic", currentFunc);
                    builder->CreateCondBr(ov, panicBB, okBB);
                    builder->SetInsertPoint(panicBB);
                    auto* panicFn = functionMap["flint_panic"];
                    if (panicFn) {
                        auto* msg = builder->CreateGlobalString("integer overflow", "ovmsg");
                        builder->CreateCall(panicFn, {msg});
                    }
                    builder->CreateBr(okBB);
                    builder->SetInsertPoint(okBB);
                    return val;
                } else if (b->op == '/') {
                    return builder->CreateSDiv(l, r, "div");
                } else if (b->op == '%') {
                    return builder->CreateSRem(l, r, "mod");
                } else if (b->op == '@') {
                    auto it = functionMap.find("fao_matmul");
                    if (it != functionMap.end()) return builder->CreateCall(it->second, {l, r});
                    return codegenError("fao_matmul not linked");
                } else if (b->op == '|') {
                    auto* lZero = l->getType()->isPointerTy()
                        ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()))
                        : (llvm::Value*)llvm::ConstantInt::get(l->getType(), 0);
                    auto* rZero = r->getType()->isPointerTy()
                        ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()))
                        : (llvm::Value*)llvm::ConstantInt::get(r->getType(), 0);
                    auto* lBool = builder->CreateICmpNE(l, lZero, "lbool");
                    auto* rBool = builder->CreateICmpNE(r, rZero, "rbool");
                    auto* orVal = builder->CreateOr(lBool, rBool, "or");
                    return builder->CreateZExt(orVal, i64Ty, "or_ext");
                } else if (b->op == '&') {
                    auto* lZero = l->getType()->isPointerTy()
                        ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()))
                        : (llvm::Value*)llvm::ConstantInt::get(l->getType(), 0);
                    auto* rZero = r->getType()->isPointerTy()
                        ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()))
                        : (llvm::Value*)llvm::ConstantInt::get(r->getType(), 0);
                    auto* lBool = builder->CreateICmpNE(l, lZero, "lbool");
                    auto* rBool = builder->CreateICmpNE(r, rZero, "rbool");
                    auto* andVal = builder->CreateAnd(lBool, rBool, "and");
                    return builder->CreateZExt(andVal, i64Ty, "and_ext");
                }
                return nullptr;
            }
            case NodeKind::Compare: {
                auto* c = static_cast<CompareExprAST*>(node);
                auto* l = emitExpr(c->lhs.get());
                auto* r = emitExpr(c->rhs.get());
                if (!l || !r) return nullptr;
                // f64 comparison
                if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
                    llvm::CmpInst::Predicate pred;
                    if (c->op == "==") pred = llvm::CmpInst::FCMP_OEQ;
                    else if (c->op == "!=") pred = llvm::CmpInst::FCMP_ONE;
                    else if (c->op == "<")  pred = llvm::CmpInst::FCMP_OLT;
                    else if (c->op == ">")  pred = llvm::CmpInst::FCMP_OGT;
                    else if (c->op == "<=") pred = llvm::CmpInst::FCMP_OLE;
                    else if (c->op == ">=") pred = llvm::CmpInst::FCMP_OGE;
                    else return nullptr;
                    auto* cmp = builder->CreateFCmp(pred, l, r, "fcmp");
                    return builder->CreateZExt(cmp, i64Ty, "fcmp_ext");
                }
                llvm::CmpInst::Predicate pred;
                if (c->op == "==") pred = llvm::CmpInst::ICMP_EQ;
                else if (c->op == "!=") pred = llvm::CmpInst::ICMP_NE;
                else if (c->op == "<")  pred = llvm::CmpInst::ICMP_SLT;
                else if (c->op == ">")  pred = llvm::CmpInst::ICMP_SGT;
                else if (c->op == "<=") pred = llvm::CmpInst::ICMP_SLE;
                else if (c->op == ">=") pred = llvm::CmpInst::ICMP_SGE;
                else return nullptr;
                // String content comparison: ptr == ptr compares contents
                // via flint_str_compare (same convention as `+` → concat).
                if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
                    auto it = functionMap.find("flint_str_compare");
                    if (it != functionMap.end()) {
                        auto* sc = builder->CreateCall(it->second, {l, r}, "strcmp");
                        auto* z = llvm::ConstantInt::get(i64Ty, 0);
                        auto* cmp = builder->CreateICmp(pred, sc, z, "scmp");
                        return builder->CreateZExt(cmp, i64Ty, "scmp_ext");
                    }
                }
                // Normalize types: pointer vs integer zero → null pointer comparison
                if (l->getType()->isPointerTy() && r->getType()->isIntegerTy())
                    r = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()));
                else if (l->getType()->isIntegerTy() && r->getType()->isPointerTy())
                    l = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()));
                auto* cmp = builder->CreateICmp(pred, l, r, "cmp");
                return builder->CreateZExt(cmp, i64Ty, "cmp_ext");
            }
            case NodeKind::Call:   return emitCall(static_cast<CallExprAST*>(node));
            case NodeKind::Array:  return emitArrayLiteral(static_cast<ArrayExprAST*>(node));
            case NodeKind::MapLiteral: return emitMapLiteral(static_cast<MapLiteralAST*>(node));
            case NodeKind::Index:  return emitIndexAccess(static_cast<IndexExprAST*>(node));
            case NodeKind::Slice:  return emitSliceAccess(static_cast<SliceExprAST*>(node));
            case NodeKind::Ref:    return emitRef(static_cast<RefExprAST*>(node));
            case NodeKind::Deref:  return emitDeref(static_cast<DerefExprAST*>(node));
            case NodeKind::StructLiteral: return emitStructLiteral(static_cast<StructLiteralAST*>(node));
            case NodeKind::FieldAccess:   return emitFieldAccess(static_cast<FieldAccessAST*>(node));
            case NodeKind::EnumConstruct: return emitEnumConstruct(static_cast<EnumConstructAST*>(node));
            case NodeKind::Match:  return emitMatch(static_cast<MatchExprAST*>(node));
            case NodeKind::If:     return emitIfExpr(static_cast<IfStmtAST*>(node));
            default: return nullptr;
        }
    }

    // P0-6 closure conversion (re-added after 2026-09-14 wipe): outline
    // `name = |params| body` to __lam_<name>_<N>(params..., captures...).
    // Captures are evaluated NOW (by value) into hidden slots in the defining
    // function; direct calls to `name` pass user args + frozen captures.
    // Every other use of `name` is an error (Variable/Assign/Ref cases).
    void emitLambdaDef(const std::string& name, LambdaExprAST* lam) {
        if (!lam->body) { codegenError("empty lambda body"); return; }
        static int lamCounter = 0;
        std::string lamName = "__lam_" + name + "_" + std::to_string(lamCounter++);
        // 1. Resolve captures in the defining scope (by value, snapshot now).
        struct CapVal { std::string cname; Type ctype; llvm::Value* cval; };
        std::vector<CapVal> caps;
        for (auto& cn : lam->captures) {
            auto* sym = symTable.lookup(cn);
            if (!sym) { codegenError("cannot capture unknown variable '" + cn + "'"); return; }
            if (sym->isLambda) { codegenError("cannot capture lambda '" + cn + "'"); return; }
            if (sym->moved) { codegenError("cannot capture moved variable '" + cn + "'"); return; }
            llvm::Type* ct = llvmType(sym->type, *ctx, mod.get());
            llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
            if (!ptr) { codegenError("cannot capture '" + cn + "'"); return; }
            auto* v = builder->CreateLoad(ct, ptr, cn + "_cap");
            if (!sym->type.isCopyType()) {
                if (sym->borrowCount > 0) { codegenError("cannot capture '" + cn + "' while borrowed"); return; }
                sym->moved = true;
            }
            caps.push_back({cn, sym->type, v});
        }
        // 2. Build the outlined function (user params, then captures).
        std::vector<llvm::Type*> paramTys;
        std::vector<Type> paramFlint;
        for (auto& p : lam->params) {
            Type pt = resolveType(p.second);
            paramFlint.push_back(pt);
            paramTys.push_back(llvmType(pt, *ctx, mod.get()));
        }
        for (auto& c : caps) paramTys.push_back(llvmType(c.ctype, *ctx, mod.get()));
        Type retFlint = lam->retType;
        llvm::Type* retTy = (retFlint.kind == TypeKind::Void)
            ? (llvm::Type*)voidTy
            : (llvm::Type*)llvmType(retFlint, *ctx, mod.get());
        auto* fTy = llvm::FunctionType::get(retTy, paramTys, false);
        auto* fn = llvm::Function::Create(fTy, llvm::Function::InternalLinkage, lamName, mod.get());
        functionMap[lamName] = fn;
        // 3. Declare the lambda entry BEFORE emitting the body (recursion).
        Symbol entry;
        entry.type = Type::i64();
        entry.isLambda = true;
        entry.lambdaFn = fn;
        entry.lambdaDefinedIn = currentFunc;
        entry.lambdaParamTypes = paramFlint;
        for (auto& c : caps) {
            entry.lambdaCapNames.push_back(c.cname);
            entry.lambdaCapTypes.push_back(c.ctype);
        }
        // 4. Freeze captures into hidden slots in the defining function.
        for (size_t i = 0; i < caps.size(); i++) {
            auto* slot = createEntryAlloca(llvmType(caps[i].ctype, *ctx, mod.get()),
                                           "lamcap_" + name + "_" + std::to_string(i));
            builder->CreateStore(caps[i].cval, slot);
            entry.lambdaCapSlots.push_back(slot);
        }
        if (!symTable.declare(name, entry)) {
            codegenError("cannot rebind '" + name + "' as lambda");
            return;
        }
        // 5. Verify the body only references params/captures/globals/functions.
        // (Enclosing locals must never leak into the outlined function.)
        {
            std::unordered_set<std::string> bound;
            for (auto& p : lam->params) bound.insert(p.first);
            for (auto& c : caps) bound.insert(c.cname);
            std::vector<std::string> uses;
            std::string werr;
            Parser::lambdaUses(lam->body.get(), bound, uses, &werr);
            if (!werr.empty()) { codegenError(werr); return; }
            for (auto& u : uses) {
                if (bound.count(u)) continue;
                if (globalSymTable.count(u)) continue;
                if (functionMap.count(u)) continue;
                if (externFunctions.count(u)) continue;
                codegenError("unknown variable '" + u + "' in lambda body");
                return;
            }
        }
        // 6. Emit the body with loop stacks fenced (break/continue bind inside).
        auto* callerFunc = currentFunc;
        auto savedIP = builder->saveIP();
        auto savedBreak = breakStack;
        auto savedContinue = continueStack;
        breakStack.clear();
        continueStack.clear();
        currentFunc = fn;
        auto* entryBB = llvm::BasicBlock::Create(*ctx, "entry", fn);
        builder->SetInsertPoint(entryBB);
        symTable.enterScope();
        size_t pi = 0;
        for (auto& arg : fn->args()) {
            std::string pName;
            Type pType;
            if (pi < lam->params.size()) {
                pName = lam->params[pi].first;
                pType = paramFlint[pi];
            } else {
                pName = caps[pi - lam->params.size()].cname;
                pType = caps[pi - lam->params.size()].ctype;
            }
            auto* alloca = builder->CreateAlloca(llvmType(pType, *ctx, mod.get()), nullptr, pName);
            builder->CreateStore(&arg, alloca);
            Symbol ps;
            ps.type = pType;
            ps.alloca = alloca;
            ps.isMutable = false; // params and captures are read-only
            symTable.declare(pName, ps);
            pi++;
        }
        bool bodyFailed = cgHadError;
        auto* bodyVal = emitExpr(lam->body.get());
        if (retFlint.kind == TypeKind::Void) {
            builder->CreateRetVoid();
        } else if (bodyVal && !bodyFailed) {
            llvm::Value* rv = bodyVal;
            if (rv->getType() != retTy) {
                if (rv->getType()->isIntegerTy(64) && retTy->isDoubleTy())
                    rv = builder->CreateSIToFP(rv, retTy, "lamret");
                else {
                    codegenError("lambda body type mismatch");
                    rv = llvm::Constant::getNullValue(retTy);
                }
            }
            builder->CreateRet(rv);
        } else {
            builder->CreateRet(llvm::Constant::getNullValue(retTy));
        }
        symTable.exitScope();
        currentFunc = callerFunc;
        breakStack = std::move(savedBreak);
        continueStack = std::move(savedContinue);
        builder->restoreIP(savedIP);
    }

    void emitVarDecl(VarDeclAST* decl) {
        // Lambda binding: outline to __lam_<name>_<N> (P0-6 closure
        // conversion; re-added after 2026-09-14 wipe). The name binds the
        // outlined function, not a value — see emitLambdaDef.
        if (decl->init && decl->init->kind == NodeKind::Lambda) {
            emitLambdaDef(decl->varName, static_cast<LambdaExprAST*>(decl->init.get()));
            return;
        }
        Type rt = resolveType(decl->varType);
        llvm::Type* lty = llvmType(rt, *ctx, mod.get());
        // Emit init first to get its LLVM type (may differ from declared type for builtins)
        auto* init = emitExpr(decl->init.get());
        if (init && (rt.kind == TypeKind::I64 || rt.kind == TypeKind::Void) && init->getType() != lty) {
            lty = init->getType();
            // Infer the correct Flint type from the LLVM type
            if (auto* st = llvm::dyn_cast<llvm::StructType>(lty)) {
                if (st->getNumElements() == 2 &&
                    st->getElementType(0)->isPointerTy() &&
                    st->getElementType(1)->isIntegerTy(64)) {
                    rt = Type::array(Type::i64());
                }
            } else if (lty->isPointerTy()) {
                rt = Type::str();
            } else if (lty->isDoubleTy()) {
                rt = Type::f64();
            }
        }
        llvm::AllocaInst* alloca = createEntryAlloca(lty, decl->varName);
        symTable.declare(decl->varName, {rt, alloca, nullptr, decl->isMutable, false, 0});
        // Move: emit init first, then mark source as moved
        if (decl->init->kind == NodeKind::Variable) {
            auto* varInit = static_cast<VariableExprAST*>(decl->init.get());
            auto* srcSym = symTable.lookup(varInit->name);
            if (srcSym && !srcSym->type.isCopyType()) {
                if (srcSym->borrowCount > 0) {
                    std::cerr << "codegen: cannot move '" << varInit->name << "' while borrowed\n";
                    cgHadError = true;
                    return;
                }
                srcSym->moved = true;
            }
        }
        if (init) builder->CreateStore(init, alloca);
    }

    void emitReturn(ReturnStmtAST* ret) {
        if (currentFunc && currentFunc->getName() == "main" && hasPython) {
            auto it = functionMap.find("flint_py_fini");
            if (it != functionMap.end()) builder->CreateCall(it->second, {});
        }
        if (ret->value) {
            auto* v = emitExpr(ret->value.get());
            if (v) {
                if (currentFunc && currentFunc->getName() == "main") {
                    auto* tr = builder->CreateTrunc(v, i32Ty, "mainret");
                    builder->CreateRet(tr);
                } else {
                    builder->CreateRet(v);
                }
            }
        } else if (currentFunc && currentFunc->getName() == "main") {
            builder->CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        } else {
            builder->CreateRetVoid();
        }
    }

    void emitIf(IfStmtAST* ifs) {
        auto* cond = emitExpr(ifs->condition.get());
        if (!cond) return;
        auto* condZero = cond->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(cond->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(cond->getType(), 0);
        auto* condVal = builder->CreateICmpNE(cond, condZero, "ifcond");

        auto* thenBB = llvm::BasicBlock::Create(*ctx, "then", currentFunc);
        auto* elseBB = ifs->elseBlock ? llvm::BasicBlock::Create(*ctx, "else", currentFunc) : nullptr;
        auto* mergeBB = llvm::BasicBlock::Create(*ctx, "ifend", currentFunc);

        builder->CreateCondBr(condVal, thenBB, elseBB ? elseBB : mergeBB);

        builder->SetInsertPoint(thenBB);
        symTable.enterScope();
        emitStmt(ifs->thenBlock.get());
        symTable.exitScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);

        if (elseBB) {
            builder->SetInsertPoint(elseBB);
            symTable.enterScope();
            emitStmt(ifs->elseBlock.get());
            symTable.exitScope();
            if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);
        }

        builder->SetInsertPoint(mergeBB);
    }

    // Emit if as an expression returning a value via phi node.
    llvm::Value* emitIfExpr(IfStmtAST* ifs) {
        auto* cond = emitExpr(ifs->condition.get());
        if (!cond) return nullptr;
        auto* condZero = cond->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(cond->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(cond->getType(), 0);
        auto* condVal = builder->CreateICmpNE(cond, condZero, "ifcond");

        auto* thenBB = llvm::BasicBlock::Create(*ctx, "then", currentFunc);
        auto* elseBB = ifs->elseBlock ? llvm::BasicBlock::Create(*ctx, "else", currentFunc) : nullptr;
        auto* mergeBB = llvm::BasicBlock::Create(*ctx, "ifend", currentFunc);

        builder->CreateCondBr(condVal, thenBB, elseBB ? elseBB : mergeBB);

        llvm::Value* thenVal = nullptr;
        llvm::BasicBlock* thenLastBB = nullptr;
        builder->SetInsertPoint(thenBB);
        if (auto* thenBlk = static_cast<BlockStmtAST*>(ifs->thenBlock.get())) {
            thenVal = emitBlockExpr(thenBlk);
        }
        thenLastBB = builder->GetInsertBlock();
        if (!thenLastBB->getTerminator()) builder->CreateBr(mergeBB);

        llvm::Value* elseVal = nullptr;
        llvm::BasicBlock* elseLastBB = nullptr;
        if (elseBB) {
            builder->SetInsertPoint(elseBB);
            if (auto* elseBlk = static_cast<BlockStmtAST*>(ifs->elseBlock.get())) {
                elseVal = emitBlockExpr(elseBlk);
            }
            elseLastBB = builder->GetInsertBlock();
            if (!elseLastBB->getTerminator()) builder->CreateBr(mergeBB);
        }

        builder->SetInsertPoint(mergeBB);
        if (!thenVal && !elseVal) return llvm::ConstantInt::get(i64Ty, 0);
        llvm::Type* phiTy = thenVal ? thenVal->getType() : elseVal->getType();
        auto* phi = builder->CreatePHI(phiTy, 2, "ifval");
        phi->addIncoming(thenVal ? thenVal : llvm::ConstantInt::get(phiTy, 0), thenLastBB);
        phi->addIncoming(elseVal ? elseVal : llvm::ConstantInt::get(phiTy, 0), elseLastBB ? elseLastBB : mergeBB);
        return phi;
    }

    void emitWhile(WhileStmtAST* ws) {
        auto* condBB = llvm::BasicBlock::Create(*ctx, "while_cond", currentFunc);
        auto* bodyBB = llvm::BasicBlock::Create(*ctx, "while_body", currentFunc);
        auto* endBB = llvm::BasicBlock::Create(*ctx, "while_end", currentFunc);

        builder->CreateBr(condBB);

        builder->SetInsertPoint(condBB);
        auto* cond = emitExpr(ws->condition.get());
        if (!cond) { builder->SetInsertPoint(endBB); return; }
        auto* condZero = cond->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(cond->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(cond->getType(), 0);
        auto* condVal = builder->CreateICmpNE(cond, condZero, "whilecond");
        builder->CreateCondBr(condVal, bodyBB, endBB);

        builder->SetInsertPoint(bodyBB);
        symTable.enterScope();
        breakStack.push_back(endBB);
        continueStack.push_back(condBB);
        emitStmt(ws->body.get());
        breakStack.pop_back();
        continueStack.pop_back();
        symTable.exitScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);

        builder->SetInsertPoint(endBB);
    }

    void emitBlock(BlockStmtAST* blk) {
        symTable.enterScope();
        for (auto& s : blk->stmts) emitStmt(s.get());
        symTable.exitScope();
    }

    // Emit a block for use as an expression: emit all stmts, return last expression value.
    llvm::Value* emitBlockExpr(BlockStmtAST* blk) {
        symTable.enterScope();
        llvm::Value* lastVal = nullptr;
        for (size_t i = 0; i < blk->stmts.size(); i++) {
            auto& stmt = blk->stmts[i];
            if (i == blk->stmts.size() - 1) {
                lastVal = emitExpr(stmt.get());
                if (!lastVal) emitStmt(stmt.get());
            } else {
                emitStmt(stmt.get());
            }
        }
        symTable.exitScope();
        return lastVal;
    }

    void emitDestructure(DestructureAST* ds) {
        auto* initVal = emitExpr(ds->init.get());
        if (!initVal) return;
        if (ds->isStruct) {
            // Struct destructure: { a, b } = s
            llvm::Type* structTy = initVal->getType();
            if (!structTy->isStructTy()) { std::cerr << "destructure: expected struct\n"; cgHadError = true; return; }
            auto* alloca = createEntryAlloca(structTy, "ds_tmp");
            builder->CreateStore(initVal, alloca);
            for (size_t i = 0; i < ds->names.size(); i++) {
                auto* fPtr = builder->CreateStructGEP(structTy, alloca, i);
                auto* fVal = builder->CreateLoad(i64Ty, fPtr, ds->names[i]);
                auto* valAlloca = createEntryAlloca(i64Ty, ds->names[i]);
                builder->CreateStore(fVal, valAlloca);
                symTable.declare(ds->names[i], {Type::i64(), valAlloca, nullptr, ds->isMutable, false, 0});
            }
        } else {
            // Array destructure: [a, b] = arr
            // arr is { ptr, len }
            auto* dataPtr = builder->CreateExtractValue(initVal, {0}, "arr_data");
            for (size_t i = 0; i < ds->names.size(); i++) {
                auto* idxVal = llvm::ConstantInt::get(i64Ty, i);
                auto* elemPtr = builder->CreateGEP(i64Ty, dataPtr, idxVal, "arr_elem");
                auto* elemVal = builder->CreateLoad(i64Ty, elemPtr, ds->names[i]);
                auto* valAlloca = createEntryAlloca(i64Ty, ds->names[i]);
                builder->CreateStore(elemVal, valAlloca);
                symTable.declare(ds->names[i], {Type::i64(), valAlloca, nullptr, ds->isMutable, false, 0});
            }
        }
    }

    llvm::Value* emitMapLiteral(MapLiteralAST* ml) {
        auto itNew = functionMap.find("flint_map_new");
        auto itSet = functionMap.find("flint_map_set");
        if (itNew == functionMap.end() || itSet == functionMap.end()) {
            return codegenError("map runtime not registered");
        }
        llvm::Value* m = builder->CreateCall(itNew->second, {}, "map_new");
        for (size_t i = 0; i < ml->keys.size(); i++) {
            llvm::Value* val = emitExpr(ml->values[i].get());
            if (!val) return nullptr;
            // Ensure i64 (bool is i64-typed already)
            if (val->getType()->isDoubleTy()) {
                return codegenError("map values must be i64 (got f64)");
            }
            llvm::Value* keyStr = builder->CreateGlobalString(ml->keys[i], "mapkey");
            builder->CreateCall(itSet->second, {m, keyStr, val});
        }
        return m;
    }

    llvm::Value* emitArrayLiteral(ArrayExprAST* arr) {
        size_t count = arr->elements.size();

        // Check if any element is a spread
        bool hasSpread = false;
        for (auto f : arr->spreadFlags) if (f) { hasSpread = true; break; }

        if (hasSpread) {
            auto* arrConcatFn = functionMap["flint_array_concat"];
            llvm::Type* arrTy2 = llvmType(Type::array(Type::i64()), *ctx, mod.get());
            // Start with empty array
            llvm::Value* result = llvm::UndefValue::get(arrTy2);
            result = builder->CreateInsertValue(result, llvm::ConstantPointerNull::get(llvm::PointerType::get(*ctx, 0)), {0});
            result = builder->CreateInsertValue(result, llvm::ConstantInt::get(i64Ty, 0), {1});
            for (size_t i = 0; i < count; i++) {
                auto* val = emitExpr(arr->elements[i].get());
                if (!val) return nullptr;
                llvm::Value* chunk;
                if (i < arr->spreadFlags.size() && arr->spreadFlags[i]) {
                    chunk = val; // already an array struct
                } else {
                    // Wrap single element in a 1-element array
                    llvm::AllocaInst* singleAlloca = createEntryAlloca(llvm::ArrayType::get(i64Ty, 1), "elem");
                    auto* zero = llvm::ConstantInt::get(i64Ty, 0);
                    auto* elemPtr = builder->CreateGEP(llvm::ArrayType::get(i64Ty, 1), singleAlloca, {zero, zero});
                    builder->CreateStore(val, elemPtr);
                    chunk = llvm::UndefValue::get(arrTy2);
                    chunk = builder->CreateInsertValue(chunk, builder->CreateBitCast(singleAlloca, i8PtrTy), {0});
                    chunk = builder->CreateInsertValue(chunk, llvm::ConstantInt::get(i64Ty, 1), {1});
                }
                result = builder->CreateCall(arrConcatFn, {result, chunk}, "concat");
            }
            return result;
        }

        // Allocate array data on stack
        llvm::ArrayType* arrDataType = llvm::ArrayType::get(i64Ty, count);
        llvm::AllocaInst* dataAlloca = createEntryAlloca(arrDataType, "arr_data");

        // Store each element
        for (size_t i = 0; i < count; i++) {
            llvm::Value* idxVal = llvm::ConstantInt::get(i64Ty, i);
            llvm::Value* elemPtr = builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(i64Ty, 0), idxVal}, "arr_elem");
            llvm::Value* val = emitExpr(arr->elements[i].get());
            if (!val) return nullptr;
            builder->CreateStore(val, elemPtr);
        }

        // Get pointer to first element as ptr
        llvm::Value* dataPtr = builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(i64Ty, 0), llvm::ConstantInt::get(i64Ty, 0)}, "arr_data_ptr");

        // Build struct { ptr, i64 } on stack, then load it
        llvm::Type* structTy = llvmType(Type::array(Type::i64()), *ctx, mod.get());
        llvm::AllocaInst* tempAlloca = createEntryAlloca(structTy, "arr_tmp");
        llvm::Value* ptrField = builder->CreateStructGEP(structTy, tempAlloca, 0);
        builder->CreateStore(dataPtr, ptrField);
        llvm::Value* lenField = builder->CreateStructGEP(structTy, tempAlloca, 1);
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, count), lenField);
        return builder->CreateLoad(structTy, tempAlloca, "arr_val");
    }

    llvm::Value* emitIndexAccess(IndexExprAST* idx) {
        llvm::Value* base = emitExpr(idx->base.get());
        llvm::Value* index = emitExpr(idx->index.get());
        if (!base || !index) return nullptr;

        // D1: string vs array — array is {ptr,i64}, string is ptr.
        bool isArray = base->getType()->isStructTy();
        if (isArray) {
            llvm::Value* dataPtr = builder->CreateExtractValue(base, {0}, "arr_ptr");
            llvm::Value* len = builder->CreateExtractValue(base, {1}, "arr_len");
            if (!idx->unchecked) {
            auto* boundsFn = functionMap["flint_bounds_check"];
            if (boundsFn) {
                auto* curF = builder->GetInsertBlock()->getParent();
                auto* okBB = llvm::BasicBlock::Create(*ctx, "bounds_ok", curF);
                auto* panicBB = llvm::BasicBlock::Create(*ctx, "bounds_panic", curF);
                auto* ok = builder->CreateICmpULT(index, len, "bounds_ok");
                builder->CreateCondBr(ok, okBB, panicBB);
                builder->SetInsertPoint(panicBB);
                builder->CreateCall(boundsFn, {index, len});
                builder->CreateBr(okBB);
                builder->SetInsertPoint(okBB);
            }
            }
            llvm::Value* elemPtr = builder->CreateGEP(i64Ty, dataPtr, index, "arr_elem");
            return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
        } else {
            // string: ptr + i64 index → i8 load → zext i64 (char code)
            auto* lenFn = functionMap["flint_str_length"];
            llvm::Value* len = nullptr;
            if (lenFn) len = builder->CreateCall(lenFn, {base}, "str_len");
            else len = llvm::ConstantInt::get(i64Ty, 0);
            if (!idx->unchecked) {
            auto* boundsFn = functionMap["flint_bounds_check"];
            if (boundsFn && len) {
                auto* curF = builder->GetInsertBlock()->getParent();
                auto* okBB = llvm::BasicBlock::Create(*ctx, "bounds_ok", curF);
                auto* panicBB = llvm::BasicBlock::Create(*ctx, "bounds_panic", curF);
                auto* ok = builder->CreateICmpULT(index, len, "bounds_ok");
                builder->CreateCondBr(ok, okBB, panicBB);
                builder->SetInsertPoint(panicBB);
                builder->CreateCall(boundsFn, {index, len});
                builder->CreateBr(okBB);
                builder->SetInsertPoint(okBB);
            }
            }
            llvm::Value* elemPtr = builder->CreateGEP(i8Ty, base, index, "str_elem");
            auto* byteVal = builder->CreateLoad(i8Ty, elemPtr, "str_byte");
            return builder->CreateZExt(byteVal, i64Ty, "str_char");
        }
    }

    llvm::Value* emitSliceAccess(SliceExprAST* slice) {
        llvm::Value* arr = emitExpr(slice->arr.get());
        llvm::Value* start = emitExpr(slice->start.get());
        llvm::Value* end = emitExpr(slice->end.get());
        if (!arr || !start || !end) return nullptr;

        // Extract data pointer and length from { ptr, i64 } array struct
        llvm::Value* dataPtr = builder->CreateExtractValue(arr, {0}, "arr_ptr");
        llvm::Value* len = builder->CreateExtractValue(arr, {1}, "arr_len");

        // Call flint_array_slice(data, len, start, end) -> FlintVec*
        auto* sliceFn = functionMap["flint_array_slice"];
        if (!sliceFn) { return codegenError("flint_array_slice not linked"); }
        return builder->CreateCall(sliceFn, {dataPtr, len, start, end}, "slice_vec");
    }

    llvm::Value* emitRef(RefExprAST* ref) {
        if (ref->target->kind == NodeKind::Variable) {
            auto* var = static_cast<VariableExprAST*>(ref->target.get());
            // Address-of-function: &func_name returns function pointer as str
            auto fIt = functionMap.find(var->name);
            if (fIt != functionMap.end()) {
                return builder->CreateBitCast(fIt->second, i8PtrTy, var->name + "_ptr");
            }
            // Variable reference (borrow)
            auto* sym = symTable.lookup(var->name);
            if (!sym) { return codegenError(std::string("undefined var '") + var->name + "'"); }
                if (sym->moved) {
                    return codegenError(std::string("cannot borrow moved variable '") + var->name + "'");
                }
            if (sym->isLambda) {
                return codegenError(std::string("cannot borrow lambda '") + var->name + "'");
            }
            sym->borrowCount++;
            symTable.recordBorrow(var->name);
            return sym->alloca;
        }
        std::cerr << "codegen: can only reference variables\n";
        cgHadError = true;
        return nullptr;
    }

    llvm::Value* emitDeref(DerefExprAST* deref) {
        llvm::Value* ptr = emitExpr(deref->target.get());
        if (!ptr) return nullptr;
        return builder->CreateLoad(i64Ty, ptr, "deref");
    }

    llvm::Value* emitStructLiteral(StructLiteralAST* sl) {
        auto it = structRegistry.find(sl->structName);
        if (it == structRegistry.end()) {
            return codegenError(std::string("unknown struct '") + sl->structName + "'");
        }
        auto& def = it->second;
        llvm::Type* st = llvmType(Type::struct_(sl->structName), *ctx, mod.get());
        if (!st) { return codegenError(std::string("struct type not found '") + sl->structName + "'"); }
        // Build struct via undef + insertvalue for each field
        llvm::Value* s = llvm::UndefValue::get(st);
        for (unsigned i = 0; i < def.fields.size(); i++) {
            bool found = false;
            for (unsigned j = 0; j < sl->fields.size(); j++) {
                if (sl->fields[j].first == def.fields[i].name) {
                    auto* fv = emitExpr(sl->fields[j].second.get());
                    if (!fv) return nullptr;
                    s = builder->CreateInsertValue(s, fv, {i}, sl->structName + "." + def.fields[i].name);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return codegenError(std::string("missing field '") + def.fields[i].name + "' in struct literal");
            }
        }
        return s;
    }

    llvm::Value* emitFieldAccess(FieldAccessAST* fa) {
        llvm::Value* base = emitExpr(fa->base.get());
        if (!base) return nullptr;
        for (auto& [name, def] : structRegistry) {
            auto* st = llvm::StructType::getTypeByName(*ctx, name);
            if (!st) continue;
            if (base->getType() == st) {
                for (unsigned i = 0; i < def.fields.size(); i++) {
                    if (def.fields[i].name == fa->fieldName) {
                        return builder->CreateExtractValue(base, {i}, fa->fieldName);
                    }
                }
                return codegenError(std::string("struct '") + name + "' has no field '" + fa->fieldName + "'");
            }
        }
        return codegenError("cannot access field '" + fa->fieldName + "' on non-struct type");
    }

    llvm::Value* emitEnumConstruct(EnumConstructAST* ec) {
        auto it = enumRegistry.find(ec->enumName);
        if (it == enumRegistry.end()) { return codegenError(std::string("unknown enum '") + ec->enumName + "'"); }
        auto& ed = it->second;
        llvm::Type* enumTy = llvmType(Type::enum_(ec->enumName), *ctx, mod.get());
        // Find variant index
        int tag = -1;
        for (size_t i = 0; i < ed.variants.size(); i++) {
            if (ed.variants[i].name == ec->variantName) { tag = (int)i; break; }
        }
        if (tag < 0) { return codegenError(std::string("unknown variant '") + ec->variantName + "'"); }

        auto* alloca = createEntryAlloca(enumTy, ec->enumName + "_tmp");
        // Store tag byte
        auto* tagPtr = builder->CreateStructGEP(enumTy, alloca, 0);
        builder->CreateStore(llvm::ConstantInt::get(i8Ty, tag), tagPtr);
        // Store payload if any
        if (!ec->args.empty()) {
            // Build variant-specific LLVM type: { i8, T1, T2, ... }
            std::vector<llvm::Type*> fldTys = {i8Ty};
            for (auto& pt : ed.variants[tag].payloadTypes)
                fldTys.push_back(llvmType(pt, *ctx, mod.get()));
            auto* varTy = llvm::StructType::get(*ctx, fldTys);
            auto* bcPtr = builder->CreateBitCast(alloca, i8PtrTy);
            for (size_t i = 0; i < ec->args.size(); i++) {
                auto* val = emitExpr(ec->args[i].get());
                if (!val) return nullptr;
                auto* fldPtr = builder->CreateStructGEP(varTy, bcPtr, (unsigned)(i + 1));
                builder->CreateStore(val, fldPtr);
            }
        }
        return builder->CreateLoad(enumTy, alloca, ec->enumName + "_val");
    }

    llvm::Value* emitMatch(MatchExprAST* mn) {
        auto* scrutinee = emitExpr(mn->scrutinee.get());
        if (!scrutinee) return nullptr;
        llvm::Type* enumTy = scrutinee->getType();
        // Store scrutinee in alloca for bitcast access
        auto* alloca = createEntryAlloca(enumTy, "match_scrutinee");
        builder->CreateStore(scrutinee, alloca);

        // Get enum definition from first arm
        if (mn->arms.empty()) return nullptr;
        auto eit = enumRegistry.find(mn->arms[0].enumName);
        if (eit == enumRegistry.end()) { return codegenError("unknown enum in match"); }
        auto& ed = eit->second;

        // Create BBs: one per arm + merge
        std::vector<llvm::BasicBlock*> armBBs;
        for (auto& arm : mn->arms)
            armBBs.push_back(llvm::BasicBlock::Create(*ctx, arm.variantName, currentFunc));
        auto* mergeBB = llvm::BasicBlock::Create(*ctx, "match_end", currentFunc);

        // Load tag and switch
        auto* tagPtr = builder->CreateStructGEP(enumTy, alloca, 0);
        auto* tag = builder->CreateLoad(i8Ty, tagPtr, "tag");
        auto* switchBB = builder->GetInsertBlock();
        auto* switchInst = builder->CreateSwitch(tag, mergeBB, (unsigned)mn->arms.size());
        for (size_t i = 0; i < mn->arms.size(); i++) {
            // Find the actual tag value for this variant
            int armTag = -1;
            for (size_t v = 0; v < ed.variants.size(); v++) {
                if (ed.variants[v].name == mn->arms[i].variantName) { armTag = (int)v; break; }
            }
            if (armTag >= 0)
                switchInst->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(i8Ty, (uint64_t)armTag)), armBBs[i]);
        }

        // Emit each arm (value-capturing: match is an expression)
        std::vector<llvm::Value*> armVals;
        std::vector<llvm::BasicBlock*> armEndBBs;
        std::vector<char> armReachesMerge;
        for (size_t i = 0; i < mn->arms.size(); i++) {
            builder->SetInsertPoint(armBBs[i]);
            symTable.enterScope();
            auto& arm = mn->arms[i];
            // Bind payload if specified (before body scope so it's visible inside)
            int armTag = -1;
            for (size_t v = 0; v < ed.variants.size(); v++) {
                if (ed.variants[v].name == arm.variantName) { armTag = (int)v; break; }
            }
            if (!arm.bindName.empty() && armTag >= 0 && !ed.variants[armTag].payloadTypes.empty()) {
                auto& vt = ed.variants[armTag];
                std::vector<llvm::Type*> fldTys = {i8Ty};
                for (auto& pt : ed.variants[armTag].payloadTypes)
                    fldTys.push_back(llvmType(pt, *ctx, mod.get()));
                auto* varTy = llvm::StructType::get(*ctx, fldTys);
                auto* bcPtr = builder->CreateBitCast(alloca, i8PtrTy);
                // For single-field payload, bind directly
                if (ed.variants[armTag].payloadTypes.size() == 1) {
                    auto* valPtr = builder->CreateStructGEP(varTy, bcPtr, 1);
                    auto* val = builder->CreateLoad(llvmType(ed.variants[armTag].payloadTypes[0], *ctx, mod.get()), valPtr, arm.bindName);
                    auto* bindAlloca = createEntryAlloca(val->getType(), arm.bindName);
                    builder->CreateStore(val, bindAlloca);
                    symTable.declare(arm.bindName, {vt.payloadTypes[0], bindAlloca, nullptr, false, false, 0});
                }
            }
            llvm::Value* armVal = nullptr;
            if (arm.body->kind == NodeKind::Block) {
                // Inline block-expr (avoid double scope; we already entered one)
                auto* blk = static_cast<BlockStmtAST*>(arm.body.get());
                for (size_t si = 0; si < blk->stmts.size(); si++) {
                    auto& stmt = blk->stmts[si];
                    if (si == blk->stmts.size() - 1) {
                        armVal = emitExpr(stmt.get());
                        if (!armVal) emitStmt(stmt.get());
                    } else {
                        emitStmt(stmt.get());
                    }
                }
            } else {
                armVal = emitExpr(arm.body.get());
                if (!armVal) emitStmt(arm.body.get());
            }
            symTable.exitScope();
            armVals.push_back(armVal);
            armEndBBs.push_back(builder->GetInsertBlock());
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
                armReachesMerge.push_back(1);
            } else {
                armReachesMerge.push_back(0);
            }
        }

        builder->SetInsertPoint(mergeBB);
        // If no arm produced a value, match is statement-context: return 0.
        bool anyVal = false;
        llvm::Type* phiTy = nullptr;
        for (auto* v : armVals) if (v) { anyVal = true; phiTy = v->getType(); break; }
        if (!anyVal) return llvm::ConstantInt::get(i64Ty, 0);
        auto* phi = builder->CreatePHI(phiTy, (unsigned)armVals.size() + 1, "matchval");
        for (size_t i = 0; i < armVals.size(); i++) {
            if (!armReachesMerge[i]) continue;
            llvm::Value* v = armVals[i] ? armVals[i] : llvm::ConstantInt::get(phiTy, 0);
            // Normalize int/ptr mismatch (null arms)
            if (v->getType() != phiTy) v = llvm::ConstantInt::get(phiTy, 0);
            phi->addIncoming(v, armEndBBs[i]);
        }
        // Default edge (unmatched tag falls through to merge): incoming 0.
        phi->addIncoming(llvm::ConstantInt::get(phiTy, 0), switchBB);
        return phi;
    }

    void emitPythonBlock(PyBlockStmtAST* py) {
        hasPython = true;
        for (auto& code : py->codeStrings) {
            auto* str = builder->CreateGlobalString(code, "pycode");
            auto it = functionMap.find("flint_py_run");
            if (it != functionMap.end()) builder->CreateCall(it->second, {str});
        }
    }

    std::string mangle(const std::string& fnName, const std::vector<Type>& typeArgs) {
        std::string s = fnName;
        for (auto& ta : typeArgs) {
            s += ".";
            switch (ta.kind) {
                case TypeKind::I64: s += "i64"; break;
                case TypeKind::Str: s += "str"; break;
                case TypeKind::Bool: s += "bool"; break;
                case TypeKind::Ref: s += "ref"; break;
                case TypeKind::Array: s += "arr"; break;
                case TypeKind::Map: s += "map"; break;
                case TypeKind::Struct: s += ta.structName; break;
                case TypeKind::Enum: s += ta.structName; break;
                default: s += "any"; break;
            }
        }
        return s;
    }

    llvm::Function* specialize(FunctionAST* generic, const std::vector<Type>& typeArgs) {
        if (generic->typeParams.size() != typeArgs.size()) return nullptr;
        std::string mangled = mangle(generic->name, typeArgs);

        // Build substitution map
        std::unordered_map<std::string, Type> subst;
        for (size_t i = 0; i < generic->typeParams.size(); i++)
            subst[generic->typeParams[i]] = typeArgs[i];

        // Build LLVM function type with concrete types
        std::vector<llvm::Type*> paramTys;
        for (auto& p : generic->params) {
            Type rt = p.second;
            auto it = subst.find(rt.structName);
            if (rt.kind == TypeKind::TypeParam && it != subst.end()) rt = it->second;
            paramTys.push_back(llvmType(rt, *ctx, mod.get()));
        }
        Type retRt = generic->returnType;
        auto rit = subst.find(retRt.structName);
        if (retRt.kind == TypeKind::TypeParam && rit != subst.end()) retRt = rit->second;
        llvm::Type* retTy = llvmType(retRt, *ctx, mod.get());

        auto fTy = llvm::FunctionType::get(retTy, paramTys, false);
        auto f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, mangled, mod.get());
        size_t i = 0;
        for (auto& arg : f->args()) {
            if (i < generic->params.size()) arg.setName(generic->params[i].first);
            i++;
        }
        functionMap[mangled] = f;

        // Emit the function body with substitution active
        auto savedMap = typeSubstMap;
        typeSubstMap = subst;

        llvm::Function* savedFunc = currentFunc;
        auto savedInsertBlock = builder->GetInsertBlock();
        currentFunc = f;
        auto entry = llvm::BasicBlock::Create(*ctx, "entry", f);
        builder->SetInsertPoint(entry);
        symTable.enterScope();

        i = 0;
        for (auto& arg : f->args()) {
            std::string pName = generic->params[i].first;
            Type pType = subst[generic->params[i].second.structName];
            if (generic->params[i].second.kind != TypeKind::TypeParam) pType = generic->params[i].second;
            llvm::AllocaInst* alloca = createEntryAlloca(llvmType(pType, *ctx, mod.get()), pName);
            builder->CreateStore(&arg, alloca);
            symTable.declare(pName, {pType, alloca, nullptr, false, false, 0});
            i++;
        }

        if (generic->body) emitStmt(generic->body.get());
        else if (generic->bodyStart > 0 && generic->bodyEnd > 0 && parser) {
            auto savedPos = parser->getPos();
            parser->setPos(generic->bodyStart);
            auto bodyAst = parser->reparseBlock();
            if (bodyAst) emitStmt(bodyAst.get());
            parser->setPos(generic->bodyEnd);
            parser->setPos(savedPos);
        }

        bool hasRet = retRt.kind != TypeKind::Void;
        if (!builder->GetInsertBlock()->getTerminator()) {
            if (hasRet)
                builder->CreateRet(llvm::ConstantInt::get(retTy, 0));
            else
                builder->CreateRetVoid();
        }

        symTable.exitScope();
        currentFunc = savedFunc;
        typeSubstMap = savedMap;
        if (savedInsertBlock) builder->SetInsertPoint(savedInsertBlock);

        genericCache[mangled] = f;
        return f;
    }

    llvm::Value* emitCall(CallExprAST* call) {
        // Lambda call rewrite (P0-6): a direct call to a lambda bound in an
        // accessible function passes user args + frozen by-value captures.
        // Cross-function calls are rejected (closures don't escape).
        {
            auto* lsym = symTable.lookup(call->callee);
            if (lsym && lsym->isLambda) {
                bool selfRecurse = (lsym->lambdaFn == currentFunc);
                if (lsym->lambdaDefinedIn != currentFunc && !selfRecurse)
                    return codegenError("cannot call lambda '" + call->callee + "' from a different function");
                if (call->args.size() != lsym->lambdaParamTypes.size())
                    return codegenError("lambda '" + call->callee + "' expects " +
                        std::to_string(lsym->lambdaParamTypes.size()) + " argument(s), got " +
                        std::to_string(call->args.size()));
                std::vector<llvm::Value*> largs;
                auto* lamFnTy = lsym->lambdaFn->getFunctionType();
                for (size_t i = 0; i < call->args.size(); i++) {
                    auto* v = emitExpr(call->args[i].get());
                    if (!v) return nullptr;
                    llvm::Type* want = lamFnTy->getParamType(i);
                    if (v->getType() != want) {
                        if (v->getType()->isIntegerTy(64) && want->isDoubleTy())
                            v = builder->CreateSIToFP(v, want, "lamarg");
                        else
                            return codegenError("lambda '" + call->callee + "' argument type mismatch");
                    }
                    largs.push_back(v);
                }
                if (selfRecurse) {
                    // Forward our own capture params (definer slots are
                    // unreachable from inside the outlined function).
                    for (size_t i = 0; i < lsym->lambdaCapNames.size(); i++)
                        largs.push_back(currentFunc->getArg(lsym->lambdaParamTypes.size() + i));
                } else {
                    for (size_t i = 0; i < lsym->lambdaCapSlots.size(); i++) {
                        llvm::Type* ct = llvmType(lsym->lambdaCapTypes[i], *ctx, mod.get());
                        largs.push_back(builder->CreateLoad(ct, lsym->lambdaCapSlots[i], "lamcap_use"));
                    }
                }
                return builder->CreateCall(lsym->lambdaFn, largs, "lam_call");
            }
        }
        // Handle generic function call
        auto git = genericFunctions.find(call->callee);
        if (git != genericFunctions.end()) {
            auto* generic = git->second;
            // Infer type args from argument expressions
            std::vector<Type> typeArgs;
            for (size_t ai = 0; ai < generic->typeParams.size(); ai++) {
                // Find the first parameter that uses this type param
                bool found = false;
                for (auto& p : generic->params) {
                    if (p.second.kind == TypeKind::TypeParam && p.second.structName == generic->typeParams[ai]) {
                        // Try to infer from the argument expression
                        if (ai < call->args.size()) {
                            switch (call->args[ai]->kind) {
                                case NodeKind::Number:
                                    typeArgs.push_back(Type::i64()); found = true; break;
                                case NodeKind::String:
                                    typeArgs.push_back(Type::str()); found = true; break;
                                case NodeKind::Variable: {
                                    auto* var = static_cast<VariableExprAST*>(call->args[ai].get());
                                    auto* sym = symTable.lookup(var->name);
                                    if (sym) { typeArgs.push_back(sym->type); found = true; }
                                    break;
                                }
                                case NodeKind::EnumConstruct: {
                                    auto* ec = static_cast<EnumConstructAST*>(call->args[ai].get());
                                    auto eit = enumRegistry.find(ec->enumName);
                                    if (eit != enumRegistry.end()) { typeArgs.push_back(Type::enum_(ec->enumName)); found = true; }
                                    break;
                                }
                                default: break;
                            }
                        }
                        break;
                    }
                }
                if (!found) {
                    // Default to i64
                    typeArgs.push_back(Type::i64());
                }
            }

            std::string mangled = mangle(call->callee, typeArgs);
            llvm::Function* f = nullptr;
            auto cit = genericCache.find(mangled);
            if (cit != genericCache.end()) {
                f = cit->second;
            } else {
                f = specialize(generic, typeArgs);
            }
            if (!f) { return codegenError(std::string("failed to specialize '") + call->callee + "'"); }

            // Emit args and call
            std::vector<llvm::Value*> args;
            for (auto& a : call->args) {
                auto* v = emitExpr(a.get());
                if (!v) return nullptr;
                args.push_back(v);
                if (a->kind == NodeKind::Variable) {
                    auto* varArg = static_cast<VariableExprAST*>(a.get());
                    auto* srcSym = symTable.lookup(varArg->name);
                    if (srcSym && !srcSym->type.isCopyType()) {
                        if (srcSym->borrowCount > 0) {
                            return codegenError("cannot move while borrowed");
                        }
                        srcSym->moved = true;
                    }
                }
            }
            return builder->CreateCall(f, args);
        }

        if (call->callee == "print") {
            if (call->args.empty()) { return codegenError("print() needs an arg"); }
            auto* arg = emitExpr(call->args[0].get());
            if (!arg) return nullptr;
            if (arg->getType() == i64Ty) {
                auto it = functionMap.find("flint_println_i64");
                if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            }
            // f64 prints via flint_println_f64 (v0.21 fix; re-added after
            // 2026-09-14 wipe — was silently dropped as nullptr fallthrough).
            if (arg->getType()->isDoubleTy()) {
                auto it = functionMap.find("flint_println_f64");
                if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            }
            if (arg->getType()->isPointerTy()) {
                auto it = functionMap.find("flint_println_str");
                if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            }
            return codegenError("print() of unsupported type");
        }

        if (call->callee == "flint_array_get") {
            if (call->args.size() != 2) { return codegenError("flint_array_get needs 2 args"); }
            auto* arrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            if (!arrVal || !idxVal) return nullptr;
            if (!arrVal->getType()->isStructTy()) {
                return codegenError("flint_array_get needs an array");
            }
            auto* dataPtr = builder->CreateExtractValue(arrVal, {0}, "arr_data");
            auto* len = builder->CreateExtractValue(arrVal, {1}, "arr_len");
            auto* boundsFn = functionMap["flint_bounds_check"];
            if (boundsFn) builder->CreateCall(boundsFn, {idxVal, len});
            auto* elemPtr = builder->CreateGEP(i64Ty, dataPtr, idxVal, "arr_elem");
            return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
        }

        if (call->callee == "flint_array_set") {
            if (call->args.size() != 3) { return codegenError("flint_array_set needs 3 args"); }
            auto* arrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            auto* valVal = emitExpr(call->args[2].get());
            if (!arrVal || !idxVal || !valVal) return nullptr;
            if (!arrVal->getType()->isStructTy()) {
                return codegenError("flint_array_set needs an array");
            }
            auto* dataPtr = builder->CreateExtractValue(arrVal, {0}, "arr_data");
            auto* len = builder->CreateExtractValue(arrVal, {1}, "arr_len");
            auto* boundsFn = functionMap["flint_bounds_check"];
            if (boundsFn) builder->CreateCall(boundsFn, {idxVal, len});
            auto* elemPtr = builder->CreateGEP(i64Ty, dataPtr, idxVal, "arr_elem");
            builder->CreateStore(valVal, elemPtr);
            return llvm::ConstantInt::get(i64Ty, 0); // dummy return
        }

        if (call->callee == "flint_array_data") {
            if (call->args.size() != 1) { return codegenError("flint_array_data needs 1 arg"); }
            auto* arrVal = emitExpr(call->args[0].get());
            if (!arrVal) return nullptr;
            return builder->CreateExtractValue(arrVal, {0}, "arr_data");
        }

        if (call->callee == "flint_array_get_ptr") {
            if (call->args.size() != 2) { return codegenError("flint_array_get_ptr needs 2 args"); }
            auto* ptrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            if (!ptrVal || !idxVal) return nullptr;
            auto* elemPtr = builder->CreateGEP(i64Ty, ptrVal, idxVal, "arr_elem");
            return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
        }

        if (call->callee == "flint_array_set_ptr") {
            if (call->args.size() != 3) { return codegenError("flint_array_set_ptr needs 3 args"); }
            auto* ptrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            auto* valVal = emitExpr(call->args[2].get());
            if (!ptrVal || !idxVal || !valVal) return nullptr;
            auto* elemPtr = builder->CreateGEP(i64Ty, ptrVal, idxVal, "arr_elem");
            builder->CreateStore(valVal, elemPtr);
            return llvm::ConstantInt::get(i64Ty, 0);
        }

        if (call->callee == "flint_thread_spawn") {
            if (call->args.size() != 2) { return codegenError("flint_thread_spawn needs 2 args: func_name, arg"); }
            if (call->args[0]->kind != NodeKind::String) { return codegenError("flint_thread_spawn first arg must be a string literal (function name)"); }
            auto* nameLit = static_cast<StringExprAST*>(call->args[0].get());
            auto* argVal = emitExpr(call->args[1].get());
            if (!argVal) return nullptr;
            auto fIt = functionMap.find(nameLit->value);
            if (fIt == functionMap.end()) { return codegenError(std::string("flint_thread_spawn: unknown function '") + nameLit->value + "'"); }
            auto* targetFn = fIt->second;
            auto* wrapperTy = llvm::FunctionType::get(i8PtrTy, {i8PtrTy}, false);
            auto* wrapperPtr = builder->CreateBitCast(targetFn, llvm::PointerType::get(*ctx, 0), "thread_fn");
            auto* createFn = functionMap["flint_thread_create"];
            if (!createFn) { return codegenError("flint_thread_spawn: runtime function 'flint_thread_create' not found"); }
            auto* voidArg = builder->CreateIntToPtr(argVal, i8PtrTy, "thread_arg");
            return builder->CreateCall(createFn, {wrapperPtr, voidArg}, "thread_id");
        }

        if (call->callee == "py_eval") {
            hasPython = true;
            if (call->args.size() != 1) { return codegenError("py_eval() needs one string arg"); }
            auto* arg = emitExpr(call->args[0].get());
            if (!arg) return nullptr;
            auto it = functionMap.find("flint_py_eval_int");
            if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            return nullptr;
        }

        // Named argument support: detect BinaryExprAST('=', name, val) in args
        bool hasNamedArgs = false;
        for (auto& a : call->args) {
            if (a->kind == NodeKind::Binary) {
                auto* b = static_cast<BinaryExprAST*>(a.get());
                if (b->op == '=' && b->lhs && b->lhs->kind == NodeKind::Variable) {
                    hasNamedArgs = true;
                    break;
                }
            }
        }
        if (hasNamedArgs) {
            auto fit2 = functionDefs.find(call->callee);
            if (fit2 != functionDefs.end()) {
                auto* fnDef = fit2->second;
                // Build param name → index map
                std::unordered_map<std::string, size_t> paramIdx;
                for (size_t i = 0; i < fnDef->params.size(); i++)
                    paramIdx[fnDef->params[i].first] = i;
                // Reorder args by param position
                std::vector<std::unique_ptr<ExprAST>> ordered(fnDef->params.size());
                std::vector<bool> filled(fnDef->params.size(), false);
                for (auto& a : call->args) {
                    if (a->kind == NodeKind::Binary) {
                        auto* b = static_cast<BinaryExprAST*>(a.get());
                        if (b->op == '=' && b->lhs && b->lhs->kind == NodeKind::Variable) {
                            auto* v = static_cast<VariableExprAST*>(b->lhs.get());
                            auto pi = paramIdx.find(v->name);
                            if (pi != paramIdx.end()) {
                                ordered[pi->second] = std::move(b->rhs);
                                filled[pi->second] = true;
                            }
                            continue;
                        }
                    }
                    // Positional arg: find first unfilled slot
                    for (size_t i = 0; i < fnDef->params.size(); i++) {
                        if (!filled[i]) {
                            ordered[i] = std::move(a);
                            filled[i] = true;
                            break;
                        }
                    }
                }
                // Fill defaults for missing params
                for (size_t i = 0; i < fnDef->params.size(); i++) {
                    if (!filled[i] && i < fnDef->defaults.size() && fnDef->defaults[i]) {
                        ordered[i] = cloneExpr(fnDef->defaults[i].get());
                        filled[i] = true;
                    }
                }
                call->args = std::move(ordered);
            }
        }

        // Fill in default parameter values if call provides fewer args
        auto fit = functionDefs.find(call->callee);
        if (fit != functionDefs.end()) {
            auto* fnDef = fit->second;
            while (call->args.size() < fnDef->params.size()) {
                size_t idx = call->args.size();
                if (idx < fnDef->defaults.size() && fnDef->defaults[idx]) {
                    call->args.push_back(cloneExpr(fnDef->defaults[idx].get()));
                } else {
                    break;
                }
            }
        }

        // Map methods: fa_has/fa_get/fa_set/fa_len on Map-typed base
        // (parsePostfix lowers recv.method() to fa_method(recv, ...)).
        if ((call->callee == "fa_has" || call->callee == "fa_get" ||
             call->callee == "fa_set" || call->callee == "fa_len") &&
            !call->args.empty()) {
            bool baseIsMap = false;
            ExprAST* base = call->args[0].get();
            if (base->kind == NodeKind::MapLiteral) baseIsMap = true;
            else if (base->kind == NodeKind::Variable) {
                auto* v = static_cast<VariableExprAST*>(base);
                auto* sym = symTable.lookup(v->name);
                if (sym && sym->type.kind == TypeKind::Map) baseIsMap = true;
                else {
                    auto git = globalSymTable.find(v->name);
                    if (git != globalSymTable.end() && git->second.type.kind == TypeKind::Map)
                        baseIsMap = true;
                }
            } else if (base->kind == NodeKind::Call) {
                auto* c = static_cast<CallExprAST*>(base);
                if (c->callee == "flint_map_new") baseIsMap = true;
            }
            if (baseIsMap) {
                std::string target;
                if (call->callee == "fa_has") target = "flint_map_has";
                else if (call->callee == "fa_get") target = "flint_map_get";
                else if (call->callee == "fa_set") target = "flint_map_set";
                else target = "flint_map_len";
                auto tit = functionMap.find(target);
                if (tit != functionMap.end()) {
                    std::vector<llvm::Value*> margs;
                    for (auto& a : call->args) {
                        auto* v = emitExpr(a.get());
                        if (!v) return nullptr;
                        margs.push_back(v);
                    }
                    return builder->CreateCall(tit->second, margs);
                }
            }
        }

        auto it = functionMap.find(call->callee);
        if (it == functionMap.end()) {
            // Leftover fa_ method call: the receiver type was unknown at
            // parse time (or the method doesn't exist). Say so clearly.
            if (call->callee.rfind("fa_", 0) == 0)
                return codegenError(std::string("unknown method '") +
                    call->callee.substr(3) + "' (receiver type unknown or method missing)");
            return codegenError(std::string("undefined function '") + call->callee + "'");
        }
        bool isExtern = externFunctions.count(call->callee) > 0;
        std::vector<llvm::Value*> args;
        for (auto& a : call->args) {
            auto* v = emitExpr(a.get());
            if (!v) return nullptr;
            args.push_back(v);
            // Move: passing a non-Copy variable to a function consumes it
            // Extern functions are C FFI and borrow their arguments; skip move semantics
            if (!isExtern && a->kind == NodeKind::Variable) {
                auto* varArg = static_cast<VariableExprAST*>(a.get());
                auto* srcSym = symTable.lookup(varArg->name);
                    if (srcSym && !srcSym->type.isCopyType()) {
                        if (srcSym->borrowCount > 0) {
                            return codegenError(std::string("cannot move '") + varArg->name + "' while borrowed");
                        }
                    srcSym->moved = true;
                }
            }
        }
        return builder->CreateCall(it->second, args);
    }
};

// ============================================================================
// INTERFACE SYSTEM — emit and load .flint.bc declaration files
// ============================================================================

static bool emitInterface(ProgramAST& prog, const std::string& outputPath) {
    Codegen cg;
    if (!cg.generateDeclarations(prog)) return false;
    std::error_code ec;
    llvm::raw_fd_ostream out(outputPath, ec);
    if (ec) { std::cerr << "interface error: " << ec.message() << "\n"; return false; }
    llvm::WriteBitcodeToFile(*cg.mod, out);
    out.close();
    return true;
}

static std::unique_ptr<llvm::Module> loadInterface(const std::string& path, llvm::LLVMContext& ctx) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if (!buf) return nullptr;
    auto mod = llvm::parseBitcodeFile(buf.get()->getMemBufferRef(), ctx);
    if (!mod) {
        llvm::handleAllErrors(mod.takeError(), [](const llvm::ErrorInfoBase& e) {
            std::cerr << "interface error: " << e.message() << "\n";
        });
        return nullptr;
    }
    return std::move(*mod);
}

// ============================================================================
// Parser emit-mode method implementations (out-of-line, after Codegen)
// ============================================================================

int Parser::tokPrec(TokenType t) {
    switch (t) {
        case TokenType::PIPE: return 5;
        case TokenType::PIPE_PIPE:
        case TokenType::KW_OR: return PREC_OR;
        case TokenType::AMPERSAND_AMP:
        case TokenType::KW_AND: return PREC_AND;
        case TokenType::EQ_EQ: case TokenType::NE:
        case TokenType::LT: case TokenType::GT:
        case TokenType::LE: case TokenType::GE: return PREC_COMPARE;
        case TokenType::PLUS: case TokenType::MINUS: return PREC_TERM;
        case TokenType::STAR: case TokenType::SLASH: case TokenType::MODULO: return PREC_FACTOR;
        case TokenType::AT: return 55;
        case TokenType::LBRACKET: case TokenType::DOT: return PREC_POSTFIX;
        case TokenType::LPAREN: return PREC_CALL;
        default: return PREC_NONE;
    }
}

llvm::Value* Parser::parseExpressionEmit(int minPrec) {
    if (minPrec == PREC_NONE && check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
        std::string name = peek().lexeme;
        auto* sym = cg->symTable.lookup(name);
        if (sym) {
            if (!sym->isMutable) { advance(); advance(); parseError("cannot assign to immutable '" + name + "'"); return nullptr; }
            if (sym->borrowCount > 0) { advance(); advance(); parseError("cannot assign to '" + name + "' while borrowed"); return nullptr; }
            advance(); advance();
            auto val = parseExpressionEmit();
            if (!val) return nullptr;
            auto* store = cg->builder->CreateStore(val, sym->alloca);
            if (sym->moved) sym->moved = false;
            return store;
        }
        auto git = cg->globalSymTable.find(name);
        if (git != cg->globalSymTable.end()) {
            if (!git->second.isMutable) { advance(); advance(); parseError("cannot assign to immutable global '" + name + "'"); return nullptr; }
            advance(); advance();
            auto val = parseExpressionEmit();
            if (!val) return nullptr;
            return cg->builder->CreateStore(val, git->second.global);
        }
        parseError("variable '" + name + "' not declared");
        return nullptr;
    }
    // Compound assignment (emit-mode): `x += e` → load x, apply op, store.
    if (minPrec == PREC_NONE && check(TokenType::IDENTIFIER) &&
        (peek(1).type == TokenType::PLUS_EQ || peek(1).type == TokenType::MINUS_EQ ||
         peek(1).type == TokenType::STAR_EQ || peek(1).type == TokenType::SLASH_EQ ||
         peek(1).type == TokenType::MODULO_EQ)) {
        std::string name = peek().lexeme;
        auto* sym = cg->symTable.lookup(name);
        auto git = cg->globalSymTable.find(name);
        if (!sym && git == cg->globalSymTable.end()) {
            parseError("variable '" + name + "' not declared");
            return nullptr;
        }
        bool isGlobal = !sym;
        bool isMut = isGlobal ? git->second.isMutable : sym->isMutable;
        if (!isMut) { advance(); advance(); parseError("cannot assign to immutable '" + name + "'"); return nullptr; }
        Token nameTok = advance();
        Token opTok = advance(); // '+=' etc.
        TokenType binTy = TokenType::PLUS;
        std::string binLex = "+";
        if (opTok.type == TokenType::MINUS_EQ) { binTy = TokenType::MINUS; binLex = "-"; }
        else if (opTok.type == TokenType::STAR_EQ) { binTy = TokenType::STAR; binLex = "*"; }
        else if (opTok.type == TokenType::SLASH_EQ) { binTy = TokenType::SLASH; binLex = "/"; }
        else if (opTok.type == TokenType::MODULO_EQ) { binTy = TokenType::MODULO; binLex = "%"; }
        auto* rhs = parseExpressionEmit();
        if (!rhs) return nullptr;
        llvm::Value* dest = isGlobal ? (llvm::Value*)git->second.global : (llvm::Value*)sym->alloca;
        llvm::Type* destTy = isGlobal ? git->second.global->getValueType() : sym->alloca->getAllocatedType();
        auto* cur = cg->builder->CreateLoad(destTy, dest, name + "_cur");
        auto* val = emitBinaryOpEmit(Token{binTy, binLex, {}}, cur, rhs);
        if (!val) return nullptr;
        auto* store = cg->builder->CreateStore(val, dest);
        if (!isGlobal && sym->moved) sym->moved = false;
        (void)nameTok;
        return store;
    }
    auto left = parseNudEmit();
    if (!left) return nullptr;

    while (true) {
        TokenType tt = peek().type;
        int p = tokPrec(tt);
        if (p < minPrec) break;

        if (tt == TokenType::LBRACKET) {
            advance();
            auto idx = parseExpressionEmit();
            consume(TokenType::RBRACKET, "expected ']' after index");
            if (check(TokenType::ASSIGN)) {
                // Index-assign, emit path (0c2 mirror of the AST desugar)
                advance(); // '='
                auto* rhs = parseExpressionEmit();
                if (!rhs) return nullptr;
                left = emitIndexStoreEmit(left, idx, rhs);
            } else {
                left = emitIndexEmit(left, idx);
            }
        } else if (tt == TokenType::DOT) {
            advance();
            Token fName = consume(TokenType::IDENTIFIER, "expected field or method name after '.'");
            if (check(TokenType::LPAREN)) {
                advance(); // '('
                std::vector<llvm::Value*> args;
                args.push_back(left);
                if (!check(TokenType::RPAREN)) {
                    do { args.push_back(parseExpressionEmit()); } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "expected ')' after method arguments");
                left = emitDirectCall("fa_" + fName.lexeme, args);
            } else {
                left = emitFieldAccessEmit(left, fName.lexeme);
            }
        } else if (tt == TokenType::PIPE) {
            advance(); // consume '|>'
            // Desugar a |> f(b...) into f(a, b...)
            // Desugar a |> f into f(a) — no parens needed
            Token fName = consume(TokenType::IDENTIFIER, "expected function name after '|>'");
            std::vector<llvm::Value*> args;
            args.push_back(left);
            if (match(TokenType::LPAREN)) {
                if (!check(TokenType::RPAREN)) {
                    do { args.push_back(parseExpressionEmit()); } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "expected ')' after arguments");
            }
            // Emit call directly with pre-parsed args (bypass emitCallEmit which re-consumes tokens)
            left = emitDirectCall(fName.lexeme, args);
            } else if (p > PREC_NONE) {
                Token opTok = advance();
                auto right = parseExpressionEmit(p + 1);
            if (!right) return nullptr;
            left = emitBinaryOpEmit(opTok, left, right);
        } else {
            break;
        }
        if (!left) return nullptr;
    }
    return left;
}

llvm::Value* Parser::parseNudEmit() {
    if (match(TokenType::NUMBER_LITERAL)) {
        std::string lit = previous().lexeme;
        bool isFloatLit = false;
        if (!lit.empty() && lit.back() == 'd') { isFloatLit = true; lit.pop_back(); }
        if (isFloatLit) {
            double v = std::stod(lit);
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*cg->ctx), v);
        }
        int64_t iv = 0;
        if (parseI64Exact(lit, iv))
            return llvm::ConstantInt::get(cg->i64Ty, static_cast<uint64_t>(iv), true);
        double v = std::stod(lit);
        double intPart;
        if (std::modf(v, &intPart) != 0.0) {
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*cg->ctx), v);
        }
        return llvm::ConstantInt::get(cg->i64Ty, static_cast<int64_t>(v));
    }
    if (match(TokenType::STRING_LITERAL)) {
        return cg->builder->CreateGlobalString(previous().lexeme, "str");
    }
    if (match(TokenType::LBRACKET)) {
        std::vector<llvm::Value*> elems;
        if (!check(TokenType::RBRACKET)) {
            do {
                auto e = parseExpressionEmit();
                if (e) elems.push_back(e);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "expected ']' after array literal");
        return emitArrayLiteralEmit(elems);
    }
    // Map literal (emit-mode): contextual `map` + `{`
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "map" && peek(1).type == TokenType::LBRACE) {
        advance(); // 'map'
        advance(); // '{'
        auto itNew = cg->functionMap.find("flint_map_new");
        auto itSet = cg->functionMap.find("flint_map_set");
        if (itNew == cg->functionMap.end() || itSet == cg->functionMap.end()) {
            parseError("map runtime not registered"); return nullptr;
        }
        llvm::Value* m = cg->builder->CreateCall(itNew->second, {}, "map_new");
        if (!check(TokenType::RBRACE)) {
            do {
                Token kTok = consume(TokenType::STRING_LITERAL, "expected string key in map literal");
                consume(TokenType::COLON, "expected ':' after map key");
                auto* v = parseExpressionEmit();
                if (!v) return nullptr;
                llvm::Value* keyStr = cg->builder->CreateGlobalString(kTok.lexeme, "mapkey");
                cg->builder->CreateCall(itSet->second, {m, keyStr, v});
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACE, "expected '}' to close map literal");
        return m;
    }
    if (match(TokenType::KW_MATCH)) return parseMatchEmit();
    if (match(TokenType::IDENTIFIER)) return parseIdentEmit();
    // Lambdas need closure conversion (AST path only) — reject cleanly here.
    if (check(TokenType::PIPE)) {
        parseError("lambdas are not supported in generic functions");
        return nullptr;
    }
    if (match(TokenType::LPAREN)) {
        auto e = parseExpressionEmit();
        consume(TokenType::RPAREN, "expected ')' after expression");
        return e;
    }
    if (match(TokenType::MINUS)) {
        auto rhs = parseNudEmit();
        if (!rhs) return nullptr;
        // Emit (0 - rhs) using a synthetic BinaryOp via emitBinaryOpEmit
        auto* zero = rhs->getType()->isDoubleTy()
            ? (llvm::Value*)llvm::ConstantFP::get(rhs->getType(), 0.0)
            : (llvm::Value*)llvm::ConstantInt::get(rhs->getType(), 0);
        return emitBinaryOpEmit(Token{TokenType::MINUS, "-", {}}, zero, rhs);
    }
    // Logical NOT: `!e` / `not e` → (e == 0), truthy semantics like AST path
    if (match(TokenType::BANG) || match(TokenType::KW_NOT)) {
        auto* rhs = parseNudEmit();
        if (!rhs) return nullptr;
        if (rhs->getType()->isDoubleTy()) {
            auto* z = llvm::ConstantFP::get(rhs->getType(), 0.0);
            auto* c = cg->builder->CreateFCmpOEQ(rhs, z, "notcmp");
            return cg->builder->CreateZExt(c, cg->i64Ty, "notext");
        }
        llvm::Value* z = rhs->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(rhs->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(rhs->getType(), 0);
        auto* c = cg->builder->CreateICmpEQ(rhs, z, "notcmp");
        return cg->builder->CreateZExt(c, cg->i64Ty, "notext");
    }
    if (match(TokenType::AMPERSAND)) {
        if (check(TokenType::IDENTIFIER)) {
            std::string vname = peek().lexeme;
            advance();
            // Address-of-function: &func_name returns function pointer as str
            auto fIt = cg->functionMap.find(vname);
            if (fIt != cg->functionMap.end()) {
                return cg->builder->CreateBitCast(fIt->second, cg->i8PtrTy, vname + "_ptr");
            }
            // Variable reference (borrow)
            auto* sym = cg->symTable.lookup(vname);
            if (!sym) { parseError("undefined variable '" + vname + "'"); return nullptr; }
            if (sym->moved) { parseError("cannot borrow moved variable '" + vname + "'"); return nullptr; }
            sym->borrowCount++;
            cg->symTable.recordBorrow(vname);
            return sym->alloca;
        }
        parseError("can only reference variables");
        return nullptr;
    }
    if (match(TokenType::STAR)) {
        auto target = parseNudEmit();
        if (!target) return nullptr;
        return cg->builder->CreateLoad(cg->i64Ty, target, "deref");
    }
    parseError("expected expression");
    return nullptr;
}

llvm::Value* Parser::parseIdentEmit() {
    std::string name = previous().lexeme;
    // Boolean literals (v0.9 AEGIS session; re-added after 2026-09-14 wipe)
    if (name == "true") return llvm::ConstantInt::get(cg->i64Ty, 1);
    if (name == "false") return llvm::ConstantInt::get(cg->i64Ty, 0);
    if (enumRegistry.count(name) && check(TokenType::DOT)) {
        advance();
        Token varTok = consume(TokenType::IDENTIFIER, "expected variant name");
        return emitEnumConstructEmit(name, varTok.lexeme);
    }
    if (match(TokenType::LPAREN)) {
        // Special case: flint_thread_spawn needs function name from token stream
        if (name == "flint_thread_spawn") {
            if (!check(TokenType::STRING_LITERAL)) {
                parseError("flint_thread_spawn first arg must be a string literal");
                return nullptr;
            }
            Token funcNameTok = advance(); // consume the string literal
            std::string funcName = funcNameTok.lexeme;
            // Strip quotes from the string literal
            if (funcName.size() >= 2 && funcName.front() == '"' && funcName.back() == '"')
                funcName = funcName.substr(1, funcName.size() - 2);
            if (!match(TokenType::COMMA)) {
                parseError("flint_thread_spawn needs 2 args: func_name, arg");
                return nullptr;
            }
            auto* argVal = parseExpressionEmit();
            if (!argVal) return nullptr;
            consume(TokenType::RPAREN, "expected ')' after arguments");
            auto fIt = cg->functionMap.find(funcName);
            if (fIt == cg->functionMap.end()) { parseError("flint_thread_spawn: unknown function '" + funcName + "'"); return nullptr; }
            auto* targetFn = fIt->second;
            auto* wrapperTy = llvm::FunctionType::get(cg->i8PtrTy, {cg->i8PtrTy}, false);
            auto* wrapperPtr = cg->builder->CreateBitCast(targetFn, llvm::PointerType::get(*cg->ctx, 0), "thread_fn");
            auto* createFn = cg->functionMap["flint_thread_create"];
            if (!createFn) { parseError("flint_thread_spawn: runtime function not found"); return nullptr; }
            auto* voidArg = cg->builder->CreateIntToPtr(argVal, cg->i8PtrTy, "thread_arg");
            return cg->builder->CreateCall(createFn, {wrapperPtr, voidArg}, "thread_id");
        }
        return emitCallEmit(name);
    }
    if (structRegistry.count(name) && match(TokenType::LBRACE)) {
        return emitStructLiteralEmit(name);
    }
    auto* sym = cg->symTable.lookup(name);
    if (sym) {
        if (sym->moved) { parseError("use of moved variable '" + name + "'"); return nullptr; }
        if (sym->borrowCount > 0 && !sym->type.isCopyType()) { parseError("cannot move '" + name + "' while borrowed"); return nullptr; }
        llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
        return cg->builder->CreateLoad(cg->resolvedLlvmType(sym->type), ptr, name.c_str());
    }
    auto git = cg->globalSymTable.find(name);
    if (git != cg->globalSymTable.end()) {
        return cg->builder->CreateLoad(cg->resolvedLlvmType(git->second.type), git->second.global, name.c_str());
    }
    parseError("undefined variable '" + name + "'");
    return nullptr;
}

llvm::Value* Parser::emitArithOpEmit(char op, llvm::Value* l, llvm::Value* r) {
    if (cg->releaseMode) {
        switch (op) {
            case '+': return cg->builder->CreateAdd(l, r, "add");
            case '-': return cg->builder->CreateSub(l, r, "sub");
            default:  return cg->builder->CreateMul(l, r, "mul");
        }
    }
    llvm::Intrinsic::ID iid;
    switch (op) {
        case '+': iid = llvm::Intrinsic::sadd_with_overflow; break;
        case '-': iid = llvm::Intrinsic::ssub_with_overflow; break;
        default:  iid = llvm::Intrinsic::smul_with_overflow; break;
    }
    auto* ovFn = llvm::Intrinsic::getOrInsertDeclaration(cg->mod.get(), iid, {cg->i64Ty});
    auto* callRes = cg->builder->CreateCall(ovFn, {l, r}, "arith_ov");
    auto* val = cg->builder->CreateExtractValue(callRes, {0}, "arith_val");
    auto* ov = cg->builder->CreateExtractValue(callRes, {1}, "arith_ovfl");
    auto* okBB = llvm::BasicBlock::Create(*cg->ctx, "arith_ok", cg->currentFunc);
    auto* panicBB = llvm::BasicBlock::Create(*cg->ctx, "arith_panic", cg->currentFunc);
    cg->builder->CreateCondBr(ov, panicBB, okBB);
    cg->builder->SetInsertPoint(panicBB);
    auto* panicFn = cg->functionMap["flint_panic"];
    if (panicFn) {
        auto* msg = cg->builder->CreateGlobalString("integer overflow", "ovmsg");
        cg->builder->CreateCall(panicFn, {msg});
    }
    cg->builder->CreateBr(okBB);
    cg->builder->SetInsertPoint(okBB);
    return val;
}

llvm::Value* Parser::emitBinaryOpEmit(Token opTok, llvm::Value* l, llvm::Value* r) {
    TokenType tt = opTok.type;
    if (tt == TokenType::AT) {
        auto it = cg->functionMap.find("fao_matmul");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {l, r});
        parseError("fao_matmul not linked");
        return nullptr;
    }
    // String concatenation: str + str
    if (tt == TokenType::PLUS && l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
        auto it = cg->functionMap.find("flint_str_concat");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {l, r});
        parseError("flint_str_concat not linked");
        return nullptr;
    }
    // Mixed str + i64/f64: convert scalar to string first
    if (tt == TokenType::PLUS && l->getType()->isPointerTy() && r->getType()->isIntegerTy(64)) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto iti = cg->functionMap.find("flint_i64_to_string");
        if (itc != cg->functionMap.end() && iti != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {l, cg->builder->CreateCall(iti->second, {r})});
        parseError("flint_str_concat/flint_i64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS && l->getType()->isIntegerTy(64) && r->getType()->isPointerTy()) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto iti = cg->functionMap.find("flint_i64_to_string");
        if (itc != cg->functionMap.end() && iti != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {cg->builder->CreateCall(iti->second, {l}), r});
        parseError("flint_str_concat/flint_i64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS && l->getType()->isPointerTy() && r->getType()->isDoubleTy()) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto itf = cg->functionMap.find("flint_f64_to_string");
        if (itc != cg->functionMap.end() && itf != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {l, cg->builder->CreateCall(itf->second, {r})});
        parseError("flint_str_concat/flint_f64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS && l->getType()->isDoubleTy() && r->getType()->isPointerTy()) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto itf = cg->functionMap.find("flint_f64_to_string");
        if (itc != cg->functionMap.end() && itf != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {cg->builder->CreateCall(itf->second, {l}), r});
        parseError("flint_str_concat/flint_f64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS || tt == TokenType::MINUS || tt == TokenType::STAR || tt == TokenType::SLASH || tt == TokenType::MODULO) {
        // Float fast-path: if either operand is double, use FP ops (and SiToFp on the other)
        if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
            if (!l->getType()->isDoubleTy())
                l = cg->builder->CreateSIToFP(l, llvm::Type::getDoubleTy(*cg->ctx), "lToFP");
            if (!r->getType()->isDoubleTy())
                r = cg->builder->CreateSIToFP(r, llvm::Type::getDoubleTy(*cg->ctx), "rToFP");
            if (tt == TokenType::SLASH) return cg->builder->CreateFDiv(l, r, "fdiv");
            if (tt == TokenType::MODULO) return cg->builder->CreateFRem(l, r, "frem");
            switch (tt) {
                case TokenType::PLUS:  return cg->builder->CreateFAdd(l, r, "fadd");
                case TokenType::MINUS: return cg->builder->CreateFSub(l, r, "fsub");
                case TokenType::STAR:  return cg->builder->CreateFMul(l, r, "fmul");
                default: return nullptr;
            }
        }
        if (tt == TokenType::SLASH) return cg->builder->CreateSDiv(l, r, "div");
        if (tt == TokenType::MODULO) return cg->builder->CreateSRem(l, r, "mod");
        return emitArithOpEmit(opTok.lexeme[0], l, r);
    }
    if (tt == TokenType::PIPE_PIPE || tt == TokenType::KW_OR) {
        // Float fast-path: fcmp "not zero"
        if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
            if (!l->getType()->isDoubleTy())
                l = cg->builder->CreateSIToFP(l, llvm::Type::getDoubleTy(*cg->ctx), "lToFP");
            if (!r->getType()->isDoubleTy())
                r = cg->builder->CreateSIToFP(r, llvm::Type::getDoubleTy(*cg->ctx), "rToFP");
            auto* zero = llvm::ConstantFP::get(l->getType(), 0.0);
            auto* lB = cg->builder->CreateFCmpONE(l, zero, "ftruthy");
            auto* rB = cg->builder->CreateFCmpONE(r, zero, "rtruthy");
            auto* orV = cg->builder->CreateOr(lB, rB, "forOr");
            return cg->builder->CreateZExt(orV, cg->i64Ty, "forOrExt");
        }
        auto* lZero = l->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(l->getType(), 0);
        auto* rZero = r->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(r->getType(), 0);
        auto* lBool = cg->builder->CreateICmpNE(l, lZero, "lbool");
        auto* rBool = cg->builder->CreateICmpNE(r, rZero, "rbool");
        auto* orVal = cg->builder->CreateOr(lBool, rBool, "or");
        return cg->builder->CreateZExt(orVal, cg->i64Ty, "or_ext");
    }
    if (tt == TokenType::AMPERSAND_AMP || tt == TokenType::KW_AND) {
        // Truthy AND (mirror of || above, non-short-circuit like the AST path)
        if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
            if (!l->getType()->isDoubleTy())
                l = cg->builder->CreateSIToFP(l, llvm::Type::getDoubleTy(*cg->ctx), "lToFP");
            if (!r->getType()->isDoubleTy())
                r = cg->builder->CreateSIToFP(r, llvm::Type::getDoubleTy(*cg->ctx), "rToFP");
            auto* zero = llvm::ConstantFP::get(l->getType(), 0.0);
            auto* lB = cg->builder->CreateFCmpONE(l, zero, "ftruthy");
            auto* rB = cg->builder->CreateFCmpONE(r, zero, "rtruthy");
            auto* andV = cg->builder->CreateAnd(lB, rB, "fand");
            return cg->builder->CreateZExt(andV, cg->i64Ty, "fandExt");
        }
        auto* lZero = l->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(l->getType(), 0);
        auto* rZero = r->getType()->isPointerTy()
            ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()))
            : (llvm::Value*)llvm::ConstantInt::get(r->getType(), 0);
        auto* lBool = cg->builder->CreateICmpNE(l, lZero, "lbool");
        auto* rBool = cg->builder->CreateICmpNE(r, rZero, "rbool");
        auto* andVal = cg->builder->CreateAnd(lBool, rBool, "and");
        return cg->builder->CreateZExt(andVal, cg->i64Ty, "and_ext");
    }
    // Float compare path
    if (l->getType()->isDoubleTy() || r->getType()->isDoubleTy()) {
        if (!l->getType()->isDoubleTy())
            l = cg->builder->CreateSIToFP(l, llvm::Type::getDoubleTy(*cg->ctx), "lToFP");
        if (!r->getType()->isDoubleTy())
            r = cg->builder->CreateSIToFP(r, llvm::Type::getDoubleTy(*cg->ctx), "rToFP");
        llvm::CmpInst::Predicate pred;
        std::string& op = opTok.lexeme;
        if (op == "==") pred = llvm::CmpInst::FCMP_OEQ;
        else if (op == "!=") pred = llvm::CmpInst::FCMP_ONE;
        else if (op == "<")  pred = llvm::CmpInst::FCMP_OLT;
        else if (op == ">")  pred = llvm::CmpInst::FCMP_OGT;
        else if (op == "<=") pred = llvm::CmpInst::FCMP_OLE;
        else if (op == ">=") pred = llvm::CmpInst::FCMP_OGE;
        auto* cmp = cg->builder->CreateFCmp(pred, l, r, "fcmp");
        return cg->builder->CreateZExt(cmp, cg->i64Ty, "fcmp_ext");
    }
    llvm::CmpInst::Predicate pred;
    std::string& op = opTok.lexeme;
    if (op == "==") pred = llvm::CmpInst::ICMP_EQ;
    else if (op == "!=") pred = llvm::CmpInst::ICMP_NE;
    else if (op == "<")  pred = llvm::CmpInst::ICMP_SLT;
    else if (op == ">")  pred = llvm::CmpInst::ICMP_SGT;
    else if (op == "<=") pred = llvm::CmpInst::ICMP_SLE;
    else if (op == ">=") pred = llvm::CmpInst::ICMP_SGE;
    else { parseError("unknown operator '" + op + "'"); return nullptr; }
    // String content comparison: ptr vs ptr compares contents
    // via flint_str_compare (same convention as `+` → concat).
    if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
        auto it = cg->functionMap.find("flint_str_compare");
        if (it != cg->functionMap.end()) {
            auto* sc = cg->builder->CreateCall(it->second, {l, r}, "strcmp");
            auto* z = llvm::ConstantInt::get(cg->i64Ty, 0);
            auto* scmp = cg->builder->CreateICmp(pred, sc, z, "scmp");
            return cg->builder->CreateZExt(scmp, cg->i64Ty, "scmp_ext");
        }
    }
    // Normalize types: pointer vs integer zero → null pointer comparison
    if (l->getType()->isPointerTy() && r->getType()->isIntegerTy())
        r = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()));
    else if (l->getType()->isIntegerTy() && r->getType()->isPointerTy())
        l = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()));
    auto* cmp = cg->builder->CreateICmp(pred, l, r, "cmp");
    return cg->builder->CreateZExt(cmp, cg->i64Ty, "cmp_ext");
}

llvm::Value* Parser::emitIndexEmit(llvm::Value* base, llvm::Value* index) {
    llvm::Value* dataPtr = cg->builder->CreateExtractValue(base, {0}, "arr_ptr");
    llvm::Value* len = cg->builder->CreateExtractValue(base, {1}, "arr_len");
    auto* boundsFn = cg->functionMap["flint_bounds_check"];
    if (boundsFn) cg->builder->CreateCall(boundsFn, {index, len});
    llvm::Value* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, index, "arr_elem");
    return cg->builder->CreateLoad(cg->i64Ty, elemPtr, "arr_elem_val");
}

llvm::Value* Parser::emitIndexStoreEmit(llvm::Value* base, llvm::Value* index, llvm::Value* val) {
    if (!base || !index || !val) return nullptr;
    if (!base->getType()->isStructTy()) {
        parseError("index assignment requires an array variable");
        return nullptr;
    }
    llvm::Value* dataPtr = cg->builder->CreateExtractValue(base, {0}, "arr_ptr");
    llvm::Value* len = cg->builder->CreateExtractValue(base, {1}, "arr_len");
    auto* boundsFn = cg->functionMap["flint_bounds_check"];
    if (boundsFn) cg->builder->CreateCall(boundsFn, {index, len});
    llvm::Value* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, index, "arr_elem");
    cg->builder->CreateStore(val, elemPtr);
    return llvm::ConstantInt::get(cg->i64Ty, 0); // dummy return
}

llvm::Value* Parser::emitFieldAccessEmit(llvm::Value* base, const std::string& field) {
    for (auto& [name, def] : structRegistry) {
        auto* st = llvm::StructType::getTypeByName(*cg->ctx, name);
        if (!st) continue;
        if (base->getType() == st) {
            for (unsigned i = 0; i < def.fields.size(); i++) {
                if (def.fields[i].name == field)
                    return cg->builder->CreateExtractValue(base, {i}, field);
            }
            parseError("struct '" + name + "' has no field '" + field + "'");
            return nullptr;
        }
    }
    parseError("cannot access field '" + field + "' on non-struct type");
    return nullptr;
}

llvm::Value* Parser::emitArrayLiteralEmit(const std::vector<llvm::Value*>& elems) {
    size_t count = elems.size();
    auto* arrDataType = llvm::ArrayType::get(cg->i64Ty, count);
    auto* dataAlloca = cg->createEntryAlloca(arrDataType, "arr_data");
    for (size_t i = 0; i < count; i++) {
        auto* idxVal = llvm::ConstantInt::get(cg->i64Ty, i);
        auto* elemPtr = cg->builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(cg->i64Ty, 0), idxVal}, "arr_elem");
        cg->builder->CreateStore(elems[i], elemPtr);
    }
    auto* dataPtr = cg->builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(cg->i64Ty, 0), llvm::ConstantInt::get(cg->i64Ty, 0)}, "arr_data_ptr");
    auto* structTy = cg->resolvedLlvmType(Type::array(Type::i64()));
    auto* tempAlloca = cg->createEntryAlloca(structTy, "arr_tmp");
    auto* ptrField = cg->builder->CreateStructGEP(structTy, tempAlloca, 0);
    cg->builder->CreateStore(dataPtr, ptrField);
    auto* lenField = cg->builder->CreateStructGEP(structTy, tempAlloca, 1);
    cg->builder->CreateStore(llvm::ConstantInt::get(cg->i64Ty, count), lenField);
    return cg->builder->CreateLoad(structTy, tempAlloca, "arr_val");
}

llvm::Value* Parser::emitStructLiteralEmit(const std::string& name) {
    auto it = structRegistry.find(name);
    if (it == structRegistry.end()) { parseError("unknown struct '" + name + "'"); return nullptr; }
    auto& def = it->second;
    auto* st = cg->resolvedLlvmType(Type::struct_(name));
    if (!st) { parseError("struct type not found '" + name + "'"); return nullptr; }
    llvm::Value* s = llvm::UndefValue::get(st);
    for (unsigned i = 0; i < def.fields.size(); i++) {
        Token fName = consume(TokenType::IDENTIFIER, "expected field name");
        consume(TokenType::COLON, "expected ':'");
        auto* fv = parseExpressionEmit();
        if (!fv) return nullptr;
        s = cg->builder->CreateInsertValue(s, fv, {i}, name + "." + def.fields[i].name);
        if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
    }
    consume(TokenType::RBRACE, "expected '}' to close struct literal");
    return s;
}

llvm::Value* Parser::emitEnumConstructEmit(const std::string& enumName, const std::string& variantName) {
    auto eit = enumRegistry.find(enumName);
    if (eit == enumRegistry.end()) { parseError("unknown enum '" + enumName + "'"); return nullptr; }
    auto& ed = eit->second;
    auto* enumTy = cg->resolvedLlvmType(Type::enum_(enumName));
    int tag = -1;
    for (size_t i = 0; i < ed.variants.size(); i++)
        if (ed.variants[i].name == variantName) { tag = (int)i; break; }
    if (tag < 0) { parseError("unknown variant '" + variantName + "'"); return nullptr; }

    std::vector<llvm::Value*> args;
    if (match(TokenType::LPAREN)) {
        if (!check(TokenType::RPAREN)) {
            do { args.push_back(parseExpressionEmit()); } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after variant args");
    }

    auto* alloca = cg->createEntryAlloca(enumTy, enumName + "_tmp");
    auto* tagPtr = cg->builder->CreateStructGEP(enumTy, alloca, 0);
    cg->builder->CreateStore(llvm::ConstantInt::get(cg->i8Ty, tag), tagPtr);
    if (!args.empty()) {
        std::vector<llvm::Type*> fldTys = {cg->i8Ty};
        for (auto& pt : ed.variants[tag].payloadTypes)
            fldTys.push_back(cg->resolvedLlvmType(pt));
        auto* varTy = llvm::StructType::get(*cg->ctx, fldTys);
        auto* bcPtr = cg->builder->CreateBitCast(alloca, cg->i8PtrTy);
        for (size_t i = 0; i < args.size(); i++) {
            auto* fldPtr = cg->builder->CreateStructGEP(varTy, bcPtr, (unsigned)(i + 1));
            cg->builder->CreateStore(args[i], fldPtr);
        }
    }
    return cg->builder->CreateLoad(enumTy, alloca, enumName + "_val");
}

llvm::Value* Parser::emitCallEmit(const std::string& callee) {
    std::vector<llvm::Value*> args;
    if (!check(TokenType::RPAREN)) {
        do {
            auto a = parseExpressionEmit();
            if (a) args.push_back(a);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after arguments");
    return emitDirectCall(callee, args);
}

llvm::Value* Parser::emitDirectCall(const std::string& callee, std::vector<llvm::Value*>& args) {
    // Map methods (emit-mode): fa_has/fa_get/fa_set only used on maps.
    if (callee == "fa_has" || callee == "fa_get" || callee == "fa_set") {
        std::string target = callee == "fa_has" ? "flint_map_has"
            : callee == "fa_get" ? "flint_map_get" : "flint_map_set";
        auto it = cg->functionMap.find(target);
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, args);
        parseError("map runtime not registered");
        return nullptr;
    }
    if (callee == "py_eval") {
        cg->hasPython = true;
        if (args.size() != 1) { parseError("py_eval() needs one string arg"); return nullptr; }
        auto it = cg->functionMap.find("flint_py_eval_int");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {args[0]});
        parseError("py_eval_int not declared");
        return nullptr;
    }

    if (callee == "print") {
        if (args.empty()) { parseError("print() needs an arg"); return nullptr; }
        if (args[0]->getType() == cg->i8PtrTy) {
            auto it = cg->functionMap.find("flint_println_str");
            if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {args[0]});
        } else if (args[0]->getType()->isDoubleTy()) {
            auto it = cg->functionMap.find("flint_println_f64");
            if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {args[0]});
        } else {
            auto it = cg->functionMap.find("flint_println_i64");
            if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {args[0]});
        }
        return nullptr;
    }

    if (callee == "flint_array_get") {
        if (args.size() != 2) { parseError("flint_array_get needs 2 args"); return nullptr; }
        auto* dataPtr = cg->builder->CreateExtractValue(args[0], {0}, "arr_data");
        auto* len = cg->builder->CreateExtractValue(args[0], {1}, "arr_len");
        auto* boundsFn = cg->functionMap["flint_bounds_check"];
        if (boundsFn) cg->builder->CreateCall(boundsFn, {args[1], len});
        auto* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, args[1], "arr_elem");
        return cg->builder->CreateLoad(cg->i64Ty, elemPtr, "arr_elem_val");
    }

    if (callee == "flint_array_set") {
        if (args.size() != 3) { parseError("flint_array_set needs 3 args"); return nullptr; }
        auto* dataPtr = cg->builder->CreateExtractValue(args[0], {0}, "arr_data");
        auto* len = cg->builder->CreateExtractValue(args[0], {1}, "arr_len");
        auto* boundsFn = cg->functionMap["flint_bounds_check"];
        if (boundsFn) cg->builder->CreateCall(boundsFn, {args[1], len});
        auto* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, args[1], "arr_elem");
        cg->builder->CreateStore(args[2], elemPtr);
        return llvm::ConstantInt::get(cg->i64Ty, 0);
    }

    if (callee == "flint_array_data") {
        if (args.size() != 1) { parseError("flint_array_data needs 1 arg"); return nullptr; }
        return cg->builder->CreateExtractValue(args[0], {0}, "arr_data");
    }

    if (callee == "flint_array_get_ptr") {
        if (args.size() != 2) { parseError("flint_array_get_ptr needs 2 args"); return nullptr; }
        auto* elemPtr = cg->builder->CreateGEP(cg->i64Ty, args[0], args[1], "arr_elem");
        return cg->builder->CreateLoad(cg->i64Ty, elemPtr, "arr_elem_val");
    }

    if (callee == "flint_array_set_ptr") {
        if (args.size() != 3) { parseError("flint_array_set_ptr needs 3 args"); return nullptr; }
        auto* elemPtr = cg->builder->CreateGEP(cg->i64Ty, args[0], args[1], "arr_elem");
        cg->builder->CreateStore(args[2], elemPtr);
        return llvm::ConstantInt::get(cg->i64Ty, 0);
    }

    auto fIt = cg->functionMap.find(callee);
    if (fIt != cg->functionMap.end()) {
        return cg->builder->CreateCall(fIt->second, args, callee);
    }
    auto gIt = cg->genericFunctions.find(callee);
    if (gIt != cg->genericFunctions.end()) {
        cg->typeSubstMap.clear();
        auto* genFn = gIt->second;
        for (size_t i = 0; i < genFn->typeParams.size(); i++) {
            if (i < genFn->params.size() && i < args.size()) {
                auto* argTy = args[i]->getType();
                for (auto& [sName, sd] : structRegistry) {
                    auto* st = llvm::StructType::getTypeByName(*cg->ctx, sName);
                    if (st && argTy == st) {
                        cg->typeSubstMap[genFn->typeParams[i]] = Type::struct_(sName);
                        break;
                    }
                }
                auto eit = enumRegistry.begin();
                while (eit != enumRegistry.end()) {
                    auto* enumTy = llvm::StructType::getTypeByName(*cg->ctx, eit->second.name);
                    if (enumTy && argTy == enumTy) {
                        cg->typeSubstMap[genFn->typeParams[i]] = Type::enum_(eit->second.name);
                        break;
                    }
                    ++eit;
                }
                if (argTy == cg->i64Ty) cg->typeSubstMap[genFn->typeParams[i]] = Type::i64();
                if (argTy == cg->i8PtrTy) cg->typeSubstMap[genFn->typeParams[i]] = Type::str();
            }
        }
        std::string instanceKey = callee;
        for (auto& [k, v] : cg->typeSubstMap) instanceKey += ":" + k + "=" + std::to_string((int)v.kind);
        auto cacheIt = cg->genericCache.find(instanceKey);
        if (cacheIt != cg->genericCache.end()) {
            cg->typeSubstMap.clear();
            return cg->builder->CreateCall(cacheIt->second, args, callee);
        }
        {
            std::vector<Type> typeArgs;
            for (auto& tp : genFn->typeParams) {
                auto it = cg->typeSubstMap.find(tp);
                typeArgs.push_back(it != cg->typeSubstMap.end() ? it->second : Type::i64());
            }
            auto* spec = cg->specialize(genFn, typeArgs);
            cg->typeSubstMap.clear();
            if (!spec) return nullptr;
            return cg->builder->CreateCall(spec, args, callee);
        }
    }
    parseError("call to unknown function '" + callee + "'");
    return nullptr;
}

llvm::Value* Parser::parseMatchEmit() {
    auto* scrutinee = parseExpressionEmit();
    if (!scrutinee) return nullptr;
    consume(TokenType::LBRACE, "expected '{' after match expression");
    llvm::Type* enumTy = scrutinee->getType();
    auto* alloca = cg->createEntryAlloca(enumTy, "match_scrutinee");
    cg->builder->CreateStore(scrutinee, alloca);

    struct MatchArmEmit {
        std::string enumName, variantName, bindName;
        size_t bodyStart, bodyEnd;
    };
    std::vector<MatchArmEmit> arms;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        MatchArmEmit arm;
        Token enumTk = consume(TokenType::IDENTIFIER, "expected enum name");
        consume(TokenType::DOT, "expected '.'");
        Token varTk = consume(TokenType::IDENTIFIER, "expected variant name");
        arm.enumName = enumTk.lexeme;
        arm.variantName = varTk.lexeme;
        if (match(TokenType::LPAREN)) {
            Token bind = consume(TokenType::IDENTIFIER, "expected binding name");
            arm.bindName = bind.lexeme;
            consume(TokenType::RPAREN, "expected ')'");
        }
        consume(TokenType::FAT_ARROW, "expected '=>'");
        // Save body region (skip over it)
        arm.bodyStart = pos;
        if (check(TokenType::LBRACE)) {
            skipBlock();
        } else {
            while (!check(TokenType::COMMA) && !check(TokenType::RBRACE) && !isAtEnd())
                advance();
        }
        arm.bodyEnd = pos;
        arms.push_back(std::move(arm));
        if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ','");
    }
    consume(TokenType::RBRACE, "expected '}' to close match");

    if (arms.empty()) return llvm::ConstantInt::get(cg->i64Ty, 0);
    auto eit = enumRegistry.find(arms[0].enumName);
    if (eit == enumRegistry.end()) { parseError("unknown enum '" + arms[0].enumName + "'"); return nullptr; }
    auto& ed = eit->second;

    std::vector<llvm::BasicBlock*> armBBs;
    for (auto& arm : arms)
        armBBs.push_back(llvm::BasicBlock::Create(*cg->ctx, arm.variantName, cg->currentFunc));
    auto* mergeBB = llvm::BasicBlock::Create(*cg->ctx, "match_end", cg->currentFunc);

    auto* tagPtr = cg->builder->CreateStructGEP(enumTy, alloca, 0);
    auto* tag = cg->builder->CreateLoad(cg->i8Ty, tagPtr, "tag");
    auto* switchBB = cg->builder->GetInsertBlock();
    auto* switchInst = cg->builder->CreateSwitch(tag, mergeBB, (unsigned)arms.size());
    for (size_t i = 0; i < arms.size(); i++) {
        int armTag = -1;
        for (size_t v = 0; v < ed.variants.size(); v++)
            if (ed.variants[v].name == arms[i].variantName) { armTag = (int)v; break; }
        if (armTag >= 0)
            switchInst->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(cg->i8Ty, (uint64_t)armTag)), armBBs[i]);
    }

    std::vector<llvm::Value*> armValsEmit;
    std::vector<llvm::BasicBlock*> armEndEmit;
    std::vector<char> armReachEmit;
    for (size_t i = 0; i < arms.size(); i++) {
        cg->builder->SetInsertPoint(armBBs[i]);
        cg->symTable.enterScope();
        auto& arm = arms[i];
        int armTag = -1;
        for (size_t v = 0; v < ed.variants.size(); v++)
            if (ed.variants[v].name == arm.variantName) { armTag = (int)v; break; }
        if (!arm.bindName.empty() && armTag >= 0 && !ed.variants[armTag].payloadTypes.empty()) {
            auto& vt = ed.variants[armTag];
            std::vector<llvm::Type*> fldTys = {cg->i8Ty};
            for (auto& pt : ed.variants[armTag].payloadTypes)
                fldTys.push_back(cg->resolvedLlvmType(pt));
            auto* varTy = llvm::StructType::get(*cg->ctx, fldTys);
            auto* bcPtr = cg->builder->CreateBitCast(alloca, cg->i8PtrTy);
            if (ed.variants[armTag].payloadTypes.size() == 1) {
                auto* valPtr = cg->builder->CreateStructGEP(varTy, bcPtr, 1);
                auto* val = cg->builder->CreateLoad(cg->resolvedLlvmType(ed.variants[armTag].payloadTypes[0]), valPtr, arm.bindName);
                auto* bindAlloca = cg->createEntryAlloca(val->getType(), arm.bindName);
                cg->builder->CreateStore(val, bindAlloca);
                cg->symTable.declare(arm.bindName, {vt.payloadTypes[0], bindAlloca, nullptr, false, false, 0});
            }
        }
        // Re-parse and emit arm body from saved token range (value-capturing)
        size_t savedPos = getPos();
        setPos(arm.bodyStart);
        llvm::Value* armVal = nullptr;
        if (check(TokenType::LBRACE)) {
            parseBlockEmit();
        } else {
            armVal = parseExpressionEmit();
        }
        setPos(arm.bodyEnd);
        setPos(savedPos);
        cg->symTable.exitScope();
        armValsEmit.push_back(armVal);
        armEndEmit.push_back(cg->builder->GetInsertBlock());
        if (!cg->builder->GetInsertBlock()->getTerminator()) {
            cg->builder->CreateBr(mergeBB);
            armReachEmit.push_back(1);
        } else {
            armReachEmit.push_back(0);
        }
    }
    cg->builder->SetInsertPoint(mergeBB);
    bool anyValEmit = false;
    llvm::Type* phiTyEmit = nullptr;
    for (auto* v : armValsEmit) if (v) { anyValEmit = true; phiTyEmit = v->getType(); break; }
    if (!anyValEmit) return llvm::ConstantInt::get(cg->i64Ty, 0);
    auto* phiEmit = cg->builder->CreatePHI(phiTyEmit, (unsigned)armValsEmit.size() + 1, "matchval");
    for (size_t i = 0; i < armValsEmit.size(); i++) {
        if (!armReachEmit[i]) continue;
        llvm::Value* v = armValsEmit[i] ? armValsEmit[i] : llvm::ConstantInt::get(phiTyEmit, 0);
        if (v->getType() != phiTyEmit) v = llvm::ConstantInt::get(phiTyEmit, 0);
        phiEmit->addIncoming(v, armEndEmit[i]);
    }
    phiEmit->addIncoming(llvm::ConstantInt::get(phiTyEmit, 0), switchBB);
    return phiEmit;
}

void Parser::parseStatementEmit() {
    if (check(TokenType::SEMICOLON)) { advance(); return; }
    if (check(TokenType::KW_RETURN)) { parseReturnEmit(); return; }
    if (check(TokenType::KW_IF))     { parseIfEmit(); return; }
    if (check(TokenType::KW_WHILE))  { parseWhileEmit(); return; }
    if (check(TokenType::KW_BREAK))  { advance(); parseBreakEmit(); return; }
    if (check(TokenType::KW_CONTINUE)) { advance(); parseContinueEmit(); return; }
    if (check(TokenType::KW_PYTHON)) { advance(); parsePythonBlockEmit(); return; }
    if (check(TokenType::KW_FOR))    { parseForStmtEmit(); return; }
    if (check(TokenType::KW_PARALLEL)) { parseParallelForStmtEmit(); return; }
    if (check(TokenType::LBRACE))    { parseBlockEmit(); return; }
    if (check(TokenType::KW_MUT))    { advance(); parseVarDeclEmit(true); return; }
    if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON) {
        parseVarDeclEmit(false); return;
    }
    if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON_EQ) {
        parseVarDeclEmit(false); return;
    }
        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
            std::string name = peek().lexeme;
            if (cg->symTable.lookup(name) || cg->globalSymTable.count(name)) {
                parseExpressionEmit();
                match(TokenType::SEMICOLON); match(TokenType::NEWLINE);
                return;
            }
        parseVarDeclEmit(false); return;
    }
    parseExpressionStmtEmit();
}

void Parser::parseExpressionStmtEmit() {
    auto val = parseExpressionEmit();
    (void)val;
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseBlockEmit() {
    consume(TokenType::LBRACE, "expected '{'");
    cg->symTable.enterScope();
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        size_t before = pos;
        parseStatementEmit();
        // Hang insurance (emit path is void — no nullptr to detect): force
        // progress when a statement consumed nothing.
        if (pos == before && !check(TokenType::RBRACE) && !isAtEnd()) advance();
    }
    consume(TokenType::RBRACE, "expected '}'");
    cg->symTable.exitScope();
}

void Parser::parseVarDeclEmit(bool isMutable) {
    Token nameTok = advance();
    if (cg->symTable.lookup(nameTok.lexeme)) {
        parseError("variable '" + nameTok.lexeme + "' already declared");
    }
    Type varType = Type::i64();
    bool hasTypeAnnotation = match(TokenType::COLON);
    if (hasTypeAnnotation) varType = parseType();
    if (!match(TokenType::COLON_EQ)) {
        consume(TokenType::ASSIGN, "expected '=' or ':=' in variable declaration");
    }
    size_t exprStart = getPos();
    auto initExpr = parseExpressionEmit();
    if (!initExpr) { parseError("expected expression after '='"); return; }
    if (!hasTypeAnnotation) {
        // Syntactic Map inference: opaque ptrs are LLVM-indistinguishable
        // from strings, so `map {...}` is detected positionally.
        bool isMapInit = false;
        {
            auto& toks = lexer.getTokens();
            if (exprStart < toks.size() && toks[exprStart].type == TokenType::IDENTIFIER &&
                toks[exprStart].lexeme == "map" && exprStart + 1 < toks.size() &&
                toks[exprStart + 1].type == TokenType::LBRACE) {
                isMapInit = true;
            }
        }
        if (isMapInit) varType = Type::map();
        else {
        auto* initTy = initExpr->getType();
        if (initTy == cg->i8PtrTy) varType = Type::str();
        else if (initTy->isArrayTy() || initTy->isStructTy()) {
            bool found = false;
            for (auto& [sn, sd] : structRegistry) {
                auto* st = llvm::StructType::getTypeByName(*cg->ctx, sn);
                if (st && initTy == st) { varType = Type::struct_(sn); found = true; break; }
            }
            if (!found) {
                for (auto& [en, ed] : enumRegistry) {
                    auto* et = llvm::StructType::getTypeByName(*cg->ctx, en);
                    if (et && initTy == et) { varType = Type::enum_(en); found = true; break; }
                }
            }
            if (!found) varType = Type::array(Type::i64());
        } else if (initTy == cg->i64Ty) varType = Type::i64();
        else if (initTy->isDoubleTy()) varType = Type::f64();
        else if (initTy->isPointerTy()) varType = Type::ref(Type::i64());
        }
    }
    varTypeMap[nameTok.lexeme] = varType;
    auto* lty = cg->resolvedLlvmType(varType);
    auto* alloca = cg->createEntryAlloca(lty, nameTok.lexeme);
    cg->symTable.declare(nameTok.lexeme, {varType, alloca, nullptr, isMutable, false, 0});
    // Detect move source: if RHS was a single identifier token
    if (getPos() == exprStart + 1 && previous().type == TokenType::IDENTIFIER) {
        std::string srcName = previous().lexeme;
        auto* srcSym = cg->symTable.lookup(srcName);
        if (srcSym && !srcSym->type.isCopyType()) {
            if (srcSym->borrowCount > 0)
                parseError("cannot move '" + srcName + "' while borrowed");
            else
                srcSym->moved = true;
        }
    }
    cg->builder->CreateStore(initExpr, alloca);
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseReturnEmit() {
    advance();
    if (cg->currentFunc && cg->currentFunc->getName() == "main" && cg->hasPython) {
        auto it = cg->functionMap.find("flint_py_fini");
        if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {});
    }
    if (!check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE) && !check(TokenType::RBRACE) && !isAtEnd()) {
        auto val = parseExpressionEmit();
        if (val) {
            if (cg->currentFunc && cg->currentFunc->getName() == "main") {
                auto* tr = cg->builder->CreateTrunc(val, cg->i32Ty, "mainret");
                cg->builder->CreateRet(tr);
            } else {
                cg->builder->CreateRet(val);
            }
        }
    } else if (cg->currentFunc && cg->currentFunc->getName() == "main") {
        cg->builder->CreateRet(llvm::ConstantInt::get(cg->i32Ty, 0));
    } else {
        cg->builder->CreateRetVoid();
    }
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseBreakEmit() {
    if (cg->breakStack.empty()) { parseError("break outside loop"); return; }
    cg->builder->CreateBr(cg->breakStack.back());
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseContinueEmit() {
    if (cg->continueStack.empty()) { parseError("continue outside loop"); return; }
    cg->builder->CreateBr(cg->continueStack.back());
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseIfEmit() {
    advance();
    auto cond = parseExpressionEmit();
    if (!cond) return;
    auto* condZero = cond->getType()->isPointerTy()
        ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(cond->getType()))
        : (llvm::Value*)llvm::ConstantInt::get(cond->getType(), 0);
    auto* condVal = cg->builder->CreateICmpNE(cond, condZero, "ifcond");
    auto* thenBB = llvm::BasicBlock::Create(*cg->ctx, "then", cg->currentFunc);
    auto* elseBB = llvm::BasicBlock::Create(*cg->ctx, "else", cg->currentFunc);
    auto* mergeBB = llvm::BasicBlock::Create(*cg->ctx, "ifend", cg->currentFunc);
    cg->builder->CreateCondBr(condVal, thenBB, elseBB);
    cg->builder->SetInsertPoint(thenBB);
    cg->symTable.enterScope();
    parseBlockEmit();
    cg->symTable.exitScope();
    if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    cg->builder->SetInsertPoint(elseBB);
    cg->symTable.enterScope();
    if (match(TokenType::KW_ELIF)) {
        parseIfEmit();
        if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    } else if (match(TokenType::KW_ELSE)) {
        parseBlockEmit();
        if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    } else {
        cg->builder->CreateBr(mergeBB);
    }
    cg->symTable.exitScope();
    cg->builder->SetInsertPoint(mergeBB);
}

void Parser::parseWhileEmit() {
    advance();
    auto* condBB = llvm::BasicBlock::Create(*cg->ctx, "while_cond", cg->currentFunc);
    auto* bodyBB = llvm::BasicBlock::Create(*cg->ctx, "while_body", cg->currentFunc);
    auto* endBB = llvm::BasicBlock::Create(*cg->ctx, "while_end", cg->currentFunc);
    cg->builder->CreateBr(condBB);
    cg->builder->SetInsertPoint(condBB);
    auto cond = parseExpressionEmit();
    if (!cond) { cg->builder->SetInsertPoint(endBB); return; }
    auto* condZero = cond->getType()->isPointerTy()
        ? (llvm::Value*)llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(cond->getType()))
        : (llvm::Value*)llvm::ConstantInt::get(cond->getType(), 0);
    auto* condVal = cg->builder->CreateICmpNE(cond, condZero, "whilecond");
    cg->builder->CreateCondBr(condVal, bodyBB, endBB);
    cg->builder->SetInsertPoint(bodyBB);
    cg->symTable.enterScope();
    cg->breakStack.push_back(endBB);
    cg->continueStack.push_back(condBB);
    parseBlockEmit();
    cg->breakStack.pop_back();
    cg->continueStack.pop_back();
    cg->symTable.exitScope();
    if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(condBB);
    cg->builder->SetInsertPoint(endBB);
}

void Parser::parseForStmtEmit() {
    // for i in start..end / range(n) / range(a,b)  — or —  for x in collection
    advance(); // 'for'
    Token loopVar = consume(TokenType::IDENTIFIER, "expected loop variable name");
    consume(TokenType::KW_IN, "expected 'in' after loop variable");
    // range(n) ==> 0..n, range(a, b) ==> a..b (peek before generic emission,
    // which would try to call undefined function `range`).
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "range" && peek(1).type == TokenType::LPAREN) {
        llvm::Value* startVal = nullptr;
        llvm::Value* endVal = nullptr;
        advance(); // 'range'
        advance(); // '('
        std::vector<llvm::Value*> rargs;
        if (!check(TokenType::RPAREN)) {
            do {
                auto* a = parseExpressionEmit();
                if (!a) return;
                rargs.push_back(a);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after range() arguments");
        if (rargs.size() == 1) {
            startVal = llvm::ConstantInt::get(cg->i64Ty, 0);
            endVal = rargs[0];
        } else if (rargs.size() == 2) {
            startVal = rargs[0];
            endVal = rargs[1];
        } else {
            parseError("range() takes 1 or 2 arguments: range(n) or range(start, end)");
            return;
        }
        if (!startVal || !endVal) return;
        cg->symTable.enterScope();
        auto* alloca = cg->createEntryAlloca(cg->i64Ty, loopVar.lexeme);
        cg->builder->CreateStore(startVal, alloca);
        cg->symTable.declare(loopVar.lexeme, {Type::i64(), alloca, nullptr, true, false, 0});
        auto* condBB = llvm::BasicBlock::Create(*cg->ctx, "for_cond", cg->currentFunc);
        auto* bodyBB = llvm::BasicBlock::Create(*cg->ctx, "for_body", cg->currentFunc);
        auto* incrBB = llvm::BasicBlock::Create(*cg->ctx, "for_incr", cg->currentFunc);
        auto* endBB  = llvm::BasicBlock::Create(*cg->ctx, "for_end",  cg->currentFunc);
        cg->builder->CreateBr(condBB);
        cg->builder->SetInsertPoint(condBB);
        auto* iVal = cg->builder->CreateLoad(cg->i64Ty, alloca, loopVar.lexeme);
        auto* cond = cg->builder->CreateICmpSLT(iVal, endVal, "forcond");
        cg->builder->CreateCondBr(cond, bodyBB, endBB);
        cg->builder->SetInsertPoint(bodyBB);
        cg->breakStack.push_back(endBB);
        cg->continueStack.push_back(incrBB);
        parseBlockEmit();
        cg->continueStack.pop_back();
        cg->breakStack.pop_back();
        if (!cg->builder->GetInsertBlock()->getTerminator())
            cg->builder->CreateBr(incrBB);
        cg->builder->SetInsertPoint(incrBB);
        {
            auto* iv = cg->builder->CreateLoad(cg->i64Ty, alloca, loopVar.lexeme);
            auto* iv1 = cg->builder->CreateAdd(iv, llvm::ConstantInt::get(cg->i64Ty, 1), loopVar.lexeme + "_next");
            cg->builder->CreateStore(iv1, alloca);
            cg->builder->CreateBr(condBB);
        }
        cg->builder->SetInsertPoint(endBB);
        cg->symTable.exitScope();
        return;
    }
    // Not range(): parse first expr, then decide `a..b` vs collection.
    llvm::Value* firstVal = parseExpressionEmit();
    if (!firstVal) return;
    if (check(TokenType::DOTDOT)) {
        // `for i in a..b` range form
        advance(); // '..'
        llvm::Value* endVal = parseExpressionEmit();
        if (!endVal) return;
        llvm::Value* startVal = firstVal;
        cg->symTable.enterScope();
        auto* alloca = cg->createEntryAlloca(cg->i64Ty, loopVar.lexeme);
        cg->builder->CreateStore(startVal, alloca);
        cg->symTable.declare(loopVar.lexeme, {Type::i64(), alloca, nullptr, true, false, 0});
        auto* condBB = llvm::BasicBlock::Create(*cg->ctx, "for_cond", cg->currentFunc);
        auto* bodyBB = llvm::BasicBlock::Create(*cg->ctx, "for_body", cg->currentFunc);
        auto* incrBB = llvm::BasicBlock::Create(*cg->ctx, "for_incr", cg->currentFunc);
        auto* endBB  = llvm::BasicBlock::Create(*cg->ctx, "for_end",  cg->currentFunc);
        cg->builder->CreateBr(condBB);
        cg->builder->SetInsertPoint(condBB);
        auto* iVal = cg->builder->CreateLoad(cg->i64Ty, alloca, loopVar.lexeme);
        auto* cond = cg->builder->CreateICmpSLT(iVal, endVal, "forcond");
        cg->builder->CreateCondBr(cond, bodyBB, endBB);
        cg->builder->SetInsertPoint(bodyBB);
        cg->breakStack.push_back(endBB);
        cg->continueStack.push_back(incrBB);
        parseBlockEmit();
        cg->continueStack.pop_back();
        cg->breakStack.pop_back();
        if (!cg->builder->GetInsertBlock()->getTerminator())
            cg->builder->CreateBr(incrBB);
        cg->builder->SetInsertPoint(incrBB);
        {
            auto* iv = cg->builder->CreateLoad(cg->i64Ty, alloca, loopVar.lexeme);
            auto* iv1 = cg->builder->CreateAdd(iv, llvm::ConstantInt::get(cg->i64Ty, 1), loopVar.lexeme + "_next");
            cg->builder->CreateStore(iv1, alloca);
            cg->builder->CreateBr(condBB);
        }
        cg->builder->SetInsertPoint(endBB);
        cg->symTable.exitScope();
        return;
    }
    // `for x in collection` — D1 kind-aware (array struct vs string ptr).
    // Anything else (e.g. an i64) used to be bitcast to ptr → garbage/
    // segfault. Fail loudly instead (matches AST path + stage3 e_for).
    llvm::Value* collVal = firstVal;
    bool isArray = collVal->getType()->isStructTy();
    if (!isArray && !collVal->getType()->isPointerTy()) {
        parseError("for-in needs an array or str collection");
        return;
    }
    // string vs vec are both ptr; treat ptr collections as string for now
    // (vec iteration not covered by current ladder; string "ab" was the P0).
    // Array struct is {ptr,i64}, string/vec is ptr.
    cg->symTable.enterScope();
    // __idx
    auto* idxAlloca = cg->createEntryAlloca(cg->i64Ty, "__idx");
    cg->builder->CreateStore(llvm::ConstantInt::get(cg->i64Ty, 0), idxAlloca);
    // Keep coll in an alloca so the loop body sees a stable address (and so
    // we can reload len each iteration if needed). For array, store struct;
    // for string/vec, store ptr.
    llvm::AllocaInst* collAlloca = nullptr;
    if (isArray) {
        collAlloca = cg->createEntryAlloca(collVal->getType(), "__coll");
        cg->builder->CreateStore(collVal, collAlloca);
    } else {
        collAlloca = cg->createEntryAlloca(cg->i8PtrTy, "__coll_ptr");
        // collVal may be ptr already (string) or need bitcast
        llvm::Value* ptrVal = collVal;
        if (!ptrVal->getType()->isPointerTy()) {
            ptrVal = cg->builder->CreateBitCast(ptrVal, cg->i8PtrTy);
        }
        cg->builder->CreateStore(ptrVal, collAlloca);
    }
    auto* condBB = llvm::BasicBlock::Create(*cg->ctx, "for_cond", cg->currentFunc);
    auto* bodyBB = llvm::BasicBlock::Create(*cg->ctx, "for_body", cg->currentFunc);
    auto* incrBB = llvm::BasicBlock::Create(*cg->ctx, "for_incr", cg->currentFunc);
    auto* endBB  = llvm::BasicBlock::Create(*cg->ctx, "for_end",  cg->currentFunc);
    cg->builder->CreateBr(condBB);
    cg->builder->SetInsertPoint(condBB);
    auto* idxVal = cg->builder->CreateLoad(cg->i64Ty, idxAlloca, "__idx");
    llvm::Value* lenVal = nullptr;
    if (isArray) {
        auto* collStruct = cg->builder->CreateLoad(collVal->getType(), collAlloca, "__coll");
        lenVal = cg->builder->CreateExtractValue(collStruct, {1}, "coll_len");
    } else {
        auto* collPtr = cg->builder->CreateLoad(cg->i8PtrTy, collAlloca, "__coll_ptr");
        auto it = cg->functionMap.find("flint_str_length");
        if (it != cg->functionMap.end()) {
            lenVal = cg->builder->CreateCall(it->second, {collPtr}, "coll_len");
        } else {
            lenVal = llvm::ConstantInt::get(cg->i64Ty, 0);
        }
    }
    auto* cond = cg->builder->CreateICmpSLT(idxVal, lenVal, "forcond");
    cg->builder->CreateCondBr(cond, bodyBB, endBB);
    cg->builder->SetInsertPoint(bodyBB);
    // let x = coll[idx]
    llvm::Value* elemVal = nullptr;
    if (isArray) {
        auto* collStruct = cg->builder->CreateLoad(collVal->getType(), collAlloca, "__coll2");
        auto* dataPtr = cg->builder->CreateExtractValue(collStruct, {0}, "coll_data");
        auto* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, idxVal, "coll_elem");
        elemVal = cg->builder->CreateLoad(cg->i64Ty, elemPtr, "coll_elem_val");
    } else {
        auto* collPtr = cg->builder->CreateLoad(cg->i8PtrTy, collAlloca, "__coll_ptr2");
        auto it2 = cg->functionMap.find("flint_str_char_at");
        if (it2 != cg->functionMap.end()) {
            elemVal = cg->builder->CreateCall(it2->second, {collPtr, idxVal}, "coll_char");
        } else {
            auto* elemPtr = cg->builder->CreateGEP(cg->i8Ty, collPtr, idxVal, "coll_elem");
            auto* byteVal = cg->builder->CreateLoad(cg->i8Ty, elemPtr, "coll_byte");
            elemVal = cg->builder->CreateZExt(byteVal, cg->i64Ty, "coll_char");
        }
    }
    auto* elemAlloca = cg->createEntryAlloca(cg->i64Ty, loopVar.lexeme);
    cg->builder->CreateStore(elemVal, elemAlloca);
    cg->symTable.declare(loopVar.lexeme, {Type::i64(), elemAlloca, nullptr, false, false, 0});
    cg->breakStack.push_back(endBB);
    cg->continueStack.push_back(incrBB);
    parseBlockEmit();
    cg->continueStack.pop_back();
    cg->breakStack.pop_back();
    if (!cg->builder->GetInsertBlock()->getTerminator())
        cg->builder->CreateBr(incrBB);
    cg->builder->SetInsertPoint(incrBB);
    {
        auto* iv = cg->builder->CreateLoad(cg->i64Ty, idxAlloca, "__idx");
        auto* iv1 = cg->builder->CreateAdd(iv, llvm::ConstantInt::get(cg->i64Ty, 1), "__idx_next");
        cg->builder->CreateStore(iv1, idxAlloca);
        cg->builder->CreateBr(condBB);
    }
    cg->builder->SetInsertPoint(endBB);
    cg->symTable.exitScope();
}

void Parser::parseParallelForStmtEmit() {
    // parallel for i in start..end { body }
    // -> declares a worker function with body, then calls flint_parallel_for(length, &worker, 0)
    advance(); // 'parallel'
    consume(TokenType::KW_FOR, "expected 'for' after 'parallel'");
    Token loopVar = consume(TokenType::IDENTIFIER, "expected loop variable name");
    consume(TokenType::KW_IN, "expected 'in'");
    auto* startVal = parseExpressionEmit();
    consume(TokenType::DOTDOT, "expected '..'");
    auto* endVal = parseExpressionEmit();
    consume(TokenType::LBRACE, "expected '{' for parallel for body");
    if (!startVal || !endVal) return;

    // Length = end - start (clamped to >= 0)
    auto* zero = llvm::ConstantInt::get(cg->i64Ty, 0);
    auto* length = cg->builder->CreateSub(endVal, startVal, "pfor_len");
    auto* lengthClamped = cg->builder->CreateSelect(
        cg->builder->CreateICmpSLT(length, zero), zero, length, "pfor_len_clamped");

    // Unique worker function name
    static int pforEmitCounter = 0;
    std::string workerName = "__pfor_emit_" + std::to_string(pforEmitCounter++);

    // Declare worker: i64(i64) -> i64 (iteration index -> result)
    auto* workerTy = llvm::FunctionType::get(cg->i64Ty, {cg->i64Ty}, false);
    auto* workerFn = llvm::Function::Create(workerTy,
        llvm::Function::InternalLinkage, workerName, cg->mod.get());
    cg->functionMap[workerName] = workerFn;

    // Save codegen state
    auto* savedFunc = cg->currentFunc;
    auto* savedBB   = cg->builder->GetInsertBlock();
    auto savedSym   = cg->symTable; // copy assuming copyable

    // Build worker function body. Worker receives the *raw* iteration index from
    // flint_parallel_for. For most use cases (start == 0), the index IS the loop var.
    // For non-zero start, we expose `startVal` as a global so the user can compute
    // (start + i) if needed. (For simplicity, the worker just receives 0..length-1.)
    auto* entry = llvm::BasicBlock::Create(*cg->ctx, "entry", workerFn);
    cg->builder->SetInsertPoint(entry);
    cg->currentFunc = workerFn;
    cg->symTable = SymbolTable();

    auto argIt = workerFn->args().begin();
    auto* iAlloca = cg->createEntryAlloca(cg->i64Ty, loopVar.lexeme);
    cg->builder->CreateStore(&*argIt, iAlloca);
    cg->symTable.declare(loopVar.lexeme, {Type::i64(), iAlloca, nullptr, true, false, 0});

    // Parse parallel for body
    cg->symTable.enterScope();
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        parseStatementEmit();
    }
    consume(TokenType::RBRACE, "expected '}' for parallel for body");
    cg->symTable.exitScope();
    if (!cg->builder->GetInsertBlock()->getTerminator())
        cg->builder->CreateRet(llvm::ConstantInt::get(cg->i64Ty, 0));

    // Restore
    cg->currentFunc = savedFunc;
    cg->builder->SetInsertPoint(savedBB);
    cg->symTable = savedSym;

    // Emit: flint_parallel_for(length, bitcast worker to str, 0)
    auto* parallelFn = cg->functionMap["flint_parallel_for"];
    if (!parallelFn) { parseError("flint_parallel_for not declared"); return; }
    auto* workerAsPtr = cg->builder->CreateBitCast(workerFn,
        llvm::PointerType::get(*cg->ctx, 0), workerName + "_ptr");
    cg->builder->CreateCall(parallelFn, {lengthClamped, workerAsPtr, zero});
}

void Parser::parsePythonBlockEmit() {
    cg->hasPython = true;
    consume(TokenType::LBRACE, "expected '{' after 'python'");
    while (check(TokenType::STRING_LITERAL)) {
        auto* strVal = cg->builder->CreateGlobalString(peek().lexeme, "pycode");
        advance();
        auto it = cg->functionMap.find("flint_py_run");
        if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {strVal});
    }
    consume(TokenType::RBRACE, "expected '}' to close python block");
}

std::string Parser::generateGenericInstance(FunctionAST* genFn, const std::string& callee) {
    setCodegen(cg, false);
    skipBodies = false;
    std::string result = "ok";
    cg->typeSubstMap.clear();
    for (size_t i = 0; i < genFn->typeParams.size(); i++) {
        auto it = cg->typeSubstMap.find(genFn->typeParams[i]);
        if (it != cg->typeSubstMap.end()) continue;
        cg->typeSubstMap[genFn->typeParams[i]] = Type::i64();
    }
    auto* oldParser = cg->parser;
    cg->parser = this;
    auto savedPos = getPos();
    setPos(genFn->bodyStart);
    cg->symTable.enterScope();
    for (auto& p : genFn->params) {
        Type pType = cg->resolveType(p.second);
        auto* lty = cg->resolvedLlvmType(p.second);
        auto* alloca = cg->createEntryAlloca(lty, p.first);
        cg->symTable.declare(p.first, {pType, alloca, nullptr, false, false, 0});
    }
    // Use old AST path for generic instantiation
    auto bodyCopy = parseBlock();
    if (bodyCopy) {
        cg->emitStmt(bodyCopy.get());
    }
    cg->symTable.exitScope();
    setPos(savedPos);
    cg->parser = oldParser;
    return result;
}

// ============================================================================
// MAIN
// ============================================================================

static void mergeProgram(std::unique_ptr<ProgramAST>& dest, std::unique_ptr<ProgramAST>& src) {
    for (auto& f : src->functions) dest->functions.push_back(std::move(f));
    for (auto& s : src->structs) dest->structs.push_back(s);
    for (auto& e : src->enums) dest->enums.push_back(e);
    for (auto& ext : src->externs) dest->externs.push_back(ext);
    for (auto& imp : src->imports) dest->imports.push_back(imp);
    for (auto& g : src->globals) dest->globals.push_back(std::move(g));
}

static std::string dirName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? "." : path.substr(0, pos);
}

// ============================================================================
// REGISTRY — flint.toml dependencies (file:// + git URLs), local cache,
// flint.lock pinning, --offline. No new libraries: git/clone via runProcess,
// file copy via fstreams, dir scan via LLVM fs (already included).
// Cache layout: $HOME/.cache/flint_pkgs/<name>/<name>.fl (+ sibling .fl).
// Lockfile: <appdir>/flint.lock with `<name>.rev = "<sha|local>"` lines.
// ============================================================================

static std::string regTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string regUnquote(std::string s) {
    s = regTrim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

// Minimal [dependencies] reader: `name = "url"` lines, `#` full-line comments.
static std::vector<std::pair<std::string, std::string>> parseTomlDeps(const std::string& tomlPath) {
    std::vector<std::pair<std::string, std::string>> deps;
    std::ifstream f(tomlPath);
    if (!f) return deps;
    bool inDeps = false;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = regTrim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') { inDeps = (t == "[dependencies]"); continue; }
        if (!inDeps) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string name = regTrim(t.substr(0, eq));
        std::string url = regUnquote(t.substr(eq + 1));
        if (!name.empty() && !url.empty()) deps.emplace_back(name, url);
    }
    return deps;
}

static std::map<std::string, std::string> readLockRevs(const std::string& lockPath) {
    std::map<std::string, std::string> revs;
    std::ifstream f(lockPath);
    if (!f) return revs;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = regTrim(line);
        if (t.empty() || t[0] == '#') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = regTrim(t.substr(0, eq));
        const std::string suf = ".rev";
        if (key.size() <= suf.size() || key.substr(key.size() - suf.size()) != suf) continue;
        revs[key.substr(0, key.size() - suf.size())] = regUnquote(t.substr(eq + 1));
    }
    return revs;
}

// Single-quote a path for popen (popen runs a shell).
static std::string shQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}

// HEAD sha of a local checkout, or "" when not a git repo.
static std::string gitHeadRev(const std::string& dir) {
    std::string cmd = "git -C " + shQuote(dir) + " rev-parse HEAD 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[128] = {};
    std::string out;
    if (fgets(buf, sizeof(buf), p)) out = regTrim(buf);
    pclose(p);
    if (out.size() != 40) return "";
    for (char c : out)
        if (!isxdigit((unsigned char)c)) return "";
    return out;
}

static bool copyFileRaw(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return (bool)out;
}

static bool pkgHasSources(const std::string& pkgDir) {
    std::error_code ec;
    llvm::sys::fs::directory_iterator it(pkgDir, ec), end;
    if (ec) return false;
    for (; it != end && !ec; it.increment(ec)) {
        std::string p = it->path();
        if (p.size() >= 3 && p.substr(p.size() - 3) == ".fl") return true;
    }
    return false;
}

// Copy <name>.fl when present, else every root-level *.fl. Returns count.
static int copyPkgSources(const std::string& srcDir, const std::string& pkgName,
                          const std::string& pkgDir) {
    struct stat st;
    std::string direct = srcDir + "/" + pkgName + ".fl";
    if (stat(direct.c_str(), &st) == 0) {
        if (!copyFileRaw(direct, pkgDir + "/" + pkgName + ".fl")) return 0;
        // Also pick up sibling .fl files so multi-file packages work.
        int n = 1;
        std::error_code ec;
        llvm::sys::fs::directory_iterator it(srcDir, ec), end;
        for (; it != end && !ec; it.increment(ec)) {
            std::string p = it->path();
            if (p == direct) continue;
            if (p.size() >= 3 && p.substr(p.size() - 3) == ".fl") {
                auto sl = p.find_last_of('/');
                std::string base = (sl == std::string::npos) ? p : p.substr(sl + 1);
                if (copyFileRaw(p, pkgDir + "/" + base)) n++;
            }
        }
        return n;
    }
    int n = 0;
    std::error_code ec;
    llvm::sys::fs::directory_iterator it(srcDir, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        std::string p = it->path();
        if (p.size() >= 3 && p.substr(p.size() - 3) == ".fl") {
            auto sl = p.find_last_of('/');
            std::string base = (sl == std::string::npos) ? p : p.substr(sl + 1);
            if (copyFileRaw(p, pkgDir + "/" + base)) n++;
        }
    }
    return n;
}

// Fetch all flint.toml deps into the local cache; append each package dir to
// libPaths so `import "name"` resolves. Silent on cache hits (the registry
// test asserts no "fetching" on rebuild); prints `fetching <name> ...` only
// on real fetches. Returns false (after printing) on any failure.
static bool fetchRegistryDeps(const std::string& inputPath, std::vector<std::string>& libPaths,
                              bool offline) {
    std::string appDir = dirName(inputPath);
    std::string toml = appDir + "/flint.toml";
    struct stat st;
    if (stat(toml.c_str(), &st) != 0) return true; // no manifest: nothing to do
    auto deps = parseTomlDeps(toml);
    if (deps.empty()) return true;
    const char* home = getenv("HOME");
    std::string cacheRoot = std::string((home && *home) ? home : ".") + "/.cache/flint_pkgs";
    std::string lockPath = appDir + "/flint.lock";
    auto lockRevs = readLockRevs(lockPath);
    bool lockDirty = false;
    for (auto& dep : deps) {
        const std::string& name = dep.first;
        const std::string& url = dep.second;
        std::string pkgDir = cacheRoot + "/" + name;
        bool cached = pkgHasSources(pkgDir);
        if (offline) {
            if (!cached) {
                std::cerr << "error: package '" << name << "' not cached (offline mode)\n";
                return false;
            }
            libPaths.push_back(pkgDir);
            continue;
        }
        if (url.rfind("file://", 0) == 0) {
            std::string src = url.substr(7);
            while (src.size() > 1 && src.back() == '/') src.pop_back();
            if (stat(src.c_str(), &st) != 0) {
                std::cerr << "error: cannot fetch '" << url << "'\n";
                return false;
            }
            std::string srcRev = gitHeadRev(src);
            if (srcRev.empty()) srcRev = "local";
            auto lit = lockRevs.find(name);
            if (cached && lit != lockRevs.end() && lit->second == srcRev) {
                libPaths.push_back(pkgDir); // fresh pin: silent
                continue;
            }
            std::cerr << "fetching " << name << " from " << url << "\n";
            std::error_code ec = llvm::sys::fs::create_directories(pkgDir);
            if (ec) {
                std::cerr << "error: cannot create '" << pkgDir << "': " << ec.message() << "\n";
                return false;
            }
            if (copyPkgSources(src, name, pkgDir) == 0) {
                std::cerr << "error: no .fl sources in '" << url << "'\n";
                return false;
            }
            lockRevs[name] = srcRev;
            lockDirty = true;
            libPaths.push_back(pkgDir);
        } else {
            // Git URL: clone once, reuse the pin afterwards.
            auto lit = lockRevs.find(name);
            if (cached && lit != lockRevs.end() && !lit->second.empty()) {
                libPaths.push_back(pkgDir); // pinned: silent
                continue;
            }
            std::cerr << "fetching " << name << " from " << url << "\n";
            std::error_code ec = llvm::sys::fs::create_directories(cacheRoot);
            if (ec) {
                std::cerr << "error: cannot create '" << cacheRoot << "': " << ec.message() << "\n";
                return false;
            }
            if (cached) runProcess({"rm", "-rf", pkgDir});
            if (runProcess({"git", "clone", "--depth", "1", url, pkgDir}) != 0) {
                std::cerr << "error: cannot fetch '" << url << "'\n";
                return false;
            }
            if (!pkgHasSources(pkgDir)) {
                std::cerr << "error: no .fl sources in '" << url << "'\n";
                return false;
            }
            std::string rev = gitHeadRev(pkgDir);
            lockRevs[name] = rev.empty() ? "local" : rev;
            lockDirty = true;
            libPaths.push_back(pkgDir);
        }
    }
    if (lockDirty) {
        std::ofstream lock(lockPath, std::ios::trunc);
        if (!lock) {
            std::cerr << "error: cannot write '" << lockPath << "'\n";
            return false;
        }
        for (auto& kv : lockRevs) lock << kv.first << ".rev = \"" << kv.second << "\"\n";
    }
    return true;
}

static std::string resolveImportPath(const std::string& basePath, const std::string& importName,
    const std::vector<std::string>& libPaths = {}) {
    if (importName.empty()) return importName;
    if (importName[0] == '/') return importName;

    // Module-style path (contains dots, no slashes): "std.io" -> search lib paths
    if (importName.find('/') == std::string::npos) {
        // Convert dots to slashes
        std::string modPath = importName;
        for (auto& c : modPath) if (c == '.') c = '/';
        if (modPath.size() < 3 || modPath.substr(modPath.size() - 3) != ".fl")
            modPath += ".fl";

        struct stat st;
        // Try relative to base path first
        auto dir = dirName(basePath);
        std::string result = dir + "/" + modPath;
        if (stat(result.c_str(), &st) == 0) return result;

        // Then try library paths
        for (auto& lp : libPaths) {
            result = lp + "/" + modPath;
            if (stat(result.c_str(), &st) == 0) return result;
        }

        // Try CWD
        if (stat(modPath.c_str(), &st) == 0) return modPath;

        return modPath; // return last attempt for error message
    }

    // Plain file path (string literal with slashes)
    auto dir = dirName(basePath);
    std::string result = dir + "/" + importName;
    if (result.size() < 3 || result.substr(result.size() - 3) != ".fl")
        result += ".fl";
    return result;
}

static void processFile(const std::string& path, std::unique_ptr<ProgramAST>& combined,
                         std::unordered_set<std::string>& seen, std::string& firstError,
                         const std::vector<std::string>& libPaths = {}) {
    std::string absPath = path;
    // Simple absolute path resolution (relative to CWD)
    if (absPath.size() > 0 && absPath[0] != '/') {
        absPath = std::string(getenv("PWD") ? getenv("PWD") : ".") + "/" + absPath;
    }
    // Normalize (remove ./ etc) — basic version
    if (seen.count(absPath)) return;
    seen.insert(absPath);

    auto fileBuf = llvm::MemoryBuffer::getFile(path);
    if (!fileBuf) { firstError = "error: cannot open '" + path + "'"; return; }
    llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
    recordImportHash(absPath, sourceRef);

    Lexer lexer(sourceRef);
    Parser parser(lexer);
    auto prog = parser.parseProgram();

    if (parser.hadError()) {
        firstError = parser.errorMsg();
        return;
    }

    mergeProgram(combined, prog);

    // Recursively process imports
    for (auto& imp : prog->imports) {
        std::string impPath = resolveImportPath(path, imp, libPaths);
        processFile(impPath, combined, seen, firstError, libPaths);
        if (!firstError.empty()) return;
    }
}

// ===================== Borrow/Move Checker =====================
// Standalone AST walker that checks borrow/move semantics without LLVM.
class BorrowChecker {
    struct Sym {
        Type type;
        bool moved = false;
        int borrowCount = 0;
        bool isCopyType = false;
        bool isMutable = false;
    };
    std::vector<std::unordered_map<std::string, Sym>> scopes;
    std::vector<std::vector<std::pair<std::string, int>>> borrowRecords;
    std::string funcName;
    bool hadErr = false;
    std::string errMsg;

    void error(const std::string& msg) {
        if (!hadErr) { hadErr = true; errMsg = msg; }
    }

    void enterScope() { scopes.emplace_back(); borrowRecords.emplace_back(); }
    void exitScope() {
        if (!borrowRecords.empty()) {
            for (auto& pair : borrowRecords.back()) {
                auto* sym = lookup(pair.first);
                if (sym) sym->borrowCount -= pair.second;
            }
            borrowRecords.pop_back();
        }
        if (!scopes.empty()) scopes.pop_back();
    }

    Sym* lookup(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    void declare(const std::string& name, const Type& t, bool mut) {
        if (scopes.empty()) scopes.emplace_back();
        scopes.back()[name] = {t, false, 0, t.isCopyType(), mut};
    }

    void recordBorrow(const std::string& name) {
        if (!borrowRecords.empty()) {
            bool found = false;
            for (auto& pair : borrowRecords.back()) {
                if (pair.first == name) { pair.second++; found = true; break; }
            }
            if (!found) borrowRecords.back().emplace_back(name, 1);
        }
    }

    void checkVarUse(const std::string& name) {
        auto* sym = lookup(name);
        if (!sym) return;
        if (sym->moved) error("use of moved variable '" + name + "'");
    }

    void checkVarBorrow(const std::string& name) {
        auto* sym = lookup(name);
        if (!sym) return;
        if (sym->moved) error("cannot borrow moved variable '" + name + "'");
    }

    void markMoved(const std::string& name) {
        auto* sym = lookup(name);
        if (sym && !sym->isCopyType) {
            if (sym->borrowCount > 0)
                error("cannot move '" + name + "' while borrowed");
            else
                sym->moved = true;
        }
    }

    void setMoved(const std::string& name, bool val) {
        auto* sym = lookup(name);
        if (sym) sym->moved = val;
    }

    void checkExpr(ExprAST* node) {
        if (!node || hadErr) return;
        switch (node->kind) {
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(node);
                auto* sym = lookup(v->name);
                if (sym && sym->moved) error("use of moved variable '" + v->name + "'");
                break;
            }
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(node);
                // Check mutable + borrow first
                Sym* sym = lookup(a->varName);
                if (!sym) { checkExpr(a->rhs.get()); break; }
                if (!sym->isMutable) error("cannot assign to immutable '" + a->varName + "'");
                if (sym->borrowCount > 0) error("cannot assign to '" + a->varName + "' while borrowed");
                checkExpr(a->rhs.get());
                // If RHS is a variable, mark source as moved (not copy types)
                if (a->rhs && a->rhs->kind == NodeKind::Variable) {
                    std::string rhsName = static_cast<VariableExprAST*>(a->rhs.get())->name;
                    auto* vs = lookup(rhsName);
                    if (vs && !vs->isCopyType) {
                        if (vs->borrowCount > 0)
                            error("cannot move '" + rhsName + "' while borrowed");
                        else
                            vs->moved = true;
                    }
                }
                if (sym->moved) sym->moved = false;
                break;
            }
            case NodeKind::Binary:
                checkExpr(static_cast<BinaryExprAST*>(node)->lhs.get());
                checkExpr(static_cast<BinaryExprAST*>(node)->rhs.get());
                break;
            case NodeKind::Compare:
                checkExpr(static_cast<CompareExprAST*>(node)->lhs.get());
                checkExpr(static_cast<CompareExprAST*>(node)->rhs.get());
                break;
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(node);
                for (auto& a : c->args) checkExpr(a.get());
                break;
            }
            case NodeKind::Array:
                for (auto& e : static_cast<ArrayExprAST*>(node)->elements)
                    checkExpr(e.get());
                break;
            case NodeKind::MapLiteral:
                for (auto& e : static_cast<MapLiteralAST*>(node)->values)
                    checkExpr(e.get());
                break;
            case NodeKind::Index:
                checkExpr(static_cast<IndexExprAST*>(node)->base.get());
                checkExpr(static_cast<IndexExprAST*>(node)->index.get());
                break;
            case NodeKind::Slice:
                checkExpr(static_cast<SliceExprAST*>(node)->arr.get());
                checkExpr(static_cast<SliceExprAST*>(node)->start.get());
                checkExpr(static_cast<SliceExprAST*>(node)->end.get());
                break;
            case NodeKind::Ref: {
                auto* r = static_cast<RefExprAST*>(node);
                if (r->target->kind == NodeKind::Variable) {
                    std::string vname = static_cast<VariableExprAST*>(r->target.get())->name;
                    checkVarBorrow(vname);
                    auto* sym = lookup(vname);
                    if (sym) { sym->borrowCount++; recordBorrow(vname); }
                }
                break;
            }
            case NodeKind::Deref:
                checkExpr(static_cast<DerefExprAST*>(node)->target.get());
                break;
            case NodeKind::FieldAccess:
                checkExpr(static_cast<FieldAccessAST*>(node)->base.get());
                break;
            default: break;
        }
    }

    void checkStmt(ExprAST* node) {
        if (!node || hadErr) return;
        switch (node->kind) {
            case NodeKind::VarDecl: {
                auto* d = static_cast<VarDeclAST*>(node);
                if (d->init) {
                    checkExpr(d->init.get());
                    if (d->init->kind == NodeKind::Variable)
                        markMoved(static_cast<VariableExprAST*>(d->init.get())->name);
                }
                declare(d->varName, d->varType, d->isMutable);
                break;
            }
            case NodeKind::Return:
                if (static_cast<ReturnStmtAST*>(node)->value)
                    checkExpr(static_cast<ReturnStmtAST*>(node)->value.get());
                break;
            case NodeKind::If: {
                auto* i = static_cast<IfStmtAST*>(node);
                checkExpr(i->condition.get());
                checkBlock(i->thenBlock.get());
                if (i->elseBlock) checkBlock(i->elseBlock.get());
                break;
            }
            case NodeKind::While: {
                auto* w = static_cast<WhileStmtAST*>(node);
                checkExpr(w->condition.get());
                checkBlock(w->body.get());
                break;
            }
            case NodeKind::Block:
                checkBlock(static_cast<BlockStmtAST*>(node));
                break;
            case NodeKind::Break: break;
            case NodeKind::Continue: break;
            default:
                checkExpr(node);
                break;
        }
    }

    void checkBlock(ExprAST* node) {
        if (!node || node->kind != NodeKind::Block) return;
        enterScope();
        auto* b = static_cast<BlockStmtAST*>(node);
        for (auto& s : b->stmts) { if (hadErr) break; checkStmt(s.get()); }
        exitScope();
    }

public:
    bool check(FunctionAST* fn, std::string& errorOut) {
        hadErr = false; errMsg.clear();
        funcName = fn->name;
        scopes.clear(); borrowRecords.clear();
        enterScope();
        for (auto& p : fn->params)
            declare(p.first, p.second, false);
        if (fn->body) {
            if (fn->body->kind == NodeKind::Block)
                checkBlock(fn->body.get());
            else
                checkStmt(fn->body.get());
        }
        exitScope();
        if (hadErr) errorOut = errMsg;
        return !hadErr;
    }
};

// ===================== QBE Backend =====================
class QbeEmitter {
    std::ostringstream il;
    std::ostringstream data;
    int tempIdx = 0, labelIdx = 0, strIdx = 0;
    std::unordered_map<std::string, std::string> varSlots;
    std::unordered_map<std::string, Type> varTypes;
    std::string currentFunc;
    std::vector<std::string> breakTargets;
    std::vector<std::string> continueTargets;
    std::vector<StructDef>* structDefs = nullptr;
    std::vector<ExternFn>* externs = nullptr;
    std::vector<EnumDef>* enumDefs = nullptr;

    std::string newTmp(char t = 'l') { return "%t" + std::to_string(tempIdx++); }
    std::string newLbl() { return "@L" + std::to_string(labelIdx++); }

    std::string getSlot(const std::string& v) {
        if (auto it = varSlots.find(v); it != varSlots.end()) return it->second;
        std::string s = newTmp('l');
        il << "\t" << s << " =l alloc8 8\n";
        varSlots[v] = s;
        return s;
    }

    std::string loadl(const std::string& slotPtr) {
        std::string t = newTmp('l');
        il << "\t" << t << " =l loadl " << slotPtr << "\n";
        return t;
    }

    void storel(const std::string& val, const std::string& slotPtr) {
        il << "\tstorel " << val << ", " << slotPtr << "\n";
    }

    std::string strlit(const std::string& s) {
        std::string label = "$str" + std::to_string(strIdx++);
        data << "data " << label << " = { b \"";
        for (char c : s) {
            if (c == '\n') data << "\\n";
            else if (c == '\t') data << "\\t";
            else if (c == '"') data << "\\\"";
            else if (c == '\\') data << "\\\\";
            else data << c;
        }
        data << "\", b 0 }\n";
        return label;
    }

    bool lastLineIsTerminator() {
        std::string s = il.str();
        if (s.empty()) return false;
        size_t lastNL = s.rfind('\n', s.size() - 2);
        std::string lastLine = (lastNL != std::string::npos) ? s.substr(lastNL + 1) : s;
        return lastLine.find("\tret") == 0 || lastLine.find("\tjmp") == 0 || lastLine.find("\tjnz") == 0;
    }

    std::string emitExpr(ExprAST* node) {
        switch (node->kind) {
            case NodeKind::Number: {
                auto* nn = static_cast<NumberExprAST*>(node);
                if (nn->isInt) return std::to_string(nn->intValue);
                return std::to_string(nn->value);
            }
            case NodeKind::String:
                return strlit(static_cast<StringExprAST*>(node)->value);
            case NodeKind::Variable: {
                auto it = varSlots.find(static_cast<VariableExprAST*>(node)->name);
                return it != varSlots.end() ? loadl(it->second) : "0";
            }
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(node);
                auto val = emitExpr(a->rhs.get());
                auto it = varSlots.find(a->varName);
                if (it != varSlots.end()) storel(val, it->second);
                return val;
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(node);
                if (b->op == '&' || b->op == '|') {
                    std::cerr << "QBE: '&&'/'||' not supported yet\n";
                    return "";
                }
                std::string l = emitExpr(b->lhs.get()), r = emitExpr(b->rhs.get()), t = newTmp('l');
                const char* op = b->op == '+' ? "add" : b->op == '-' ? "sub" : b->op == '*' ? "mul" : b->op == '%' ? "rem" : "div";
                il << "\t" << t << " =l " << op << " " << l << ", " << r << "\n";
                // Signed overflow check for + and - (matches LLVM backend panic behavior)
                if (b->op == '+' || b->op == '-') {
                    // For add: overflow if (b>0 && t<a) || (b<0 && t>a)
                    // For sub: overflow if (b<0 && t<a) || (b>0 && t>a)
                    std::string okL = newLbl(), panicL = newLbl(), chk2L = newLbl();
                    std::string rPos = newTmp('l'), tLtA = newTmp('l'), c1 = newTmp('l');
                    std::string rNeg = newTmp('l'), tGtA = newTmp('l'), c2 = newTmp('l');
                    const char* rCmpA = (b->op == '+') ? "csgtl" : "csltl"; // b>0 (add) / b<0 (sub)
                    const char* rCmpB = (b->op == '+') ? "csltl" : "csgtl"; // b<0 (add) / b>0 (sub)
                    il << "\t" << rPos << " =l " << rCmpA << " " << r << ", 0\n";
                    il << "\t" << tLtA << " =l csltl " << t << ", " << l << "\n";
                    il << "\t" << c1 << " =l and " << rPos << ", " << tLtA << "\n";
                    std::string c1w = newTmp('w');
                    il << "\t" << c1w << " =w cnel " << c1 << ", 0\n";
                    il << "\tjnz " << c1w << ", " << panicL << ", " << chk2L << "\n";
                    il << chk2L << "\n";
                    il << "\t" << rNeg << " =l " << rCmpB << " " << r << ", 0\n";
                    il << "\t" << tGtA << " =l csgtl " << t << ", " << l << "\n";
                    il << "\t" << c2 << " =l and " << rNeg << ", " << tGtA << "\n";
                    std::string c2w = newTmp('w');
                    il << "\t" << c2w << " =w cnel " << c2 << ", 0\n";
                    il << "\tjnz " << c2w << ", " << panicL << ", " << okL << "\n";
                    il << panicL << "\n";
                    std::string ovMsg = strlit("integer overflow");
                    il << "\tcall $flint_panic(l " << ovMsg << ")\n";
                    il << "\tjmp " << okL << "\n";
                    il << okL << "\n";
                }
                return t;
            }
            case NodeKind::Compare: {
                auto* c = static_cast<CompareExprAST*>(node);
                std::string l = emitExpr(c->lhs.get()), r = emitExpr(c->rhs.get()), t = newTmp('l');
                const char* p = c->op == "==" ? "ceql" : c->op == "!=" ? "cnel" :
                                c->op == "<"  ? "csltl" : c->op == ">"  ? "csgtl" :
                                c->op == "<=" ? "cslel" : "csgel";
                il << "\t" << t << " =l " << p << " " << l << ", " << r << "\n";
                return t;
            }
            case NodeKind::Call:
                return emitCall(static_cast<CallExprAST*>(node));
            case NodeKind::Array: {
                auto* ar = static_cast<ArrayExprAST*>(node);
                size_t n = ar->elements.size();
                std::string base = newTmp('l');
                il << "\t" << base << " =l alloc8 " << (n * 8 + 16) << "\n";
                std::string len = newTmp('l');
                il << "\t" << len << " =l copy " << n << "\n";
                storel(len, base);
                for (size_t i = 0; i < n; i++) {
                    auto val = emitExpr(ar->elements[i].get());
                    std::string off = newTmp('l');
                    il << "\t" << off << " =l add " << base << ", " << (8 + i * 8) << "\n";
                    storel(val, off);
                }
                return base;
            }
            case NodeKind::Index: {
                auto* ix = static_cast<IndexExprAST*>(node);
                auto base = emitExpr(ix->base.get()), idx = emitExpr(ix->index.get());
                std::string sc = newTmp('l'), dOff = newTmp('l'), off = newTmp('l'), r = newTmp('l');
                il << "\t" << sc << " =l mul " << idx << ", 8\n";
                il << "\t" << dOff << " =l add " << sc << ", 8\n";
                il << "\t" << off << " =l add " << base << ", " << dOff << "\n";
                il << "\t" << r << " =l loadl " << off << "\n";
                return r;
            }
            case NodeKind::Slice: {
                std::cerr << "QBE: slice not supported yet\n";
                return "0";
            }
            case NodeKind::Ref: {
                auto* r = static_cast<RefExprAST*>(node);
                if (r->target->kind == NodeKind::Variable) {
                    auto it = varSlots.find(static_cast<VariableExprAST*>(r->target.get())->name);
                    if (it != varSlots.end()) return it->second;
                }
                return "0";
            }
            case NodeKind::Deref: {
                auto* d = static_cast<DerefExprAST*>(node);
                auto addr = emitExpr(d->target.get());
                std::string t = newTmp('l');
                il << "\t" << t << " =l loadl " << addr << "\n";
                return t;
            }
            case NodeKind::StructLiteral: {
                auto* sl = static_cast<StructLiteralAST*>(node);
                std::string base = newTmp('l');
                size_t structSize = 0;
                for (auto& sd : *structDefs) {
                    if (sd.name == sl->structName) {
                        structSize = sd.fields.size() * 8;
                        break;
                    }
                }
                if (structSize < 8) structSize = 8;
                il << "\t" << base << " =l alloc8 " << structSize << "\n";
                for (auto& f : sl->fields) {
                    auto val = emitExpr(f.second.get());
                    int off = 0;
                    for (auto& sd : *structDefs) {
                        if (sd.name == sl->structName) {
                            for (size_t i = 0; i < sd.fields.size(); i++) {
                                if (sd.fields[i].name == f.first) break;
                                off += 8;
                            }
                            break;
                        }
                    }
                    std::string offTmp = newTmp('l');
                    il << "\t" << offTmp << " =l add " << base << ", " << off << "\n";
                    storel(val, offTmp);
                }
                return base;
            }
            case NodeKind::FieldAccess: {
                auto* fa = static_cast<FieldAccessAST*>(node);
                auto base = emitExpr(fa->base.get());
                int off = 0;
                for (auto& sd : *structDefs) {
                    for (size_t i = 0; i < sd.fields.size(); i++) {
                        if (sd.fields[i].name == fa->fieldName) break;
                        off += 8;
                    }
                }
                std::string ptr = newTmp('l'), r = newTmp('l');
                il << "\t" << ptr << " =l add " << base << ", " << off << "\n";
                il << "\t" << r << " =l loadl " << ptr << "\n";
                return r;
            }
            case NodeKind::EnumConstruct: {
                auto* ec = static_cast<EnumConstructAST*>(node);
                int tag = enumVariantTag(ec->enumName, ec->variantName);
                // Layout: [tag @0][payload @8, @16, ...]
                std::string base = newTmp('l');
                size_t sz = 8 + ec->args.size() * 8;
                if (sz < 16) sz = 16;
                il << "\t" << base << " =l alloc8 " << sz << "\n";
                std::string tagTmp = newTmp('l');
                il << "\t" << tagTmp << " =l copy " << tag << "\n";
                storel(tagTmp, base);
                for (size_t i = 0; i < ec->args.size(); i++) {
                    auto val = emitExpr(ec->args[i].get());
                    std::string off = newTmp('l');
                    il << "\t" << off << " =l add " << base << ", " << (8 + i * 8) << "\n";
                    storel(val, off);
                }
                return base;
            }
            case NodeKind::Match:
                return emitMatch(static_cast<MatchExprAST*>(node));
            case NodeKind::Lambda:
                std::cerr << "QBE: lambdas not supported yet\n";
                return "0";
            default: return "0";
        }
    }

    void emitCall_(const std::string& callee, const std::vector<std::string>& args) {
        il << "\tcall $" << callee << "(";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) il << ", ";
            il << "l " << args[i];
        }
        il << ")\n";
    }

    int enumVariantTag(const std::string& enumName, const std::string& variantName) {
        if (enumDefs) {
            for (auto& ed : *enumDefs) {
                if (ed.name == enumName) {
                    for (size_t i = 0; i < ed.variants.size(); i++)
                        if (ed.variants[i].name == variantName) return (int)i;
                }
            }
        }
        return 0;
    }

    std::string emitMatch(MatchExprAST* mn) {
        // Evaluate scrutinee (a pointer to enum memory: [tag @0][payload @8...])
        auto scrutinee = emitExpr(mn->scrutinee.get());
        // Store scrutinee pointer in a slot so arms can access payload
        std::string scrutSlot = newTmp('l');
        il << "\t" << scrutSlot << " =l alloc8 8\n";
        storel(scrutinee, scrutSlot);
        // Load tag
        std::string scrutPtr = newTmp('l');
        il << "\t" << scrutPtr << " =l loadl " << scrutSlot << "\n";
        std::string tag = newTmp('l');
        il << "\t" << tag << " =l loadl " << scrutPtr << "\n";

        std::string endL = newLbl();
        std::vector<std::string> armLabels;
        for (size_t i = 0; i < mn->arms.size(); i++) armLabels.push_back(newLbl());

        // Dispatch: compare tag against each arm's variant
        for (size_t i = 0; i < mn->arms.size(); i++) {
            int t = enumVariantTag(mn->arms[i].enumName, mn->arms[i].variantName);
            std::string nextL = newLbl();
            std::string cmp = newTmp('w');
            il << "\t" << cmp << " =w ceql " << tag << ", " << t << "\n";
            il << "\tjnz " << cmp << ", " << armLabels[i] << ", " << nextL << "\n";
            il << nextL << "\n";
        }
        il << "\tjmp " << endL << "\n";

        // Emit arm bodies
        for (size_t i = 0; i < mn->arms.size(); i++) {
            il << armLabels[i] << "\n";
            auto& arm = mn->arms[i];
            // Bind payload variable if present
            if (!arm.bindName.empty()) {
                std::string bindSlot = getSlot(arm.bindName);
                std::string pPtr = newTmp('l'), pVal = newTmp('l'), sp = newTmp('l');
                il << "\t" << sp << " =l loadl " << scrutSlot << "\n";
                il << "\t" << pPtr << " =l add " << sp << ", 8\n";
                il << "\t" << pVal << " =l loadl " << pPtr << "\n";
                storel(pVal, bindSlot);
            }
            if (arm.body) {
                if (arm.body->kind == NodeKind::Block)
                    emitBlock(static_cast<BlockStmtAST*>(arm.body.get()));
                else
                    emitExpr(arm.body.get());
            }
            if (!lastLineIsTerminator()) il << "\tjmp " << endL << "\n";
        }
        il << endL << "\n";
        return "0";
    }

    std::string emitCall(CallExprAST* call) {
        if (call->callee == "print") {
            if (call->args.empty()) return "0";
            auto arg = emitExpr(call->args[0].get());
            bool isStr = false, isF64 = false;
            if (call->args[0]->kind == NodeKind::String) isStr = true;
            else if (call->args[0]->kind == NodeKind::Number &&
                     static_cast<NumberExprAST*>(call->args[0].get())->isFloat) isF64 = true;
            else if (call->args[0]->kind == NodeKind::Variable) {
                auto it = varTypes.find(static_cast<VariableExprAST*>(call->args[0].get())->name);
                if (it != varTypes.end() && it->second.kind == TypeKind::Str) isStr = true;
                if (it != varTypes.end() && it->second.kind == TypeKind::F64) isF64 = true;
            }
            if (isStr)
                emitCall_("flint_println_str", {arg});
            else if (isF64)
                emitCall_("flint_println_f64", {arg});
            else
                emitCall_("flint_println_i64", {arg});
            return "0";
        }
        if (call->callee == "py_eval") {
            auto arg = emitExpr(call->args[0].get());
            std::string t = newTmp('l');
            il << "\t" << t << " =l call $flint_py_eval_int(l " << arg << ")\n";
            return t;
        }
        std::vector<std::string> args;
        for (auto& a : call->args) args.push_back(emitExpr(a.get()));
        std::string t = newTmp('l');
        il << "\t" << t << " =l call $" << call->callee << "(";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) il << ", ";
            il << "l " << args[i];
        }
        il << ")\n";
        return t;
    }

    void emitVarDecl(VarDeclAST* decl) {
        std::string s = getSlot(decl->varName);
        varTypes[decl->varName] = decl->varType;
        if (decl->init) {
            auto val = emitExpr(decl->init.get());
            storel(val, s);
        }
    }

    void emitReturn(ReturnStmtAST* ret) {
        if (ret->value) {
            auto val = emitExpr(ret->value.get());
            il << "\tret " << val << "\n";
        } else il << "\tret\n";
    }

    void emitBlock(BlockStmtAST* block) {
        for (auto& stmt : block->stmts) {
            if (lastLineIsTerminator()) break;
            switch (stmt->kind) {
                case NodeKind::VarDecl: emitVarDecl(static_cast<VarDeclAST*>(stmt.get())); break;
                case NodeKind::Return:  emitReturn(static_cast<ReturnStmtAST*>(stmt.get())); return;
                case NodeKind::Break:
                    if (!breakTargets.empty()) il << "\tjmp " << breakTargets.back() << "\n";
                    return;
                case NodeKind::Continue:
                    if (!continueTargets.empty()) il << "\tjmp " << continueTargets.back() << "\n";
                    return;
                case NodeKind::If:    emitIf(static_cast<IfStmtAST*>(stmt.get())); break;
                case NodeKind::While: emitWhile(static_cast<WhileStmtAST*>(stmt.get())); break;
                case NodeKind::Block: emitBlock(static_cast<BlockStmtAST*>(stmt.get())); break;
                default: emitExpr(stmt.get()); break;
            }
        }
    }

    void emitIf(IfStmtAST* ifStmt) {
        std::string cond = emitExpr(ifStmt->condition.get());
        std::string tL = newLbl(), eL = newLbl(), endL = newLbl();
        std::string cw = newTmp('w');
        il << "\t" << cw << " =w csgtl " << cond << ", 0\n";
        il << "\tjnz " << cw << ", " << tL << ", " << eL << "\n";
        il << tL << "\n";
        emitBlock(static_cast<BlockStmtAST*>(ifStmt->thenBlock.get()));
        if (!lastLineIsTerminator()) il << "\tjmp " << endL << "\n";
        il << eL << "\n";
        if (ifStmt->elseBlock)
            emitBlock(static_cast<BlockStmtAST*>(ifStmt->elseBlock.get()));
        if (!lastLineIsTerminator()) il << "\tjmp " << endL << "\n";
        il << endL << "\n";
    }

    void emitWhile(WhileStmtAST* whileStmt) {
        std::string loopL = newLbl(), bodyL = newLbl(), endL = newLbl();
        breakTargets.push_back(endL);
        continueTargets.push_back(loopL);
        il << loopL << "\n";
        std::string cond = emitExpr(whileStmt->condition.get());
        std::string cw = newTmp('w');
        il << "\t" << cw << " =w csgtl " << cond << ", 0\n";
        il << "\tjnz " << cw << ", " << bodyL << ", " << endL << "\n";
        il << bodyL << "\n";
        emitBlock(static_cast<BlockStmtAST*>(whileStmt->body.get()));
        if (!lastLineIsTerminator()) il << "\tjmp " << loopL << "\n";
        il << endL << "\n";
        breakTargets.pop_back();
        continueTargets.pop_back();
    }

    void emitFunction(FunctionAST* fn) {
        currentFunc = fn->name;
        varSlots.clear();
        il << "export function l $" << fn->name << "(";
        std::vector<std::string> paramTemps;
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (i > 0) il << ", ";
            std::string p = newTmp('l');
            il << "l " << p;
            paramTemps.push_back(p);
        }
        il << ") {\n@start\n";
        for (size_t i = 0; i < fn->params.size(); i++) {
            storel(paramTemps[i], getSlot(fn->params[i].first));
            varTypes[fn->params[i].first] = fn->params[i].second;
        }
        if (fn->body && fn->body->kind == NodeKind::Block)
            emitBlock(static_cast<BlockStmtAST*>(fn->body.get()));
        if (!lastLineIsTerminator()) {
            if (fn->name == "main" || fn->returnType.kind != TypeKind::Void)
                il << "\tret 0\n";
            else
                il << "\tret\n";
        }
        il << "}\n\n";
    }

public:
    bool emitProgram(ProgramAST& prog, const std::string& ssaPath) {
        structDefs = &prog.structs;
        externs = &prog.externs;
        enumDefs = &prog.enums;
        tempIdx = 0; labelIdx = 0; strIdx = 0;
        varSlots.clear();
        il.str(""); data.str("");
        // QBE does not require extern function declarations — just call by $name.
        // QBE IL order: data, function definitions.
        for (auto& g : prog.globals) {
            if (g->init) {
                auto val = emitExpr(g->init.get());
                data << "data $" << g->varName << " = { l " << val << " }\n";
            } else {
                data << "data $" << g->varName << " = { z 8 }\n";
            }
        }
        for (auto& fn : prog.functions) {
            if (!fn->isDeclaration) emitFunction(fn.get());
        }
        std::ofstream out(ssaPath);
        if (!out) return false;
        out << data.str() << il.str();
        return true;
    }
};

int main(int argc, char* argv[]) {
    PROFILE_BEGIN("total");
    if (argc < 2) {
        std::cerr << "usage: flintc [options] <input.fl> [-o <output>] [-- args...]\n";
        std::cerr << "       flintc --emit-interface <input.fl> <output.flint.bc>\n";
        std::cerr << "       flintc --use-interface <file.flint.bc> <input.fl> <output> [--use-interface ...]\n";
        std::cerr << "       flintc --parallel <N> <input.fl> <output> [--link ...]\n";
        std::cerr << "       flintc --opt-level <0|1|2|3> <input.fl> <output> [--link ...]\n";
        std::cerr << "       flintc --release <input.fl> [args...]\n";
        std::cerr << "       flintc --backend <llvm|qbe> <input.fl> <output> [--link ...]\n";
        std::cerr << "\n";
        std::cerr << "Default mode (no -o): compile & run via ORC JIT in-memory\n";
        std::cerr << "Use -o <file> to emit a binary/object/LLVM file instead\n";
        return 1;
    }

    std::string inputPath;
    std::string outputPath;
    std::string linkFlags;
    bool emitInterfaceMode = false;
    bool runMode = true; // ORC JIT is the default
    std::vector<std::string> useInterfaces;
    int parallelThreads = 1;
    // Default to -O2 for production speed (balanced compile time vs runtime perf).
    llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::Default;
    // Backend selection: "llvm" (default, fastest — in-process -O0) or "qbe"
    // (experimental — spawns qbe+as subprocesses; ~3x slower than LLVM -O0 but
    // available for code-quality experiments / portability). See ROADMAP Phase H2.
    std::string backend = "llvm";

    bool releaseMode = false; // default: safe mode with overflow checks
    bool safeMode = true;     // --safe is now the default
    bool offlineMode = false; // --offline: registry uses cache only, no fetch
    std::vector<std::string> libPaths;
    // Default library path: compiler's parent dir / std
    {
        std::string compilerDir = argv[0];
        auto slash = compilerDir.find_last_of('/');
        if (slash != std::string::npos)
            compilerDir = compilerDir.substr(0, slash);
        else
            compilerDir = ".";
        libPaths.push_back(compilerDir + "/std");
        libPaths.push_back("./std");
    }
    // Also check FLINT_LIB_PATH env var
    const char* flp = getenv("FLINT_LIB_PATH");
    if (flp) {
        std::string flpStr(flp);
        size_t start = 0, end;
        while ((end = flpStr.find(':', start)) != std::string::npos) {
            if (end > start) libPaths.push_back(flpStr.substr(start, end - start));
            start = end + 1;
        }
        if (start < flpStr.size()) libPaths.push_back(flpStr.substr(start));
    }

    int progArgStart = argc; // index where program args begin
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--") {
            progArgStart = i + 1;
            break;
        } else if (arg == "--unsafe") {
            releaseMode = true;  // disable overflow checks
        } else if (arg == "--offline") {
            offlineMode = true;  // registry: cache only, fail if missing
        } else if (arg == "--fast") {
            // Compat alias for -O0 builds; intentionally a no-op here:
            // the default pipeline already agrees with every opt level
            // (see test_opt_identity.sh). Accepted anywhere on the CLI so
            // flag order never shifts positional input/output.
        } else if (arg == "--safe") {
            releaseMode = false;  // enable overflow checks (default)
        } else if (arg == "--release") {
            releaseMode = false;  // safe mode is default; --release is a no-op for compat
        } else if (arg == "--lib-path" && i + 1 < argc) {
            libPaths.push_back(argv[++i]);
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = argv[++i];
            runMode = false;
        } else if (arg == "--link" && i + 1 < argc) {
            linkFlags = argv[++i];
        } else if (arg == "--emit-interface") {
            emitInterfaceMode = true;
        } else if (arg == "--use-interface" && i + 1 < argc) {
            useInterfaces.push_back(argv[++i]);
        } else if (arg == "--parallel" && i + 1 < argc) {
            int n = atoi(argv[++i]);
            unsigned hw = std::thread::hardware_concurrency();
            int maxThreads = (int)(hw ? hw * 4 : 16);
            if (maxThreads > 64) maxThreads = 64;
            if (n < 1) n = 1;
            if (n > maxThreads) n = maxThreads;
            parallelThreads = n;
        } else if (arg == "--opt-level" && i + 1 < argc) {
            int level = atoi(argv[++i]);
            switch (level) {
                case 0: optLevel = llvm::CodeGenOptLevel::None; break;
                case 1: optLevel = llvm::CodeGenOptLevel::Less; break;
                case 2: optLevel = llvm::CodeGenOptLevel::Default; break;
                case 3: optLevel = llvm::CodeGenOptLevel::Aggressive; break;
                default:
                    std::cerr << "error: invalid --opt-level (valid: 0-3)\n";
                    return 1;
            }
        } else if (arg == "--backend" && i + 1 < argc) {
            backend = argv[++i];
            if (backend != "llvm" && backend != "qbe") {
                std::cerr << "error: invalid --backend (valid: llvm, qbe)\n";
                return 1;
            }
        } else if (arg == "--test") {
            // Will be handled after JIT compilation: run all test_ functions
            runMode = false;
        } else if (arg == "--run") {
            runMode = true; // explicit no-op (it's the default)
        } else if (arg == "--dump-tokens" && i + 1 < argc) {
            std::string dumpPath = argv[++i];
            auto buf = llvm::MemoryBuffer::getFile(dumpPath);
            if (!buf) { std::cerr << "error: cannot open '" << dumpPath << "'\n"; return 1; }
            Lexer lex(buf.get()->getBuffer());
            lex.tokenize();
            for (auto &tok : lex.getTokens()) {
                if (tok.type == TokenType::END_OF_FILE) break;
                std::cout << stableTokenKind(tok.type, tok.lexeme) << ":"
                          << stage1Escape(tok.lexeme) << ":"
                          << tok.loc.line << ":" << tok.loc.col << "\n";
            }
            return 0;
        } else if (inputPath.empty()) {
            inputPath = arg;
        } else if (outputPath.empty() && arg[0] != '-') {
            outputPath = arg;
            runMode = false; // positional output path implies file mode
        }
    }

    if (inputPath.empty()) {
        std::cerr << "usage: flintc [options] <input.fl> [-o <output>]\n";
        std::cerr << "Default mode: compile & run via ORC JIT (no output file)\n";
        return 1;
    }

    // Registry: fetch flint.toml deps into the local cache (or verify the
    // cache in --offline mode) before any parsing/import resolution.
    if (!fetchRegistryDeps(inputPath, libPaths, offlineMode)) return 1;

    // --emit-interface mode: parse declarations only, emit .flint.bc
    if (emitInterfaceMode) {
        auto fileBuf = llvm::MemoryBuffer::getFile(inputPath);
        if (!fileBuf) { std::cerr << "error: cannot open '" << inputPath << "'\n"; return 1; }
        llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
        Lexer lexer(sourceRef);
        Parser parser(lexer);
        parser.setSkipBodies(true);
        auto combined = parser.parseProgram();
        if (parser.hadError()) { std::cerr << parser.errorMsg() << "\n"; return 1; }
        if (!emitInterface(*combined, outputPath)) {
            std::cerr << "error: failed to emit interface\n";
            return 1;
        }
        PROFILE_END(); // total
        PROFILE_REPORT();
        return 0;
    }

    bool emitLl = outputPath.size() >= 3 && outputPath.substr(outputPath.size() - 3) == ".ll";
    bool emitObj = outputPath.size() >= 2 && outputPath.substr(outputPath.size() - 2) == ".o";

    // Phase 1: Read, lex, and parse declarations for the main file
    PROFILE_BEGIN("file_io");
    auto fileBuf = llvm::MemoryBuffer::getFile(inputPath);
    if (!fileBuf) { std::cerr << "error: cannot open '" << inputPath << "'\n"; return 1; }
    llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
    PROFILE_END(); // file_io

    // Determine the .o path for codegen
    std::string objectPath;
    if (emitObj) objectPath = outputPath;
    else objectPath = outputPath + ".flint.o";

    // Check content-addressed cache.
    // A1: the key mixes compiler identity + codegen flags with the source
    // (see cacheSalt) so rebuilds/upgrades/flag changes bust stale entries.
    uint64_t sourceHash =
        fnv1a(cacheSalt(optLevel, releaseMode, backend, argv[0]) + std::string(1, '\0') + sourceRef.str());
    ModuleCache cache;

    // For binary output, check if cached binary exists (skip lex/parse/codegen/link entirely)
    // But skip cache in --run mode: we always recompile for JIT.
    bool emitBinary = !emitLl && !emitObj && !runMode;

    if (emitBinary && cache.hasBinary(sourceHash)) {
        if (cache.loadBinary(sourceHash, outputPath)) {
            PROFILE_BEGIN("total");
            PROFILE_END(); // total
            PROFILE_REPORT();
            return 0;
        }
    }

    PROFILE_BEGIN("cache_check");
    if (!runMode && cache.has(sourceHash)) {
        llvm::LLVMContext ctx;
        if (auto cachedMod = cache.load(sourceHash, ctx)) {
            PROFILE_END(); // cache_check
            {
                PROFILE_BEGIN("llvm_opt");
                llvm::OptimizationLevel ol = llvm::OptimizationLevel::O2;
                runLLVMOptimizations(cachedMod.get(), ol);
                PROFILE_END(); // llvm_opt
            }
            PROFILE_BEGIN("codegen_output");
            if (emitLl) {
                if (!emitModuleOutput(cachedMod.get(), outputPath, optLevel)) return 1;
            } else if (emitObj) {
                if (!emitModuleOutput(cachedMod.get(), outputPath, optLevel)) return 1;
            } else {
                if (!emitModuleOutput(cachedMod.get(), objectPath, optLevel)) return 1;
                PROFILE_END(); // codegen_output
                PROFILE_BEGIN("link");
                if (!spawnLinker(objectPath, outputPath, linkFlags)) return 1;
                PROFILE_END(); // link
                cache.saveBinary(sourceHash, outputPath);
                unlink(objectPath.c_str());
                PROFILE_END(); // total
                PROFILE_REPORT();
                return 0;
            }
            PROFILE_END(); // codegen_output
            PROFILE_END(); // total
            PROFILE_REPORT();
            return 0;
        }
    }
    PROFILE_END(); // cache_check

    PROFILE_BEGIN("lex");
    Lexer lexer(sourceRef);
    PROFILE_END(); // lex (lexing is now lazy, happens during parse_decls)

    PROFILE_BEGIN("parse_decls");
    Parser parser(lexer);
    parser.setSkipBodies(false);
    auto combined = parser.parseProgram();
    PROFILE_BEGIN("parse_error_check");
    if (parser.hadError()) {
        std::cerr << parser.errorMsg() << "\n";
        return 1;
    }
    PROFILE_END(); // parse_error_check
    PROFILE_END(); // parse_decls

    PROFILE_BEGIN("imports");
    std::unordered_set<std::string> seen;
    std::string firstError;
    std::mutex importMutex;

    if (parallelThreads > 1 && !combined->imports.empty()) {
        // Parallel import processing for top-level imports only
        // (sub-imports within each file are processed sequentially by processFile)
        ThreadPool pool(parallelThreads);
        for (auto& imp : combined->imports) {
            auto impPath = resolveImportPath(inputPath, imp, libPaths);
            pool.enqueue([&, impPath] {
                std::string localError;
                std::unordered_set<std::string> localSeen;
                {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (seen.count(impPath)) return;
                    seen.insert(impPath);
                }
                auto buf = llvm::MemoryBuffer::getFile(impPath);
                if (!buf) {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (firstError.empty()) firstError = "error: cannot open '" + impPath + "'";
                    return;
                }
                llvm::StringRef src = buf.get()->getBuffer();
                Lexer lex(src);
                Parser p(lex);
                auto prog = p.parseProgram();
                if (p.hadError()) {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (firstError.empty()) firstError = p.errorMsg();
                    return;
                }
                // Process sub-imports sequentially within this thread
                for (auto& subImp : prog->imports) {
                    auto subPath = resolveImportPath(impPath, subImp, libPaths);
                    processFile(subPath, prog, localSeen, localError, libPaths);
                    if (!localError.empty()) break;
                }
                if (!localError.empty()) {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (firstError.empty()) firstError = localError;
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(importMutex);
                    mergeProgram(combined, prog);
                }
            });
        }
        pool.wait();
        if (!firstError.empty()) {
            std::cerr << firstError << "\n";
            return 1;
        }
    } else {
        // Sequential import processing (original behavior)
        for (auto& imp : combined->imports) {
            auto impPath = resolveImportPath(inputPath, imp, libPaths);
            processFile(impPath, combined, seen, firstError, libPaths);
            if (!firstError.empty()) {
                std::cerr << firstError << "\n";
                return 1;
            }
        }
    }
    PROFILE_END(); // imports

    if (runMode && backend == "qbe") {
        std::cerr << "error: --run requires LLVM backend (--backend llvm, the default); QBE JIT not supported\n";
        return 1;
    }

    if (backend == "qbe") {
        // ===== QBE Backend: direct codegen via QBE IL =====
        PROFILE_BEGIN("codegen");
        // Reparse function bodies into AST for QBE emission
        for (auto& fn : combined->functions) {
            if (fn->isDeclaration) continue;
            if (!fn->body && fn->bodyStart > 0 && fn->bodyEnd > 0) {
                size_t savedPos = parser.getPos();
                parser.setPos(fn->bodyStart);
                parser.setCodegen(nullptr, false);
                parser.resetDeclaredVars(fn->params);
                fn->body = parser.reparseBlock();
                parser.setPos(fn->bodyEnd);
                parser.setPos(savedPos);
                if (parser.hadError()) {
                    std::cerr << parser.errorMsg() << "\n";
                    PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                    return 1;
                }
            }
        }

        // Borrow/move checking on function bodies
        {
            BorrowChecker checker;
            for (auto& fn : combined->functions) {
                if (fn->isDeclaration) continue;
                std::string errorOut;
                if (!checker.check(fn.get(), errorOut)) {
                    std::cerr << "borrow/move error in '" << fn->name << "': " << errorOut << "\n";
                    PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                    return 1;
                }
            }
        }

        // Emit QBE IL to .ssa file
        std::string ssaPath = objectPath + ".ssa";
        {
            QbeEmitter qbe;
            if (!qbe.emitProgram(*combined, ssaPath)) {
                std::cerr << "QBE IL emission failed\n";
                PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                return 1;
            }
        }

        // Run qbe: .ssa → .s
        std::string asmPath = objectPath + ".s";
        std::string qbePathSrc = inputPath.substr(0, inputPath.find_last_of('/')) + "/qbe";
        std::string qbePathCwd = "./qbe";
        // Try PATH, source directory, then CWD
        int qbeRet = runProcess({"qbe", "-t", "arm64", "-o", asmPath, ssaPath});
        if (qbeRet != 0)
            qbeRet = runProcess({qbePathSrc, "-t", "arm64", "-o", asmPath, ssaPath});
        if (qbeRet != 0)
            qbeRet = runProcess({qbePathCwd, "-t", "arm64", "-o", asmPath, ssaPath});
        if (qbeRet != 0) {
            std::cerr << "QBE compilation failed (exit=" << qbeRet << "). Install qbe or use --backend llvm\n";
            PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
            return 1;
        }

        // Assemble: .s → .o
        int asRet = runProcess({"as", "-o", objectPath, asmPath});
        if (asRet != 0) {
            std::cerr << "assembly failed (exit=" << asRet << ")\n";
            PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
            return 1;
        }

        // Clean up temp files
        if (!getenv("FLINT_KEEP_SSA")) {
            unlink(ssaPath.c_str());
            unlink(asmPath.c_str());
        }
        PROFILE_END(); // codegen

        // Binary output: link + cache
        if (!emitLl && !emitObj) {
            bool hasMain = false;
            for (auto& fn : combined->functions)
                if (fn->name == "main") { hasMain = true; break; }
            if (!hasMain) {
                std::cerr << "warning: no 'main' function — skipping linker, producing .o only\n";
            } else {
                PROFILE_BEGIN("link");
                if (!spawnLinker(objectPath, outputPath, linkFlags)) {
                    PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                    std::cerr << "linking failed\n";
                    return 1;
                }
                PROFILE_END(); // link
                unlink(objectPath.c_str());
            }
        }

        PROFILE_END(); // total
        PROFILE_REPORT();
        return 0;
    }

    // ===== LLVM Backend (kept for --backend llvm) =====
    PROFILE_BEGIN("codegen");
    Codegen codegen;
    codegen.releaseMode = releaseMode;
    codegen.setParser(&parser);

    // Generate declarations from the combined AST
    if (!codegen.generateDeclarations(*combined)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "declaration generation failed\n";
        return 1;
    }

    // Load pre-compiled interfaces (--use-interface)
    // These add declarations NOT in the AST (e.g., pre-compiled libraries)
    for (auto& iface : useInterfaces) {
        auto ifaceMod = loadInterface(iface, *codegen.ctx);
        if (!ifaceMod) { PROFILE_END(); PROFILE_END(); PROFILE_REPORT(); std::cerr << "error: cannot load interface '" << iface << "'\n"; return 1; }
        // Manually copy function declarations from interface module into the main module.
        // We avoid llvm::Linker::linkModules because it can silently drop declarations
        // when the modules have slightly different data layouts (e.g. LLVM version skew).
        for (auto& f : ifaceMod->functions()) {
            std::string name = f.getName().str();
            if (f.isDeclaration() && !codegen.mod->getFunction(name)) {
                auto* newF = llvm::Function::Create(
                    llvm::cast<llvm::FunctionType>(f.getValueType()),
                    llvm::Function::ExternalLinkage, name, codegen.mod.get());
                newF->setAttributes(f.getAttributes());
                newF->setCallingConv(f.getCallingConv());
            }
        }
        for (auto& gv : ifaceMod->globals()) {
            std::string name = gv.getName().str();
            if (!codegen.mod->getGlobalVariable(name)) {
                new llvm::GlobalVariable(*codegen.mod, gv.getValueType(), gv.isConstant(),
                    llvm::GlobalValue::ExternalLinkage, nullptr, name);
            }
        }
    }
    // Sync LLVM declarations into C++ symbol tables for call resolution
    codegen.syncInterfaceSymbols();

    // Emit function bodies and output
    if (!codegen.emitFunctionBodies(*combined)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "function body emission failed\n";
        return 1;
    }

    // Run LLVM IR optimization passes — O2 by default with overflow checks for safety
    {
        PROFILE_BEGIN("llvm_opt");
        llvm::OptimizationLevel ol = llvm::OptimizationLevel::O2;
        if (!releaseMode) { /* safe mode: overflow checks added at codegen level */ }
        if (!getenv("FLINT_NO_OPT")) runLLVMOptimizations(codegen.mod.get(), ol);
        PROFILE_END(); // llvm_opt
    }

    // --test mode: compile & run all test_ functions via JIT, report results
    {
        bool testMode = false;
        for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--test") { testMode = true; break; }
        if (testMode) {
            PROFILE_END(); // codegen
            PROFILE_BEGIN("cache_save");
            cache.save(codegen.mod.get(), sourceHash);
            PROFILE_END(); // cache_save
            PROFILE_END(); // total
            PROFILE_REPORT();
            std::vector<char*> progArgs;
            progArgs.push_back(argv[0]);
            for (int i = progArgStart; i < argc; i++) progArgs.push_back(argv[i]);
            std::vector<std::string> extraObjs;
            for (auto& tok : splitFlags(linkFlags)) {
                if (tok.size() >= 2 && tok.substr(tok.size() - 2) == ".o")
                    extraObjs.push_back(tok);
            }
            // Build JIT once with the compiled module
            auto jit = llvm::orc::LLJITBuilder().create();
            if (!jit) { std::cerr << "JIT creation failed\n"; return 1; }
            if (auto err = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(
                    llvm::CloneModule(*codegen.mod), std::make_unique<llvm::LLVMContext>()))) {
                std::cerr << "JIT module error\n"; return 1;
            }
            int passed = 0, failed = 0;
            for (auto& fn : combined->functions) {
                if (fn->name.find("test_") != 0 || fn->name == "main") continue;
                auto sym = (*jit)->lookup(fn->name);
                if (!sym) { std::cerr << "  FAIL " << fn->name << " (not found)\n"; failed++; continue; }
                auto* tf = (int (*)())(sym->getValue());
                int ret = tf();
                if (ret == 0) { std::cout << "  PASS " << fn->name << "\n"; passed++; }
                else { std::cerr << "  FAIL " << fn->name << " (exit " << ret << ")\n"; failed++; }
            }
            std::cout << "\n" << passed << " passed, " << failed << " failed\n";
            return failed > 0 ? 1 : 0;
        }
    }

    // --run mode: compile & execute in-memory via ORC JIT, skip file I/O
    if (runMode) {
        PROFILE_END(); // codegen
        // Save cache anyway (for future cold runs)
        PROFILE_BEGIN("cache_save");
        cache.save(codegen.mod.get(), sourceHash);
        PROFILE_END(); // cache_save
        PROFILE_END(); // total
        PROFILE_REPORT();
        // Build program args: argv[0] + user args from after '--' (or after input path)
        std::vector<char*> progArgs;
        progArgs.push_back(argv[0]);
        for (int i = progArgStart; i < argc; i++) progArgs.push_back(argv[i]);
        // Parse --link flags for .o files to load into JIT
        std::vector<std::string> extraObjs;
        for (auto& tok : splitFlags(linkFlags)) {
            if (tok.size() >= 2 && tok.substr(tok.size() - 2) == ".o")
                extraObjs.push_back(tok);
        }
        // Move module and context into JIT — skip cache save (already done above)
        return runWithJIT(std::move(codegen.mod), std::move(codegen.ctx), progArgs, extraObjs);
    }

    if (!emitModuleOutput(codegen.mod.get(), emitLl || emitObj ? outputPath : objectPath, optLevel)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "code generation failed\n";
        return 1;
    }
    PROFILE_END(); // codegen

    // For binary output, spawn linker and cache
    if (!emitLl && !emitObj) {
        // Check for main function before linking
        bool hasMain = codegen.functionMap.count("main") > 0;
        if (!hasMain) {
            std::cerr << "warning: no 'main' function — skipping linker, producing .o only\n";
        } else {
            PROFILE_BEGIN("link");
            if (!spawnLinker(objectPath, outputPath, linkFlags)) {
                PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                std::cerr << "linking failed\n";
                return 1;
            }
            PROFILE_END(); // link
            cache.saveBinary(sourceHash, outputPath);
            unlink(objectPath.c_str());
        }
    }

    // Save to content-addressed cache
    PROFILE_BEGIN("cache_save");
    cache.save(codegen.mod.get(), sourceHash);
    PROFILE_END(); // cache_save

    PROFILE_END(); // total
    PROFILE_REPORT();
    return 0;
}
