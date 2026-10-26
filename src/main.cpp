#include "llvm/Object/ObjectFile.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/MDBuilder.h"
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
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
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
#include <cmath>
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
    struct Record { const char* phase; Clock::time_point start; Clock::duration wall; int parent; };
    std::vector<Record> records;
    std::vector<size_t> stack;
    Clock::time_point programStart;
public:
    Timer() { programStart = Clock::now(); }
    void begin(const char* phase) {
        // A3: record the stack parent so tools/flamegraph.py can rebuild
        // the tree (flat wall times alone cannot nest phases).
        int parent = stack.empty() ? -1 : (int)stack.back();
        records.push_back({phase, Clock::now(), {}, parent});
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
            out << "    {\"phase\":\"" << r.phase << "\",\"wall_ns\":" << r.wall.count()
                << ",\"parent\":" << r.parent << "}";
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

// A3: count explicit safety checks in a module: bounds/null runtime calls
// + integer overflow intrinsics. Counting only — never changes codegen.
// Used by --check-bce to show how many checks O2 already eliminates.
static long countChecks(llvm::Module* mod) {
    long n = 0;
    for (auto& f : *mod) {
        for (auto& bb : f) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) continue;
                llvm::Function* callee = call->getCalledFunction();
                if (!callee) continue;
                llvm::StringRef name = callee->getName();
                if (name == "flint_bounds_check" || name == "flint_null_check") { n++; continue; }
                if (name.contains(".with.overflow")) n++;
            }
        }
    }
    return n;
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

// Does this object file reference the embedded-CPython bridge?
//
// The question is asked of the artifact being linked, not only of a flag
// recorded during codegen: the module cache can serve a cached module and skip
// codegen entirely, so a side-effect flag is unset on exactly the run that
// needs it. Reading the ELF symbol table through LLVM costs no subprocess and,
// unlike popen("nm ... " + path), cannot be shell-injected by the path.
static bool objectNeedsPython(const std::string& objPath) {
    auto parsed = llvm::object::ObjectFile::createObjectFile(objPath);
    if (!parsed) return false;
    llvm::object::ObjectFile* obj = parsed->getBinary();
    for (const auto& sym : obj->symbols()) {
        // getName() returns Expected; an unreadable name cannot match.
        llvm::StringRef name = *sym.getName();
        if (name.starts_with("flint_py_")) return true;
    }
    return false;
}

// Whether the object references ANY of the given symbol prefixes.
//
// Same technique as objectNeedsPython (read the ELF symbol table through
// LLVM, no subprocess, no shell), generalized so the linker can decide
// whether a program needs a native library at all. A program that never
// touches matrices should not carry a libopenblas dependency, and one that
// never opens a database should not carry libsqlite3.
static bool objectNeedsSymbols(const std::string& objPath,
                               const std::vector<std::string>& prefixes) {
    auto parsed = llvm::object::ObjectFile::createObjectFile(objPath);
    if (!parsed) return false;
    llvm::object::ObjectFile* obj = parsed->getBinary();
    for (const auto& sym : obj->symbols()) {
        llvm::StringRef name = *sym.getName();
        for (auto& p : prefixes)
            if (name.starts_with(p)) return true;
    }
    return false;
}

// Whether the program being compiled uses the embedded-CPython bridge.
//
// Linking pyruntime.o (and, via python3-config, libpython) unconditionally
// gave every AOT binary a libpython.so NEEDED entry, so a program that never
// calls Python still failed to run on a machine without Python. Codegen sets
// this while emitting a `python { }` block or a py_eval/py_eval_float call.
//
// It is a file-scope flag rather than a Codegen member because the linker runs
// from several call sites, in two different compilation paths, and all of them
// need the same answer. Compilation here is single-threaded.
static bool gProgramUsesPython = false;

// The safety posture of THIS build: "safe" (the default) or "unsafe" (the
// explicit --unsafe escape hatch).
//
// --unsafe is a MANUAL, TERMINAL-ONLY opt-out. It is never enabled by a
// domain declaration, an optimization profile, a package, or a tool -- only
// by a person, on the command line. It is deliberately VISIBLE (a warning
// at compile time) and AUDITABLE (the artifact carries a marker, so an
// unsafe binary can be identified later, e.g. in a supply chain).
//
// What it does NOT change: the ownership rules, the borrow/lease checks,
// the escape rules, the null checks, the div-by-zero trap, the AEGIS
// backstop, or the one-error-per-program contract.
static std::string gSafetyPosture = "safe";

// Phase 2 / 2A.0 -- the type audit. Off by default; it prints the places
// where the parser's inference and the codegen's inference disagree.
static bool gTypeAudit = false;
// Phase 2 / 2A: the checking phase is ON by default. --no-type-check is the
// differential oracle (and the escape hatch for a false positive).
static bool gTypeCheck = true;

static bool gUnsafeWarned = false;

// Stamp the artifact with its safety posture. The marker is referenced from
// llvm.used so that --gc-sections cannot drop it: the whole point is that
// an unsafe binary announces itself.
static void stampSafetyPosture(llvm::Module* mod) {
    if (!mod) return;
    std::string text = "flint:safety=" + gSafetyPosture;
    auto* c = llvm::ConstantDataArray::getString(mod->getContext(), text, true);
    auto* g = new llvm::GlobalVariable(*mod, c->getType(), true,
                                       llvm::GlobalValue::PrivateLinkage,
                                       c, "__flint_safety_posture");
    g->setSection(".rodata");
    g->setAlignment(llvm::MaybeAlign(1));
    // Keep it: unreferenced private globals are dropped by --gc-sections.
    llvm::appendToUsed(*mod, {g});
}

static void markUsesPython() { gProgramUsesPython = true; }

// ---------------------------------------------------------------------------
// SIZE ATTRIBUTION (Phase 0 measurement infrastructure)
//
// "How big is this artifact, and what is in it?" answered by the compiler
// itself, so the size consequence of a change is visible where the change
// is made. Precedent: Bloaty (per compile-unit / symbol / section), cargo-
// bloat (per crate), twiggy (monomorphization).
//
// The categories come from the ARTIFACT, not from a model: ELF sections,
// symbol classes, dynamic dependencies, and the object set that was linked.
// Nothing here guesses.
//
// NOTE on the two axes: "file bytes" (what the linker wrote) and
// "attributed bytes" (what the symbols add up to) OVERLAP -- a function's
// bytes are inside a section. They are reported separately on purpose, the
// way Bloaty reports file size and VM size separately.
struct SizeReport {
    uint64_t fileBytes = 0;
    uint64_t sectionBytes = 0;
    uint64_t headerBytes = 0;   // ELF headers, symbol tables, relocations
    uint64_t bssBytes = 0;      // reserved at startup, not stored in the file
    uint64_t rustBytes = 0;     // Rust stdlib symbols (_R...)
    uint64_t cruntimeBytes = 0; // C runtime symbols (flint_...)
    uint64_t appBytes = 0;      // everything else (the program itself)
    std::vector<std::pair<std::string, uint64_t>> sections;
    std::vector<std::string> needed;   // DT_NEEDED entries
    std::vector<std::string> objects;  // object files that were linked
};

// The objects the last link pulled in, recorded by spawnLinker.
static std::vector<std::string> gLastLinkObjects;

// Reads the artifact and fills the report.
//
// Sections come from the portable ObjectFile API; the symbol table and the
// dynamic dependencies are parsed from the ELF bytes directly (this LLVM's
// generic SymbolRef has no size, and its ELF accessor needs the file's class
// resolved before it can be used). Everything is read from the artifact, so
// the numbers cannot drift from what was actually produced.
static bool readSizeReport(const std::string& path, SizeReport& out) {
    uint64_t fileSize = 0;
    if (llvm::sys::fs::file_size(path, fileSize)) return false;
    out.fileBytes = fileSize;

    auto parsed = llvm::object::ObjectFile::createObjectFile(path);
    if (!parsed) return false;
    llvm::object::ObjectFile* obj = parsed->getBinary();

    for (const auto& sec : obj->sections()) {
        uint64_t sz = sec.getSize();
        auto nm = sec.getName();
        std::string name = nm ? nm->str() : std::string();
        if (name.empty()) continue;   // the ELF null section (index 0)
        out.sections.push_back({name, sz});
        out.sectionBytes += sz;
        if (name == ".bss") out.bssBytes = sz;
    }
    if (out.fileBytes > out.sectionBytes)
        out.headerBytes = out.fileBytes - out.sectionBytes;

    // The raw bytes, for the symbol table and the dynamic section.
    auto mb = llvm::MemoryBuffer::getFile(path);
    if (!mb) return true;   // sections alone are still a useful report
    llvm::StringRef bytes = mb.get()->getBuffer();
    bool isElf = bytes.size() > 6 && bytes[0] == 0x7F && bytes[1] == 'E' &&
                 bytes[2] == 'L' && bytes[3] == 'F';
    if (!isElf) return true;
    bool is64 = bytes[4] == 2;   // EI_CLASS: 1 = 32-bit, 2 = 64-bit

    // Section contents by name, for .symtab / .strtab / .dynamic / .dynstr.
    std::unordered_map<std::string, llvm::StringRef> contents;
    for (const auto& sec : obj->sections()) {
        auto nm = sec.getName();
        if (!nm) continue;
        auto c = sec.getContents();
        if (c) contents[std::string(*nm)] = *c;
    }

    // Symbol attribution from .symtab.
    auto symtab = contents.find(".symtab");
    auto strtab = contents.find(".strtab");
    if (symtab != contents.end() && strtab != contents.end()) {
        llvm::StringRef st = symtab->second;
        llvm::StringRef strs = strtab->second;
        const size_t entSize = is64 ? 24 : 16;
        const size_t hdrSize = is64 ? 24 : 16;
        if (st.size() >= hdrSize) {
            size_t n = (st.size() - hdrSize) / entSize;
            for (size_t i = 0; i < n; i++) {
                llvm::StringRef e = st.substr(hdrSize + i * entSize, entSize);
                uint64_t nameOff = 0, size = 0;
                if (is64) {
                    uint32_t no = 0;
                    std::memcpy(&no, e.data(), 4);
                    std::memcpy(&size, e.data() + 16, 8);
                    nameOff = no;
                } else {
                    uint32_t no = 0;
                    std::memcpy(&no, e.data(), 4);
                    std::memcpy(&size, e.data() + 8, 4);
                    nameOff = no;
                }
                if (!size || nameOff >= strs.size()) continue;
                llvm::StringRef name = strs.substr(nameOff).split('\0').first;
                if (name.empty()) continue;
                if (name.starts_with("_R")) out.rustBytes += size;
                else if (name.starts_with("flint_")) out.cruntimeBytes += size;
                else out.appBytes += size;
            }
        }
    }

    // Dynamic dependencies from .dynamic + .dynstr.
    auto dyn = contents.find(".dynamic");
    auto dynstr = contents.find(".dynstr");
    if (dyn != contents.end() && dynstr != contents.end()) {
        llvm::StringRef d = dyn->second;
        llvm::StringRef ds = dynstr->second;
        const size_t entSize = is64 ? 16 : 8;
        size_t n = d.size() / entSize;
        for (size_t i = 0; i < n; i++) {
            llvm::StringRef e = d.substr(i * entSize, entSize);
            uint64_t tag = 0, val = 0;
            if (is64) {
                int64_t t = 0, v = 0;
                std::memcpy(&t, e.data(), 8);
                std::memcpy(&v, e.data() + 8, 8);
                tag = (uint64_t)t; val = (uint64_t)v;
            } else {
                uint32_t t = 0, v = 0;
                std::memcpy(&t, e.data(), 4);
                std::memcpy(&v, e.data() + 4, 4);
                tag = t; val = v;
            }
            if (tag == 0) break;            // DT_NULL
            if (tag == 1) {                 // DT_NEEDED
                if (val < ds.size())
                    out.needed.push_back(ds.substr(val).split('\0').first.str());
            }
        }
    }
    return true;
}

static void printSizeReport(const SizeReport& r, const std::string& path) {
    auto kb = [](uint64_t v) { return v / 1024.0; };
    std::cout << "size: " << path << "\n";
    std::cout << "  file bytes:      " << r.fileBytes
              << " (" << std::fixed << std::setprecision(1) << kb(r.fileBytes) << " KB)\n";
    std::cout << "  section bytes:   " << r.sectionBytes << "\n";
    std::cout << "  header/metadata: " << r.headerBytes << "\n";
    std::cout << "  .bss reserved:   " << r.bssBytes << " (not in the file)\n";
    std::cout << "  attributed (symbols, overlaps sections):\n";
    std::cout << "    Rust stdlib:    " << r.rustBytes << "\n";
    std::cout << "    C runtime:     " << r.cruntimeBytes << "\n";
    std::cout << "    application:   " << r.appBytes << "\n";
    std::cout << "  sections:";
    for (auto& sec : r.sections)
        std::cout << " " << sec.first << "=" << sec.second;
    std::cout << "\n";
    std::cout << "  dynamic deps:";
    if (r.needed.empty()) std::cout << " (none)";
    for (auto& d : r.needed) std::cout << " " << d;
    std::cout << "\n";
    std::cout << "  objects offered to the linker (unused ones are dropped by"
                 " --gc-sections):";
    for (auto& o : r.objects) std::cout << " " << o;
    std::cout << "\n";
}

static void writeSizeReportJson(const SizeReport& r, const std::string& path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec);
    if (ec) { std::cerr << "cannot write size report: " << ec.message() << "\n"; return; }
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    };
    os << "{\n";
    os << "  \"file_bytes\": " << r.fileBytes << ",\n";
    os << "  \"section_bytes\": " << r.sectionBytes << ",\n";
    os << "  \"header_bytes\": " << r.headerBytes << ",\n";
    os << "  \"bss_bytes\": " << r.bssBytes << ",\n";
    os << "  \"attributed\": {\"rust\": " << r.rustBytes
       << ", \"c_runtime\": " << r.cruntimeBytes
       << ", \"application\": " << r.appBytes << "},\n";
    os << "  \"sections\": {";
    bool first = true;
    for (auto& sec : r.sections) {
        if (!first) os << ",";
        first = false;
        os << "\"" << esc(sec.first) << "\": " << sec.second;
    }
    os << "},\n";
    os << "  \"needed\": [";
    for (size_t i = 0; i < r.needed.size(); i++) {
        if (i) os << ",";
        os << "\"" << esc(r.needed[i]) << "\"";
    }
    os << "],\n";
    os << "  \"objects_offered\": [";
    for (size_t i = 0; i < r.objects.size(); i++) {
        if (i) os << ",";
        os << "\"" << esc(r.objects[i]) << "\"";
    }
    os << "]\n}\n";
}

// --print-size / --print-size-json=<path>: report the artifact's size
// attribution after the link. Off by default; never implied by anything.
static bool gPrintSize = false;
static std::string gPrintSizeJsonPath;

static void maybeReportSize(const std::string& outputPath) {
    if (!gPrintSize && gPrintSizeJsonPath.empty()) return;
    SizeReport rep;
    if (!readSizeReport(outputPath, rep)) {
        std::cerr << "size: cannot read " << outputPath << "\n";
        return;
    }
    rep.objects = gLastLinkObjects;
    if (gPrintSize) printSizeReport(rep, outputPath);
    if (!gPrintSizeJsonPath.empty()) writeSizeReportJson(rep, gPrintSizeJsonPath);
}

static bool spawnLinker(const std::string& objPath, const std::string& outputPath,
                        const std::string& extraFlags = "", bool useBlas = true) {
    if (!isSafePath(objPath) || !isSafePath(outputPath)) {
        std::cerr << "linker: unsafe path rejected\n";
        return false;
    }

    // The standard-library objects that may be linked in. Declared first:
    // the native-library decisions below inspect their symbol tables.
    const char* stdLibs[] = {"flint_tensor.o", "flint_blas.o", "flint_ai.o",
                             "flint_ai_opt.o", "flint_math.o", "flint_fmt.o",
                             "flint_coll.o", "flint_nn.o", "flint_util.o",
                             "flint_value.o", "flint_toml.o", "flint_yaml.o", "flint_time.o",
                             "flint_serial.o", "flint_crypto.o", "flint_net.o",
                             "flint_aegis.o", "flint_chan.o", "flint_std.o",
                             "ffi_helper.o"};
    struct stat st;
    // Build the argv array directly — no shell, no string interpolation.
    std::vector<std::string> args = {"clang", objPath, "runtime.o"};
    // --as-needed makes the linker drop a DT_NEEDED entry for a library whose
    // symbols the program never uses. Without it every binary carried
    // libsqlite3, libcurl and libopenblas as hard runtime dependencies, so a
    // hello-world would not start on a machine without them.
    args.push_back("-Wl,--as-needed");
    // Dead-code elimination at link time. The runtime objects are compiled
    // with -ffunction-sections/-fdata-sections (see build.sh), so the linker
    // can drop every module the program does not use. Without this flag a
    // hello-world carried the whole standard library -- sqlite, http, crypto,
    // png, the Rust stdlib -- 743 KB instead of 8 KB.
    args.push_back("-Wl,--gc-sections");
    // libm is unconditional: the runtime's math builtins (flint_pow, and the
    // tensor/BLAS objects' pow) need it. It used to arrive by accident inside
    // `python3-config --ldflags`, so a machine without Python could not link
    // a program that only did arithmetic. Depending on the Python toolchain
    // for libm is not a dependency anyone wants.
    args.push_back("-lm");
    // C libraries the Rust stdlib modules wrap. Absolute paths beat -l flags
    // here: Termux keeps them under $PREFIX/lib, which is not on the default
    // linker path for every invocation.
    //
    // They are added only when the program can actually reach them: the
    // decision is made on the LINKED OBJECT SET, because the app object
    // references flint_* wrappers, not the C symbols underneath. With
    // --as-needed in effect, a library whose symbols are unused leaves no
    // DT_NEEDED entry either way; deciding up front keeps the link line
    // honest and keeps the dependency off a program that does not use it.
    struct stat libSt;
    auto addLib = [&](const char* rel, const char* flag) {
        char full[512];
        if (const char* pfx = getenv("PREFIX")) {
            snprintf(full, sizeof full, "%s/lib/%s", pfx, rel);
            if (stat(full, &libSt) == 0) { args.push_back(full); return; }
        }
        if (stat(rel, &libSt) == 0) { args.push_back(rel); return; }
        (void)flag;
    };
    bool needsSqlite = false, needsCurl = false;
    for (auto* lib : stdLibs) {
        if (stat(lib, &st) != 0) continue;
        if (objectNeedsSymbols(lib, {"sqlite3_"})) needsSqlite = true;
        if (objectNeedsSymbols(lib, {"curl_"})) needsCurl = true;
    }
    if (needsSqlite) addLib("libsqlite3.so", "-lsqlite3");
    if (needsCurl) addLib("libcurl.so", "-lcurl");

    // Include standard library .o files if they exist
    gLastLinkObjects.clear();
    gLastLinkObjects.push_back("runtime.o");
    for (auto* lib : stdLibs) {
        if (stat(lib, &st) == 0) { args.push_back(lib); gLastLinkObjects.push_back(lib); }
    }

    // OpenBLAS, when it is installed. flint_blas.o references cblas_dgemm
    // weakly, so this is an optimisation rather than a requirement: without the
    // library the symbol resolves to NULL at runtime and matmul falls back to
    // the portable loop. Passing -lopenblas when it does not exist would fail
    // the link, hence the probe.
    if (const char* p = getenv("PREFIX")) {
        std::string blasLib = std::string(p) + "/lib/libopenblas.so";
        // Only when the program uses the matrix/tensor path: an unrelated
        // program should not acquire a BLAS runtime dependency.
        // Only when the program reaches a matrix/tensor/AI entry point: an
        // unrelated program should not acquire a BLAS runtime dependency.
        // (--as-needed would also drop the unused entry, but deciding here
        // keeps the dependency out of the link line itself.)
        // The entry points that reach the BLAS-backed kernels: the tensor
        // API is `flt_*` (flt_create_2d, flt_matmul, ...), the AI/NN
        // wrappers call into it, and flint_blas.o is the object that
        // references cblas_dgemm. Getting these names wrong silently
        // demotes a matmul to the portable loop -- measured: n=200 took
        // 14.4 s instead of ~0.1 s.
        bool needsBlas = objectNeedsSymbols(objPath,
            {"flt_", "flint_matmul", "flint_nn", "flint_ai", "flint_blas"});
        if (useBlas && needsBlas && stat(blasLib.c_str(), &st) == 0) {
            // --no-as-needed for THIS library: flint_blas.o references
            // cblas_dgemm WEAKLY (so the runtime can probe for it and fall
            // back to the portable loop), and --as-needed ignores weak
            // undefined symbols -- it dropped libopenblas even for a
            // program that wanted the fast path. Measured: n=200 matmul
            // took 14.4 s with the library dropped and ~0.1 s with it.
            args.push_back("-Wl,--no-as-needed");
            args.push_back("-lopenblas");
            args.push_back("-Wl,--as-needed");
        }
    }

    // Include pyruntime.o only when this program actually uses Python, and
    // only when it was built (a machine without python3-dev has no .o).
    // Either signal counts: the flag covers a direct compile, the symbol scan
    // covers a cache-served module that skipped codegen.
    bool needsPython = gProgramUsesPython || objectNeedsPython(objPath);
    if (needsPython && stat("pyruntime.o", &st) == 0) {
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
    stampSafetyPosture(mod);
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
    // The middle-end pipeline needs a TargetMachine for every target-dependent
    // decision: TargetTransformInfo supplies the vector-register width and the
    // cost model to the loop vectorizer and the unroller. A PassBuilder built
    // with no TM reports "no vector registers" and silently declines every
    // loop -- the same module vectorizes under `opt -O2`, which always has a
    // TM. This was the real reason `--unsafe` loops (no checks at all) still
    // stayed scalar and 12 % behind C.
    initTargets();
    std::unique_ptr<TargetMachine> tm;
    Triple triple = mod->getTargetTriple();
    if (!triple.str().empty()) {
        std::string error;
        if (auto* target = TargetRegistry::lookupTarget(triple, error)) {
            tm.reset(target->createTargetMachine(triple.str(), "generic", "", {},
                Reloc::PIC_, std::nullopt));
        }
    }
    PassBuilder PB(tm.get());
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
// Shared JIT object loading: runtime + AI + stdlib + python + FFI helper
// + --link extras. Moved verbatim out of runWithJIT so --test mode uses
// the identical setup (its bare JIT resolved nothing). Returns false + msg.
// Publishes a shared library's exported symbols into the JIT session.
//
// Why this exists: the ORC session resolves symbols from loaded objects and from
// libc, but not from an arbitrary dlopen'd library. A `no_std` Rust module that
// wraps libsqlite3 or libcurl therefore finds `sqlite3_open_v2` undefined, and
// ORC reports the failure misleadingly by naming the ENTRY symbol ("Symbols not
// found: [ main ]"). Defining the symbols explicitly keeps resolution narrow
// and explicit: only the listed names become visible, so a typo in a Flint
// extern still fails loudly.
//
// Narrow by design. Publishing every symbol of a library would let a Flint
// program resolve names it never declared.
static bool publishJITLibrary(llvm::orc::LLJIT& JIT,
                              const std::vector<const char*>& candidates,
                              const std::vector<std::string>& symbols) {
    struct stat st;
    void* handle = nullptr;
    char resolved[512] = {0};
    for (const char* candidate : candidates) {
        if (!candidate) continue;
        const char* slash = strrchr(candidate, '/');
        if (slash && stat(candidate, &st) != 0) continue;
        // dlopen treats a name with no slash as a library to search for in the
        // system paths, so an in-tree build must keep its "./" prefix.
        if (void* h = dlopen(candidate, RTLD_NOW | RTLD_GLOBAL)) {
            handle = h;
            snprintf(resolved, sizeof resolved, "%s", candidate);
            break;
        }
    }
    if (!handle) return false;

    llvm::orc::SymbolMap syms;
    auto pool = JIT.getExecutionSession().getSymbolStringPool();
    auto& poolRef = *pool;
    for (const auto& name : symbols) {
        if (void* addr = dlsym(handle, name.c_str())) {
            llvm::orc::ExecutorSymbolDef def(
                llvm::orc::ExecutorAddr::fromPtr(addr),
                llvm::JITSymbolFlags::Callable | llvm::JITSymbolFlags::Exported);
            syms[poolRef.intern(name)] = def;
        }
    }
    if (syms.empty()) return false;
    if (auto err = JIT.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(syms)))) {
        std::cerr << "JIT error: cannot publish symbols from " << resolved << ": "
                  << llvm::toString(std::move(err)) << "\n";
        return false;
    }
    return true;
}

static bool loadJITObjects(llvm::orc::LLJIT& JIT, const std::vector<std::string>& extraObjs) {
    using namespace llvm;
    using namespace llvm::orc;
    struct stat st;
    auto loadOne = [&](const std::string& objName, bool required) -> bool {
        auto buf = MemoryBuffer::getFile(objName);
        if (!buf) {
            if (required) std::cerr << "JIT error: cannot open '" << objName << "': " << buf.getError().message() << "\n";
            return !required;
        }
        if (auto err = JIT.addObjectFile(std::move(*buf))) {
            std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
            return false;
        }
        return true;
    };
    if (stat("runtime.o", &st) == 0 && !loadOne("runtime.o", true)) return false;
    // OpenBLAS for the JIT path too. flint_blas.o reaches cblas_dgemm through a
    // WEAK symbol, so unless the library itself is loaded the probe resolves to
    // NULL and every matmul silently takes the portable triple loop -- which is
    // why JIT matmul was ~11x slower than the AOT path while looking correct.
    {
        const char* blasLib = nullptr;
        char cand[512];
        const char* pfx = getenv("PREFIX");
        if (pfx) {
            snprintf(cand, sizeof cand, "%s/lib/libopenblas.so", pfx);
            if (stat(cand, &st) == 0) blasLib = cand;
        }
        if (!blasLib) {
            const char* named[] = { "libopenblas.so", "libopenblas.so.0", nullptr };
            for (int i = 0; named[i] && !blasLib; i++)
                if (stat(named[i], &st) == 0) blasLib = named[i];
        }
        if (blasLib) {
            // Loaded with plain dlopen rather than through the JIT: LLJIT's
            // addObjectFile takes relocatable objects, and cblas_dgemm is a WEAK
            // reference that only resolves if the shared library is loaded.
            void* h = dlopen(blasLib, RTLD_NOW | RTLD_GLOBAL);
            (void)h;   // non-fatal: matmul falls back to the portable loop
        }
    }
    const char* aiObjs[] = {"flint_tensor.o", "flint_blas.o", "flint_ai.o", "flint_ai_opt.o"};
    const char* libObjs[] = {"flint_math.o", "flint_fmt.o", "flint_coll.o",
                            "flint_nn.o", "flint_util.o", "flint_time.o"};
    for (auto* objName : libObjs)
        if (stat(objName, &st) == 0 && !loadOne(objName, true)) return false;
    for (auto* objName : aiObjs)
        if (stat(objName, &st) == 0 && !loadOne(objName, true)) return false;
    // Rust stdlib modules (runtime/rust) load as a plain object, exactly like
    // the C modules, because they are #![no_std]: no allocator, no TLS, no
    // libc handles. That is what makes this work at all -- a Rust `std` object
    // aborts inside JIT memory on Android ("increment_dso_handle_reference_
    // counter: Couldn't find soinfo by dso_handle"), and two Rust staticlibs
    // cannot share one link (duplicate std symbols). Both were measured, not
    // guessed; tests/test_rust_stdlib.sh guards both properties.
    const char* stdObjs[] = {"flint_serial.o", "flint_crypto.o", "flint_net.o",
                             "flint_aegis.o", "flint_chan.o", "flint_std.o"};
    for (auto* objName : stdObjs)
        if (stat(objName, &st) == 0 && !loadOne(objName, true)) return false;
    // libsqlite3: a C engine the Rust stdlib module wraps. Optional by design --
    // a Flint program that never touches sqlite runs fine without it.
    {
        char cand[512];
        std::vector<const char*> candidates = {nullptr, nullptr, nullptr};
        if (const char* pfx = getenv("PREFIX")) {
            snprintf(cand, sizeof cand, "%s/lib/libsqlite3.so", pfx);
            candidates[0] = cand;
        }
        candidates[1] = "./libsqlite3.so";
        candidates[2] = "/usr/lib/libsqlite3.so";
        static const char* kSqliteSymbols[] = {
            "sqlite3_open_v2", "sqlite3_close_v2", "sqlite3_prepare_v2",
            "sqlite3_step", "sqlite3_finalize", "sqlite3_column_count",
            "sqlite3_column_text", "sqlite3_errmsg", "sqlite3_changes",
            "sqlite3_libversion"};
        std::vector<std::string> names(std::begin(kSqliteSymbols), std::end(kSqliteSymbols));
        publishJITLibrary(JIT, candidates, names);
    }

    // libcurl, for the Rust stdlib's HTTPS client. Same reasoning as libsqlite3:
    // a shared library's symbols are not libc symbols, so they must be
    // published into the ORC session explicitly.
    {
        char cand[512];
        std::vector<const char*> candidates = {nullptr, nullptr, nullptr};
        if (const char* pfx = getenv("PREFIX")) {
            snprintf(cand, sizeof cand, "%s/lib/libcurl.so", pfx);
            candidates[0] = cand;
        }
        candidates[1] = "./libcurl.so";
        candidates[2] = "/usr/lib/libcurl.so";
        static const char* kCurlSymbols[] = {
            "curl_easy_init", "curl_easy_cleanup", "curl_easy_setopt",
            "curl_easy_perform", "curl_easy_getinfo", "curl_global_init",
            "curl_easy_strerror", "curl_version"};
        std::vector<std::string> names(std::begin(kCurlSymbols), std::end(kCurlSymbols));
        publishJITLibrary(JIT, candidates, names);
    }
    if (stat("pyruntime.o", &st) == 0 && !loadOne("pyruntime.o", true)) return false;
    // Auto-load ffi_helper.o if present (unless already listed in extraObjs)
    bool hasFfiHelper = false;
    for (auto& obj : extraObjs) if (obj.find("ffi_helper") != std::string::npos) { hasFfiHelper = true; break; }
    if (!hasFfiHelper && stat("ffi_helper.o", &st) == 0) {
        auto buf = MemoryBuffer::getFile("ffi_helper.o");
        if (buf) {
            if (auto err = JIT.addObjectFile(std::move(*buf))) {
                std::cerr << "JIT error: " << toString(std::move(err)) << "\n";
                return false;
            }
        }
    }
    // Load extra .o files from --link flags
    for (auto& obj : extraObjs)
        if (!loadOne(obj, true)) return false;
    return true;
}

// Pre-load libpython.so so JIT can resolve Python symbols.
// Tries unversioned first, then newer-to-older versioned names:
// the old hardcoded libpython3.13.so broke silently on 3.14 systems.
// Idempotent; shared by runWithJIT and --test mode. Returns the handle
// (null when nothing found — callers warn only if Python is used).
static void* preloadPython() {
    static bool done = false;
    static void* handle = nullptr;
    if (!done) {
        done = true;
        const char* pyLibs[] = {"libpython3.so",     "libpython3.14.so",
                                "libpython3.13.so",  "libpython3.12.so",
                                "libpython3.11.so",  "libpython3.10.so",
                                "libpython3.9.so",   "libpython.so"};
        for (auto* lib : pyLibs) {
            handle = dlopen(lib, RTLD_LAZY | RTLD_GLOBAL);
            if (handle) break;
        }
    }
    return handle;
}

// JIT path: debug metadata is for artifacts (.ll text, .o objects), never
// for JIT execution — strip it before handing the module to ORC (the JIT
// engine with an unfinalized DIBuilder hung; stripping is also simply the
// right scope: no debugger attaches to JIT-computed code).
static int runWithJIT(std::unique_ptr<llvm::Module> mod, std::unique_ptr<llvm::LLVMContext> ctx,
                      const std::vector<char*>& progArgs,
                      const std::vector<std::string>& extraObjs = {}) {
    using namespace llvm;
    using namespace llvm::orc;

    // Debug metadata never enters the JIT (see comment above this fn).
    // The JIT artifact carries the same safety stamp as an AOT binary.
    stampSafetyPosture(mod.get());
    if (mod->getModuleFlag("Debug Info Version")) {
        llvm::StripDebugInfo(*mod.get());
    }

    void* pythonHandle = preloadPython();
    if (!pythonHandle) {
        // Loud only when the program actually calls Python (else every
        // JIT run would warn and pollute tutorial/differential output).
        if (auto* pyInit = mod->getFunction("flint_py_init")) {
            if (!pyInit->use_empty())
                std::cerr << "JIT warning: libpython not found; "
                             "Python calls will fail to resolve\n";
        }
    }

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

    if (!loadJITObjects(*JIT, extraObjs)) return 1;

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
                             const std::string& backend, const char* argv0,
                             bool loopOpt = false,
                             const std::string& targetTriple = "",
                             bool debugInfo = false,
                             bool rangeFacts = false) {
    struct stat st;
    uint64_t mt = 0;
    if (argv0 && stat(argv0, &st) == 0) mt = (uint64_t)st.st_mtime;
    // The cached module links the runtime, so the runtime's build state is part
    // of the key. Without this, editing runtime/*.c left every cached module
    // bound to the previous runtime: strrev appeared to take 17 s under the
    // quarantine where the same source compiled fresh took 2 ms, because the
    // stale entry still linked the pre-fix flint_aegis.o.
    uint64_t rt = 0;
    static const char* kRuntimeObjs[] = {
        "flint_aegis.o", "flint_chan.o", "flint_tensor.o", "flint_serial.o",
        "flint_crypto.o", "flint_net.o", "flint_ai.o", "flint_ai_opt.o", "runtime.o",
    };
    for (const char* obj : kRuntimeObjs) {
        struct stat rs;
        if (stat(obj, &rs) == 0) {
            rt = rt * 1000003u + (uint64_t)rs.st_mtime;
            rt = rt * 1000003u + (uint64_t)rs.st_size;
        } else {
            rt = rt * 1000003u + 0x9e3779b9u; // absent: still distinguishes
        }
    }
    return "flintc-v" + std::string(kFlintVersion) + "-o" + std::to_string((int)ol)
         + (releaseMode ? "-unsafe" : "-safe") + "-" + backend + "-m" + std::to_string(mt)
         + "-rt" + std::to_string(rt)
         + (loopOpt ? "-loopopt" : "")
         + (targetTriple.empty() ? "" : "-tgt" + targetTriple)
         + (debugInfo ? "-dbg" : "")
         + (rangeFacts ? "-rangefacts" : "-checked");
}

// B1: cold panic/bounds helpers for generated IR. flint_panic never returns
// (runtime abort), so mark Cold+NoReturn; flint_bounds_check returns on the
// hot path, so Cold only. Branch weights put panic blocks out of hot blocks
// without hoisting or removing any check (checked-by-default unchanged).
static void markColdPanicFn(llvm::Function* fn) {
    if (!fn) return;
    fn->addFnAttr(llvm::Attribute::Cold);
    fn->setDoesNotReturn();
}
static void markColdCheckFn(llvm::Function* fn) {
    if (!fn) return;
    fn->addFnAttr(llvm::Attribute::Cold);
}
// C1 --loop-opt: optimizer-friendly check decls. memory(read)+nounwind
// lets LICM/GVN work across checks. Deliberately WITHOUT willreturn or
// noreturn: every trap still fires exactly as before (checked-by-default
// unchanged; hoisting/DCE of the trap itself stays forbidden).
static void markLoopCheckFn(llvm::Function* fn) {
    if (!fn) return;
    fn->setOnlyReadsMemory();
    fn->setDoesNotThrow();
}
static llvm::MDNode* coldWeightsTrue(llvm::LLVMContext& ctx) {
    // True edge is the cold panic (ov==1 / is_none==1).
    llvm::MDBuilder mb(ctx);
    return mb.createBranchWeights(1, 1000);
}
static llvm::MDNode* coldWeightsFalse(llvm::LLVMContext& ctx) {
    // False edge is the cold panic (ok==0).
    llvm::MDBuilder mb(ctx);
    return mb.createBranchWeights(1000, 1);
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
        if (ec) return false;
        // copy_file creates 0666&~umask (no exec bit); a restored binary
        // must stay executable or every use fails with 126.
        ::chmod(outputPath.c_str(), 0700);
        return true;
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

enum class TypeKind { I64, F64, Str, Bool, Void, Ptr, Array, Ref, Struct, Enum, TypeParam, Map, Func };

struct Type {
    TypeKind kind = TypeKind::Void;
    std::shared_ptr<Type> elemType;
    std::string structName;
    // Function-type payload (only when kind == Func). The parser stores the
    // declared parameter types explicitly: an indirect call through such a
    // value needs the full signature at runtime.
    std::vector<Type> funcParams;
    // Indirect member: a struct cannot contain an unqualified member of its
    // own type; elemType uses the same shared_ptr pattern.
    std::shared_ptr<Type> funcRet;

    bool operator==(const Type& o) const {
        if (kind != o.kind) return false;
        if (kind == TypeKind::Array || kind == TypeKind::Ref) {
            if (!elemType || !o.elemType) return elemType == o.elemType;
            return *elemType == *o.elemType;
        }
        if (kind == TypeKind::Struct) return structName == o.structName;
        if (kind == TypeKind::Enum) return structName == o.structName;
        if (kind == TypeKind::TypeParam) return structName == o.structName;
        if (kind == TypeKind::Func)
            return funcParams == o.funcParams && funcRet && o.funcRet &&
                   *funcRet == *o.funcRet;
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
    static Type fn_(std::vector<Type> ps, Type ret) {
        Type t; t.kind = TypeKind::Func;
        t.funcParams = std::move(ps);
        t.funcRet = std::make_shared<Type>(ret);
        return t;
    }

    // Only genuinely copyable scalars. Str and Map are bare heap pointers:
    // treating them as Copy let a moved-out binding be used again, which is
    // the use-after-free / use-after-move hole. Ptr and Ref stay -- they are
    // borrowed references, not owned values.
    bool isCopyType() const {
        return kind == TypeKind::I64 || kind == TypeKind::F64 || kind == TypeKind::Bool;
    }
    bool isCompound() const { return kind == TypeKind::Array || kind == TypeKind::Ref; }
};

// Phase 2 / 2A -- the Flint type of every registered builtin, filled as the
// builtins are registered (initBuiltins). The parser's inference reads this
// map, so the two inference engines cannot disagree about a builtin's type:
// there is ONE registry, and both read it.
static std::unordered_map<std::string, Type> gBuiltinTypes;

struct StructFieldDef {
    std::string name;
    Type type;
};

struct StructDef {
    std::string name;
    std::vector<StructFieldDef> fields;
};

struct TraitMethodDef {
    std::string name;
    size_t arity = 0; // total parameter count; the receiver, if any, is param 0
};

struct TraitDef {
    std::string name;
    std::vector<TraitMethodDef> methods;
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
        case TypeKind::Func:
            // First-class function values are env-box pointers {fnptr, caps...}
            return llvm::PointerType::get(ctx, 0);
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

// Stable Stage-1 token IDs (contract with stage1/lexer.fl):
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
// DIAGNOSTICS — a span, a stable code, labels, a renderer, and a JSON form.
//
// The same structure serves three consumers: the terminal (the rendered
// form), the LSP (the JSON form -- an IDE draws its own widgets, it does
// not want our ASCII art), and the docs (the code).
//
// The shape follows rustc's (rustc-dev-guide: "Diagnostic structure"):
// a level, a code, a message, a window (the path/line/column of the
// primary span, the source, and the primary/secondary labels), and the
// sub-diagnostics (help / notes).
// ============================================================================

// The Levenshtein distance between two strings (the classic
// two-row dynamic program). Used for the "there is a name with a similar
// spelling" suggestion.
static int editDistance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return (int)m;
    if (m == 0) return (int)n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            cur[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
        prev = cur;
    }
    return prev[m];
}

// The name in `candidates` that is closest to `name`, if one is close
// enough to be worth suggesting. The threshold is relative to the
// length: a typo is usually one or two edits.
static std::string closestName(const std::string& name,
                                const std::vector<std::string>& candidates) {
    std::string best;
    int bestDist = -1;
    for (const auto& c : candidates) {
        if (c == name) continue;
        int d = editDistance(name, c);
        if (bestDist == -1 || d < bestDist) { bestDist = d; best = c; }
    }
    if (best.empty() || bestDist < 0) return "";
    // one edit for a short name, two for a longer one, and never more
    // than a third of the name's length
    size_t len = name.size();
    int allow = len <= 4 ? 1 : (len <= 8 ? 2 : 3);
    if (bestDist > allow) return "";
    if ((size_t)bestDist * 3 > len) return "";
    return best;
}

// A position in a source file. Lines and columns are 1-indexed.
struct DiagPos {
    int line = 0;
    int col = 0;
};

// A range in a source file. An invalid span has no position: some
// diagnostics are not about a particular place ("the compiler ran out
// of memory"), and pretending otherwise would be a lie.
struct DiagSpan {
    int fileId = -1;
    DiagPos start;
    DiagPos end;
    bool valid() const { return start.line > 0; }
};

// A labelled region of the source. The primary label points at the
// problem; the secondary ones give the context.
struct DiagLabel {
    DiagSpan span;
    std::string message;
    bool primary = false;
};

// A diagnostic. `code` is stable across releases (so the docs and the
// tools can link to it), `labels` carry the spans, `help` is the
// actionable line, `notes` the context.
struct Diagnostic {
    std::string code;      // "E0042"
    std::string level;     // "error" | "warning"
    std::string message;   // the title
    std::vector<DiagLabel> labels;
    std::string help;
    std::vector<std::string> notes;
};

// One source file, with its lines pre-split (the renderer needs them).
struct SourceFileEntry {
    std::string path;
    std::string text;
    std::vector<std::string> lines;   // 1-indexed: lines[0] is unused
};

// The files the diagnostics point into. The compiler compiles one main
// file plus its imports, so this is a small table.
class SourceMap {
public:
    // The imports compile in threads, so the table is guarded.
    std::mutex mu;
    int addFile(const std::string& path, const std::string& text) {
        std::lock_guard<std::mutex> lock(mu);
        SourceFileEntry f;
        f.path = path;
        f.text = text;
        f.lines.push_back("");
        std::string cur;
        for (char c : text) {
            if (c == '\n') { f.lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        f.lines.push_back(cur);
        files.push_back(f);
        return (int)files.size() - 1;
    }
    size_t count() { std::lock_guard<std::mutex> lock(mu); return files.size(); }
    std::string filePath(int id) {
        std::lock_guard<std::mutex> lock(mu);
        if (id < 0 || id >= (int)files.size()) return "<input>";
        return files[id].path;
    }
    std::string line(int id, int lineNo) {
        std::lock_guard<std::mutex> lock(mu);
        if (id < 0 || id >= (int)files.size()) return "";
        const SourceFileEntry& f = files[id];
        if (lineNo < 1 || lineNo >= (int)f.lines.size()) return "";
        return f.lines[lineNo];
    }
private:
    std::vector<SourceFileEntry> files;
};

SourceMap gSourceMap;

// The output format: "human" (the default) or "json" (one JSON object
// per line -- the LSP's input, and the shape rustc's --error-format
// json uses).
std::string gDiagFormat = "human";

// Every diagnostic of this run, in order. The imports compile in
// threads, so the list is guarded.
std::vector<Diagnostic> gDiagnostics;
std::mutex gDiagMutex;

// JSON string escaping (the diagnostics carry source text and names).
static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// The machine-readable form: one JSON object per diagnostic, one object
// per line (a stream, so a tool can consume it as it arrives).
static std::string diagnosticToJson(const Diagnostic& d) {
    std::string out = "{";
    out += "\"message\":\"" + jsonEscape(d.message) + "\",";
    out += "\"code\":\"" + jsonEscape(d.code) + "\",";
    out += "\"level\":\"" + jsonEscape(d.level.empty() ? "error" : d.level) + "\",";
    out += "\"spans\":[";
    bool first = true;
    for (const auto& l : d.labels) {
        if (!l.span.valid()) continue;
        if (!first) out += ",";
        first = false;
        out += "{";
        out += "\"file_name\":\"" + jsonEscape(gSourceMap.filePath(l.span.fileId)) + "\",";
        out += "\"line_start\":" + std::to_string(l.span.start.line) + ",";
        out += "\"line_end\":" + std::to_string(l.span.end.line) + ",";
        out += "\"column_start\":" + std::to_string(l.span.start.col) + ",";
        out += "\"column_end\":" + std::to_string(l.span.end.col) + ",";
        out += "\"is_primary\":" + std::string(l.primary ? "true" : "false") + ",";
        out += "\"label\":\"" + jsonEscape(l.message) + "\"";
        out += "}";
    }
    out += "]";
    if (!d.help.empty()) out += ",\"help\":\"" + jsonEscape(d.help) + "\"";
    if (!d.notes.empty()) {
        out += ",\"notes\":[";
        for (size_t i = 0; i < d.notes.size(); i++) {
            if (i) out += ",";
            out += "\"" + jsonEscape(d.notes[i]) + "\"";
        }
        out += "]";
    }
    out += "}";
    return out;
}

// The terminal form. The shape is rustc's:
//
//   error[E0042]: mismatched types
//    --> file.fl:4:12
//     |
//   3 | let age: Int = "twenty";
//     |          ---   -------- expected an 'Int', found a 'String'
//     |
//   help: cast or convert the value
static std::string renderDiagnostic(const Diagnostic& d) {
    std::string out;
    std::string level = d.level.empty() ? "error" : d.level;
    out += level;
    if (!d.code.empty()) out += "[" + d.code + "]";
    out += ": " + d.message + "\n";

    const DiagLabel* primary = nullptr;
    for (const auto& l : d.labels) if (l.primary) { primary = &l; break; }
    if (!primary && !d.labels.empty()) primary = &d.labels[0];

    if (!primary || !primary->span.valid()) {
        for (const auto& n : d.notes) out += "note: " + n + "\n";
        if (!d.help.empty()) out += "help: " + d.help + "\n";
        return out;
    }

    const DiagSpan& sp = primary->span;
    std::string path = gSourceMap.filePath(sp.fileId);
    out += " --> " + path + ":" + std::to_string(sp.start.line) + ":" +
           std::to_string(sp.start.col) + "\n";

    // the lines the labels touch
    int firstLine = sp.start.line, lastLine = sp.end.line;
    for (const auto& l : d.labels) {
        if (!l.span.valid()) continue;
        if (l.span.start.line < firstLine) firstLine = l.span.start.line;
        if (l.span.end.line > lastLine) lastLine = l.span.end.line;
    }
    if (firstLine < 1) firstLine = 1;
    if (lastLine - firstLine > 8) lastLine = firstLine + 8;   // never print a whole file

    int gw = (int)std::to_string(lastLine).size();
    std::string pad(gw, ' ');
    out += pad + " |\n";
    for (int ln = firstLine; ln <= lastLine; ln++) {
        std::string text = gSourceMap.line(sp.fileId, ln);
        out += std::to_string(ln) + " | " + text + "\n";
        // the underlines: '-' for a range, '^' for a single character
        std::string marks(text.size() + 1, ' ');
        bool any = false;
        for (const auto& l : d.labels) {
            if (!l.span.valid()) continue;
            if (l.span.start.line > ln || l.span.end.line < ln) continue;
            size_t s = (ln == l.span.start.line) ? (size_t)std::max(1, l.span.start.col) - 1 : 0;
            size_t e = (ln == l.span.end.line) ? (size_t)std::max(1, l.span.end.col) - 1 : text.size();
            if (s > text.size()) s = text.size();
            if (e < s) e = s;
            if (e > text.size()) e = text.size();
            // The primary label is drawn with '^', the secondary with '-'
            // (rustc's convention), so the eye finds the problem first.
            char mk = l.primary ? '^' : '-';
            for (size_t i = s; i < e; i++) marks[i] = mk;
            if (e == s && e < marks.size()) marks[e] = mk;
            any = true;
        }
        if (!any) continue;
        while (!marks.empty() && marks.back() == ' ') marks.pop_back();
        out += pad + " | " + marks + "\n";
        // the label messages, under the marks
        for (const auto& l : d.labels) {
            if (!l.span.valid() || l.message.empty()) continue;
            if (l.span.start.line > ln || l.span.end.line < ln) continue;
            out += pad + " | " + l.message + "\n";
        }
    }
    out += pad + " |\n";
    for (const auto& n : d.notes) out += "note: " + n + "\n";
    if (!d.help.empty()) out += "help: " + d.help + "\n";
    return out;
}

// Record a diagnostic. In the human format it is printed as it happens
// (so a long compile still shows progress); in the JSON format the
// stream is printed at the end (one object per line).
static void emitDiagnostic(Diagnostic d) {
    if (d.level.empty()) d.level = "error";
    {
        std::lock_guard<std::mutex> lock(gDiagMutex);
        gDiagnostics.push_back(d);
    }
    // The human form is printed as it happens; the JSON form is a
    // STREAM (one object per line, flushed as it arrives) so a tool can
    // consume it while the compiler is still running.
    if (gDiagFormat == "json") std::cerr << diagnosticToJson(d) << "\n";
    else std::cerr << renderDiagnostic(d);
}

// A file-level error: a diagnostic with no span (it is not about a
// particular place in a particular file).
static void emitFileError(const char* code, const std::string& message) {
    Diagnostic d;
    d.code = code ? code : "";
    d.level = "error";
    d.message = message;
    emitDiagnostic(d);
}

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
        // Float exponent: e/E [+-]? digits (1e19, 2.5e-3, 1E+4). Consumed
        // only when digits follow (optional sign skipped first); else `1e`
        // stays `1` + ident `e`, never a split token. Forces float.
        if (peek() == 'e' || peek() == 'E') {
            size_t q = pos + 1;
            char qc = q < source.size() ? source[q] : '\0';
            if (qc == '+' || qc == '-') q++;
            char dc = q < source.size() ? source[q] : '\0';
            if (std::isdigit(dc)) {
                isFloat = true;
                advance(); // 'e'/'E'
                if (peek() == '+' || peek() == '-') advance();
                while (std::isdigit(peek())) advance();
            }
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
    MapLiteral, Cast, Tensor
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
    // Phase 2 / 2A.1 -- the facts the call site resolved:
    //   * instantiatedTypeArgs: the CONCRETE type arguments of a generic
    //     call (`idc(5)` -> [i64]), whether inferred or explicit;
    //   * resolvedImpl: for a trait-method call, the implementation that
    //     name+arity dispatch selected (`area` -> `area.Circle`).
    // They are recorded by codegen and printed by --type-audit, so the
    // resolution is inspectable instead of implicit.
    std::vector<Type> instantiatedTypeArgs;
    std::string resolvedImpl;
    CallExprAST(std::string c, std::vector<std::unique_ptr<ExprAST>> a)
        : ExprAST(NodeKind::Call), callee(std::move(c)), args(std::move(a)) {}
};

struct VarDeclAST : ExprAST {
    std::string varName;
    bool isMutable;
    Type varType;
    // Phase 2 / 2A.0 -- was the type ANNOTATED (authoritative) or INFERRED
    // (a guess)? An annotated declaration is a contract the compiler must
    // enforce; an inferred one is the ad-hoc guess that the type-checker
    // work (step 2A) will replace. The type audit compares the two
    // inference engines and uses this flag to tell a bug from a guess.
    bool typeWasAnnotated = false;
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

// Field types by struct name, at file scope.
//
// exprFlintType is a static member function but struct field types live in a
// per-Parser registry, so it had no way to type `p.x`. A file-scope mirror is
// the smallest fix that keeps generic inference able to see struct field types
// without threading a parser through every type query.
static std::unordered_map<std::string, std::vector<std::pair<std::string, Type>>> gStructFieldTypes;


struct MatchArm {
    std::string enumName;
    std::string variantName;
    // A variant can carry several payloads (`Node(i64, i64)` binds as
    // `Node(a, b)`), so this holds one name per payload field.
    std::vector<std::string> bindNames;
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

// Phase-2 `as` cast: `expr as i64|f64` (contextual `as`, NOT a keyword —
// `as` lexes as IDENTIFIER everywhere, so no lexer/token change was needed
// and --dump-tokens output is untouched). i64->f64 widens exactly;
// f64->i64 truncates toward zero (LLVM fptosi semantics); out-of-range
// inputs are LLVM-undefined (poison), never checked — guard explicitly if
// the input is untrusted. Only i64/f64 are supported; anything else is a
// loud parse/codegen error, never silent.
struct CastExprAST : ExprAST {
    std::unique_ptr<ExprAST> expr;
    Type target;
    CastExprAST(std::unique_ptr<ExprAST> e, Type t)
        : ExprAST(NodeKind::Cast), expr(std::move(e)), target(t) {}
};

// Phase-2 f64 tensor literal: `[...floats...]` (rank 1) or
// `[[...],[...]]` equal-length rows (rank 2), desugared at codegen to the
// existing builtins flint_create_1d/2d + flt_set (no runtime change).
// Values are stored as doubles; int elements promote exactly when
// representable. Detection is purely syntactic (bare numeric literals, so
// every backend agrees trivially); anything else stays an Array.
struct TensorLiteralAST : ExprAST {
    int rank = 1; // 1 or 2
    int64_t rows = 0; // rank 1: length; rank 2: row count
    int64_t cols = 0; // rank 2: column count
    std::vector<double> values; // flat row-major
    TensorLiteralAST() : ExprAST(NodeKind::Tensor) {}
};

// Phase-2 helpers (as-casts + tensor literals). Detection is purely
// syntactic so the AST, emit-mode, and Flint self-host paths agree exactly.
static bool isTensorDtypeSuffix(const std::string& w) {
    return w == "f32" || w == "f16" || w == "i8" || w == "i16" ||
           w == "i32" || w == "u8" || w == "u16" || w == "u32" ||
           w == "u64";
}

// Classify the follower of contextual `as`: 1=i64, 2=f64 (supported),
// -1=explicit reject (sets *notYet for real-but-unsupported dtypes),
// 0=not a cast trigger (leave `as` alone — variable/field use).
static int castTargetKind(TokenType follower, const std::string& followerLex,
                          bool* notYet) {
    *notYet = false;
    if (follower == TokenType::KW_I64) return 1;
    if (follower == TokenType::KW_F64) return 2;
    if (follower == TokenType::KW_STR || follower == TokenType::KW_BOOL ||
        follower == TokenType::KW_PTR)
        return -1;
    if (follower == TokenType::IDENTIFIER && isTensorDtypeSuffix(followerLex)) {
        *notYet = true;
        return -1;
    }
    return 0;
}

// Numeric-literal value for tensor detection: bare Number nodes and the exact
// unary-minus desugar (0 - lit, int-zero lhs). Paren groups return their
// inner node, so (1.0) is a Number here — and identically (num 1.0d) in
// stage 2, so all paths agree. `0.0 - x` is NOT folded (only int-zero lhs).
static bool tensorElemDouble(ExprAST* e, double& out, bool& isFloat) {
    if (!e) return false;
    if (e->kind == NodeKind::Number) {
        auto* n = static_cast<NumberExprAST*>(e);
        if (n->isInt) {
            out = static_cast<double>(n->intValue);
            isFloat = false;
            return true;
        }
        out = n->value;
        if (n->isFloat) {
            isFloat = true;
            return true;
        }
        double ip;
        if (std::modf(n->value, &ip) != 0.0) {
            isFloat = true;
            return true;
        }
        isFloat = false;
        return true;
    }
    if (e->kind == NodeKind::Binary) {
        auto* b = static_cast<BinaryExprAST*>(e);
        if (b->op == '-' && b->lhs && b->lhs->kind == NodeKind::Number) {
            auto* z = static_cast<NumberExprAST*>(b->lhs.get());
            if (z->isInt && z->intValue == 0) {
                double d;
                bool f;
                if (tensorElemDouble(b->rhs.get(), d, f)) {
                    out = -d;
                    isFloat = f;
                    return true;
                }
            }
        }
    }
    return false;
}

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
        case NodeKind::Cast: {
            auto* c = static_cast<CastExprAST*>(e);
            auto n = std::make_unique<CastExprAST>(cloneExpr(c->expr.get()), c->target);
            n->loc = c->loc; return n;
        }
        case NodeKind::Tensor: {
            auto* t = static_cast<TensorLiteralAST*>(e);
            auto n = std::make_unique<TensorLiteralAST>();
            n->loc = t->loc;
            n->rank = t->rank; n->rows = t->rows; n->cols = t->cols;
            n->values = t->values;
            return n;
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
    // Phase 2 / 2A: was the return type WRITTEN (`-> T`) or inferred from
    // the body? Only a written one is a contract the checker may enforce.
    bool returnTypeWasAnnotated = false;
    std::unique_ptr<ExprAST> body;
    bool isDeclaration = false;
    size_t bodyStart = 0;  // token index of '{' in token stream
    size_t bodyEnd = 0;    // token index past '}'
    // Indices of params declared without a type annotation. They default to
    // i64, which is wrong when a pointer (vec/sb/map handle, str) is passed --
    // the call then mismatches the callee's signature. inferUntypedParams()
    // retypes them from the actual arguments at every call site.
    std::vector<size_t> untypedParams;
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
    std::vector<TraitDef> traits;
    std::vector<std::string> imports;
    std::vector<std::unique_ptr<VarDeclAST>> globals;
    std::vector<std::unique_ptr<ExprAST>> topStmts;
};

// ============================================================================
// BOUNDED RANGE FACTS (Phase 3 / P1)
// ============================================================================
//
// `rangeFacts[expr] = [lo,hi]` means every dynamic evaluation of `expr` in
// the whole program yields an i64 in [lo,hi]. Codegen uses that fact to
// replace a checked add/sub/mul with a plain one. That is not a micro
// optimisation: the checked intrinsic puts a branch in the recursive
// function, and LLVM's accumulator-recursion transform refuses to turn
// `fib(n-1) + fib(n-2)` into a loop while that branch is there. Removing a
// PROVABLY SAFE check restores the transform and the C-level runtime.
//
// The analysis is deliberately small and closed-world:
//   * only a non-generic function with one i64 parameter and i64 return;
//   * every call site must be a direct call whose argument is a constant,
//     or a self-call `n - c` / `n + (-c)` with c > 0;
//   * the function is evaluated for every constant root, memoized by
//     argument, so `fib(45)` costs 46 evaluations, not 2^45;
//   * an unknown value, loop, alias, or call to another function makes the
//     affected result TOP (no fact), never an assumption.
//
// The caller only enables the facts for whole-program executables. An
// object/IR artifact could be called with an argument outside the analyzed
// set, so those modes keep every check.
struct IntRangeFact {
    int64_t lo;
    int64_t hi;
};

class BoundedRangeAnalysis {
public:
    BoundedRangeAnalysis(ProgramAST& program,
                         std::unordered_map<const ExprAST*, IntRangeFact>& out)
        : prog(program), out(out) {}

    void run() {
        indexProgram();
        for (auto& up : prog.functions) {
            FunctionAST* f = up.get();
            if (!eligible(f)) continue;
            analyze(f);
        }
    }

private:
    struct AV {
        enum Kind { Top, Int, Bool } kind = Top;
        int64_t lo = 0, hi = 0;
        bool b = false;
        static AV top() { return AV(); }
        static AV integer(int64_t v) { AV a; a.kind = Int; a.lo = a.hi = v; return a; }
        static AV interval(int64_t l, int64_t h) { AV a; a.kind = Int; a.lo = l; a.hi = h; return a; }
        static AV boolean(bool v) { AV a; a.kind = Bool; a.b = v; return a; }
    };
    using Env = std::unordered_map<std::string, AV>;

    // A compile-time budget: a recursive function with a huge constant root
    // must fail closed instead of hanging the compiler.
    static constexpr long kMaxSteps = 2000000;
    static constexpr int kMaxDepth = 4000;

    ProgramAST& prog;
    std::unordered_map<const ExprAST*, IntRangeFact>& out;

    FunctionAST* fn = nullptr;
    std::string param;
    std::unordered_map<int64_t, AV> memo;
    std::unordered_map<const ExprAST*, IntRangeFact> local;
    std::unordered_set<const ExprAST*> tainted;
    bool failed = false;
    bool returned = false;
    AV returnVal;
    long steps = 0;
    int depth = 0;

    static bool eligible(FunctionAST* f) {
        if (!f || f->isDeclaration || !f->body) return false;
        if (!f->typeParams.empty()) return false;
        if (f->name == "main") return false;
        if (f->params.size() != 1) return false;
        if (f->params[0].second.kind != TypeKind::I64) return false;
        if (f->returnType.kind != TypeKind::I64) return false;
        return true;
    }

    void analyze(FunctionAST* f) {
        fn = f;
        param = f->params[0].first;
        memo.clear();
        local.clear();
        tainted.clear();
        failed = false;
        returned = false;
        returnVal = AV::top();
        steps = 0;
        depth = 0;

        std::vector<int64_t> roots;
        if (!collectRoots(f, roots)) return;
        if (roots.empty()) return;

        for (int64_t root : roots) {
            returned = false;
            (void)evalFunction(root);
            if (failed) return;
        }
        if (failed) return;

        // A node that produced TOP on any evaluation has no usable fact.
        for (auto* e : tainted) local.erase(e);
        for (auto& kv : local) out[kv.first] = kv.second;
    }

    struct CallSite {
        CallExprAST* call;
        FunctionAST* owner;  // nullptr: top level or a default argument
    };

    // One pass over the program builds the call-site index. Walking the
    // whole program once per candidate function cost ~0.65 s on the stage3
    // emitter (a ~20% compile-time regression on an executable build); this
    // is O(program) once.
    std::unordered_map<std::string, std::vector<CallSite>> callsByName;
    std::unordered_set<std::string> valueUses;
    std::unordered_set<std::string> dispatchedNames;
    bool hasNameDispatch = false;

    void indexProgram() {
        std::function<void(ExprAST*, FunctionAST*)> walk =
            [&](ExprAST* e, FunctionAST* owner) {
            if (!e) return;
            switch (e->kind) {
                case NodeKind::Variable: {
                    valueUses.insert(static_cast<VariableExprAST*>(e)->name);
                    return;
                }
                case NodeKind::Call: {
                    auto* c = static_cast<CallExprAST*>(e);
                    // A name-based dispatcher can call any function with an
                    // argument we cannot see. `flint_thread_spawn("f", n)` is
                    // the one in this runtime; a string literal naming a
                    // function is refused generically too.
                    if (c->callee == "flint_thread_spawn" ||
                        c->callee == "py_eval" || c->callee == "flint_py_eval" ||
                        c->callee == "flint_py_exec") {
                        hasNameDispatch = true;
                    }
                    for (auto& a : c->args) {
                        if (a && a->kind == NodeKind::String)
                            dispatchedNames.insert(
                                static_cast<StringExprAST*>(a.get())->value);
                    }
                    callsByName[c->callee].push_back(CallSite{c, owner});
                    for (auto& a : c->args) walk(a.get(), owner);
                    return;
                }
                case NodeKind::Binary: {
                    auto* b = static_cast<BinaryExprAST*>(e);
                    walk(b->lhs.get(), owner); walk(b->rhs.get(), owner);
                    return;
                }
                case NodeKind::Compare: {
                    auto* c = static_cast<CompareExprAST*>(e);
                    walk(c->lhs.get(), owner); walk(c->rhs.get(), owner);
                    return;
                }
                case NodeKind::Assign: {
                    walk(static_cast<AssignExprAST*>(e)->rhs.get(), owner);
                    return;
                }
                case NodeKind::VarDecl: {
                    walk(static_cast<VarDeclAST*>(e)->init.get(), owner);
                    return;
                }
                case NodeKind::Return: {
                    walk(static_cast<ReturnStmtAST*>(e)->value.get(), owner);
                    return;
                }
                case NodeKind::If: {
                    auto* i = static_cast<IfStmtAST*>(e);
                    walk(i->condition.get(), owner);
                    walk(i->thenBlock.get(), owner);
                    walk(i->elseBlock.get(), owner);
                    return;
                }
                case NodeKind::While: {
                    auto* w = static_cast<WhileStmtAST*>(e);
                    walk(w->condition.get(), owner); walk(w->body.get(), owner);
                    return;
                }
                case NodeKind::Block: {
                    auto* b = static_cast<BlockStmtAST*>(e);
                    for (auto& s : b->stmts) walk(s.get(), owner);
                    return;
                }
                case NodeKind::Ref: {
                    walk(static_cast<RefExprAST*>(e)->target.get(), owner);
                    return;
                }
                case NodeKind::Deref: {
                    walk(static_cast<DerefExprAST*>(e)->target.get(), owner);
                    return;
                }
                case NodeKind::Index: {
                    auto* i = static_cast<IndexExprAST*>(e);
                    walk(i->base.get(), owner); walk(i->index.get(), owner);
                    return;
                }
                case NodeKind::Slice: {
                    auto* s = static_cast<SliceExprAST*>(e);
                    walk(s->arr.get(), owner); walk(s->start.get(), owner); walk(s->end.get(), owner);
                    return;
                }
                case NodeKind::StructLiteral: {
                    auto* s = static_cast<StructLiteralAST*>(e);
                    for (auto& fld : s->fields) walk(fld.second.get(), owner);
                    return;
                }
                case NodeKind::EnumConstruct: {
                    auto* en = static_cast<EnumConstructAST*>(e);
                    for (auto& a : en->args) walk(a.get(), owner);
                    return;
                }
                case NodeKind::Match: {
                    auto* m = static_cast<MatchExprAST*>(e);
                    walk(m->scrutinee.get(), owner);
                    for (auto& arm : m->arms) walk(arm.body.get(), owner);
                    return;
                }
                case NodeKind::FieldAccess: {
                    walk(static_cast<FieldAccessAST*>(e)->base.get(), owner);
                    return;
                }
                case NodeKind::Lambda: {
                    walk(static_cast<LambdaExprAST*>(e)->body.get(), owner);
                    return;
                }
                case NodeKind::Cast: {
                    walk(static_cast<CastExprAST*>(e)->expr.get(), owner);
                    return;
                }
                case NodeKind::Unwrap: {
                    walk(static_cast<UnwrapExprAST*>(e)->inner.get(), owner);
                    return;
                }
                case NodeKind::Array: {
                    auto* a = static_cast<ArrayExprAST*>(e);
                    for (auto& el : a->elements) walk(el.get(), owner);
                    return;
                }
                case NodeKind::Destructure: {
                    walk(static_cast<DestructureAST*>(e)->init.get(), owner);
                    return;
                }
                case NodeKind::MapLiteral: {
                    auto* m = static_cast<MapLiteralAST*>(e);
                    for (auto& v : m->values) walk(v.get(), owner);
                    return;
                }
                default:
                    return;
            }
        };
        for (auto& up : prog.functions) {
            FunctionAST* g = up.get();
            if (!g || g->isDeclaration) continue;
            // A default argument is evaluated at the CALL site, not in the
            // callee's frame, so treat it like top-level code: a call to f
            // there must have a constant argument (owner == nullptr).
            for (auto& d : g->defaults) walk(d.get(), nullptr);
            if (!g->body) continue;
            walk(g->body.get(), g);
        }
        for (auto& s : prog.topStmts) walk(s.get(), nullptr);
        for (auto& g : prog.globals) walk(g->init.get(), nullptr);
    }

    // Validate every use of `f`:
    //   * outside f: only direct calls with a constant argument (roots);
    //   * inside f: constants or `param - positive-constant` self-calls;
    //   * any use of the function as a VALUE rejects the analysis.
    bool collectRoots(FunctionAST* f, std::vector<int64_t>& roots) {
        if (valueUses.count(f->name)) return false;
        if (hasNameDispatch || dispatchedNames.count(f->name)) return false;
        auto it = callsByName.find(f->name);
        if (it == callsByName.end()) return true;   // no calls: no roots
        for (auto& site : it->second) {
            CallExprAST* c = site.call;
            if (c->args.size() != 1) return false;
            ExprAST* arg = c->args[0].get();
            int64_t cv = 0;
            if (site.owner == f) {
                if (!constArg(arg, cv) && !isDecreasingArg(arg)) return false;
            } else {
                if (!constArg(arg, cv)) return false;
                roots.push_back(cv);
            }
        }
        return true;
    }

    static bool constArg(ExprAST* e, int64_t& out) {
        if (!e) return false;
        if (e->kind == NodeKind::Number) {
            auto* n = static_cast<NumberExprAST*>(e);
            if (!n->isInt) return false;
            out = n->intValue;
            return true;
        }
        if (e->kind == NodeKind::Binary) {
            auto* b = static_cast<BinaryExprAST*>(e);
            if (b->op != '+' && b->op != '-' && b->op != '*') return false;
            int64_t l = 0, r = 0;
            if (!constArg(b->lhs.get(), l) || !constArg(b->rhs.get(), r)) return false;
            __int128 v = 0;
            if (b->op == '+') v = (__int128)l + r;
            else if (b->op == '-') v = (__int128)l - r;
            else v = (__int128)l * r;
            if (v < INT64_MIN || v > INT64_MAX) return false;
            out = (int64_t)v;
            return true;
        }
        return false;
    }

    bool isDecreasingArg(ExprAST* e) const {
        if (!e || e->kind != NodeKind::Binary) return false;
        auto* b = static_cast<BinaryExprAST*>(e);
        if (!b->lhs || b->lhs->kind != NodeKind::Variable) return false;
        if (static_cast<VariableExprAST*>(b->lhs.get())->name != param) return false;
        int64_t c = 0;
        if (!constArg(b->rhs.get(), c)) return false;
        if (b->op == '-') return c > 0;
        if (b->op == '+') return c < 0;
        return false;
    }

    AV evalFunction(int64_t k) {
        auto it = memo.find(k);
        if (it != memo.end()) return it->second;
        if (failed) return AV::top();
        if (depth >= kMaxDepth || steps >= kMaxSteps) { failed = true; return AV::top(); }
        depth++;
        Env env;
        env[param] = AV::integer(k);
        // `returned`/`returnVal` describe the function whose body is being
        // evaluated. A self-call runs ANOTHER body, and an explicit `return`
        // there must not truncate the caller: `x = f(n-1); x + 1` was
        // treated as if the caller had returned, memoizing the callee's
        // value as the caller's and feeding a wrong fact to the caller's
        // caller. Save and restore the caller's state.
        bool savedReturned = returned;
        AV savedReturnVal = returnVal;
        returned = false;
        returnVal = AV::top();
        AV v = evalExpr(fn->body.get(), env);
        returned = savedReturned;
        returnVal = savedReturnVal;
        depth--;
        if (failed) return AV::top();
        memo[k] = v;
        return v;
    }

    static AV boolToInt(const AV& v) {
        if (v.kind != AV::Bool) return v;
        return AV::integer(v.b ? 1 : 0);
    }

    AV evalExpr(ExprAST* e, Env& env) {
        if (!e || failed) return AV::top();
        if (++steps > kMaxSteps) { failed = true; return AV::top(); }
        AV v = evalExprInner(e, env);
        if (v.kind == AV::Int) {
            auto it = local.find(e);
            if (it == local.end()) {
                local[e] = IntRangeFact{v.lo, v.hi};
            } else {
                if (v.lo < it->second.lo) it->second.lo = v.lo;
                if (v.hi > it->second.hi) it->second.hi = v.hi;
            }
        } else if (v.kind == AV::Top) {
            tainted.insert(e);
        }
        return v;
    }

    AV evalExprInner(ExprAST* e, Env& env) {
        switch (e->kind) {
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(e);
                if (!n->isInt) return AV::top();
                return AV::integer(n->intValue);
            }
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(e);
                auto it = env.find(v->name);
                if (it == env.end()) return AV::top();
                return it->second;
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(e);
                AV l = boolToInt(evalExpr(b->lhs.get(), env));
                AV r = boolToInt(evalExpr(b->rhs.get(), env));
                if (l.kind != AV::Int || r.kind != AV::Int) return AV::top();
                if (b->op == '+' || b->op == '-' || b->op == '*') {
                    __int128 lo = 0, hi = 0;
                    if (b->op == '+') {
                        lo = (__int128)l.lo + r.lo;
                        hi = (__int128)l.hi + r.hi;
                    } else if (b->op == '-') {
                        lo = (__int128)l.lo - r.hi;
                        hi = (__int128)l.hi - r.lo;
                    } else {
                        __int128 p1 = (__int128)l.lo * r.lo;
                        __int128 p2 = (__int128)l.lo * r.hi;
                        __int128 p3 = (__int128)l.hi * r.lo;
                        __int128 p4 = (__int128)l.hi * r.hi;
                        __int128 mn1 = p1 < p2 ? p1 : p2;
                        __int128 mn2 = p3 < p4 ? p3 : p4;
                        __int128 mx1 = p1 > p2 ? p1 : p2;
                        __int128 mx2 = p3 > p4 ? p3 : p4;
                        lo = mn1 < mn2 ? mn1 : mn2;
                        hi = mx1 > mx2 ? mx1 : mx2;
                    }
                    if (lo < INT64_MIN || hi > INT64_MAX) return AV::top();
                    return AV::interval((int64_t)lo, (int64_t)hi);
                }
                if (b->op == '/' || b->op == '%') {
                    if (l.lo == l.hi && r.lo == r.hi && r.lo != 0) {
                        if (l.lo == INT64_MIN && r.lo == -1) return AV::top();
                        int64_t v = (b->op == '/') ? l.lo / r.lo : l.lo % r.lo;
                        return AV::integer(v);
                    }
                }
                return AV::top();
            }
            case NodeKind::Compare: {
                auto* c = static_cast<CompareExprAST*>(e);
                AV l = boolToInt(evalExpr(c->lhs.get(), env));
                AV r = boolToInt(evalExpr(c->rhs.get(), env));
                if (l.kind != AV::Int || r.kind != AV::Int) return AV::top();
                bool t = false, f = false;
                if (c->op == "<") { t = l.hi < r.lo; f = l.lo >= r.hi; }
                else if (c->op == "<=") { t = l.hi <= r.lo; f = l.lo > r.hi; }
                else if (c->op == ">") { t = l.lo > r.hi; f = l.hi <= r.lo; }
                else if (c->op == ">=") { t = l.lo >= r.hi; f = l.hi < r.lo; }
                else if (c->op == "==") {
                    t = (l.lo == l.hi && r.lo == r.hi && l.lo == r.lo);
                    f = (l.hi < r.lo || r.hi < l.lo);
                } else if (c->op == "!=") {
                    t = (l.hi < r.lo || r.hi < l.lo);
                    f = (l.lo == l.hi && r.lo == r.hi && l.lo == r.lo);
                } else {
                    return AV::top();
                }
                if (t) return AV::boolean(true);
                if (f) return AV::boolean(false);
                return AV::top();
            }
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(e);
                if (c->callee != fn->name || c->args.size() != 1) {
                    failed = true;
                    return AV::top();
                }
                AV a = evalExpr(c->args[0].get(), env);
                if (a.kind != AV::Int || a.lo != a.hi) {
                    failed = true;
                    return AV::top();
                }
                return evalFunction(a.lo);
            }
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(e);
                // Assigning a name that is not a local (the parameter or a
                // VarDecl in scope) writes a global. Globals are not modeled
                // and a self-call could mutate one behind our back, so this
                // function gets no facts.
                if (env.find(a->varName) == env.end()) {
                    failed = true;
                    return AV::top();
                }
                AV v = evalExpr(a->rhs.get(), env);
                env[a->varName] = v;
                return v;
            }
            case NodeKind::VarDecl: {
                auto* d = static_cast<VarDeclAST*>(e);
                AV v = d->init ? evalExpr(d->init.get(), env) : AV::top();
                env[d->varName] = v;
                return v;
            }
            case NodeKind::Return: {
                auto* r = static_cast<ReturnStmtAST*>(e);
                AV v = r->value ? evalExpr(r->value.get(), env) : AV::top();
                returned = true;
                returnVal = v;
                return v;
            }
            case NodeKind::If: {
                auto* i = static_cast<IfStmtAST*>(e);
                AV c = evalExpr(i->condition.get(), env);
                if (c.kind != AV::Bool) { failed = true; return AV::top(); }
                if (c.b) return evalExpr(i->thenBlock.get(), env);
                if (i->elseBlock) return evalExpr(i->elseBlock.get(), env);
                return AV::top();
            }
            case NodeKind::Block:
                return evalBlock(static_cast<BlockStmtAST*>(e), env);
            case NodeKind::While:
            case NodeKind::Break:
            case NodeKind::Continue:
            case NodeKind::PyBlock:
            case NodeKind::Match:
            case NodeKind::Destructure:
                failed = true;
                return AV::top();
            case NodeKind::String:
            case NodeKind::Tensor:
                // Leaves: no subexpressions and no side effects. (A tensor
                // literal holds doubles, not AST nodes.)
                return AV::top();
            default:
                // An expression the evaluator does not model. It may contain
                // a call that mutates a local through `&x`, or an assignment
                // in a map value, so skipping it could leave the env stale.
                // Fail closed instead of returning TOP: the function then
                // gets no facts at all.
                failed = true;
                return AV::top();
        }
    }

    // The environment is passed by REFERENCE: a branch that the analysis
    // proved taken may assign to a local, and the statements after the
    // branch must see that assignment. Copying the env here was a real
    // soundness bug -- `mut x = 0; if n > 0 { x = MAX }; x + n` was
    // "proved" [1,1] and the check was removed, so the sum wrapped instead
    // of trapping. Only the taken branch is ever evaluated, so mutating the
    // caller's env is exactly right.
    AV evalBlock(BlockStmtAST* b, Env& env) {
        AV last = AV::top();
        for (auto& s : b->stmts) {
            last = evalExpr(s.get(), env);
            if (returned || failed) break;
        }
        return returned ? returnVal : last;
    }
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
    // P0-NEW-3: set when the binding came from a float tensor literal
    // (TensorLiteralAST -> flt_create_*). Indexing that value reinterprets
    // f64 bits through an i64 GEP; the flag lets the index emitters refuse
    // instead of printing garbage.
    bool isTensor = false;
    // Phase 2 / step 2A.1 -- a FACT, not a type: the EXACT length of an
    // array value, when the compiler knows it (a literal initializer).
    // -1 means "not known".
    //
    // Why it matters: the bounds check compares the index against the
    // length LOADED FROM THE SLICE, and the loop bound is a different
    // value, so LLVM cannot relate them and the check survives (measured:
    // 3 of 6 checks survive in sum_array). When the length is a constant
    // the check compares against that constant, and LLVM folds it away
    // whenever the index is provably in range.
    //
    // Why it is on the Symbol and not in the Type: the length is a property
    // of the VALUE, not of the type -- `[i64]` may hold any length. A
    // fact in the type would be copied into places it does not hold.
    //
    // It is set ONLY from a literal initializer, so it is exact by
    // construction, and it is CLEARED on assignment (a binding can be
    // reassigned to a different array).
    // The EXACT length of an array value, as an SSA value: a constant for a
    // literal, or the argument of the flint_array_alloc call that produced
    // it. null means "not known".
    //
    // The runtime form is the important one: `arr = flint_array_alloc(n)`
    // gives an array whose length IS n (runtime/rust/src/array.rs: `len: n`),
    // and n is the SAME value the user's loop compares against -- so the
    // bounds check compares the index against the same SSA value the loop
    // does, and LLVM folds it away. With the length loaded from the slice,
    // the two were unrelated values and the check always survived
    // (measured: 3 of 6 checks survive in sum_array).
    //
    // Only LOCAL bindings may carry a non-constant fact: the value must
    // DOMINATE every use, and a global's initializer value does not
    // dominate code in other functions.
    llvm::Value* knownArrayLenValue = nullptr;
    // M4 env box: { fnptr, cap0, cap1, ... } allocated in the defining frame.
    // The first-class function VALUE is this pointer; an indirect call loads
    // the callee from field 0 and passes the box itself as the first arg.
    llvm::Value* lambdaEnv = nullptr;
    llvm::StructType* lambdaBoxTy = nullptr;
    // Phase 2 / 2D: true when the env box lives on the HEAP -- either
    // allocated there directly (a closure created in an escaping position)
    // or promoted at an escape site by promoteLambdaBox(). A heap box does
    // not depend on the defining frame, so the value may outlive it.
    bool lambdaHeapBox = false;

    // ONE predicate for "is this binding a function VALUE?", used by every
    // site that asks: binding a value, storing one in a global, returning
    // one, and calling one.
    //
    // The three sites that used to answer this question each spelled it
    // differently -- one required isLambda AND a fn type, another required
    // NOT isLambda AND a fn type, a third accepted either -- and the
    // divergence was not theoretical: copying a NAMED function's value into
    // another binding (g = h) lost the fn type and the program then failed
    // with "undefined function 'g'".
    //
    // A lambda binding is a function value; so is any binding whose type
    // is a fn type (that covers the named-function values added after M4).
    static bool isFunctionValue(const Symbol* sym) {
        if (!sym) return false;
        if (sym->isLambda) return true;
        return sym->type.kind == TypeKind::Func;
    }
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

    // Clear `moved` on every visible binding. Called after a loop body: the
    // next iteration re-runs the same statements, so a move performed in one
    // pass does not carry into the next. The binding may have been declared
    // before the loop, so this walks every scope, not just the innermost.
    void reviveMoved() {
        for (auto& sc : scopes)
            for (auto& kv : sc) kv.second.moved = false;
    }

    // Apply f to every visible binding (all open scopes). The closure
    // promotion uses this to keep ALIASES of one env box in sync: two
    // bindings may name the same box (`let h = cb`), and after promotion
    // both have to point at the heap copy.
    template <typename F>
    void forEachSymbol(F f) {
        for (auto& sc : scopes)
            for (auto& kv : sc) f(kv.second);
    }

    // Every name visible at this point (all the open scopes). Used for
    // the "there is a name with a similar spelling" suggestion.
    std::vector<std::string> visibleNames() const {
        std::vector<std::string> out;
        for (const auto& sc : scopes)
            for (const auto& kv : sc) out.push_back(kv.first);
        return out;
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
    // The file this parser reads, so a diagnostic can point into it.
    int fileId = 0;
    size_t getPos() const { return pos; }
    void setPos(size_t p) { pos = p; lexer.ensure(p); }
    bool isInEmitMode() const { return emitMode; }
    // Used by Codegen to re-parse function bodies from token ranges
    std::unique_ptr<ExprAST> reparseBlock() { return parseBlock(); }

    // Lambda-body use collector, shared with Codegen (which verifies
    // outlined bodies). The worker is a private static; this forwards it.
    static void lambdaUses(ExprAST* e, std::unordered_set<std::string>& bound,
                           std::vector<std::string>& uses, std::string* err = nullptr);

    // Single source of truth for what a *builtin call* evaluates to, used by
    // both parse-time inference (inferType / parseVarDecl) so a `let`/`var`
    // bound to a builtin call is typed the same way codegen will type it.
    //
    // It must stay in step with Codegen::initBuiltins: when the two disagree
    // (inference says str, codegen's functionMap says i64) the emitted code
    // loads the wrong type or builds a PHI from mismatched branches --
    // invalid IR that used to be emitted silently.
    //
    // Written as explicit per-name entries, NOT prefix matching: families mix
    // return kinds (flint_vec_get -> i64 but flint_vec_new -> ptr), so a
    // `flint_vec_` prefix test types every vec call as a pointer.
    // The type of a call's result.
    //
    // Phase 2 / 2A: the compiler's OWN registry is consulted first, so a
    // builtin's type is whatever the compiler registered it as. The
    // hand-written lists below used to be the only source, and they had
    // drifted from the registry -- the type audit measured 178 declarations
    // where the parser and the codegen disagreed, always "the parser says
    // i64" (a call to flint_regex_replace, flint_str_*, ...). One registry
    // removes the class, not just the instances.
    Type inferCallType(const std::string& callee) {
        // An INDIRECT call (`h = inc; h(5)`) is typed by the binding's fn
        // signature. A local binding shadows a builtin of the same name, so
        // it is consulted first.
        auto vt = varTypeMap.find(callee);
        if (vt != varTypeMap.end() && vt->second.kind == TypeKind::Func &&
            vt->second.funcRet)
            return *vt->second.funcRet;
        auto bit = gBuiltinTypes.find(callee);
        if (bit != gBuiltinTypes.end()) return bit->second;
        // A USER function: its declared return type, from the pre-pass (so a
        // forward reference resolves) or from the signatures seen so far.
        auto rit = fnReturnTypes.find(callee);
        if (rit != fnReturnTypes.end()) return rit->second;
        return inferCallTypeFallback(callee);
    }

    // Phase 2 / 2A: record the VALUE-level signature of a parsed function
    // (`inc` in `h = inc`), not just its call-level return type. The two are
    // different questions: `fnReturnTypes` answers "what does a call
    // return", this answers "what is the function itself".
    //
    // A generic or void function has no fn value -- codegen's namedFnThunk
    // returns nullptr for both -- so it is deliberately absent: better no
    // guess than a wrong one.
    void recordFnSignature(const FunctionAST* fn) {
        if (!fn) return;
        if (!fn->typeParams.empty() ||
            fn->returnType.kind == TypeKind::Void ||
            fn->returnType.kind == TypeKind::TypeParam) {
            fnSignatures.erase(fn->name);
            return;
        }
        std::vector<Type> ps;
        ps.reserve(fn->params.size());
        for (auto& p : fn->params) {
            if (p.second.kind == TypeKind::TypeParam) {
                fnSignatures.erase(fn->name);
                return;
            }
            ps.push_back(p.second);
        }
        fnSignatures[fn->name] = Type::fn_(std::move(ps), fn->returnType);
    }

    static Type inferCallTypeFallback(const std::string& callee) {
        // Void builtins first: several share a prefix with a value-returning
        // family (flint_map_free vs flint_map_new).
        static const std::unordered_set<std::string> kVoid = {
            "flint_println_i64", "flint_println_str", "flint_println_f64",
            "flint_panic", "flint_py_init", "flint_py_run", "flint_py_fini",
            "flint_bounds_check", "flint_str_free", "flint_print", "flint_println",
            "flint_csv_free",
            "flint_free", "flint_array_write_i64", "flint_array_free",
            "flint_null_check", "flint_array_set_ptr", "flint_srand",
            "flint_vec_free", "flint_vec_set", "flint_vec_push", "flint_vec_clear",
            "flint_map_free", "flint_map_set", "flint_sb_free", "flint_sb_append",
            "flint_sb_append_char", "flint_sb_append_i64", "flint_sb_append_f64",
            "flint_exit", "flint_sleep_ms", "flint_assert", "flint_assert_eq_i64",
            "flint_assert_eq_f64", "flint_assert_eq_str", "flint_tcp_close",
            "flint_aegis_write_i64", "flint_aegis_write_i64_unchecked",
            "flint_aegis_write_u8", "flint_chan_free",
        };
        if (kVoid.count(callee)) return Type::void_();

        // Aggregate handles: a map or array is a {ptr, i64} pair value, and
        // only these three builtins produce one.
        if (callee == "flint_map_new" || callee == "flint_array_alloc" ||
            callee == "flint_array_concat")
            return callee == "flint_map_new" ? Type::map() : Type::array(Type::i64());

        // Opaque handles and strings: bare pointers.
        static const std::unordered_set<std::string> kPtr = {
            "flint_vec_new", "flint_array_slice", "flint_sb_new", "flint_sb_build",
            "flint_map_keys", "flint_array_data", "flint_array_data_ptr",
            "flint_array_get_ptr",
            "flint_str_concat", "flint_str_substring", "flint_str_upper",
            "flint_str_lower", "flint_i64_to_string", "flint_f64_to_string",
            "flint_str_trim", "flint_str_replace", "flint_str_split",
            "flint_str_join", "flint_str_from_i64", "flint_str_from_f64",
            "flint_base64_encode", "flint_base64_decode", "flint_hex_encode",
            "flint_hex_decode", "flint_json_decode",
            "flint_tcp_connect", "flint_tcp_listen", "flint_tcp_accept",
            "flint_tcp_recv", "flint_tcp_recv_all", "flint_dns_resolve",
            "flint_http_get", "flint_http_post",
        };
        if (kPtr.count(callee)) return Type::str();

        // Everything else declared i64 in initBuiltins (vec/sb/map/array
        // accessors, math, time, aegis leases, chan send, ...).
        return Type::i64();
    }

    // Reset per-function declared-variable state and seed with the given params.
    // Needed when re-parsing multiple function bodies in sequence (QBE backend),
    // otherwise a variable declared in one function is seen as already-declared
    // in the next, turning `x = ...` VarDecls into Assigns.
    void resetDeclaredVars(const std::vector<std::pair<std::string, Type>>& params) {
        declaredVars.clear();
        for (auto& p : params) declaredVars.insert(p.first);
    }

    // Statements that can be an implicit function return value.
    //
    // This must be an ALLOWLIST of value-producing kinds, not a blocklist of
    // statement kinds: a blocklist silently admits Assign (`m = f()` as the
    // last line), which then typed the function's return as the assigned type
    // and emitted `ret ptr` from a void function -- invalid IR. Anything not
    // listed here has no value and must never become a return.
    static bool isValueExpr(NodeKind k) {
        switch (k) {
            case NodeKind::Number: case NodeKind::String: case NodeKind::Variable:
            case NodeKind::Binary: case NodeKind::Compare: case NodeKind::Call:
            case NodeKind::Array: case NodeKind::Index: case NodeKind::Slice:
            case NodeKind::Ref: case NodeKind::Deref: case NodeKind::StructLiteral:
            case NodeKind::EnumConstruct: case NodeKind::FieldAccess:
            case NodeKind::Lambda:             case NodeKind::Unwrap: case NodeKind::MapLiteral:
            case NodeKind::Cast: case NodeKind::Tensor:
            // An `if` used in expression position is a value (`fn f() -> i64
            // { if c { 1 } else { 2 } }`). Excluding it broke implicit returns
            // of if-expressions -- fib() silently returned 0.
            case NodeKind::If:
                return true;
            default:
                return false;
        }
    }

    // Trait declaration (M3/B5): `trait Name { fn m(p: T) -> Ret ... }`.
    // A trait carries SIGNATURES ONLY. There is deliberately no impl syntax:
    // implementations are ordinary functions of the same name whose arity
    // matches, resolved at the method-call site. No body is stored, so this
    // parses in skipBodies/interface mode too.
    void parseTraitDef(ProgramAST& prog) {
        advance(); // 'trait' (identifier, contextual)
        Token nameTok = consume(TokenType::IDENTIFIER, "expected trait name after 'trait'");
        TraitDef td;
        td.name = nameTok.lexeme;
        if (traitRegistry.count(td.name)) {
            parseError("E0118", "trait '" + td.name + "' is already declared");
            return;
        }
        consume(TokenType::LBRACE, "expected '{' after trait name");
        match(TokenType::NEWLINE);
        while (!isAtEnd() && !check(TokenType::RBRACE)) {
            bool isFn = check(TokenType::KW_FN) || check(TokenType::KW_DEF);
            if (!isFn) {
                parseError("E0049", "expected 'fn' or 'def' in trait '" + td.name + "' body");
                return;
            }
            advance();
            Token mTok = consume(TokenType::IDENTIFIER, "expected method name");
            TraitMethodDef md;
            md.name = mTok.lexeme;
            if (match(TokenType::LT)) {
                parseError("E0137", "generic trait methods are not supported: declare the "
                           "method without type parameters");
                return;
            }
            consume(TokenType::LPAREN, "expected '(' after method name");
            if (check(TokenType::RPAREN)) {
                advance();
            } else {
                do {
                    consume(TokenType::IDENTIFIER, "expected parameter name in trait method");
                    if (match(TokenType::COLON)) parseType(); // signatures only
                    md.arity++;
                } while (match(TokenType::COMMA));
                consume(TokenType::RPAREN, "expected ')' after trait method parameters");
            }
            if (match(TokenType::ARROW)) parseType();
            for (auto& m : td.methods)
                if (m.name == md.name) {
                    parseError("E0119", "trait method '" + md.name + "' is declared twice in '" + td.name + "'");
                    return;
                }
            td.methods.push_back(md);
            match(TokenType::NEWLINE);
            match(TokenType::SEMICOLON);
        }
        consume(TokenType::RBRACE, "expected '}' to close trait body");
        match(TokenType::NEWLINE);
        traitRegistry[td.name] = td;
        prog.traits.push_back(td);
    }

    // Phase 2 / 2A: a DECLARATION PRE-PASS.
    //
    // A function's body may call a function declared LATER in the file.
    // Without a pre-pass the parser cannot know that call's return type and
    // infers i64 -- which made
    //
    //     fn user() { s = later()
    //                  s }
    //     fn later() -> str { return "hello" }
    //
    // infer `user()` as returning i64, and the compiler then emitted invalid
    // IR ("Function return type does not match operand type of return
    // inst!") and refused to build a program that is perfectly valid. The
    // type audit is what exposed it.
    //
    // The pre-pass records the return types it can see. It deliberately
    // handles only the ANNOTATED forms (i64/f64/str/bool/void and a known
    // struct or enum name): a function whose return type is INFERRED from
    // its body cannot be known before the body is parsed, and guessing
    // would be exactly the silent wrongness this work removes.
    // Reads a type from the token stream, for the pre-pass. Only the
    // shapes the pre-pass can read without the full type parser (which
    // would report errors for shapes it does not model): the scalars, a
    // known struct or enum name, and a fn type.
    bool readTypeFromTokens(const std::vector<Token>& toks, size_t& i, Type& out) {
        if (i >= toks.size()) return false;
        const Token& t = toks[i];
        if (t.type == TokenType::KW_I64) { out = Type::i64(); i++; return true; }
        if (t.type == TokenType::KW_F64) { out = Type::f64(); i++; return true; }
        if (t.type == TokenType::KW_STR) { out = Type::str(); i++; return true; }
        if (t.type == TokenType::KW_BOOL) { out = Type::boolean(); i++; return true; }
        // `ptr` is a keyword token, and leaving it out here made the
        // declaration pre-pass blind to `fn f() -> ptr`: a forward call
        // then inferred i64 for a pointer result. Same class as the tensor
        // drift -- two engines, one shape they disagreed on.
        if (t.type == TokenType::KW_PTR) { out = Type::ptr_(); i++; return true; }
        if (t.type == TokenType::KW_FN) {
            // fn(T1, T2) -> R
            i++;
            if (i >= toks.size() || toks[i].type != TokenType::LPAREN) return false;
            i++;
            std::vector<Type> ps;
            while (i < toks.size() && toks[i].type != TokenType::RPAREN) {
                Type pt;
                if (!readTypeFromTokens(toks, i, pt)) return false;
                ps.push_back(pt);
                if (i < toks.size() && toks[i].type == TokenType::COMMA) i++;
            }
            if (i >= toks.size()) return false;
            i++;                                   // ')'
            if (i >= toks.size() || toks[i].type != TokenType::ARROW) return false;
            i++;
            Type rt;
            if (!readTypeFromTokens(toks, i, rt)) return false;
            out = Type::fn_(ps, rt);
            return true;
        }
        if (t.type == TokenType::IDENTIFIER) {
            if (structRegistry.count(t.lexeme)) { out = Type::struct_(t.lexeme); i++; return true; }
            if (enumRegistry.count(t.lexeme)) { out = Type::enum_(t.lexeme); i++; return true; }
        }
        return false;
    }

    void prescanSignatures() {
        // The lexer is LAZY (it tokenizes on demand), so the whole file is
        // tokenized first -- otherwise the pre-pass would see an empty
        // stream and find nothing.
        while (lexer.ensure(lexer.getTokens().size()).type != TokenType::END_OF_FILE) {}
        const auto& toks = lexer.getTokens();
        size_t i = 0;
        while (i < toks.size()) {
            const Token& t = toks[i];
            bool isFn = (t.type == TokenType::KW_FN || t.type == TokenType::KW_DEF);
            if (!isFn) { i++; continue; }
            // name
            if (i + 1 >= toks.size() || toks[i + 1].type != TokenType::IDENTIFIER) { i++; continue; }
            std::string name = toks[i + 1].lexeme;
            // scan the parameter list for the "->" that precedes the body
            size_t j = i + 2;
            int depth = 0;
            while (j < toks.size()) {
                TokenType tt = toks[j].type;
                if (tt == TokenType::LPAREN) depth++;
                else if (tt == TokenType::RPAREN) depth--;
                else if (depth == 0 && tt == TokenType::LBRACE) break;      // body: no annotation
                // A bodyless declaration ends at the newline. Without this
                // stop the scan for a VOID declaration (`fn f(t: ptr)`)
                // ran into the NEXT declaration and recorded its return
                // type under f's name -- and consumed that declaration, so
                // its own signature was never recorded (measured: the last
                // `-> ptr` extern in examples/ai_arch_bench.fl, whose call
                // then inferred i64).
                else if (depth == 0 && (tt == TokenType::NEWLINE ||
                                        tt == TokenType::SEMICOLON)) break;
                else if (depth == 0 && tt == TokenType::ARROW) {
                    // the return type, read from the tokens (no parseType:
                    // it would report errors for shapes this pass does not
                    // model)
                    size_t k = j + 1;
                    Type r;
                    if (readTypeFromTokens(toks, k, r)) fnReturnTypes[name] = r;
                    break;
                }
                j++;
            }
            i = j;
        }
    }

    std::unique_ptr<ProgramAST> parseProgram() {
        auto prog = std::make_unique<ProgramAST>();
        // Phase 2 / 2A: the signatures first, so a forward reference
        // resolves (see prescanSignatures).
        prescanSignatures();
        while (!isAtEnd()) {
            if (check(TokenType::KW_EXTERN)) {
                parseExternBlock(*prog);
            } else if (check(TokenType::IDENTIFIER) && peek().lexeme == "trait"
                       && peek(1).type == TokenType::IDENTIFIER
                       && peek(2).type == TokenType::LBRACE) {
                // `trait`, like `as`, is DETECTED CONTEXTUALLY, never by a
                // lexer keyword. The trigger requires the full declaration
                // shape (trait Name {), so a variable named `trait` at top
                // level still parses as a variable, and keywordless
                // `trait(x) {}` functions stay legal.
                parseTraitDef(*prog);
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
                {
                    std::vector<std::pair<std::string, Type>> ftys;
                    for (auto& f : sd.fields) ftys.emplace_back(f.name, f.type);
                    gStructFieldTypes[sd.name] = std::move(ftys);
                }
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
                    parseError("E0054", "expected module path (string literal or identifier)");
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
                    // M6: `name (something not a fn shape)` used to be parsed
                    // silently as an expression statement (garbage accepted,
                    // rc=0). One clean error, one line, no cascade.
                    parseError("E0050", "expected a top-level statement or a 'fn name(params) { body }' definition; '(name) (' here does not look like either");
                    skipToLineEndTokens();
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
                    // M6: `name name` without a body shape used to be silently
                    // accepted (rc=0, nothing happened). One clean error.
                    parseError("E0051", "expected a top-level statement or a 'fn name(params) { body }' definition; 'name name' here does not look like either");
                    skipToLineEndTokens();
                }
            } else {
                // Top-level statement (expression, call, etc.) — treat as script
                auto stmt = parseExprStmt();
                if (stmt) {
                    prog->topStmts.push_back(std::move(stmt));
                    declaredVars.clear();
                } else {
                    parseError("E0132", "unrecognized top-level construct");
                    advance();
                }
            }
        }
        // A2b: hoist `parallel for` workers desugared during the parse.
        for (auto& w : pendingFns) prog->functions.push_back(std::move(w));
        pendingFns.clear();
        return prog;
    }

    // M6: after a one-off error, skip the rest of the line so a single bad
    // construct yields one error instead of one error per leftover token.
    void skipToLineEndTokens() {
        while (!isAtEnd() && !check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON))
            advance();
        match(TokenType::NEWLINE);
        match(TokenType::SEMICOLON);
    }

    // Try to parse a function definition without a leading keyword (fn/def).
    // Pattern: name [params] -> retType? { body }
    // Only called when we're confident it's a function def.
    std::unique_ptr<FunctionAST> parseFunctionDefNoKeyword() {
        Token nameTok = consume(TokenType::IDENTIFIER, "expected function name");
        std::vector<std::pair<std::string, Type>> params;
        std::vector<std::unique_ptr<ExprAST>> defaults;
        std::vector<size_t> untyped;
        if (match(TokenType::LPAREN)) {
            if (!check(TokenType::RPAREN)) {
                do {
                    Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                    Type pType = Type::i64();
                    bool hadType = match(TokenType::COLON);
                    if (hadType) pType = parseType();
                    if (match(TokenType::ASSIGN)) {
                        defaults.push_back(parseExpression());
                    } else {
                        defaults.push_back(nullptr);
                    }
                    params.emplace_back(pName.lexeme, pType);
                    if (!hadType) untyped.push_back(params.size() - 1);
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
        bool retAnnotated = false;
        if (match(TokenType::ARROW)) { retType = parseType(); retAnnotated = true; }
        auto fn = std::make_unique<FunctionAST>();
        fn->name = nameTok.lexeme;
        fn->params = std::move(params);
        fn->untypedParams = std::move(untyped);
        fn->defaults = std::move(defaults);
        fn->returnType = retType;
        fn->returnTypeWasAnnotated = retAnnotated;
        // Phase 2 / 2A: record the signature so a call to THIS function
        // infers its declared return type (the parse-time engine used to
        // answer i64 for every user function).
        fnReturnTypes[fn->name] = retType;
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
                if (Parser::isValueExpr(last->kind)) {
                    // Void-typed last expression -> the function returns void.
                    Type t = inferType(last.get());
                    if (t.kind != TypeKind::Void) {
                        retType = t;
                        fn->returnType = retType;
                    }
                }
            }
        }
        // Implicit last-expr return
        if (fn->body && fn->body->kind == NodeKind::Block && retType.kind != TypeKind::Void) {
            auto* block = static_cast<BlockStmtAST*>(fn->body.get());
            if (!block->stmts.empty()) {
                auto& last = block->stmts.back();
                if (Parser::isValueExpr(last->kind)) {
                    auto retStmt = std::make_unique<ReturnStmtAST>(std::move(last));
                    retStmt->loc = nameTok.loc;
                    block->stmts.back() = std::move(retStmt);
                }
            }
        }
        // The body may have refined an inferred return type; keep the
        // parse-time signature table in step with it.
        fnReturnTypes[fn->name] = fn->returnType;
        recordFnSignature(fn.get());
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
                    // The parse-time engine types a call by its callee's
                    // signature; an extern is a function too.
                    fnReturnTypes[ext.name] = ext.returnType;
                    match(TokenType::SEMICOLON);
                    match(TokenType::NEWLINE);
                    prog.externs.push_back(ext);
                }
                consume(TokenType::RBRACE, "expected '}' to close extern block");
            }
        } else {
            parseError("E0048", "expected '\"C\"' after extern");
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
    // Phase 2 / 2A: the RETURN TYPES of the functions parsed so far, so a
    // call to an already-declared function infers its real type instead of
    // the builtins table's i64 fallback.
    std::unordered_map<std::string, Type> fnReturnTypes;
    // The full signatures of the functions parsed so far (name -> fn type),
    // so a NAMED function used as a value is typed like codegen types it.
    // recordFnSignature is the one writer.
    std::unordered_map<std::string, Type> fnSignatures;
    std::unordered_set<std::string> globalVarNames;
    std::unordered_map<std::string, Type> varTypeMap;
    // Phase 2 / 2A: the types visible at the end of the LAST block parsed
    // (a snapshot, because the scope restore drops them). Used only by the
    // return-type inference, which runs after the body is parsed.
    std::unordered_map<std::string, Type> lastBlockTypes;
    std::unordered_map<std::string, StructDef> structRegistry;
    std::unordered_map<std::string, TraitDef> traitRegistry;
    std::unordered_map<std::string, EnumDef> enumRegistry;
    std::vector<std::string> parserTypeParams;
    // A2b: worker functions desugared from `parallel for` (parseParallelForStmt).
    // They cannot live inside the enclosing block (codegen only walks
    // prog->functions), so they wait here until parseProgram drains them.
    std::vector<std::unique_ptr<FunctionAST>> pendingFns;
    Codegen* cg = nullptr;
    bool emitMode = false;
    // P0-NEW-3 (emit path): set when parseNudEmit just produced a tensor
    // literal value; parseVarDeclEmit consumes it to mark the binding.
    // Narrow provenance: composition results (f() + [1.5]) may keep a stale
    // flag — such programs are already built on garbage values.
    bool lastEmitValueTensor = false;
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

    // A parse error. The FIRST one wins (the one-error-per-input
    // contract: a cascade of parser errors is noise), and it is a
    // structured diagnostic like every other: the code, the span of
    // the offending token, and the rendered form.
    void parseError(const char* code, const std::string& message) {
        if (!error) {
            error = true;
            auto& t = peek();
            Diagnostic d;
            d.code = code ? code : "";
            d.level = "error";
            d.message = message;
            DiagLabel l;
            l.primary = true;
            l.span.fileId = fileId;
            l.span.start.line = t.loc.line;
            l.span.start.col = t.loc.col;
            // the token's end: its length on its own line
            l.span.end.line = t.loc.line;
            l.span.end.col = t.loc.col + (int)(t.lexeme.empty() ? 1 : t.lexeme.size());
            d.labels.push_back(l);
            // ONE printer: emitDiagnostic renders the human form (or
            // streams the JSON object). `msg` keeps the one-line summary
            // for the import chain and for the "did it fail" test.
            emitDiagnostic(d);
            msg = "error";
            if (!d.code.empty()) msg += "[" + d.code + "]";
            msg += ": " + message;
        }
    }

    // Consumes a token, or reports and RECOVERS.
    //
    // Recovery means ADVANCING. The previous version reported the error and
    // returned a default Token without moving the position, so any parse loop
    // driven by `while (!check(X) && !isAtEnd())` could spin forever: nothing
    // in the loop body changed the position, so neither did the loop's exit
    // condition. A malformed `match` arm on a multi-payload variant did exactly
    // that and the compiler hung with no message, no exit code, and no CPU
    // spike -- the worst failure mode a compiler has.
    //
    // Every recover-and-continue parser must make progress. Returning the token
    // actually at the cursor (rather than a default-constructed one) keeps the
    // caller's `lexeme` meaningful in diagnostics.
    Token consume(TokenType t, const std::string& err) {
        if (check(t)) return advance();
        parseError("E0138", err);
        if (isAtEnd()) return Token{};
        return advance();
    }

    // ---- type ----
    Type parseType() {
        if (match(TokenType::KW_I64))  return Type::i64();
        if (match(TokenType::KW_F64))  return Type::f64();
        if (match(TokenType::KW_STR))  return Type::str();
        if (match(TokenType::KW_BOOL)) return Type::boolean();
        if (match(TokenType::KW_PTR))  return Type::ptr_();
        if (check(TokenType::KW_FN) && peek(1).type == TokenType::LPAREN) {
            // First-class function type: `fn(T1, T2) -> Ret`. The declared
            // signature is stored verbatim: an indirect call through such a
            // value needs the exact parameter and return types at runtime.
            advance(); // 'fn'
            advance(); // '('
            std::vector<Type> ps;
            if (!check(TokenType::RPAREN)) {
                do { ps.push_back(parseType()); } while (match(TokenType::COMMA));
            }
            consume(TokenType::RPAREN, "expected ')' after fn type parameters");
            if (!match(TokenType::ARROW)) {
                parseError("E0006", "a fn type needs an explicit return type (fn(...) -> Ret); without it an indirect call cannot know what the callee returns");
                return Type::i64();
            }
            Type ret = parseType();
            return Type::fn_(std::move(ps), ret);
        }
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
            parseError("E0129", "unknown type '" + name + "'");
            return Type::i64();
        }
        parseError("E0055", "expected type (i64, str, bool, [T], &T, struct name, or enum name)");
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
            case NodeKind::Cast: return static_cast<CastExprAST*>(expr)->target;
            // A tensor handle is an OPAQUE POINTER, exactly like the value
            // flt_create_1d/2d return: the compiler registers those builtins
            // with an LLVM pointer return, and inferCallType reads that
            // registry, so a call is typed str. A literal must be typed the
            // same way or the two spellings of one value disagree -- the
            // type audit measured 21 declarations of exactly that drift
            // (parse=i64, codegen=str) before this line changed.
            case NodeKind::Tensor: return Type::str();
            // A slice (`arr[1:4]`) is `flint_array_slice`, which returns a
            // FlintVec* -- an OPAQUE POINTER, typed `str` by the codegen
            // rule for pointer initializers. Without this case the parse
            // engine answered i64 and the type audit reported drift on
            // examples/slice_demo.fl (`sliced parse=i64 codegen=str`).
            case NodeKind::Slice: return Type::str();
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(expr);
                auto it = varTypeMap.find(v->name);
                if (it != varTypeMap.end()) return it->second;
                // The block's own scope may already have been closed (the
                // return-type inference runs after the body was parsed).
                auto bt = lastBlockTypes.find(v->name);
                if (bt != lastBlockTypes.end()) return bt->second;
                // A NAMED function used as a value (`h = inc`). Codegen
                // materialises a thunk with exactly this signature; without
                // the lookup the parse engine answered i64 and the audit
                // measured `h parse=i64 codegen=fn` in every fn-value test.
                auto fs = fnSignatures.find(v->name);
                if (fs != fnSignatures.end()) return fs->second;
                return Type::i64();
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(expr);
                Type lt = b->lhs ? inferType(b->lhs.get()) : Type::i64();
                Type rt = b->rhs ? inferType(b->rhs.get()) : Type::i64();
                if (b->op == '+' &&
                    (lt.kind == TypeKind::Str || rt.kind == TypeKind::Str))
                    return Type::str();
                // Phase 2 / 2A: arithmetic with a float operand is f64.
                // Without this the parse-time engine said i64 for
                // `denom = 2.0 * k_f + 1.0` while the codegen's engine said
                // f64 -- the last big shape of the drift the type audit
                // measures.
                if (lt.kind == TypeKind::F64 || rt.kind == TypeKind::F64)
                    return Type::f64();
                return Type::i64();
            }
            case NodeKind::Lambda: {
                // A lambda literal has first-class fn type: declared params
                // plus the (parse-inferred) return type of its body.
                auto* l = static_cast<LambdaExprAST*>(expr);
                std::vector<Type> ps;
                ps.reserve(l->params.size());
                for (auto& p : l->params) ps.push_back(p.second);
                return Type::fn_(std::move(ps), l->retType);
            }
            case NodeKind::Unwrap: return Type::i64();
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(expr);
                if (c->callee == "print") return Type::void_();
                // A USER function: its declared return type. The parser
                // records signatures as it goes (fnReturnTypes), so a call
                // to a function declared EARLIER is typed correctly. A
                // forward reference is still unknown here -- single-pass
                // parsing cannot know it -- and falls back to i64.
                auto rit = fnReturnTypes.find(c->callee);
                if (rit != fnReturnTypes.end()) return rit->second;
                return inferCallType(c->callee);
            }
            case NodeKind::FieldAccess: {
                // The field's declared type, from the parsed struct table
                // (the same source the codegen's engine uses).
                auto* f = static_cast<FieldAccessAST*>(expr);
                Type base = f->base ? inferType(f->base.get()) : Type::i64();
                if (base.kind == TypeKind::Struct) {
                    auto sit = gStructFieldTypes.find(base.structName);
                    if (sit != gStructFieldTypes.end()) {
                        for (auto& fd : sit->second)
                            if (fd.first == f->fieldName) return fd.second;
                    }
                }
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
        std::vector<size_t> untyped;
        if (!check(TokenType::RPAREN)) {
            do {
                Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                Type pType = Type::i64();
                bool hadType = match(TokenType::COLON);
                if (hadType) {
                    pType = parseType();
                }
                if (match(TokenType::ASSIGN)) {
                    defaults.push_back(parseExpression());
                } else {
                    defaults.push_back(nullptr);
                }
                params.emplace_back(pName.lexeme, pType);
                if (!hadType) untyped.push_back(params.size() - 1);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after parameters");

        Type retType = Type::void_();
        bool retAnnotated = false;
        if (match(TokenType::ARROW)) {
            retType = parseType();
            retAnnotated = true;
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
        fn->untypedParams = std::move(untyped);
        fn->defaults = std::move(defaults);
        fn->returnType = retType;
        fn->returnTypeWasAnnotated = retAnnotated;
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
                        if (Parser::isValueExpr(last->kind)) {
                            // A void-typed last expression cannot be a return
                            // value: inferring void here would leave the
                            // function declared void while codegen still wraps
                            // the call in a `return`, emitting `ret <void call>`
                            // from a void function. Keep the declared void.
                            Type t = inferType(last.get());
                            if (t.kind != TypeKind::Void) {
                                retType = t;
                                fn->returnType = retType;
                            }
                        }
                    }
                }
                // Phase 2 / 2A: a function whose last statement is an
                // EXPLICIT `return` gets its return type from that
                // expression. Without this the type stayed void and the
                // codegen emitted `ret ptr` from a void function -- invalid
                // IR, so a perfectly valid program
                //
                //     fn user() { return later() }
                //     fn later() -> str { return "hello" }
                //
                // failed to compile with an internal LLVM error.
                if (retType.kind == TypeKind::Void && fn->body &&
                    fn->body->kind == NodeKind::Block) {
                    auto* block = static_cast<BlockStmtAST*>(fn->body.get());
                    for (auto& st : block->stmts) {
                        if (!st || st->kind != NodeKind::Return) continue;
                        auto* r = static_cast<ReturnStmtAST*>(st.get());
                        if (!r->value) break;
                        Type t = inferType(r->value.get());
                        if (t.kind != TypeKind::Void) {
                            retType = t;
                            fn->returnType = retType;
                        }
                        break;
                    }
                }
                // Implicit last-expr return: for non-void functions, wrap the last
                // expression statement in an explicit return.
                if (fn->body && fn->body->kind == NodeKind::Block && retType.kind != TypeKind::Void) {
                    auto* block = static_cast<BlockStmtAST*>(fn->body.get());
                    if (!block->stmts.empty()) {
                        auto& last = block->stmts.back();
                        if (Parser::isValueExpr(last->kind)) {
                            auto retStmt = std::make_unique<ReturnStmtAST>(std::move(last));
                            retStmt->loc = nameTok.loc;
                            block->stmts.back() = std::move(retStmt);
                        }
                    }
                }
            }
        }
        // Phase 2 / 2A: the parse-time engine needs the signature of THIS
        // function for later declarations (`s = make_shape(...)`). The
        // keywordless path recorded it; this path is the one `parseProgram`
        // uses and it did not, so every call to a user function still
        // inferred i64 while codegen knew the real type -- the drift the
        // audit measured.
        fnReturnTypes[fn->name] = fn->returnType;
        recordFnSignature(fn.get());
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
        // Phase 2 / 2A: the types visible at the END of this block, kept
        // as a SNAPSHOT. The scope restore below discards the block's
        // declarations -- but the function's return type is inferred AFTER
        // the body is parsed, so without the snapshot the inference saw only
        // the default i64. That made
        //
        //     fn user() { s = later()
        //                  s }
        //     fn later() -> str { return "hello" }
        //
        // infer `user()` as returning i64 and emit invalid IR ("return type
        // does not match operand type"), refusing a valid program.
        lastBlockTypes = varTypeMap;
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
            // Declare the loop var while parsing the body so `i as f64`
            // (and any `i <ident>` use) sees a variable, not a bare call
            // via the optional-parens rule (which only fires for unknown
            // names). parseBlock saves/restores outer scope, so erase after
            // when we introduced it (shadowing restores the old type).
            bool hadLoop = declaredVars.count(loopVar.lexeme) != 0;
            Type savedTy = Type::i64();
            auto sit = varTypeMap.find(loopVar.lexeme);
            if (sit != varTypeMap.end()) savedTy = sit->second;
            declaredVars.insert(loopVar.lexeme);
            varTypeMap[loopVar.lexeme] = Type::i64();
            auto body = parseBlock();
            if (!hadLoop) {
                declaredVars.erase(loopVar.lexeme);
                varTypeMap.erase(loopVar.lexeme);
            } else {
                varTypeMap[loopVar.lexeme] = savedTy;
            }
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
                parseError("E0107", "range() needs 1 or 2 arguments");
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
            // Same loop-var scope fix as the range path above.
            bool hadCLoop = declaredVars.count(loopVar.lexeme) != 0;
            Type savedCTy = Type::i64();
            auto csit = varTypeMap.find(loopVar.lexeme);
            if (csit != varTypeMap.end()) savedCTy = csit->second;
            declaredVars.insert(loopVar.lexeme);
            varTypeMap[loopVar.lexeme] = Type::i64();
            auto body = parseBlock();
            if (!hadCLoop) {
                declaredVars.erase(loopVar.lexeme);
                varTypeMap.erase(loopVar.lexeme);
            } else {
                varTypeMap[loopVar.lexeme] = savedCTy;
            }
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
                parseError("E0079", "for-in needs an array or str collection");
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
        // B6 migration: `parallel for` is removed rather than fixed. The old
        // desugar ran a worker on pthreads with by-value snapshots, which is
        // a cross-thread use-after-free for non-copyable captures, and the
        // runtime call had no result channel. The token stays for lexing;
        // the syntax is refused here with migration advice.
        parseError("E0004", "'parallel for' was removed: it could not capture variables and had no result channel. Use a plain 'for' loop, or move the work into a Rust runtime function such as flint_parallel_reduce");
        return nullptr;
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
                parseError("E0135", "variable '" + nameTok.lexeme + "' already declared");
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
            parseError("E0135", "variable '" + nameTok.lexeme + "' already declared");
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
            parseError("E0053", "expected expression after '='");
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
                case NodeKind::Variable:
                    // varTypeMap, the closed block's snapshot, and the
                    // named-function table all live in inferType; copying
                    // half of the rule here is how the fn-value drift
                    // happened.
                    varType = inferType(init.get());
                    break;
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
                case NodeKind::Cast:
                    // `y = expr as f64` records f64 so varTypeMap stays
                    // honest (annotations, methodRecvKind, later inference).
                    varType = static_cast<CastExprAST*>(init.get())->target;
                    break;
                // A tensor literal is an opaque pointer, the same type
                // inferCallType gives flt_create_1d/2d (see inferType).
                case NodeKind::Tensor:
                    varType = Type::str();
                    break;
                case NodeKind::Call: {
                    auto* cInit = static_cast<CallExprAST*>(init.get());
                    // Same table inferType uses, so a global bound to a builtin
                    // call is typed identically to the value codegen produces.
                    varType = inferCallType(cInit->callee);
                    break;
                }
                // Phase 2 / 2A: every other initializer shape goes through
                // the general inferencer instead of keeping the default i64.
                //
                // The switch above handled eight shapes; everything else --
                // an interpolated string (a `+` chain), a field access, an
                // index, a call to a USER function -- silently became i64
                // here, while the codegen's engine inferred the real type.
                // That is the drift the type audit measures (135 declarations
                // corpus-wide), and it is why the parse-time type could not
                // be trusted for anything.
                default: {
                    if (init) varType = inferType(init.get());
                    break;
                }
            }
        }
        varTypeMap[nameTok.lexeme] = varType;
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);

        auto decl = std::make_unique<VarDeclAST>(nameTok.lexeme, isMutable, varType, std::move(init));
        decl->typeWasAnnotated = hasTypeAnnotation;
        // 2A: the declaration site is the variable NAME, so a diagnostic can
        // point at it (it used to default to line 1).
        decl->loc = nameTok.loc;
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
                    for (auto& bn : arm.bindNames) ab.insert(bn);
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
            case NodeKind::Cast:
                collectLambdaUses(static_cast<CastExprAST*>(e)->expr.get(), bound, uses, err);
                break;
            // TensorLiteralAST holds only doubles (no sub-expressions).
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
                            parseError("E0097", "method '" + fName.lexeme + "' needs " +
                                std::to_string(methodArity - 1) + " argument(s)");
                        else if (recvKind == 1)
                            parseError("E0126", "unknown method '" + fName.lexeme +
                                "' for str (available: len, upper, lower, trim, reverse, get, repeat, startswith, endswith, indexof, replace)");
                        else
                            parseError("E0125", "unknown method '" + fName.lexeme +
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
            } else if (check(TokenType::IDENTIFIER) && peek().lexeme == "as") {
                // Phase-2 `as` cast (contextual: `as` stays an IDENTIFIER, so
                // `as` as a variable/field name never triggers this — only
                // `expr as <type>` does). Binds tighter than any binary op.
                bool notYet = false;
                int tk = castTargetKind(peek(1).type, peek(1).lexeme, &notYet);
                if (tk == 0) break;
                Token asTok = advance(); // 'as'
                Token tyTok = advance(); // target
                if (tk < 0) {
                    if (notYet)
                        parseError("E0014", "as-cast to '" + tyTok.lexeme + "' is not yet supported (only 'i64' and 'f64'; f64->i64 truncates toward zero)");
                    else
                        parseError("E0015", "as-casts support only 'i64' and 'f64' (got '" + tyTok.lexeme + "')");
                    return nullptr;
                }
                auto c = std::make_unique<CastExprAST>(
                    std::move(lhs), tk == 2 ? Type::f64() : Type::i64());
                c->loc = asTok.loc;
                lhs = std::move(c);
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
                parseError("E0136", "variable '" + name + "' not declared");
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
                parseError("E0136", "variable '" + name + "' not declared");
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
                parseError("E0085", "index assignment requires an array variable");
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
                    parseError("E0109", "right side of '|>' must be a function call or identifier");
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
                    if (!v) { parseError("E0056", "expected value in map literal"); break; }
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
                    // Phase-2: dtype-suffixed numbers are loud, never silent.
                    if (check(TokenType::IDENTIFIER) && isTensorDtypeSuffix(peek().lexeme)) {
                        parseError("E0100", "numeric dtype suffix '" + peek().lexeme + "' is not yet supported (tensor literals are f64-only; convert with `as f64` / `as i64`)");
                        return nullptr;
                    }
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RBRACKET, "expected ']' after array literal");
            // Phase-2 tensor literal: f64 rank<=2 desugars to flint_create_*.
            // Poison rule: any float element or any [...] row claims the
            // literal — it becomes a tensor or a LOUD error, never a silent
            // garbage array (float arrays never worked: C++ stored double
            // bits as i64, self-host errored). Pure-int/var/str/call
            // literals keep the exact old array path ([1,2,3] untouched).
            bool hasSpread = false;
            for (bool f : spreadFlags) if (f) hasSpread = true;
            if (!hasSpread && !elems.empty()) {
                struct RowInfo { std::vector<double> vals; bool anyFloat = false; };
                bool poisoned = false;
                bool flatNums = true;
                bool allRows = true;
                std::vector<double> flatVals;
                bool flatFloat = false;
                std::vector<RowInfo> rows;
                bool shapeErr = false;
                std::string shapeMsg;
                for (auto& e : elems) {
                    double d = 0.0;
                    bool f = false;
                    if (tensorElemDouble(e.get(), d, f)) {
                        flatVals.push_back(d);
                        flatFloat = flatFloat || f;
                        allRows = false;
                        if (f) poisoned = true;
                        continue;
                    }
                    flatNums = false;
                    if (e->kind == NodeKind::Tensor) {
                        // Inner [...] already converted: a rank-1 tensor
                        // node IS a row (it has >=1 float by construction).
                        poisoned = true;
                        auto* tn = static_cast<TensorLiteralAST*>(e.get());
                        if (tn->rank != 1) {
                            parseError("E0113", "tensor literals support only rank 1 and 2 (f64)");
                            return nullptr;
                        }
                        RowInfo ri;
                        ri.vals = tn->values;
                        ri.anyFloat = true;
                        rows.push_back(std::move(ri));
                        continue;
                    }
                    if (e->kind == NodeKind::Array) {
                        poisoned = true;
                        auto* row = static_cast<ArrayExprAST*>(e.get());
                        bool rspread = false;
                        for (bool sf : row->spreadFlags) if (sf) rspread = true;
                        RowInfo ri;
                        bool ok = !rspread && !row->elements.empty();
                        if (ok) {
                            for (auto& re : row->elements) {
                                double rd = 0.0;
                                bool rf = false;
                                if (tensorElemDouble(re.get(), rd, rf)) {
                                    ri.vals.push_back(rd);
                                    ri.anyFloat = ri.anyFloat || rf;
                                } else if (re->kind == NodeKind::Array) {
                                    shapeErr = true;
                                    shapeMsg = "tensor literals support only rank 1 and 2 (f64)";
                                    ok = false;
                                    break;
                                } else {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if (ok) {
                            rows.push_back(std::move(ri));
                            continue;
                        }
                        if (!shapeErr) {
                            shapeErr = true;
                            shapeMsg = "tensor literal rows must be non-empty all-numeric f64 literal lists";
                        }
                    } else {
                        allRows = false;
                    }
                }
                if (shapeErr) {
                    parseError("E0139", shapeMsg);
                    return nullptr;
                }
                if (poisoned) {
                    if (flatNums) {
                        if (!flatFloat) {
                            parseError("E0112", "tensor literal needs at least one f64 element (add a decimal point); nested i64 arrays are not supported");
                            return nullptr;
                        }
                        auto t = std::make_unique<TensorLiteralAST>();
                        t->rank = 1;
                        t->rows = (int64_t)flatVals.size();
                        t->values = std::move(flatVals);
                        t->loc = previous().loc;
                        return t;
                    }
                    if (allRows && rows.size() == elems.size()) {
                        size_t cols = rows[0].vals.size();
                        bool ragged = false;
                        bool anyF = false;
                        for (auto& r : rows) {
                            if (r.vals.size() != cols) ragged = true;
                            anyF = anyF || r.anyFloat;
                        }
                        if (ragged) {
                            parseError("E0106", "ragged tensor literal: all rows must have the same length");
                            return nullptr;
                        }
                        if (!anyF) {
                            parseError("E0112", "tensor literal needs at least one f64 element (add a decimal point); nested i64 arrays are not supported");
                            return nullptr;
                        }
                        auto t = std::make_unique<TensorLiteralAST>();
                        t->rank = 2;
                        t->rows = (int64_t)rows.size();
                        t->cols = (int64_t)cols;
                        for (auto& r : rows)
                            for (double v : r.vals) t->values.push_back(v);
                        t->loc = previous().loc;
                        return t;
                    }
                    parseError("E0116", "tensor-shaped array literal is neither a flat f64 list nor equal-length f64 rows (use bare f64 literals, or build dynamic tensors with flint_create_1d/2d + flt_set)");
                    return nullptr;
                }
            }
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
            // M6: `|p| -> T { ... }` is not a lambda form (lambdas have no
            // return annotation). Previously the body parse silently
            // accepted it and codegen later said "empty lambda body". One
            // syntax error, then skip to line end so the cascade stops.
            if (check(TokenType::ARROW)) {
                parseError("E0007", "a lambda has no '-> Ret' form: write an expression body (|x| x + 1); use a named fn when you need a return annotation");
                skipToLineEndTokens();
                return nullptr;
            }
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
                if (!walkErr.empty()) { parseError("E0140", walkErr); return nullptr; }
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
                    if (!check(TokenType::RPAREN)) {
                        do {
                            arm.bindNames.push_back(
                                consume(TokenType::IDENTIFIER, "expected binding name").lexeme);
                        } while (match(TokenType::COMMA));
                    }
                    consume(TokenType::RPAREN, "expected ')' after pattern bindings");
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
            // M2b: explicit type arguments, `f::<T1, T2>(...)`.
            //
            // `::` is two COLON tokens to the lexer, so no lexer change is
            // needed and the token contract (lexdiff) stays intact. Nothing
            // in the language used `::` before, so the form is unambiguous.
            std::vector<Type> explicitTypeArgs;
            if (check(TokenType::COLON) && peek(1).type == TokenType::COLON) {
                advance(); // ':'
                advance(); // ':'
                if (!match(TokenType::LT)) {
                    parseError("E0047", "expected '<' after '::' before explicit type arguments");
                    return nullptr;
                }
                do {
                    explicitTypeArgs.push_back(parseType());
                } while (match(TokenType::COMMA));
                consume(TokenType::GT, "expected '>' after explicit type arguments");
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
                c->typeArgs = std::move(explicitTypeArgs);
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
    llvm::Value* emitTensorLiteralEmit(TensorLiteralAST& t);
    // Phase-2 emit-mode tensor scan: `[` already consumed. 1=tensor (*t
    // filled, consumed through `]`), 0=not tensor-shaped (consumed nothing
    // past `[`, caller rewinds to the array path), -1=hard shape error
    // (reported, consumed through `]`).
    int tryParseTensorEmit(TensorLiteralAST& t);
    // One numeric element for the scan above (redundant parens + leading
    // minus allowed). 1=numeric, 0=not, -1=dtype suffix (reported).
    int parseTensorNumEmit(double& out, bool& isF);
    llvm::Value* emitStructLiteralEmit(const std::string& name);
    llvm::Value* emitEnumConstructEmit(const std::string& enumName, const std::string& variantName);
    llvm::Value* emitCallEmit(const std::string& callee,
                              const std::vector<Type>& explicitTypeArgs = {});
    llvm::Value* emitDirectCall(const std::string& callee, std::vector<llvm::Value*>& args,
                                 const std::vector<Type>& explicitTypeArgs = {});
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

// One observed call-site type for an unannotated parameter.
// One observed call-site type for an unannotated parameter.
//
// A pointer argument is strictly more informative than an integer one: a
// pointer can only be passed to a pointer parameter, while `5` fits an i64
// param *and* is what an unannotated numeric param defaults to. So pointer
// evidence outranks integer evidence instead of the two merely conflicting --
// treating them as equal left `fn lex_into(src, kinds)` at i64 whenever one
// caller passed a real handle and another passed a number, and every handle
// call then contradicted the callee's signature.
struct ArgSlot {
    Type t;              // best type seen so far
    bool sawPtr = false; // a pointer was passed here
    bool sawOther = false; // a non-pointer, non-current type was passed
    bool any = false;
};

static std::string llvmTypeToString(llvm::Type* t) {
    if (!t) return "<none>";
    if (t->isDoubleTy()) return "f64";
    if (t->isIntegerTy(1)) return "bool";
    if (t->isIntegerTy(8)) return "i8";
    if (t->isIntegerTy(64)) return "i64";
    if (t->isPointerTy()) return "pointer";
    if (t->isArrayTy()) return "array";
    if (t->isStructTy()) {
        auto* st = llvm::cast<llvm::StructType>(t);
        if (st->hasName()) return st->getName().str();
        if (st->getNumElements() == 2 && st->getElementType(0)->isPointerTy() &&
            st->getElementType(1)->isIntegerTy(64)) {
            return "slice";
        }
        return "struct";
    }
    if (t->isVoidTy()) return "void";
    std::string s;
    llvm::raw_string_ostream os(s);
    t->print(os);
    os.flush();
    return s;
}

class Codegen {
public:
    // --target <triple>: non-host modules are EMIT-only outputs (.ll text or
    // .o object). The datalayout and triple then describe the requested
    // machine instead of the compiled-in Android host triple, so cross
    // targets produce truthful metadata rather than a lying triple line.
    // --run stays host-only because the ORC JIT is built for the native
    // machine; main() refuses the combination loudly.
    Codegen(const std::string& tripleOverride = "") :
                ctx(std::make_unique<llvm::LLVMContext>()),
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
        if (!tripleOverride.empty()) {
            initTargets();
            std::string err;
            auto* tgt = llvm::TargetRegistry::lookupTarget(tripleOverride, err);
            if (!tgt) {
                std::cerr << "error: --target '" << tripleOverride << "': " << err << "\n";
                std::exit(1);
            }
            auto* tm = tgt->createTargetMachine(llvm::Triple(tripleOverride), "generic", "", {}, llvm::Reloc::PIC_,
                std::nullopt);
            if (!tm) { std::cerr << "error: no TargetMachine for '" << tripleOverride << "'\n"; std::exit(1); }
            mod->setTargetTriple(llvm::Triple(tripleOverride));
            mod->setDataLayout(tm->createDataLayout());
            delete tm;
        }
        // Phase 2 / 2A: register the builtins NOW, so the registry of
        // builtin TYPES exists before the parser runs and the parser's
        // inference reads the compiler's own table. (initBuiltins is
        // idempotent; generateDeclarations calls it again, harmlessly.)
        initBuiltins();
    }

    void setParser(Parser* p) { parser = p; }
    bool usedPython() const { return hasPython; }

    // --debug: real DWARF line tables. DIBuilder CU + file + one
    // DISubprogram per user function + per-statement DebugLoc seen by each
    // IRBuilder insert. TF functions stay body-bearing, so a debugger can
    // step Flint source lines. Verified by llvm-dwarfdump (on device), not
    // asserted.
    std::unique_ptr<llvm::DIBuilder> dib;
    llvm::DICompileUnit* cu = nullptr;
    llvm::DISubprogram* curDISub = nullptr;
    bool debugInfo = false;

    void initDebugSource(const std::string& srcPath) {
        debugInfo = true;
        std::string dir, file;
        auto slash = srcPath.find_last_of('/');
        if (slash == std::string::npos) {
            dir = ".";
            file = srcPath;
        } else {
            dir = srcPath.substr(0, slash);
            file = srcPath.substr(slash + 1);
        }
        if (dir.empty()) dir = ".";
        dib = std::make_unique<llvm::DIBuilder>(*mod, true /* AllowUnresolvedNodes */);
        // Without this flag the bitcode reader (cache round-trip) and the
        // object emitter both drop debug info outright with
        // "invalid version (0)".
        mod->addModuleFlag(llvm::Module::ModFlagBehavior::Error,
                           "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        cu = dib->createCompileUnit(
            llvm::dwarf::DW_LANG_C11,
            dib->createFile(file, dir),
            "flint",
            false /* isOptimized */,
            "",
            0);
    }

    void finishDebug() {
        if (!dib) return;
        dib->finalize();
        dib.reset();
    }

    // P0-3 error gate (re-added after 2026-09-14 wipe): every codegen
    // diagnostic routes through here so a failed compile exits nonzero and
    // partial IR is never JIT-run/emitted/linked. emitFunction returns false
    // when the flag is set (checked at its end).
    bool cgHadError = false;
    // Phase 2 / 2A: the builtins are registered once; the TYPE registry
    // they fill must exist before parsing.
    bool builtinsInitialized = false;
    // A short, stable name for a type, for the audit report.
    static std::string typeTag(const Type& t) {
        switch (t.kind) {
            case TypeKind::I64: return "i64";
            case TypeKind::F64: return "f64";
            case TypeKind::Str: return "str";
            case TypeKind::Bool: return "bool";
            case TypeKind::Void: return "void";
            case TypeKind::Ptr: return "ptr";
            case TypeKind::Array: return "array";
            case TypeKind::Map: return "map";
            case TypeKind::Struct: return "struct:" + t.structName;
            case TypeKind::Enum: return "enum:" + t.structName;
            case TypeKind::TypeParam: return "typeparam:" + t.structName;
            case TypeKind::Func: return "fn";
            default: return "?";
        }
    }

    // Phase 2 / 2A.0 -- the TYPE AUDIT. Flint has two inference engines:
    // the parser's (inferType) and the codegen's (exprFlintType and the
    // declaration ladder). They can disagree, and nothing notices. This
    // compares them at every declaration:
    //   * an ANNOTATED declaration is a contract -- a disagreement is a
    //     BUG (the program does not have the type it claims);
    //   * an INFERRED declaration is a guess -- a disagreement is DRIFT,
    //     which is what the type-checker work (step 2A) must remove.
    struct TypeDrift {
        std::string name;
        int line = 0;
        std::string parseType;
        std::string codegenType;
        bool annotated = false;
    };
    std::vector<TypeDrift> typeDrift;
    // Phase 2 / 2A.1: the facts resolved at CALL sites. Unlike a declaration
    // (where the two engines are compared), a call site has one truth: the
    // instantiation/implementation the compiler chose. Recording it here is
    // what makes --type-audit able to SHOW the choice.
    struct CallFact {
        std::string callee;
        int line = 0;
        std::vector<Type> typeArgs;
        std::string impl;
    };
    std::vector<CallFact> callFacts;
    void recordCallFact(CallExprAST* call, const std::vector<Type>& typeArgs,
                        const std::string& impl) {
        CallFact f;
        f.callee = call->callee;
        f.line = call->loc.line;
        f.typeArgs = typeArgs;
        f.impl = impl;
        callFacts.push_back(std::move(f));
    }
    bool auditTypes = false;   // set from the --type-audit flag
    // The source location of the statement/expression being emitted, so a
    // codegen diagnostic can say WHERE it happened. Parse errors have always
    // carried line/column; codegen errors did not, which left the user
    // hunting for the offending construct.
    int cgCurLine = 0;
    int cgCurCol = 0;
    // The file this codegen emits, so a diagnostic can point into it.
    int fileId = 0;

    // A codegen diagnostic: the same structure as a parse error (the
    // code, the span, the rendered form), so the terminal, the LSP and
    // the docs all read one thing.
    llvm::Value* codegenError(const char* code, const std::string& msg,
                              const std::string& suggestion = "") {
        Diagnostic d;
        d.code = code ? code : "";
        d.level = "error";
        d.message = msg;
        if (!suggestion.empty())
            // rustc's rule: a suggestion is a statement, not a question
            // ("there is a variable with a similar name: `x`"), because
            // the server cannot know what the user meant.
            d.help = "there is a name with a similar spelling: `" + suggestion + "`";
        if (cgCurLine > 0) {
            DiagLabel l;
            l.primary = true;
            l.span.fileId = fileId;
            l.span.start.line = cgCurLine;
            l.span.start.col = cgCurCol;
            // a single character: the position of the construct
            l.span.end.line = cgCurLine;
            l.span.end.col = cgCurCol + 1;
            d.labels.push_back(l);
        }
        emitDiagnostic(d);
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

        PROFILE_BEGIN("codegen_infer_params");
        inferUntypedParams(prog);
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_functions");
        declareFunctions(prog);
        PROFILE_END();

        // Populate functionDefs for default param lookup
        for (auto& fn : prog.functions) {
            functionDefs[fn->name] = fn.get();
            functionOverloads[fn->name].push_back(fn.get());
        }
        for (auto& td : prog.traits) traitRegistry[td.name] = td;
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
    std::string ufcsTypeName(const Type& t) {
        if (t.kind == TypeKind::Struct || t.kind == TypeKind::Enum)
            return t.structName;
        switch (t.kind) {
            case TypeKind::I64: return "i64";
            case TypeKind::F64: return "f64";
            case TypeKind::Str: return "str";
            case TypeKind::Bool: return "bool";
            case TypeKind::Map: return "map";
            case TypeKind::Array: return "array";
            case TypeKind::Ptr: return "ptr";
            default: return "?";
        }
    }

    // Receiver type for method resolution in the AST path: variables (locals
    // + globals), non-generic call results, and struct field accesses.
    // Everything else is resolved by arity alone.
    bool ufcsReceiverType(ExprAST* e, Type& out) {
        if (!e) return false;
        if (e->kind == NodeKind::Variable) {
            auto* v = static_cast<VariableExprAST*>(e);
            if (auto* sym = symTable.lookup(v->name)) { out = sym->type; return true; }
            auto git = globalSymTable.find(v->name);
            if (git != globalSymTable.end()) { out = git->second.type; return true; }
            return false;
        }
        if (e->kind == NodeKind::Call) {
            auto* c = static_cast<CallExprAST*>(e);
            auto fit = functionDefs.find(c->callee);
            if (fit != functionDefs.end()) {
                out = resolveType(fit->second->returnType);
                return out.kind != TypeKind::Void;
            }
        }
        if (e->kind == NodeKind::Number) {
            // Literals are scalar-typed on both paths, so method resolution
            // on them cannot diverge between the AST and emit paths.
            auto* n = static_cast<NumberExprAST*>(e);
            out = n->isFloat && !n->isInt ? Type::f64() : Type::i64();
            return true;
        }
        if (e->kind == NodeKind::FieldAccess) {
            // `a.b` — walk the container: the var/call result type must be a
            // struct/enum with a field of that name. Without this, every
            // `bx.inner.f()` receiver had an unknown type and name+arity
            // alone could pick the wrong overload.
            auto* fa = static_cast<FieldAccessAST*>(e);
            if (!fa || !fa->base) return false;
            Type base;
            if (!ufcsReceiverType(fa->base.get(), base)) return false;
            if (base.kind != TypeKind::Struct && base.kind != TypeKind::Enum) return false;
            auto sit = structRegistry.find(base.structName);
            if (sit == structRegistry.end()) return false;
            for (auto& fld : sit->second.fields)
                if (fld.name == fa->fieldName) { out = resolveType(fld.type); return true; }
            return false;
        }
        return false;
    }

    static bool anyTraitDeclares(const std::unordered_map<std::string, TraitDef>& reg,
                                 const std::string& mName) {
        for (auto& td : reg) {
            const TraitDef& t = td.second;
            for (auto& md : t.methods)
                if (md.name == mName) return true;
        }
        return false;
    }

    // M3/B5 dispatch: name+arity resolution for `x.f(args...)` (the receiver
    // is args[0]). Impls are ordinary functions. Exactly one match calls;
    // zero says which trait was unsatisfied; several matches name the types.
    // Caller gets the FunctionAST* or null + a diag string on null.
    FunctionAST* resolveUfcsImpl(const std::string& mName, size_t arity,
                                 const Type& recv, bool recvKnown,
                                 std::string& diag) {
        auto oit = functionOverloads.find(mName);
        bool traitDeclared = anyTraitDeclares(traitRegistry, mName);
        if (oit == functionOverloads.end()) {
            if (traitDeclared)
                diag = "no type in scope implements '" + mName + "'";
            else
                diag = "unknown method '" + mName + "' (receiver type unknown or method missing)";
            return nullptr;
        }
        std::vector<FunctionAST*> matches;
        for (auto* f : oit->second) {
            if (f->params.size() != arity) continue;
            if (recvKnown && arity > 0) {
                Type pt = resolveType(f->params[0].second);
                if (!(pt == recv)) continue;
            }
            matches.push_back(f);
        }
        if (matches.size() == 1) { diag.clear(); return matches[0]; }
        if (matches.size() > 1) {
            diag = "ambiguous: '" + mName + "' matches multiple implementations (candidates: ";
            for (size_t i = 0; i < matches.size(); i++)
                diag += (i ? ", " : "") + ufcsTypeName(resolveType(matches[i]->params[0].second));
            diag += ")";
            return nullptr;
        }
        if (traitDeclared)
            diag = "no type in scope implements '" + mName + "'";
        else
            diag = "unknown method '" + mName + "' (receiver type unknown or method missing)";
        return nullptr;
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
    // Trait declarations (M3/B5) and every same-name function definition
    // (needed for name+arity method resolution).
    std::unordered_map<std::string, TraitDef> traitRegistry;
    std::unordered_map<std::string, std::vector<FunctionAST*>> functionOverloads;
    std::unordered_map<std::string, StructDef> structRegistry;
    std::unordered_map<std::string, EnumDef> enumRegistry;
    std::unordered_map<std::string, FunctionAST*> genericFunctions;
    bool hasPython = false;
    bool releaseMode = true;  // default: no overflow checks
    // Phase 2 / 2B: the SAFETY POLICY -- one place that decides which
    // checks are inserted.
    //
    // The posture used to be spread over two switches (`releaseMode` for
    // the overflow check, a separate flag for the bounds check), which can
    // disagree -- and a disagreement means either a missing check (unsafe)
    // or a check nobody asked for (slow). One struct, one decision point.
    //
    // The counters are exact and PRE-optimization, so the dashboard can
    // report "checks inserted" and "checks surviving" separately.
    struct SafetyPolicy {
        bool bounds = true;    // array index bounds
        bool overflow = true;  // integer overflow
        bool divZero = true;   // division by zero
        long boundsInserted = 0;
        long overflowInserted = 0;
        long divZeroInserted = 0;
    };
    SafetyPolicy safety;
    // Phase 3 / P1: bounded-range facts (filled before body emission by
    // BoundedRangeAnalysis). A fact proves an i64 result fits, so the
    // checked intrinsic is not needed for that expression.
    std::unordered_map<const ExprAST*, IntRangeFact> rangeFacts;
    bool loopOpt = false;   // C1 --loop-opt: loop-friendly check attrs (off by default)
    std::unordered_map<std::string, Type> typeSubstMap;
    std::unordered_map<std::string, llvm::Function*> genericCache;
    std::unordered_map<std::string, FunctionAST*> functionDefs; // for default param lookup
    std::unordered_map<std::string, Symbol> globalSymTable;
    std::vector<std::unique_ptr<VarDeclAST>>* programGlobals = nullptr;
    Parser* parser = nullptr;
    // B2: vars known to hold flint_time_ns results (>=0 by OS contract).
    // Cleared on any reassign to non-time value. Used only to elide the
    // ssub overflow check for time1-time0 (diff of two [0,MAX] fits).
    std::unordered_set<std::string> timeVars;
    // Phase 2 / 2D: set while emitting an expression in a position that may
    // outlive the defining frame (a return value, a global store, a call
    // argument). A lambda LITERAL created here gets a HEAP env box directly;
    // a closure bound earlier is promoted at the site by promoteLambdaBox().
    bool lambdaEscapesNow = false;
    // Phase 2 / 2D: the Symbol of the last inline lambda materialized by
    // emitExpr. `h = |...|` copies its box metadata onto h, so h can later
    // be promoted like any other closure binding.
    Symbol* lastInlineLambdaSym = nullptr;

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

    // The Flint type behind an LLVM type, for the builtins the compiler
    // registers. Phase 2 / 2A: the parser's inference reads the compiler's
    // OWN registry through this, so the two engines cannot disagree about
    // a builtin's type.
    Type flintTypeOfLlvm(llvm::Type* t) {
        if (!t) return Type::void_();
        if (t == llvm::Type::getInt64Ty(*ctx)) return Type::i64();
        if (t == llvm::Type::getInt32Ty(*ctx)) return Type::i64();
        if (t == llvm::Type::getDoubleTy(*ctx)) return Type::f64();
        if (t->isPointerTy()) return Type::str();
        if (t->isVoidTy()) return Type::void_();
        if (t->isStructTy()) {
            // An aggregate handle: {ptr, i64} is an array.
            auto* st = llvm::cast<llvm::StructType>(t);
            if (st->getNumElements() == 2 &&
                st->getElementType(0)->isPointerTy() &&
                st->getElementType(1)->isIntegerTy(64))
                return Type::array(Type::i64());
        }
        return Type::i64();
    }

    void initBuiltins() {
        // Idempotent: the registry of builtin TYPES must exist before the
        // parser runs (the parser's inference reads it), and the functions
        // themselves are created once.
        if (builtinsInitialized) return;
        builtinsInitialized = true;
        auto add = [&](const std::string& n, llvm::Type* r, std::vector<llvm::Type*> p, bool va) {
            auto f = llvm::Function::Create(llvm::FunctionType::get(r, p, va), llvm::Function::ExternalLinkage, n, mod.get());
            functionMap[n] = f;
            // Phase 2 / 2A: the builtin's FLINT type, derived from the
            // return type AT REGISTRATION. The parser used to answer from a
            // hand-written list that had drifted from this registry (the
            // type audit measured 178 declarations where the two engines
            // disagreed, always "the parser says i64"). Deriving the table
            // here makes that class of drift IMPOSSIBLE: there is one
            // registry, and the parser reads it.
            gBuiltinTypes[n] = flintTypeOfLlvm(r);
        };
        add("flint_println_i64", voidTy, {i64Ty}, false);
        add("flint_println_str", voidTy, {i8PtrTy}, false);
        add("flint_println_f64", voidTy, {llvm::Type::getDoubleTy(*ctx)}, false);
        add("flint_panic", voidTy, {i8PtrTy}, false);
        add("flint_py_init", voidTy, {}, false);
        add("flint_py_run", voidTy, {i8PtrTy}, false);
        add("flint_py_fini", voidTy, {}, false);
        add("flint_py_eval_int", i64Ty, {i8PtrTy}, false);
        // py_eval is integer-only (it calls PyLong_AsLongLong), so Python's
        // `/` -- which returns a float even for ints -- raised TypeError for
        // almost every arithmetic expression. py_eval_float takes a double and
        // accepts an int result too.
        add("flint_py_eval_float", llvm::Type::getDoubleTy(*ctx), {i8PtrTy}, false);
        add("flint_bounds_check", voidTy, {i64Ty, i64Ty}, false);
        // B1: cold-mark the panic/bounds declarations so the optimizer lays
        // panic blocks out of hot blocks. Checked-by-default unchanged.
        markColdPanicFn(functionMap["flint_panic"]);
        markColdCheckFn(functionMap["flint_bounds_check"]);
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
        // Secure line input (runtime/runtime.c). flint_input returns a heap
        // string owned by the caller; status is 0/1/2/3/4/5/6/7 as documented
        // in README "Reading input". Numeric helpers parse the whole line.
        add("flint_input", i8PtrTy, {i8PtrTy}, false);
        add("flint_input_status", i64Ty, {}, false);
        add("flint_input_i64", i64Ty, {i8PtrTy}, false);
        add("flint_input_f64", llvm::Type::getDoubleTy(*ctx), {i8PtrTy}, false);
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
        // C1: loopOpt-gated check attrs (off by default → zero IR change).
        if (loopOpt) {
            markLoopCheckFn(functionMap["flint_bounds_check"]);
            markLoopCheckFn(functionMap["flint_null_check"]);
        }
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
        // Encoders and utilities (runtime/rust/src/encoding.rs).
        add("flint_crc32c", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_crc32c_hex", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_base32_encode", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_base58_encode", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_url_encode", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_url_decode", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_uuid_v4", i64Ty, {}, false);
        add("flint_uuid_v7", i64Ty, {}, false);
        add("flint_enc_last", i8PtrTy, {}, false);
        add("flint_enc_error", i8PtrTy, {}, false);
        // Password hashing (runtime/rust/src/argon2.rs) — Argon2id with
        // OWASP's 19 MiB / 2-iteration defaults. Returns a PHC string, which
        // records the parameters so the cost can be raised later without
        // invalidating stored hashes.
        add("flint_argon2_hash", i64Ty, {i8PtrTy}, false);
        add("flint_argon2_hash_params", i64Ty, {i8PtrTy, i64Ty, i64Ty, i64Ty}, false);
        add("flint_argon2_verify", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_argon2_last", i8PtrTy, {}, false);
        add("flint_argon2_error", i8PtrTy, {}, false);
        add("flint_argon2_default_memory_kib", i64Ty, {}, false);
        // zstd (runtime/rust/src/compress.rs). Takes (pointer, length) so
        // binary payloads with embedded NULs survive.
        add("flint_zstd_decompress", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_zstd_last", i8PtrTy, {}, false);
        add("flint_zstd_len", i64Ty, {}, false);
        add("flint_zstd_capacity", i64Ty, {}, false);
        add("flint_zstd_error", i8PtrTy, {}, false);
        // PNG codec (runtime/rust/src/png.rs) — pure Rust, no libpng dependency.
        // decode takes (pointer, length); encode takes width, height, channels
        // and the interleaved 8-bit pixel buffer. The encoded file is fetched
        // with flint_png_encoded() and its length is encode's return value.
        add("flint_png_decode", i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_png_encode", i64Ty, {i64Ty, i64Ty, i64Ty, i8PtrTy, i64Ty}, false);
        add("flint_png_width", i64Ty, {}, false);
        add("flint_png_height", i64Ty, {}, false);
        add("flint_png_channels", i64Ty, {}, false);
        add("flint_png_pixel", i64Ty, {i64Ty, i64Ty, i64Ty}, false);
        add("flint_png_mean_luma", f64Ty, {}, false);
        add("flint_png_error", i8PtrTy, {}, false);
        // Brotli decode (runtime/rust/src/brotli.rs) -- decompression only, and
        // bounded: a stream that expands past max_output fails instead of
        // filling memory, which a .br asset must never be able to do.
        add("flint_brotli_decompress", i64Ty, {i8PtrTy, i64Ty, i64Ty}, false);
        add("flint_brotli_last", i8PtrTy, {}, false);
        add("flint_brotli_len", i64Ty, {}, false);
        add("flint_brotli_error", i8PtrTy, {}, false);
        add("flint_png_encoded", i8PtrTy, {}, false);
        // System information (runtime/rust/src/system.rs) — portable across
        // Linux, macOS and Windows; returns -1 with a reason where a platform
        // refuses (e.g. /proc is restricted on modern Android).
        add("flint_system_platform", i64Ty, {}, false);
        add("flint_system_supported", i64Ty, {}, false);
        add("flint_system_cpu_count", i64Ty, {}, false);
        add("flint_system_memory_total", i64Ty, {}, false);
        add("flint_system_memory_free", i64Ty, {}, false);
        add("flint_system_uptime", i64Ty, {}, false);
        add("flint_system_os_version", i64Ty, {}, false);
        add("flint_system_temp_dir", i64Ty, {}, false);
        add("flint_system_last", i8PtrTy, {}, false);
        add("flint_system_error", i8PtrTy, {}, false);
        // SQLite (runtime/rust/src/sqlite.rs — a Rust wrapper over the system
        // libsqlite3). Handles are >= 1; query results live in a module-owned
        // arena and stay valid until the next query on any connection.
        add("flint_sqlite_open", i64Ty, {i8PtrTy}, false);
        add("flint_sqlite_close", i64Ty, {i64Ty}, false);
        add("flint_sqlite_exec", i64Ty, {i64Ty, i8PtrTy}, false);
        add("flint_sqlite_query", i64Ty, {i64Ty, i8PtrTy}, false);
        add("flint_sqlite_rows", i64Ty, {}, false);
        add("flint_sqlite_cols", i64Ty, {}, false);
        add("flint_sqlite_get", i8PtrTy, {i64Ty, i64Ty}, false);
        add("flint_sqlite_error", i8PtrTy, {}, false);
        add("flint_sqlite_version", i8PtrTy, {}, false);
        // Cryptography (runtime/rust/src/crypto.rs, RustCrypto primitives).
        // Each call returns the HEX LENGTH of its result; the hex text itself
        // lives in a module-owned static buffer that stays valid until the next
        // crypto call, and is NUL-terminated so it can be used as a Flint str
        // directly. -1 means failure: read flint_crypto_error() for the reason.
        add("flint_sha256_hex", i64Ty, {i8PtrTy}, false);
        add("flint_hmac_sha256_hex", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_aes256_ctr_crypt_hex", i64Ty, {i8PtrTy, i8PtrTy, i8PtrTy}, false);
        add("flint_aes256_gcm_encrypt_hex", i64Ty, {i8PtrTy, i8PtrTy, i8PtrTy, i8PtrTy}, false);
        add("flint_aes256_gcm_decrypt_hex", i64Ty, {i8PtrTy, i8PtrTy, i8PtrTy, i8PtrTy}, false);
        add("flint_crypto_error", i8PtrTy, {}, false);
        add("flint_crypto_out_bytes", i64Ty, {}, false);
        add("flint_crypto_last", i8PtrTy, {}, false);
        // CSV parsing (runtime/rust — Rust stdlib module, see flint_std.o).
        // flint_csv_get returns a string the caller releases with
        // flint_csv_free; allocation and release stay in Rust, so no allocator
        // identity is assumed across the boundary. flint_csv_error returns
        // static storage and must NOT be freed.
        add("flint_csv_parse", i64Ty, {i8PtrTy, i8PtrTy}, false);
        add("flint_csv_rows", i64Ty, {}, false);
        add("flint_csv_cols", i64Ty, {i64Ty}, false);
        add("flint_csv_get", i8PtrTy, {i64Ty, i64Ty}, false);
        add("flint_csv_free", voidTy, {i8PtrTy}, false);
        add("flint_csv_error", i8PtrTy, {}, false);
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
        // (1-arg flint_time_format retired: the 3-arg civil-time version in
        // flint_time.o owns the name; no callers of the old one exist.)
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
        // Status of the last HTTP(S) request, so a caller can distinguish 404
        // from 200 without parsing. Registered with the rest of the net module.
        add("flint_http_status", i64Ty, {}, false);
        add("flint_http_post", i8PtrTy, {i8PtrTy, i8PtrTy, i8PtrTy, i64Ty}, false);
        // ──────────────────────────────────────────────────────
        // AEGIS LEASES (flint_aegis.o) — generational memory safety.
        // Lease ids are i64 handles; tutorial 08 uses these directly.
        // ──────────────────────────────────────────────────────
        add("flint_aegis_alloc", i64Ty, {i64Ty}, false);
        add("flint_aegis_free", i64Ty, {i64Ty}, false);
        add("flint_aegis_read_i64", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_aegis_write_i64", voidTy, {i64Ty, i64Ty, i64Ty}, false);
        // Unchecked AEGIS accessors skip the liveness and bounds checks
        // entirely -- they are a raw dereference, and calling one with a dead
        // lease or an out-of-range index is undefined behaviour, not a panic.
        // They stay reachable only under --unsafe: registering them as
        // ordinary builtins let any Flint program segfault the runtime with
        // `free(a)` then `read_i64_unchecked(a, 999999)`. releaseMode is true
        // only for --unsafe (false = safe mode, the default), and it is
        // assigned before generateDeclarations runs.
        if (!safety.overflow) {
            add("flint_aegis_read_i64_unchecked", i64Ty, {i64Ty, i64Ty}, false);
            add("flint_aegis_write_i64_unchecked", voidTy, {i64Ty, i64Ty, i64Ty}, false);
        }
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

        // ──────────────────────────────────────────────────────
        // AI / tensor builtins
        //
        // These were previously registered against a {i64,i64,ptr} struct with
        // signatures that matched no definition: fa_relu is
        // fa_relu(FArena*, FATensor*) -- two args, not one -- and fa_softmax,
        // fa_dropout, fao_relu, fao_sigmoid, fao_dropout, fao_softmax and
        // fa_default_arena do not exist at all. So all nine were uncallable
        // (the verifier rejects the call) or unlinkable, which is why 2,891
        // lines of AI runtime were unreachable from Flint code.
        //
        // FlintTensor is passed as an opaque pointer: it is a C struct whose
        // layout the compiler does not need to know, and several entry points
        // return it.
        // ──────────────────────────────────────────────────────
        auto tPtr = llvm::PointerType::get(*ctx, 0);

        // Core tensor
        add("flt_create",   tPtr, {i64Ty, llvm::PointerType::get(i64Ty, 0)}, false);
        add("flt_create_1d", tPtr, {i64Ty}, false);
        add("flt_create_2d", tPtr, {i64Ty, i64Ty}, false);
        add("flt_create_3d", tPtr, {i64Ty, i64Ty, i64Ty}, false);
        add("flt_create_4d", tPtr, {i64Ty, i64Ty, i64Ty, i64Ty}, false);
        add("flt_create_5d", tPtr, {i64Ty, i64Ty, i64Ty, i64Ty, i64Ty}, false);

        // ---- f32 tensors: same API, half the memory and half the matmul
        // bandwidth. Measured against f64 at the same shapes: 3.0x at N=100,
        // ~1.9x from N=256 up, and exactly half the footprint.
        add("flt_empty",         tPtr, {i64Ty, llvm::PointerType::get(i64Ty, 0), i32Ty}, false);
        add("flt_create_f32_1d", tPtr, {i64Ty}, false);
        add("flt_create_f32_2d", tPtr, {i64Ty, i64Ty}, false);
        add("flt_create_f32_3d", tPtr, {i64Ty, i64Ty, i64Ty}, false);
        add("flt_create_f32_4d", tPtr, {i64Ty, i64Ty, i64Ty, i64Ty}, false);
        add("flt_zeros_like_f32", tPtr, {tPtr}, false);
        add("flt_clone_f32",    tPtr, {tPtr}, false);
        add("flt_is_f32",       i64Ty, {tPtr}, false);
        add("flt_bytes",        i64Ty, {tPtr}, false);
        add("flt_to_f32",       tPtr, {tPtr}, false);
        add("flt_to_f64",       tPtr, {tPtr}, false);
        add("flt_destroy",  voidTy, {tPtr}, false);
        add("flt_clone",    tPtr, {tPtr}, false);
        add("flt_zeros_like", tPtr, {tPtr}, false);
        add("flt_size",     i64Ty, {tPtr}, false);
        add("flt_ndim",     i64Ty, {tPtr}, false);
        add("flt_shape",    i64Ty, {tPtr, i64Ty}, false);
        add("flt_get",      f64Ty, {tPtr, i64Ty}, false);
        add("flt_set",      voidTy, {tPtr, i64Ty, f64Ty}, false);
        add("flt_fill",     voidTy, {tPtr, f64Ty}, false);
        add("flt_copy",     voidTy, {tPtr, tPtr}, false);
        add("flt_randn",    voidTy, {tPtr}, false);
        add("flt_rand",     voidTy, {tPtr}, false);
        add("flt_uniform",  voidTy, {tPtr, f64Ty, f64Ty}, false);

        // Elementwise / math (return a new tensor)
        add("flt_neg",      tPtr, {tPtr}, false);
        add("flt_exp",      tPtr, {tPtr}, false);
        add("flt_log",      tPtr, {tPtr}, false);
        add("flt_sqrt",     tPtr, {tPtr}, false);
        add("flt_pow",      tPtr, {tPtr, f64Ty}, false);
        add("flt_relu",     tPtr, {tPtr}, false);
        add("flt_sigmoid",  tPtr, {tPtr}, false);
        add("flt_tanh",     tPtr, {tPtr}, false);
        add("flt_add_tt",   tPtr, {tPtr, tPtr}, false);
        add("flt_sub_tt",   tPtr, {tPtr, tPtr}, false);
        add("flt_mul_tt",   tPtr, {tPtr, tPtr}, false);
        add("flt_div_tt",   tPtr, {tPtr, tPtr}, false);
        add("flt_matmul",   tPtr, {tPtr, tPtr}, false);
        add("flt_matmul_blas", tPtr, {tPtr, tPtr}, false);
        add("flt_weights_save",  i64Ty, {i8PtrTy, tPtr, i64Ty}, false);
        add("flt_weights_load",  tPtr,  {i8PtrTy, i64Ty}, false);
        add("flt_weights_count", i64Ty, {i8PtrTy}, false);
        add("flt_sum",      f64Ty, {tPtr}, false);
        add("flt_mean",     f64Ty, {tPtr}, false);
        add("flt_reshape",  tPtr, {tPtr, i64Ty, llvm::PointerType::get(i64Ty, 0)}, false);
        add("flt_transpose", tPtr, {tPtr, i64Ty, i64Ty}, false);

        // In-place activations
        add("flt_relu_inplace",    voidTy, {tPtr}, false);
        add("flt_sigmoid_inplace", voidTy, {tPtr}, false);
        add("flt_tanh_inplace",    voidTy, {tPtr}, false);

        // Autograd tape
        add("ag_init",        voidTy, {}, false);
        add("ag_cleanup",     voidTy, {}, false);
        add("ag_track_tensor", i64Ty, {tPtr}, false);
        add("ag_backward",    voidTy, {i64Ty}, false);
        add("ag_get_grad",    llvm::PointerType::get(f64Ty, 0), {i64Ty}, false);
        add("ag_grad_id",     i64Ty, {tPtr}, false);
        add("ag_grad_at",     f64Ty, {i64Ty, i64Ty}, false);
        add("ag_grad_len",    i64Ty, {i64Ty}, false);
        add("ag_seed",        voidTy, {tPtr}, false);
        // ag_seed_scalar is the common case (a scalar loss differentiated with
        // d/dloss = 1) and was implemented in C but never registered, so it was
        // unreachable from Flint.
        add("ag_seed_scalar", voidTy, {i64Ty, llvm::Type::getDoubleTy(*ctx)}, false);
        add("ag_zero_grad",   voidTy, {i64Ty}, false);
        add("ag_matmul",      tPtr, {tPtr, tPtr}, false);
        add("ag_add",         tPtr, {tPtr, tPtr}, false);
        add("ag_mul",         tPtr, {tPtr, tPtr}, false);
        add("ag_relu",        tPtr, {tPtr}, false);
        add("ag_sigmoid",     tPtr, {tPtr}, false);
        add("ag_tanh",        tPtr, {tPtr}, false);
        add("ag_exp",         tPtr, {tPtr}, false);
        add("ag_log",         tPtr, {tPtr}, false);
        add("ag_neg",         tPtr, {tPtr}, false);

        // Optimizers (AdamState / SGDState are opaque)
        add("adam_create",     tPtr, {f64Ty}, false);
        add("adam_add_param",  voidTy, {tPtr, i64Ty, i64Ty}, false);
        add("adam_step",       voidTy, {tPtr}, false);
        add("adam_destroy",    voidTy, {tPtr}, false);
        add("sgd_create",      tPtr, {f64Ty, f64Ty}, false);
        add("sgd_add_param",   voidTy, {tPtr, i64Ty, i64Ty}, false);
        add("sgd_step",        voidTy, {tPtr}, false);
        add("sgd_destroy",     voidTy, {tPtr}, false);

        // Loss
        add("mse_loss",      f64Ty, {tPtr, tPtr}, false);
        add("mse_loss_grad", tPtr, {tPtr, tPtr}, false);

        // ──────────────────────────────────────────────────────
        // MATH (flint_math.o) — integer maths, angle conversion, special
        // functions, fp classification, and descriptive statistics.
        // ──────────────────────────────────────────────────────
        auto dbl = llvm::Type::getDoubleTy(*ctx);
        add("flint_pi",   dbl, {}, false);
        add("flint_e",    dbl, {}, false);
        add("flint_tau",  dbl, {}, false);
        add("flint_inf",  dbl, {}, false);
        add("flint_nan",  dbl, {}, false);
        add("flint_factorial", i64Ty, {i64Ty}, false);
        add("flint_gcd_i64", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_lcm_i64", i64Ty, {i64Ty, i64Ty}, false);
        add("flint_isqrt",    i64Ty, {i64Ty}, false);
        add("flint_comb",     i64Ty, {i64Ty, i64Ty}, false);
        add("flint_perm",     i64Ty, {i64Ty, i64Ty}, false);
        add("flint_fib",      i64Ty, {i64Ty}, false);
        add("flint_radians", dbl, {dbl}, false);
        add("flint_degrees", dbl, {dbl}, false);
        add("flint_cbrt",   dbl, {dbl}, false);
        add("flint_expm1",  dbl, {dbl}, false);
        add("flint_log1p",  dbl, {dbl}, false);
        add("flint_log2",   dbl, {dbl}, false);
        add("flint_hypot2", dbl, {dbl, dbl}, false);
        add("flint_hypot3", dbl, {dbl, dbl, dbl}, false);
        add("flint_copysign", dbl, {dbl, dbl}, false);
        add("flint_fmod",   dbl, {dbl, dbl}, false);
        add("flint_trunc",  dbl, {dbl}, false);
        add("flint_signum", dbl, {dbl}, false);
        add("flint_sign_i64", i64Ty, {i64Ty}, false);
        add("flint_ldexp",  dbl, {dbl, i64Ty}, false);
        add("flint_frexp",  dbl, {dbl, llvm::PointerType::get(dbl, 0)}, false);
        add("flint_is_finite", i64Ty, {dbl}, false);
        add("flint_is_inf",    i64Ty, {dbl}, false);
        add("flint_is_nan",    i64Ty, {dbl}, false);
        add("flint_is_close",  i64Ty, {dbl, dbl, dbl, dbl}, false);
        add("flint_is_in_range", i64Ty, {dbl, dbl, dbl}, false);
        add("flint_i64_in_range", i64Ty, {i64Ty, i64Ty, i64Ty}, false);
        add("flint_ulp",       dbl, {dbl}, false);
        add("flint_nextafter", dbl, {dbl, dbl}, false);
        add("flint_erf",    dbl, {dbl}, false);
        add("flint_erfc",   dbl, {dbl}, false);
        add("flint_gamma",  dbl, {dbl}, false);
        add("flint_lgamma", dbl, {dbl}, false);

        // Statistics over a vec handle
        auto coll = llvm::PointerType::get(*ctx, 0);
        add("flint_stat_mean",  i64Ty, {coll}, false);
        add("flint_stat_count", i64Ty, {coll}, false);
        add("flint_stat_min",   i64Ty, {coll}, false);
        add("flint_stat_max",   i64Ty, {coll}, false);
        add("flint_stat_median",   i64Ty, {coll}, false);
        add("flint_stat_median_f", dbl,  {coll}, false);
        add("flint_stat_mode",     i64Ty, {coll}, false);
        add("flint_stat_variance",  dbl,  {coll}, false);
        add("flint_stat_pvariance", dbl,  {coll}, false);
        add("flint_stat_stdev",     dbl,  {coll}, false);
        add("flint_stat_pstdev",    dbl,  {coll}, false);
        add("flint_stat_geometric_mean", dbl, {coll}, false);
        add("flint_stat_harmonic_mean",  dbl, {coll}, false);
        add("flint_stat_percentile", dbl, {coll, dbl}, false);
        add("flint_stat_skewness",  dbl, {coll}, false);
        add("flint_stat_kurtosis",  dbl, {coll}, false);
        add("flint_stat_covariance", dbl, {coll, coll}, false);
        add("flint_stat_correlation", dbl, {coll, coll}, false);
        add("flint_stat_linear_regression", i64Ty,
            {coll, coll, llvm::PointerType::get(dbl, 0), llvm::PointerType::get(dbl, 0)}, false);

        // Streaming accumulator
        add("flint_accum_new",     coll, {}, false);
        add("flint_accum_push",    voidTy, {coll, dbl}, false);
        add("flint_accum_mean",    dbl,  {coll}, false);
        add("flint_accum_variance", dbl, {coll}, false);
        add("flint_accum_stdev",   dbl,  {coll}, false);
        add("flint_accum_min",     dbl,  {coll}, false);
        add("flint_accum_max",     dbl,  {coll}, false);
        add("flint_accum_count",   i64Ty, {coll}, false);
        add("flint_accum_free",    voidTy, {coll}, false);

        // ──────────────────────────────────────────────────────
        // STRING FORMATTING (flint_fmt.o) — printf-style. The single most
        // used missing piece: without it every program concatenated with
        // flint_i64_to_string.
        //
        // flint_format is variadic, so it cannot go through add(): an LLVM
        // varargs function needs its FunctionType built with isVarArg=true and
        // every call site lowered with the platform's vararg ABI (a register
        // save area on AArch64). Declaring it as a fixed (char*) function would
        // silently pass garbage for the arguments after the format string, so it
        // is registered as a fixed-arity shim instead:
        //   flint_format2(fmt, a, b)  up to 4 arguments
        // which covers essentially every formatting call and keeps the call
        // sites type-checked by the verifier.
        add("flint_format1", i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_format2", i8PtrTy, {i8PtrTy, i64Ty, i64Ty}, false);
        add("flint_format3", i8PtrTy, {i8PtrTy, i64Ty, i64Ty, i64Ty}, false);
        add("flint_format4", i8PtrTy, {i8PtrTy, i64Ty, i64Ty, i64Ty, i64Ty}, false);
        // Double-precision variant, for %f/%e/%g.
        add("flint_formatd1", i8PtrTy, {i8PtrTy, dbl}, false);
        add("flint_formatd2", i8PtrTy, {i8PtrTy, dbl, i64Ty}, false);
        add("flint_formatd3", i8PtrTy, {i8PtrTy, dbl, i64Ty, i64Ty}, false);
        add("flint_formatd4", i8PtrTy, {i8PtrTy, dbl, i64Ty, i64Ty, i64Ty}, false);
        // String-argument variant: flint_formats(fmt, s, ...).
        add("flint_formats1", i8PtrTy, {i8PtrTy, i8PtrTy}, false);
        add("flint_formats2", i8PtrTy, {i8PtrTy, i8PtrTy, i64Ty}, false);
        add("flint_formats3", i8PtrTy, {i8PtrTy, i8PtrTy, i64Ty, i64Ty}, false);

        // ──────────────────────────────────────────────────────
        // COLLECTIONS (flint_coll.o) — set, priority queue, deque.
        // ──────────────────────────────────────────────────────
        add("flint_set_new",     coll, {}, false);
        add("flint_set_free",    voidTy, {coll}, false);
        add("flint_set_add",     i64Ty, {coll, i64Ty}, false);
        add("flint_set_has",     i64Ty, {coll, i64Ty}, false);
        add("flint_set_remove",  i64Ty, {coll, i64Ty}, false);
        add("flint_set_len",     i64Ty, {coll}, false);
        add("flint_set_clear",   voidTy, {coll}, false);
        add("flint_set_values",  i64Ty, {coll, llvm::PointerType::get(i64Ty, 0), i64Ty}, false);

        add("flint_heap_new",    coll, {}, false);
        add("flint_heap_free",   voidTy, {coll}, false);
        add("flint_heap_push",   voidTy, {coll, i64Ty}, false);
        add("flint_heap_pop",    i64Ty, {coll}, false);
        add("flint_heap_peek",   i64Ty, {coll}, false);
        add("flint_heap_len",    i64Ty, {coll}, false);
        add("flint_heap_empty",  i64Ty, {coll}, false);
        add("flint_heap_clear",  voidTy, {coll}, false);
        add("flint_heap_sorted", i64Ty, {coll, llvm::PointerType::get(i64Ty, 0), i64Ty}, false);

        add("flint_deque_new",       coll, {}, false);
        add("flint_deque_free",      voidTy, {coll}, false);
        add("flint_deque_push_back", voidTy, {coll, i64Ty}, false);
        add("flint_deque_push_front",voidTy, {coll, i64Ty}, false);
        add("flint_deque_pop_back",  i64Ty, {coll}, false);
        add("flint_deque_pop_front", i64Ty, {coll}, false);
        add("flint_deque_front",     i64Ty, {coll}, false);
        add("flint_deque_back",      i64Ty, {coll}, false);
        add("flint_deque_len",       i64Ty, {coll}, false);
        add("flint_deque_empty",     i64Ty, {coll}, false);
        add("flint_deque_clear",     voidTy, {coll}, false);
        add("flint_deque_values",    i64Ty, {coll, llvm::PointerType::get(i64Ty, 0), i64Ty}, false);

        // ──────────────────────────────────────────────────────
        // NN LAYERS (flint_nn.o) -- convolution, pooling, normalisation,
        // softmax/dropout, and parameter initialisation. The tensor library
        // had primitives but no layers, so "convolution" could not be
        // expressed at all.
        // ──────────────────────────────────────────────────────
        add("flt_conv2d",        tPtr, {tPtr, tPtr, tPtr, i64Ty, i64Ty}, false);
        add("flt_maxpool2d",     tPtr, {tPtr, i64Ty, i64Ty}, false);
        add("flt_avgpool2d",     tPtr, {tPtr, i64Ty, i64Ty}, false);
        add("flt_global_avgpool", tPtr, {tPtr}, false);
        add("flt_batchnorm",     tPtr, {tPtr, tPtr, tPtr, dbl}, false);
        add("flt_layer_norm",    tPtr, {tPtr, tPtr, tPtr, dbl}, false);
        add("flt_rms_norm",      tPtr, {tPtr, tPtr, dbl}, false);
        add("flt_softmax",       tPtr, {tPtr}, false);
        add("flt_dropout",       tPtr, {tPtr, dbl}, false);
        add("flt_dropout_inplace", tPtr, {tPtr, dbl}, false);
        add("flt_l2_reg",        dbl,  {tPtr}, false);
        add("flt_flatten",       tPtr, {tPtr}, false);
        add("flt_xavier_uniform", tPtr, {tPtr, dbl}, false);
        add("flt_he_uniform",    tPtr, {tPtr, dbl}, false);
        add("flt_data",          llvm::PointerType::get(dbl, 0), {tPtr}, false);
        add("flt_data_f32",      llvm::PointerType::get(*ctx, 0), {tPtr}, false);
        add("flt_set_at",       voidTy, {tPtr, i64Ty, dbl}, false);
        add("flt_get_at",       dbl,    {tPtr, i64Ty}, false);

        // ──────────────────────────────────────────────────────
        // ARENA / CHECKPOINTING (flint_ai.o) -- previously implemented but
        // never registered, so the whole layer was unreachable from Flint.
        // fa_* takes (FArena*, FATensor*), so the arena is an explicit first
        // argument; fa_default_arena gives one to pass.
        // ──────────────────────────────────────────────────────
        add("fa_default_arena",  tPtr, {}, false);
        add("farena_used",       i64Ty, {tPtr}, false);
        add("farena_reset",      voidTy, {tPtr}, false);
        // `void farena_init(FArena* a, size_t initial_size)` takes TWO arguments
        // and returns nothing. It was registered as `(tPtr, {})` -- zero
        // parameters, and a pointer return -- so any call failed to build:
        // examples/ai_arch_bench.fl reported "'farena_init' expects 0
        // argument(s), got 2" and, before the call-signature check existed,
        // leaked raw LLVM IR instead. A hand-written registration that
        // disagrees with the C definition is a silent trap, so it now matches
        // runtime/flint_ai.c:71.
        add("farena_init",       voidTy, {tPtr, i64Ty}, false);
        add("farena_destroy",    voidTy, {tPtr}, false);
        add("farena_alloc",      tPtr, {tPtr, i64Ty}, false);
        add("fcheckpoint_init",  voidTy, {}, false);

        // ──────────────────────────────────────────────────────
        // UTILITY (flint_util.o) — sorting, binary search, synchronisation,
        // a CSPRNG, and CLI parsing. flint_vec was unsorted with no way to
        // order or search it, flint_thread_create existed with no way to
        // protect shared state, and every program hand-rolled argv.
        // ──────────────────────────────────────────────────────
        add("flint_sort_i64",     i64Ty, {coll}, false);
        add("flint_sort_desc_i64", i64Ty, {coll}, false);
        add("flint_bsearch_i64",  i64Ty, {coll, i64Ty}, false);
        add("flint_bsearch_desc_i64", i64Ty, {coll, i64Ty}, false);

        add("flint_mutex_new",   tPtr, {}, false);
        add("flint_mutex_lock",  i64Ty, {tPtr}, false);
        add("flint_mutex_trylock", i64Ty, {tPtr}, false);
        add("flint_mutex_unlock", i64Ty, {tPtr}, false);
        add("flint_mutex_free",  voidTy, {tPtr}, false);

        add("flint_cond_new",      tPtr, {}, false);
        add("flint_cond_wait",     i64Ty, {tPtr, tPtr}, false);
        add("flint_cond_signal",   i64Ty, {tPtr}, false);
        add("flint_cond_broadcast", i64Ty, {tPtr}, false);
        add("flint_cond_free",     voidTy, {tPtr}, false);

        add("flint_rwlock_new",    tPtr, {}, false);
        add("flint_rwlock_rdlock", i64Ty, {tPtr}, false);
        add("flint_rwlock_wrlock", i64Ty, {tPtr}, false);
        add("flint_rwlock_unlock", i64Ty, {tPtr}, false);
        add("flint_rwlock_free",   voidTy, {tPtr}, false);

        add("flint_atomic_new", i64Ty, {i64Ty}, false);
        add("flint_atomic_get", i64Ty, {tPtr}, false);
        add("flint_atomic_set", i64Ty, {tPtr, i64Ty}, false);
        add("flint_atomic_add", i64Ty, {tPtr, i64Ty}, false);
        add("flint_atomic_sub", i64Ty, {tPtr, i64Ty}, false);
        add("flint_atomic_cas", i64Ty, {tPtr, i64Ty, i64Ty}, false);
        add("flint_atomic_free", voidTy, {tPtr}, false);

        // flint_rand_f64 is a plain LCG: fine for jitter, wrong for keys,
        // tokens and nonces. This is xoshiro256** seeded from the OS.
        add("flint_rng_seed_from_os", tPtr, {}, false);
        add("flint_rng_below",   i64Ty, {tPtr, i64Ty}, false);
        add("flint_rng_i64_range", i64Ty, {tPtr, i64Ty, i64Ty}, false);
        add("flint_rng_free",   voidTy, {tPtr}, false);

        add("flint_get_argv", i8PtrTy, {}, false);
        add("flint_cli_new",    tPtr, {i8PtrTy}, false);
        add("flint_cli_flag",   voidTy, {tPtr, i8PtrTy, i64Ty, i64Ty}, false);
        add("flint_cli_opt",    voidTy, {tPtr, i8PtrTy, i64Ty, i64Ty}, false);
        add("flint_cli_short_for", i8Ty, {tPtr, i8PtrTy}, false);
        add("flint_cli_parse",  i64Ty, {tPtr, i64Ty, llvm::PointerType::get(i8PtrTy, 0)}, false);
        add("flint_cli_ok",     i64Ty, {tPtr}, false);
        add("flint_cli_error",  i8PtrTy, {tPtr}, false);
        add("flint_cli_get_str", i8PtrTy, {tPtr, i8PtrTy}, false);
        add("flint_cli_get_i64", i64Ty, {tPtr, i8PtrTy}, false);
        add("flint_cli_has",    i64Ty, {tPtr, i8PtrTy}, false);
        add("flint_cli_positional", i8PtrTy, {tPtr, i64Ty}, false);
        add("flint_cli_usage",  i8PtrTy, {tPtr}, false);
        add("flint_cli_free",   voidTy, {tPtr}, false);

        // ──────────────────────────────────────────────────────
        // DYNAMIC VALUES + TOML (flint_value.o / flint_toml.o).
        // One value model shared by every config/interchange format, so a TOML
        // and a JSON document are walked with the same code.
        // ──────────────────────────────────────────────────────
        add("flint_value_null",  tPtr, {}, false);
        add("flint_value_bool",  tPtr, {i64Ty}, false);
        add("flint_value_i64",   tPtr, {i64Ty}, false);
        add("flint_value_f64",   tPtr, {dbl}, false);
        add("flint_value_str",   tPtr, {i8PtrTy}, false);
        add("flint_value_arr",   tPtr, {}, false);
        add("flint_value_obj",   tPtr, {}, false);
        add("flint_value_arr_add", voidTy, {tPtr, tPtr}, false);
        add("flint_value_obj_set", voidTy, {tPtr, i8PtrTy, tPtr}, false);
        add("flint_value_obj_get", tPtr,  {tPtr, i8PtrTy}, false);
        add("flint_value_obj_has", i64Ty, {tPtr, i8PtrTy}, false);
        add("flint_value_len",    i64Ty, {tPtr}, false);
        add("flint_value_at",     tPtr,  {tPtr, i64Ty}, false);
        add("flint_value_key_at", i8PtrTy, {tPtr, i64Ty}, false);
        add("flint_value_value_at", tPtr, {tPtr, i64Ty}, false);
        add("flint_value_as_i64",  i64Ty,  {tPtr, i64Ty}, false);
        add("flint_value_as_f64",  dbl,    {tPtr, dbl}, false);
        add("flint_value_as_bool", i64Ty,  {tPtr, i64Ty}, false);
        add("flint_value_as_str",  i8PtrTy, {tPtr, i8PtrTy}, false);
        add("flint_value_path",      tPtr,    {tPtr, i8PtrTy}, false);
        add("flint_value_path_i64",  i64Ty,   {tPtr, i8PtrTy, i64Ty}, false);
        add("flint_value_path_str",  i8PtrTy, {tPtr, i8PtrTy, i8PtrTy}, false);
        add("flint_value_encode",    i8PtrTy, {tPtr, i64Ty}, false);
        add("flint_value_to_string", i8PtrTy, {tPtr}, false);
        add("flint_value_free_string", voidTy, {i8PtrTy}, false);
        add("flint_value_free",     voidTy, {tPtr}, false);
        add("flint_value_clone",    tPtr,    {tPtr}, false);
        add("flint_toml_parse",     tPtr,    {i8PtrTy}, false);
        add("flint_yaml_parse",    tPtr,    {i8PtrTy}, false);

        // ──────────────────────────────────────────────────────
        // DATE / TIME / TIMEZONES (flint_time.o). Civil time and instants are
        // deliberately separate: a calendar date is a fact, "3pm" is not.
        // ──────────────────────────────────────────────────────
        auto dtPtr = llvm::PointerType::get(*ctx, 0);
        add("flint_is_leap",          i64Ty, {i64Ty}, false);
        add("flint_days_in_month",    i64Ty, {i64Ty, i64Ty}, false);
        add("flint_days_from_civil",  i64Ty, {i64Ty, i64Ty, i64Ty}, false);
        add("flint_weekday_from_days", i64Ty, {i64Ty}, false);
        add("flint_timegm",           i64Ty, {dtPtr}, false);
        add("flint_gmtime",           dtPtr, {i64Ty}, false);
        add("flint_datetime_make",    dtPtr, {i64Ty, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty}, false);
        add("flint_tz_offset_at",     i64Ty, {i8PtrTy, i64Ty}, false);
        add("flint_tz_is_dst_at",     i64Ty,   {i8PtrTy, i64Ty}, false);
        add("flint_tz_abbrev_at",     i8PtrTy, {i8PtrTy, i64Ty}, false);
        add("flint_tz_lookup",        dtPtr, {i8PtrTy}, false);
        add("flint_tz_convert",       dtPtr, {i64Ty, i8PtrTy}, false);
        add("flint_dt_year",   i64Ty, {dtPtr}, false);
        add("flint_dt_month",  i64Ty, {dtPtr}, false);
        add("flint_dt_day",    i64Ty, {dtPtr}, false);
        add("flint_dt_hour",   i64Ty, {dtPtr}, false);
        add("flint_dt_minute", i64Ty, {dtPtr}, false);
        add("flint_dt_second", i64Ty, {dtPtr}, false);
        add("flint_dt_weekday", i64Ty, {dtPtr}, false);
        add("flint_dt_yearday", i64Ty, {dtPtr}, false);
        add("flint_tz_known",         i64Ty, {i8PtrTy}, false);
        add("flint_tzdata_loaded",    i64Ty, {}, false);
        add("flint_time_format",      i8PtrTy, {i8PtrTy, i64Ty, i8PtrTy}, false);
        add("flint_time_parse",       i64Ty, {i8PtrTy, i8PtrTy, llvm::PointerType::get(i64Ty, 0)}, false);
        add("flint_time_diff",        i64Ty, {i64Ty, i64Ty}, false);
        add("flint_time_duration",    i8PtrTy, {i64Ty}, false);
        add("flint_time_free",        voidTy, {i8PtrTy}, false);
        add("flint_dt_free",          voidTy, {dtPtr}, false);
        add("flint_tz_free",          voidTy, {dtPtr}, false);

        // BLAS backend introspection -- lets Flint report which matmul path is
        // live, which matters because it differs per build.
        add("flint_blas_available", i64Ty, {}, false);
        add("flint_blas_backend",   i8PtrTy, {}, false);
        add("flint_blas_calls",     i64Ty, {}, false);
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

    // Best-effort static type of an expression, for call-site param inference.
    // Only needs to be right about the pointer-vs-integer distinction (an
    // opaque handle or a str vs a number); it reuses the same reasoning as
    // Parser::inferType but is a free function so the inference pass can call
    // it before the Parser's per-function type environment is set up.
    // `st` is the live symbol table when the caller has one. exprFlintType is
    // static because the type-inference walkers are, so it cannot reach
    // Codegen::symTable on its own -- which is why a variable used as a
    // generic argument fell through to i64 and made `first<T>(p.x)` infer T as
    // i64 even when the field was f64.
    //
    // A file-scope variable-type table was tried first and removed: it cannot
    // see inference-only locals, so it half-worked, and a half-working mirror
    // of the symbol table is worse than none -- it goes stale silently.
    static Type exprFlintType(ExprAST* e,
                              const std::unordered_map<std::string, Type>* vars = nullptr,
                              SymbolTable* st = nullptr) {
        if (!e) return Type::i64();
        switch (e->kind) {
            case NodeKind::String: return Type::str();
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(e);
                if (vars) {
                    auto it = vars->find(v->name);
                    if (it != vars->end()) return it->second;
                }
                // The live symbol table, when the caller has one: this is the
                // authoritative type for a variable, and it covers locals that
                // were never annotated.
                if (st) {
                    if (const Symbol* sym = st->lookup(v->name)) return sym->type;
                }
                return Type::i64();
            }
            case NodeKind::VarDecl: {
                auto* n = static_cast<VarDeclAST*>(e);
                return exprFlintType(n->init.get(), vars);
            }
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(e);
                if (n->isFloat) return Type::f64();
                if (n->isInt) return Type::i64();
                double ip;
                return std::modf(n->value, &ip) != 0.0 ? Type::f64() : Type::i64();
            }
            case NodeKind::Array: return Type::array(Type::i64());
            case NodeKind::MapLiteral: return Type::map();
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(e);
                if (b->op == '+') {
                    Type lt = exprFlintType(b->lhs.get());
                    Type rt = exprFlintType(b->rhs.get());
                    if (lt.kind == TypeKind::Str || rt.kind == TypeKind::Str) return Type::str();
                }
                // M2c: an arithmetic expression with a float operand is f64.
                // Returning i64 here made `id(1.5 + 2.5)` instantiate the
                // generic with i64 -- a silently wrong answer that the call
                // guard then rejected with a message about the
                // instantiation instead of the program.
                {
                    Type lt = exprFlintType(b->lhs.get(), vars, st);
                    Type rt = exprFlintType(b->rhs.get(), vars, st);
                    if (lt.kind == TypeKind::F64 || rt.kind == TypeKind::F64)
                        return Type::f64();
                }
                return Type::i64();
            }
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(e);
                if (c->callee == "print") return Type::void_();
                return Parser::inferCallTypeFallback(c->callee);
            }
            // The three cases below were missing, so generic inference fell
            // through to the `default` i64 for any of them. That is the
            // silent-wrong-answer class: `wrap<T>(arr[0])` or `first<T>(p.x)`
            // instantiated T as i64 and compiled against the wrong signature.
            case NodeKind::FieldAccess: {
                auto* f = static_cast<FieldAccessAST*>(e);
                Type base = exprFlintType(f->base.get(), vars, st);
                if (base.kind == TypeKind::Struct) {
                    auto sit = gStructFieldTypes.find(base.structName);
                    if (sit != gStructFieldTypes.end()) {
                        for (auto& fd : sit->second) {
                            if (fd.first == f->fieldName) return fd.second;
                        }
                    }
                }
                return Type::i64();
            }
            case NodeKind::EnumConstruct:
                return Type::enum_(static_cast<EnumConstructAST*>(e)->enumName);
            case NodeKind::StructLiteral:
                return Type::struct_(static_cast<StructLiteralAST*>(e)->structName);
            // M2 residue: a cast's type IS its target type. Without this
            // case `id(c(5 as f64))` fell to the default i64, instantiated
            // the generic with i64, and the call guard then rejected the
            // already-mis-instanciated call ("expects i64").
            case NodeKind::Cast:
                return static_cast<CastExprAST*>(e)->target;
            default: return Type::i64();
        }
    }

    // M2 residue: the argument type resolver for generic inference. The
    // plain walker (exprFlintType) knows numbers, strings, variables,
    // structs, enums, field access and '+'. Two shapes were still missing
    // and silently instantiated i64:
    //   * a CAST argument -- its truth is the cast target type;
    //   * a nested CALL argument -- the callee's (possibly generic) return
    //     type, resolved with the same by-parameter rule the inference
    //     loop itself uses, recursively. That makes idc(idc(Inner{a:1})
    //     ) infer T = Inner instead of instantiating i64 and erroring in
    //     the creationCallChecked guard.
    // M2c: the STRICT argument resolver for generic inference. It returns
    // false when the argument's type cannot be determined, so the caller can
    // report the parameter by name instead of instantiating i64 and
    // letting the program fail later with a message about the
    // instantiation.
    //
    // The shapes it knows: variables (symbol table), numbers, strings,
    // casts (their target type), struct/enum/array/map literals, field
    // access (through the parsed struct field table), arithmetic (f64 when
    // an operand is f64), and calls -- a generic callee resolves through the
    // by-parameter rule (recursively), a user function through its declared
    // return type, a builtin through the builtin's table.
    bool inferGenericArgType(ExprAST* argIn, Type& out, int depth = 0) {
        if (!argIn || depth > 8) return false;
        switch (argIn->kind) {
            case NodeKind::Variable: {
                auto* var = static_cast<VariableExprAST*>(argIn);
                if (const Symbol* sym = symTable.lookup(var->name)) { out = sym->type; return true; }
                auto gvit = globalSymTable.find(var->name);
                if (gvit != globalSymTable.end()) { out = gvit->second.type; return true; }
                return false; // unknown name
            }
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(argIn);
                if (n->isFloat) { out = Type::f64(); return true; }
                if (n->isInt) { out = Type::i64(); return true; }
                double ip;
                out = (std::modf(n->value, &ip) != 0.0) ? Type::f64() : Type::i64();
                return true;
            }
            case NodeKind::String: out = Type::str(); return true;
            case NodeKind::Index: {
                // The element type of an indexed value. Arrays and strings
                // are i64-valued in this dialect, so the element is i64 --
                // but only when the base really is one of them; anything
                // else is not modelled and must be reported.
                auto* ix = static_cast<IndexExprAST*>(argIn);
                Type base;
                if (!inferGenericArgType(ix->base.get(), base, depth + 1)) return false;
                if (base.kind == TypeKind::Array || base.kind == TypeKind::Str) {
                    out = Type::i64();
                    return true;
                }
                return false;
            }
            case NodeKind::Lambda: {
                // A lambda's own type is known: its parameter types and its
                // declared return type.
                auto* lam = static_cast<LambdaExprAST*>(argIn);
                if (lam->retType.kind == TypeKind::Void) return false; // not declared
                std::vector<Type> ps;
                for (auto& lp : lam->params) ps.push_back(resolveType(lp.second));
                out = Type::fn_(ps, resolveType(lam->retType));
                return true;
            }
            case NodeKind::Cast:
                out = static_cast<CastExprAST*>(argIn)->target;
                return true;
            case NodeKind::EnumConstruct:
                out = Type::enum_(static_cast<EnumConstructAST*>(argIn)->enumName);
                return true;
            case NodeKind::StructLiteral:
                out = Type::struct_(static_cast<StructLiteralAST*>(argIn)->structName);
                return true;
            case NodeKind::Array: out = Type::array(Type::i64()); return true;
            case NodeKind::MapLiteral: out = Type::map(); return true;
            case NodeKind::FieldAccess: {
                auto* f = static_cast<FieldAccessAST*>(argIn);
                Type base;
                if (!inferGenericArgType(f->base.get(), base, depth + 1)) return false;
                if (base.kind != TypeKind::Struct) return false;
                auto sit = gStructFieldTypes.find(base.structName);
                if (sit == gStructFieldTypes.end()) return false;
                for (auto& fd : sit->second) {
                    if (fd.first == f->fieldName) { out = fd.second; return true; }
                }
                return false; // no such field
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(argIn);
                Type lt, rt;
                bool lok = inferGenericArgType(b->lhs.get(), lt, depth + 1);
                bool rok = inferGenericArgType(b->rhs.get(), rt, depth + 1);
                if (!lok || !rok) return false;
                if (b->op == '+' && (lt.kind == TypeKind::Str || rt.kind == TypeKind::Str)) {
                    out = Type::str();
                    return true;
                }
                if (lt.kind == TypeKind::F64 || rt.kind == TypeKind::F64) {
                    out = Type::f64();
                    return true;
                }
                if (lt.kind == TypeKind::I64 && rt.kind == TypeKind::I64) {
                    out = Type::i64();
                    return true;
                }
                return false;
            }
            case NodeKind::Call: {
                auto* c = static_cast<CallExprAST*>(argIn);
                auto git2 = genericFunctions.find(c->callee);
                if (git2 != genericFunctions.end()) {
                    auto* gen2 = git2->second;
                    if (gen2->returnType.kind == TypeKind::TypeParam) {
                        // The return type is the type parameter: infer it from
                        // the argument filling the parameter that mentions it.
                        size_t pi2 = SIZE_MAX;
                        for (size_t qq = 0; qq < gen2->params.size(); qq++) {
                            if (gen2->params[qq].second.kind == TypeKind::TypeParam &&
                                gen2->params[qq].second.structName ==
                                    gen2->returnType.structName) {
                                pi2 = qq; break;
                            }
                        }
                        if (pi2 == SIZE_MAX || pi2 >= c->args.size()) return false;
                        return inferGenericArgType(c->args[pi2].get(), out, depth + 1);
                    }
                    out = resolveType(gen2->returnType);
                    return true;
                }
                // A user function: its declared return type, not the
                // builtins-only table (which answered i64 for every user
                // function -- a silently wrong instantiation).
                auto defIt = functionDefs.find(c->callee);
                if (defIt != functionDefs.end()) {
                    out = resolveType(defIt->second->returnType);
                    return true;
                }
                // A builtin: the builtins table knows the real return types.
                if (c->callee == "print") return false; // void
                out = Parser::inferCallTypeFallback(c->callee);
                return true;
            }
            default:
                return false; // shape not modelled: report it, do not guess
        }
    }

    // Direct sub-expressions of `e`, for tree walks that must reach every
    // nested call. Only expression-bearing fields appear; statement-only
    // nodes that carry an expression body (Block/If/While/Match) are included
    // where the type system models them as expressions.
    static std::vector<ExprAST*> exprChildren(ExprAST* e) {
        std::vector<ExprAST*> out;
        if (!e) return out;
        // Takes a raw pointer: callers pass `n->lhs.get()`.
        auto push = [&](ExprAST* c) { if (c) out.push_back(c); };
        switch (e->kind) {
            case NodeKind::Binary: {
                auto* n = static_cast<BinaryExprAST*>(e);
                push(n->lhs.get()); push(n->rhs.get()); break; }
            case NodeKind::Compare: {
                auto* n = static_cast<CompareExprAST*>(e);
                push(n->lhs.get()); push(n->rhs.get()); break; }
            case NodeKind::Assign: {
                auto* n = static_cast<AssignExprAST*>(e);
                push(n->rhs.get()); break; }
            case NodeKind::VarDecl: {
                auto* n = static_cast<VarDeclAST*>(e);
                push(n->init.get()); break; }
            case NodeKind::Call: {
                auto* n = static_cast<CallExprAST*>(e);
                for (auto& a : n->args) push(a.get());
                break; }
            case NodeKind::Index: {
                auto* n = static_cast<IndexExprAST*>(e);
                push(n->base.get()); push(n->index.get()); break; }
            case NodeKind::FieldAccess: {
                auto* n = static_cast<FieldAccessAST*>(e);
                push(n->base.get()); break; }
            case NodeKind::Ref: {
                auto* n = static_cast<RefExprAST*>(e);
                push(n->target.get()); break; }
            case NodeKind::Deref: {
                auto* n = static_cast<DerefExprAST*>(e);
                push(n->target.get()); break; }
            case NodeKind::Array: {
                auto* n = static_cast<ArrayExprAST*>(e);
                for (auto& v : n->elements) push(v.get());
                break; }
            case NodeKind::Slice: {
                auto* n = static_cast<SliceExprAST*>(e);
                push(n->arr.get()); push(n->start.get()); push(n->end.get()); break; }
            case NodeKind::StructLiteral: {
                auto* n = static_cast<StructLiteralAST*>(e);
                for (auto& f : n->fields) push(f.second.get());
                break; }
            case NodeKind::EnumConstruct: {
                auto* n = static_cast<EnumConstructAST*>(e);
                for (auto& v : n->args) push(v.get());
                break; }
            case NodeKind::Unwrap: {
                auto* n = static_cast<UnwrapExprAST*>(e);
                push(n->inner.get()); break; }
            case NodeKind::Lambda: {
                auto* n = static_cast<LambdaExprAST*>(e);
                push(n->body.get()); break; }
            case NodeKind::Return: {
                auto* n = static_cast<ReturnStmtAST*>(e);
                push(n->value.get()); break; }
            case NodeKind::MapLiteral: {
                auto* n = static_cast<MapLiteralAST*>(e);
                for (auto& v : n->values) push(v.get());
                break; }
            case NodeKind::Cast: {
                auto* n = static_cast<CastExprAST*>(e);
                push(n->expr.get()); break; }
            // TensorLiteralAST holds only doubles: no children.
            // Statement forms: calls overwhelmingly live inside these, so a
            // walk that skips them sees almost nothing.
            case NodeKind::Block: {
                auto* n = static_cast<BlockStmtAST*>(e);
                for (auto& st : n->stmts) push(st.get());
                break; }
            case NodeKind::If: {
                auto* n = static_cast<IfStmtAST*>(e);
                push(n->condition.get()); push(n->thenBlock.get()); push(n->elseBlock.get());
                break; }
            case NodeKind::While: {
                auto* n = static_cast<WhileStmtAST*>(e);
                push(n->condition.get()); push(n->body.get());
                break; }
            case NodeKind::Match: {
                auto* n = static_cast<MatchExprAST*>(e);
                push(n->scrutinee.get());
                for (auto& a : n->arms) push(a.body.get());
                break; }
            case NodeKind::Destructure: {
                auto* n = static_cast<DestructureAST*>(e);
                push(n->init.get());
                break; }
            default: break;
        }
        return out;
    }

    // Recursive walk recording, for every call site, the inferred type of each
    // argument. `conflict` marks a param seen with two different types.
    // `vars` is threaded by reference and scoped: entering a Block pushes its
    // declarations, leaving pops them. Passing it by value would give each
    // sibling statement its own copy, so `sb = f()` would not be visible to
    // the `g(sb)` on the next line.
    static void collectArgTypes(ExprAST* e,
                                std::unordered_map<std::string, std::unordered_map<size_t, ArgSlot>>& seen,
                                std::unordered_map<std::string, Type>& vars,
                                std::vector<std::string>* scope = nullptr) {
        if (!e) return;
        // Track `x = f()` / `x: T = ...` so a later `f(x)` at another call
        // site resolves x to the handle type it was given, not to i64.
        if (e->kind == NodeKind::VarDecl) {
            auto* n = static_cast<VarDeclAST*>(e);
            Type vt = exprFlintType(n->init.get(), &vars);
            vars[n->varName] = n->varType.kind == TypeKind::Void ? vt : n->varType;
        } else if (e->kind == NodeKind::Assign) {
            auto* n = static_cast<AssignExprAST*>(e);
            auto it = vars.find(n->varName);
            Type t = exprFlintType(n->rhs.get(), &vars);
            if (it == vars.end() || it->second.kind == TypeKind::I64) vars[n->varName] = t;
        }
        if (e->kind == NodeKind::Call) {
            auto* c = static_cast<CallExprAST*>(e);
            for (size_t i = 0; i < c->args.size(); i++) {
                Type t = exprFlintType(c->args[i].get(), &vars);
                if (t.kind == TypeKind::Void) continue; // says nothing about the param
                ArgSlot& sl = seen[c->callee][i];
                bool isPtr = (t.kind == TypeKind::Str || t.kind == TypeKind::Map ||
                              t.kind == TypeKind::Array || t.kind == TypeKind::Ref);
                if (!sl.any) { sl.t = t; sl.any = true; sl.sawPtr = isPtr; }
                else if (isPtr && !sl.sawPtr) { sl.t = t; sl.sawPtr = true; sl.sawOther = false; }
                else if (!isPtr && sl.sawPtr) { sl.sawOther = true; }
                else if (!(sl.t == t)) { sl.sawOther = true; }
            }
        }
        // Scope the declarations this statement introduces to its own subtree.
        std::vector<std::string> introduced;
        if (e->kind == NodeKind::Block) {
            auto* n = static_cast<BlockStmtAST*>(e);
            introduced.reserve(n->stmts.size());
            for (auto& st : n->stmts)
                if (st && st->kind == NodeKind::VarDecl) introduced.push_back(static_cast<VarDeclAST*>(st.get())->varName);
        }
        for (auto* c : exprChildren(e)) collectArgTypes(c, seen, vars);
        for (auto& nm : introduced) vars.erase(nm);
    }

    // Retype unannotated params from the arguments actually passed at call
    // sites. An unannotated param defaults to i64; when a caller passes a
    // pointer (a vec/sb/map handle, a str) the emitted call contradicts the
    // callee's signature and LLVM rejects the module.
    //
    // This previously "worked" only by accident: on AArch64 a pointer and an
    // i64 travel in the same register, so the callee read the right bits from
    // a call LLVM considered malformed -- a miscompile that is benign on one
    // ABI and a hard error on any typed target.
    //
    // A param seen with two different types keeps its i64 default rather than
    // guessing.
    void inferUntypedParams(ProgramAST& prog) {
        // Seed every unannotated param as "unknown" rather than i64, so a hop
        // that learns a real type later is not blocked by the default.
        for (auto& fn : prog.functions)
            for (size_t idx : fn->untypedParams)
                if (idx < fn->params.size()) fn->params[idx].second = Type::void_();

        // Types propagate along the call graph, so one pass is not enough:
        // `fn p_block(out) { p_emit(out, ...) }` learns out's type only from
        // whoever passes a real handle to p_block. Iterate to a fixpoint.
        //
        // Bounded by (functions + 1) rounds: each round that changes nothing
        // ends the loop, and a param can only change from unknown to a single
        // concrete type, so no round can oscillate.
        const size_t maxRounds = prog.functions.size() + 1;
        for (size_t round = 0; round < maxRounds; round++) {
            bool changed = false;

            // callee name -> param index -> observed type
            std::unordered_map<std::string, std::unordered_map<size_t, ArgSlot>> seen;
            for (auto& fn : prog.functions) {
                // A callee's own params are visible inside its body, so
                // `g(x) { f(x) }` propagates x's (possibly still unknown) type.
                std::unordered_map<std::string, Type> vars;
                for (auto& pr : fn->params) vars[pr.first] = pr.second;
                collectArgTypes(fn->body.get(), seen, vars);
            }

            for (auto& fn : prog.functions) {
                auto fit = seen.find(fn->name);
                if (fit == seen.end()) continue;
                for (size_t idx : fn->untypedParams) {
                    if (idx >= fn->params.size()) continue;
                    auto pit = fit->second.find(idx);
                    if (pit == fit->second.end()) continue;
                    if (!pit->second.sawPtr && pit->second.sawOther) continue; // ambiguous
                    Type& cur = fn->params[idx].second;
                    if (cur.kind != TypeKind::Void) continue; // already decided
                    if (pit->second.t.kind == TypeKind::Void) continue; // nothing learned
                    cur = pit->second.t;
                    changed = true;
                }
            }
            if (!changed) break;
        }

        // Anything still unknown is genuinely unconstrained here. i64 keeps the
        // historical behaviour for numeric params (`fn add(a, b)`).
        for (auto& fn : prog.functions)
            for (size_t idx : fn->untypedParams)
                if (idx < fn->params.size() && fn->params[idx].second.kind == TypeKind::Void)
                    fn->params[idx].second = Type::i64();
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

        if (debugInfo && dib) {
            unsigned line = (unsigned)(fn->loc.line ? fn->loc.line : 1);
            auto* sp = dib->createFunction(
                cu,
                fn->name,
                f->getName(),
                cu->getFile(),
                line,
                dib->createSubroutineType(dib->getOrCreateTypeArray({})),
                line,
                llvm::DINode::FlagZero,
                llvm::DISubprogram::toSPFlags(false /* isLocalToUnit */,
                                              true  /* isDefinition */,
                                              false /* isOptimized */));
            f->setSubprogram(sp);
            curDISub = sp;
            builder->SetCurrentDebugLocation(
                llvm::DebugLoc(llvm::DILocation::get(*ctx, line, 1, curDISub)));
        } else {
            curDISub = nullptr;
        }

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
            // The single-pass EMIT path (Parser::emitXxx) is entered only from
            // here: a function whose body was not built as an AST. Set
            // FLINT_TRACE_EMITPATH=1 to see when it runs — the answer decides
            // whether a fix must be applied to this path as well.
            if (getenv("FLINT_TRACE_EMITPATH"))
                llvm::errs() << "[emit-path] function " << fn->name << "\n";
            size_t savedPos = parser->getPos();
            parser->setPos(fn->bodyStart);
            parser->setCodegen(this, true);
            parser->parseBlockEmit();
            if (parser->hadError()) {
                // The diagnostic is already printed (emitDiagnostic).
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
                // The null value of the RETURN TYPE, not an integer zero:
                // an fn(...) return type is a POINTER (M4), and
                // ConstantInt::get on a pointer type is invalid IR
                // ("ret i0 0" / "does not match operand type"). A function
                // with an fn return type that falls off the end used to
                // fail with an internal LLVM error instead of compiling.
                llvm::Type* rt = llvmType(fn->returnType, *ctx, mod.get());
                builder->CreateRet(llvm::Constant::getNullValue(rt));
            } else {
                builder->CreateRetVoid();
            }
        }

        symTable.exitScope();
        curDISub = nullptr;

    // Verification gate (P0-3). The IR must be valid before it can be
        // JIT-run, emitted or linked: a bad PHI or a value defined in two
        // blocks is a codegen bug that otherwise surfaces as a mystery
        // segfault under ORC with no diagnostic. Verified per function so the
        // message names the culprit. `errs()`, not `outs()` -- stdout carries
        // program output.
        if (llvm::verifyFunction(*f, &llvm::errs())) {
            std::cerr << "function verification failed: " << fn->name << "\n";
            return false;
        }
        // P0-3 gate: any codegen diagnostic above fails the whole function
        // (partial IR is never JIT-run/emitted/linked; exit is nonzero).
        if (cgHadError) return false;
        return true;
    }

    void emitStmt(ExprAST* node) {
        // ONE error per program, on the codegen side too.
        //
        // Once codegen has reported an error, stop emitting. Continuing
        // builds IR that is no longer consistent with the source (a
        // function whose return type no longer matches its `ret`), and
        // LLVM's verifier then prints a SECOND message about the
        // compiler's internals on top of the user's real error:
        //
        //   codegen: cannot return a function value: it would escape ...
        //   Function return type does not match operand type of return inst!
        //   function body emission failed
        //
        // The user is told about the compiler's internals instead of
        // about their program. The parse side already had this contract
        // (M6); this closes the codegen side.
        if (cgHadError) return;
        if (node && node->loc.line > 0) { cgCurLine = node->loc.line; cgCurCol = node->loc.col; }
        // --debug: statement-level line attribution. IRBuilder stamps this
        // DLLocation onto every instruction it inserts until the next
        // statement changes it, so instructions map to Flint source lines.
        if (debugInfo && curDISub) {
            unsigned dl = (unsigned)(node->loc.line ? node->loc.line : 1);
            builder->SetCurrentDebugLocation(
                llvm::DebugLoc(llvm::DILocation::get(*ctx, dl, 1, curDISub)));
        }
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
        // One error per program: after the first codegen error, emit nothing
        // further (see emitStmt for why).
        if (cgHadError) return nullptr;
        // A null node is how a failed subexpression propagates. Every case
        // below switches on node->kind, so stop here instead of crashing.
        if (!node) return nullptr;
        if (node->loc.line > 0) { cgCurLine = node->loc.line; cgCurCol = node->loc.col; }
        // --debug: block-tail statements reach here via emitBlockExpr's
        // emitExpr path (bypassing emitStmt), so the line stamp lives here
        // too. Multi-line expressions map to their own lines, which is
        // still correct stepping.
        if (debugInfo && curDISub && node && node->loc.line) {
            unsigned el = (unsigned)node->loc.line;
            builder->SetCurrentDebugLocation(
                llvm::DebugLoc(llvm::DILocation::get(*ctx, el, 1, curDISub)));
        }
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
                // B1 UB audit: double->i64 outside range is UB. Keep huge
                // whole doubles as f64 (matches parse fallback to double).
                if (!std::isfinite(v) || v >= 9223372036854775807.0 ||
                    v < -9223372036854775808.0) {
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
                    if (functionMap.count(v->name)) {
                        // M4 follow-up: a named function used as a value
                        // becomes a thunk with the same env-box shape as a
                        // lambda, so the fn-value call path needs no
                        // special case.
                        llvm::Value* fv = namedFnValue(v->name);
                        if (fv) return fv;
                        return codegenError("E0080", "function '" + v->name + "' cannot be used as a value here; bind a lambda or pass it in a direct call");
                    }
                    if (genericFunctions.count(v->name))
                        return codegenError("E0083", "generic function '" + v->name +
                            "' cannot be used as a value yet; instantiate it first");
                    if (functionDefs.count(v->name))
                        return codegenError("E0080", "function '" + v->name +
                            "' cannot be used as a value here; bind a lambda or pass it in a direct call");
                    // Suggest the closest name in scope: a typo is one or
                    // two edits away from a real binding.
                    std::string near = closestName(v->name, symTable.visibleNames());
                    return codegenError("E0121", std::string("undefined var '") + v->name + "'", near);
                }
                if (sym->moved) {
                    return codegenError("E0134", std::string("use of moved variable '") + v->name + "'");
                }
                // First-class fn value (M4): the binding IS an env box
                // {fnptr, cap0, cap1, ...} allocated in the defining frame.
                // Yield the box pointer; indirect calls pull the code pointer
                // from field 0 and pass the box as the env argument.
                if (sym->isLambda) {
                    return builder->CreatePointerCast(
                        sym->lambdaEnv,
                        llvmType(sym->type, *ctx, mod.get()),
                        v->name + "_fnval");
                }
                if (sym->borrowCount > 0 && !sym->type.isCopyType()) {
                    return codegenError("E0036", std::string("cannot move '") + v->name + "' while borrowed");
                }
                llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
                return builder->CreateLoad(llvmType(sym->type, *ctx, mod.get()), ptr, v->name.c_str());
            }
            case NodeKind::Lambda: {
                // Inline lambda in value position (M4): outline, snapshot the
                // caps into a fresh env box allocated in the CURRENT frame,
                // and yield the box pointer. Escaping uses are still refused:
                // the box dies with this frame, so returning it or storing it
                // into a global are errors (checked at emitReturn / Assign).
                auto* lam = static_cast<LambdaExprAST*>(node);
                static int inlineLamCount = 0;
                std::string iname = "__inline_lam_" + std::to_string(inlineLamCount++);
                emitLambdaDef(iname, lam);
                if (cgHadError) return nullptr;
                auto* isym = symTable.lookup(iname);
                if (!isym || !isym->isLambda || !isym->lambdaEnv) {
                    return nullptr; // emitLambdaDef already reported the error
                }
                lastInlineLambdaSym = isym;
                return builder->CreatePointerCast(
                    isym->lambdaEnv,
                    llvmType(isym->type, *ctx, mod.get()),
                    "lam_val");
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
                builder->CreateCondBr(check, panicBB, okBB, coldWeightsTrue(*ctx));
                // Panic block
                builder->SetInsertPoint(panicBB);
                auto* panicFn = functionMap["flint_panic"];
                if (!panicFn) {
                    auto* fTy = llvm::FunctionType::get(voidTy, {i8PtrTy}, false);
                    panicFn = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, "flint_panic", mod.get());
                    functionMap["flint_panic"] = panicFn;
                    markColdPanicFn(panicFn);
                }
                builder->CreateCall(panicFn, {builder->CreateGlobalString("unwrap of None/Err", "unwrap_msg")});
                // flint_panic never returns, but the block still needs a
                // well-formed terminator. Emit one matching THIS function's
                // return type: `main` is i32 (C convention), other functions
                // may be i64 or void. A hardcoded `ret i64 0` made every
                // `try`/`?` inside `fn main() -> i64` fail verification.
                if (currentFunc->getReturnType()->isVoidTy()) {
                    builder->CreateRetVoid();
                } else {
                    builder->CreateRet(llvm::Constant::getNullValue(currentFunc->getReturnType()));
                }
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
                        std::string near = closestName(a->varName, symTable.visibleNames());
                        return codegenError("E0121", std::string("undefined var '") + a->varName + "'", near);
                    }
                    if (!git->second.isMutable) {
                        return codegenError("E0022", std::string("cannot assign to immutable global '") + a->varName + "'");
                    }
                    // Phase 2 / 2D: a global outlives every frame, so a
                    // capturing closure stored here is PROMOTED to the heap
                    // first; a lambda literal is created on the heap
                    // (ScopedEscape). A capture-less closure / named function
                    // already has a module-level box. This used to be an
                    // error for exactly the frame-bound case promotion now
                    // removes.
                    if (a->rhs->kind == NodeKind::Variable) {
                        auto* vn = static_cast<VariableExprAST*>(a->rhs.get());
                        Symbol* srcSym = symTable.lookup(vn->name);
                        if (srcSym && srcSym->isLambda) promoteLambdaBox(srcSym);
                    }
                    llvm::Value* val = nullptr;
                    if (a->rhs->kind == NodeKind::Lambda) {
                        ScopedEscape esc(lambdaEscapesNow);
                        val = emitExpr(a->rhs.get());
                    } else {
                        val = emitExpr(a->rhs.get());
                    }
                    if (!val) return nullptr;
                    git->second.knownArrayLenValue = nullptr;
                    return builder->CreateStore(val, git->second.global);
                }
                if (!sym->isMutable) { return codegenError("E0021", std::string("cannot assign to immutable '") + a->varName + "'"); }
                // Lambda names cannot be rebound (P0-6).
                if (sym->isLambda) {
                    return codegenError("E0040", std::string("cannot rebind lambda '") + a->varName + "'");
                }
                if (sym->borrowCount > 0) {
                    return codegenError("E0020", std::string("cannot assign to '") + a->varName + "' while borrowed");
                }
                // Phase 2 / 2D: `h = |...|` stores a fresh closure into an
                // existing binding. The literal itself does NOT escape here
                // (h is a local), so it keeps a frame box; the assignment
                // copies the box metadata onto h, and a later escape of h
                // promotes that box like any other closure binding.
                lastInlineLambdaSym = nullptr;
                auto* val = emitExpr(a->rhs.get());
                if (!val) return nullptr;
                if (a->rhs->kind == NodeKind::Lambda && lastInlineLambdaSym &&
                    sym->type.kind == TypeKind::Func) {
                    const Symbol* src = lastInlineLambdaSym;
                    sym->isLambda = true;
                    sym->lambdaFn = src->lambdaFn;
                    sym->lambdaDefinedIn = src->lambdaDefinedIn;
                    sym->lambdaParamTypes = src->lambdaParamTypes;
                    sym->lambdaCapNames = src->lambdaCapNames;
                    sym->lambdaCapTypes = src->lambdaCapTypes;
                    sym->lambdaEnv = src->lambdaEnv;
                    sym->lambdaBoxTy = src->lambdaBoxTy;
                    sym->lambdaHeapBox = src->lambdaHeapBox;
                }
                lastInlineLambdaSym = nullptr;
                if (a->rhs->kind == NodeKind::Variable) {
                    auto* varRhs = static_cast<VariableExprAST*>(a->rhs.get());
                    auto* srcSym = symTable.lookup(varRhs->name);
                    if (srcSym && !srcSym->type.isCopyType()) {
                        if (srcSym->borrowCount > 0) {
                            return codegenError("E0036", std::string("cannot move '") + varRhs->name + "' while borrowed");
                        }
                        srcSym->moved = true;
                    }
                }
                // P0-NEW-3: the binding now holds a tensor handle; index
                // refusals track existence at the binding, not the source.
                if (a->rhs->kind == NodeKind::Tensor) sym->isTensor = true;
                // Phase 2 / 2A.1: the binding may now hold a DIFFERENT
                // array, so the known length no longer holds. Clearing it is
                // the safe direction: a stale length could be longer than
                // the value's real length, which would weaken the check.
                sym->knownArrayLenValue = nullptr;
                auto* store = builder->CreateStore(val, sym->alloca);
                if (sym->moved) sym->moved = false;
                // B2: maintain time-derived set (invalidated on any non-time assign).
                if (a->rhs && a->rhs->kind == NodeKind::Call) {
                    auto* rc2 = static_cast<CallExprAST*>(a->rhs.get());
                    if (rc2->callee == "flint_time_ns" || rc2->callee == "flint_time_ns_monotonic")
                        timeVars.insert(a->varName);
                    else
                        timeVars.erase(a->varName);
                } else {
                    timeVars.erase(a->varName);
                }
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
                    return codegenError("E0069", "flint_str_concat not linked");
                }
                // Mixed str + i64/f64: convert scalar to string first
                if (b->op == '+' && l->getType()->isPointerTy() && r->getType()->isIntegerTy(64)) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto iti = functionMap.find("flint_i64_to_string");
                    if (itc != functionMap.end() && iti != functionMap.end())
                        return builder->CreateCall(itc->second, {l, builder->CreateCall(iti->second, {r})});
                    return codegenError("E0071", "flint_str_concat/flint_i64_to_string not linked");
                }
                if (b->op == '+' && l->getType()->isIntegerTy(64) && r->getType()->isPointerTy()) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto iti = functionMap.find("flint_i64_to_string");
                    if (itc != functionMap.end() && iti != functionMap.end())
                        return builder->CreateCall(itc->second, {builder->CreateCall(iti->second, {l}), r});
                    return codegenError("E0071", "flint_str_concat/flint_i64_to_string not linked");
                }
                if (b->op == '+' && l->getType()->isPointerTy() && r->getType()->isDoubleTy()) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto itf = functionMap.find("flint_f64_to_string");
                    if (itc != functionMap.end() && itf != functionMap.end())
                        return builder->CreateCall(itc->second, {l, builder->CreateCall(itf->second, {r})});
                    return codegenError("E0070", "flint_str_concat/flint_f64_to_string not linked");
                }
                if (b->op == '+' && l->getType()->isDoubleTy() && r->getType()->isPointerTy()) {
                    auto itc = functionMap.find("flint_str_concat");
                    auto itf = functionMap.find("flint_f64_to_string");
                    if (itc != functionMap.end() && itf != functionMap.end())
                        return builder->CreateCall(itc->second, {builder->CreateCall(itf->second, {l}), r});
                    return codegenError("E0070", "flint_str_concat/flint_f64_to_string not linked");
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
                            return codegenError("E0133", std::string("unsupported f64 operator '") + b->op + "'");
                    }
                }
                if (b->op == '+' || b->op == '-' || b->op == '*') {
                    if (!safety.overflow) {
                        switch (b->op) {
                            case '+': return builder->CreateAdd(l, r, "add");
                            case '-': return builder->CreateSub(l, r, "sub");
                            default:  return builder->CreateMul(l, r, "mul");
                        }
                    }
                    // B2 identity: x+0=x, 0+x=x, x-0=x, x*0=0, x*1=x, 1*x=x.
                    // Provably no overflow (result is x or 0). 0-x and x*-1
                    // CAN overflow (MIN), never elided. Local ConstantInt only.
                    if (auto* lc = llvm::dyn_cast<llvm::ConstantInt>(l)) {
                        if (b->op == '+' && lc->isZero()) return r;
                        if (b->op == '*' && lc->isZero()) return llvm::ConstantInt::get(i64Ty, 0);
                        if (b->op == '*' && lc->isOne()) return r;
                    }
                    if (auto* rc = llvm::dyn_cast<llvm::ConstantInt>(r)) {
                        if ((b->op == '+' || b->op == '-') && rc->isZero()) return l;
                        if (b->op == '*' && rc->isZero()) return llvm::ConstantInt::get(i64Ty, 0);
                        if (b->op == '*' && rc->isOne()) return l;
                    }
                    // B2 time-diff: t1-t0 where both are flint_time_ns results
                    // ([0,MAX] by CLOCK_MONOTONIC/QPC contract) cannot overflow:
                    // diff in [-MAX,MAX] fits i64. Both operands must be direct
                    // time calls or vars assigned once from time calls (tracked
                    // in timeVars, invalidated on any other assign).
                    if (b->op == '-') {
                        auto isTimeExpr = [&](ExprAST* e) -> bool {
                            if (!e) return false;
                            if (e->kind == NodeKind::Call) {
                                auto* c = static_cast<CallExprAST*>(e);
                                return c->callee == "flint_time_ns" ||
                                       c->callee == "flint_time_ns_monotonic";
                            }
                            if (e->kind == NodeKind::Variable) {
                                auto* v = static_cast<VariableExprAST*>(e);
                                if (timeVars.count(v->name) == 0) return false;
                                // Borrowed vars could be mutated via & (aliasing
                                // hole): require no outstanding borrows.
                                auto* sym = symTable.lookup(v->name);
                                if (sym && sym->borrowCount > 0) return false;
                                return true;
                            }
                            return false;
                        };
                        if (isTimeExpr(b->lhs.get()) && isTimeExpr(b->rhs.get()))
                            return builder->CreateSub(l, r, "sub_notime");
                    }
                    // Phase 3 / P1: a bounded-range fact says every dynamic
                    // evaluation of this expression yields an i64 that fits.
                    // The mathematical result is that value, so the
                    // operation cannot overflow and the trap is dead.
                    // Operands are known i64 here: the f64 and mixed paths
                    // returned above.
                    if (l->getType()->isIntegerTy(64) && r->getType()->isIntegerTy(64)) {
                        auto it = rangeFacts.find(b);
                        if (it != rangeFacts.end() &&
                            it->second.lo >= INT64_MIN && it->second.hi <= INT64_MAX) {
                            switch (b->op) {
                                case '+': return builder->CreateAdd(l, r, "add");
                                case '-': return builder->CreateSub(l, r, "sub");
                                default:  return builder->CreateMul(l, r, "mul");
                            }
                        }
                    }
                    llvm::Intrinsic::ID iid;
                    safety.overflowInserted++;
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
                    builder->CreateCondBr(ov, panicBB, okBB, coldWeightsTrue(*ctx));
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
                    emitDivByZeroCheck(r);
                    return builder->CreateSDiv(l, r, "div");
                } else if (b->op == '%') {
                    emitDivByZeroCheck(r);
                    return builder->CreateSRem(l, r, "mod");
                } else if (b->op == '@') {
                    auto it = functionMap.find("fao_matmul");
                    if (it != functionMap.end()) return builder->CreateCall(it->second, {l, r});
                    return codegenError("E0059", "fao_matmul not linked");
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
                // Aggregate operands: `==` and `!=` lower to a field-by-field
                // comparison.
                //
                // ICmp on a struct is invalid IR and LLVM asserted, aborting the
                // compiler on `x == y` for any struct or enum. Comparing the
                // raw bytes with memcmp would be wrong twice over: padding is
                // indeterminate, and memcmp cannot order values.
                //
                // Field-by-field is the correct lowering and needs no runtime
                // helper and no ABI change -- the same shape the bounds check
                // already uses. It must RECURSE: a struct field that is itself
                // a struct would otherwise be compared by pointer, which is
                // silently wrong rather than loudly broken.
                // P0-NEW-1: shared comparison core (both paths).
                return emitCompareValues(c->op, l, r);
            }
            case NodeKind::Call:   return emitCall(static_cast<CallExprAST*>(node));
            case NodeKind::Array:  return emitArrayLiteral(static_cast<ArrayExprAST*>(node));
            case NodeKind::Cast:   return emitCast(static_cast<CastExprAST*>(node));
            case NodeKind::Tensor: return emitTensorLiteral(static_cast<TensorLiteralAST*>(node));
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

    // M4 follow-up: a NAMED function used as a value (`let h = inc`).
    //
    // A lambda's value is an env box {fnptr, caps...} and calls go through
    // the uniform protocol fn(env, args...). A named function has no
    // captures, so it can join that protocol with a THUNK: a generated
    // wrapper with the same shape that ignores its env argument and
    // forwards to the real function. Then the call site needs no
    // discriminator -- a named-function value and a lambda value are the
    // same kind of thing at runtime, and the existing fn-value call path
    // works unchanged.
    //
    // The thunk is generated ONCE per named function and cached, so two
    // bindings of the same function share it.
    std::unordered_map<std::string, llvm::Function*> namedFnThunks;
    std::unordered_map<std::string, Type> namedFnTypes;

    // Returns the thunk for `name`, creating it on first use. `fnTyOut` gets
    // the Flint fn type of the function (params -> ret), so the binding
    // carries the same signature the thunk implements.
    llvm::Function* namedFnThunk(const std::string& name, Type& fnTyOut) {
        auto cached = namedFnThunks.find(name);
        auto tyIt = namedFnTypes.find(name);
        if (cached != namedFnThunks.end() && tyIt != namedFnTypes.end()) {
            fnTyOut = tyIt->second;
            return cached->second;
        }
        auto defIt = functionDefs.find(name);
        if (defIt == functionDefs.end()) return nullptr;
        FunctionAST* def = defIt->second;
        auto realIt = functionMap.find(name);
        if (realIt == functionMap.end()) return nullptr;
        llvm::Function* real = realIt->second;
        llvm::FunctionType* realTy = real->getFunctionType();
        if (!realTy) return nullptr;
        if (def->typeParams.size() > 0) return nullptr; // generic: needs instantiation
        // The Flint-level signature: declared parameter types and return
        // type. Defaults are not part of it -- a value has one fixed shape.
        std::vector<Type> ps;
        for (auto& p : def->params) ps.push_back(resolveType(p.second));
        Type ret = resolveType(def->returnType);
        if (ret.kind == TypeKind::Void) return nullptr; // a void fn has no value
        fnTyOut = Type::fn_(ps, ret);
        // The thunk: fn(env, args...) -> ret. The env parameter is ignored
        // (a named function captures nothing) but its PRESENCE is what makes
        // the value indistinguishable from a lambda at the call site.
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(llvm::PointerType::get(*ctx, 0)); // env
        for (size_t i = 0; i < realTy->getNumParams(); i++)
            paramTys.push_back(realTy->getParamType(i));
        auto* thunkTy = llvm::FunctionType::get(realTy->getReturnType(), paramTys, false);
        std::string thunkName = "__thunk_" + name;
        auto* thunk = llvm::Function::Create(thunkTy, llvm::GlobalValue::InternalLinkage,
                                             thunkName, mod.get());
        functionMap[thunkName] = thunk;
        // Body: forward every argument, drop the env.
        auto* savedFunc = currentFunc;
        auto savedIP = builder->saveIP();
        currentFunc = thunk;
        auto* entry = llvm::BasicBlock::Create(*ctx, "entry", thunk);
        builder->SetInsertPoint(entry);
        std::vector<llvm::Value*> fwd;
        for (size_t i = 1; i < thunk->arg_size(); i++)
            fwd.push_back(thunk->getArg(i));
        auto* call = builder->CreateCall(realTy, real, fwd, "thunk_call");
        builder->CreateRet(call);
        currentFunc = savedFunc;
        builder->restoreIP(savedIP);
        namedFnThunks[name] = thunk;
        namedFnTypes[name] = fnTyOut;
        return thunk;
    }

    // Materializes a named function as a value: a box {fnptr} in the
    // CURRENT frame, exactly the shape a lambda's env box has, so the
    // fn-value call path can call it with no special case. The box lives in
    // this frame, so the same escape rules as lambdas apply to it.
    llvm::Value* namedFnValue(const std::string& name) {
        Type fnTy;
        llvm::Function* thunk = namedFnThunk(name, fnTy);
        if (!thunk) return nullptr;
        // Phase 2 / 2D: a named function captures nothing, so its box is a
        // MODULE-LEVEL constant -- it does not depend on any frame, and a
        // value that depends on no frame may ESCAPE.
        auto* boxTy = llvm::StructType::get(*ctx, llvm::ArrayRef<llvm::Type*>{
            llvm::PointerType::get(*ctx, 0)});
        auto* box = new llvm::GlobalVariable(*mod, boxTy, true,
                                             llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantStruct::get(boxTy, {thunk}),
                                             "fnval." + name);
        return builder->CreatePointerCast(box, llvmType(fnTy, *ctx, mod.get()),
                                           "fnval.ptr");
    }

    // Phase 2 / 2D: RAII scope for "this expression may escape". Nesting is
    // safe: the destructor restores the previous value, not `false`.
    struct ScopedEscape {
        bool& flag;
        bool saved;
        explicit ScopedEscape(bool& f) : flag(f), saved(f) { flag = true; }
        ~ScopedEscape() { flag = saved; }
    };

    // Phase 2 / 2D: move a frame-bound env box to the HEAP so its value may
    // outlive this frame. Captures were snapshotted BY VALUE into the box, so
    // copying the whole box preserves the closure exactly. Idempotent.
    //
    // Every symbol that names the same box is updated: `let h = cb` makes h
    // a second name for one box, and after promotion both must point at the
    // heap copy (the stack copy dies with the frame).
    bool promoteLambdaBox(Symbol* sym) {
        if (!sym || !sym->isLambda || !sym->lambdaEnv) return false;
        if (sym->lambdaHeapBox) return true;
        if (llvm::isa<llvm::GlobalVariable>(sym->lambdaEnv)) {
            sym->lambdaHeapBox = true; // a static box already outlives frames
            return true;
        }
        if (!sym->lambdaBoxTy) return false;
        auto mit = functionMap.find("flint_malloc");
        if (mit == functionMap.end()) return false;
        llvm::Value* old = sym->lambdaEnv;
        auto* i8ptr = llvm::PointerType::get(*ctx, 0);
        auto* size = llvm::ConstantExpr::getSizeOf(sym->lambdaBoxTy);
        auto* raw = builder->CreateCall(mit->second, {size}, "lambox.heap.raw");
        auto* heap = builder->CreateBitCast(raw, i8ptr, "lambox.heap");
        unsigned n = sym->lambdaBoxTy->getNumElements();
        for (unsigned i = 0; i < n; i++) {
            auto* src = builder->CreateStructGEP(sym->lambdaBoxTy, old, i, "lambox.old");
            auto* v = builder->CreateLoad(sym->lambdaBoxTy->getElementType(i), src, "lambox.cap");
            auto* dst = builder->CreateStructGEP(sym->lambdaBoxTy, heap, i, "lambox.new");
            builder->CreateStore(v, dst);
        }
        symTable.forEachSymbol([&](Symbol& s) {
            if (s.isLambda && s.lambdaEnv == old) {
                s.lambdaEnv = heap;
                s.lambdaHeapBox = true;
            }
        });
        return true;
    }

    // P0-6 closure conversion (re-added after 2026-09-14 wipe): outline
    // `name = |params| body` to __lam_<name>_<N>(params..., captures...).
    // Captures are evaluated NOW (by value) into hidden slots in the defining
    // function; direct calls to `name` pass user args + frozen captures.
    // Every other use of `name` is an error (Variable/Assign/Ref cases).
    void emitLambdaDef(const std::string& name, LambdaExprAST* lam) {
        if (!lam->body) { codegenError("E0046", "empty lambda body"); return; }
        static int lamCounter = 0;
        std::string lamName = "__lam_" + name + "_" + std::to_string(lamCounter++);
        // 1. Resolve captures in the defining scope (by value, snapshot now).
        //    The snapshot VALUES go straight into the env box (4).
        struct CapVal { std::string cname; Type ctype; llvm::Value* cval; };
        std::vector<CapVal> caps;
        for (auto& cn : lam->captures) {
            auto* sym = symTable.lookup(cn);
            if (!sym) { codegenError("E0033", "cannot capture unknown variable '" + cn + "'"); return; }
            if (sym->isLambda) { codegenError("E0031", "cannot capture lambda '" + cn + "'"); return; }
            if (sym->moved) { codegenError("E0032", "cannot capture moved variable '" + cn + "'"); return; }
            llvm::Type* ct = llvmType(sym->type, *ctx, mod.get());
            llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
            if (!ptr) { codegenError("E0029", "cannot capture '" + cn + "'"); return; }
            auto* v = builder->CreateLoad(ct, ptr, cn + "_cap");
            if (!sym->type.isCopyType()) {
                if (sym->borrowCount > 0) { codegenError("E0030", "cannot capture '" + cn + "' while borrowed"); return; }
                sym->moved = true;
            }
            caps.push_back({cn, sym->type, v});
        }
        // 2. Outlined function, uniform protocol: (env, user params...) -> ret.
        //    Captures ride in the env box the call site passes; the body reads
        //    them from it, so the signature is callable through a plain fn(X)
        //    indirect call at runtime.
        std::vector<llvm::Type*> paramTys;
        std::vector<Type> paramFlint;
        for (auto& p : lam->params) {
            Type pt = resolveType(p.second);
            paramFlint.push_back(pt);
            paramTys.push_back(llvmType(pt, *ctx, mod.get()));
        }
        Type retFlint = lam->retType;
        llvm::Type* retTy = (retFlint.kind == TypeKind::Void)
            ? (llvm::Type*)voidTy
            : (llvm::Type*)llvmType(retFlint, *ctx, mod.get());
        auto* envPtrTy_i8 = llvm::PointerType::get(*ctx, 0);
        std::vector<llvm::Type*> fullParamTys = {envPtrTy_i8};
        fullParamTys.insert(fullParamTys.end(), paramTys.begin(), paramTys.end());
        auto* fTy = llvm::FunctionType::get(retTy, fullParamTys, false);
        auto* fn = llvm::Function::Create(fTy, llvm::Function::InternalLinkage, lamName, mod.get());
        functionMap[lamName] = fn;
        // 3. Declare the lambda entry BEFORE emitting the body (recursion).
        Symbol entry;
        entry.type = Type::fn_(paramFlint, retFlint);
        entry.isLambda = true;
        entry.lambdaFn = fn;
        entry.lambdaDefinedIn = currentFunc;
        entry.lambdaParamTypes = paramFlint;
        for (auto& c : caps) {
            entry.lambdaCapNames.push_back(c.cname);
            entry.lambdaCapTypes.push_back(c.ctype);
        }
        // 4. The env box: {fnptr, capped values...}. Allocated in the defining
        //    frame; the lambda VALUE is this pointer.
        std::vector<llvm::Type*> boxFieldTys = {envPtrTy_i8};
        for (auto& c : caps) boxFieldTys.push_back(llvmType(c.ctype, *ctx, mod.get()));
        auto* envTy = llvm::StructType::create(*ctx, boxFieldTys, "lambox." + lamName);
        // Phase 2 / 2D: a closure that captures NOTHING does not depend on
        // the frame that created it, so its box is a MODULE-LEVEL constant
        // instead of a frame slot. A static box has no frame lifetime, so
        // such a value may ESCAPE (be returned, stored in a global, passed
        // to a callee that keeps it) -- which is what makes closures
        // usable as callbacks.
        //
        // A closure that DOES capture keeps its frame-bound box and the
        // escape rules still apply to it.
        llvm::Value* envBox = nullptr;
        bool heapBox = false;
        if (caps.empty()) {
            auto* init = llvm::ConstantStruct::get(envTy, {fn});
            envBox = new llvm::GlobalVariable(*mod, envTy, true,
                                             llvm::GlobalValue::InternalLinkage,
                                             init, "lambox." + lamName);
        } else {
            // Phase 2 / 2D: a closure created in an ESCAPING position gets
            // its box on the heap immediately -- there is no binding to
            // promote later (e.g. `return |x| x + k`). A heap box does not
            // depend on the frame that created it.
            if (lambdaEscapesNow) {
                auto mit = functionMap.find("flint_malloc");
                if (mit != functionMap.end()) {
                    auto* size = llvm::ConstantExpr::getSizeOf(envTy);
                    auto* raw = builder->CreateCall(mit->second, {size}, "lambox.heap.raw");
                    envBox = builder->CreateBitCast(
                        raw, llvm::PointerType::get(*ctx, 0), "lambox.heap");
                    heapBox = true;
                }
            }
            if (!envBox) envBox = createEntryAlloca(envTy, "lambox_" + name);
            builder->CreateStore(llvm::Constant::getNullValue(envPtrTy_i8),
                                 builder->CreateStructGEP(envTy, envBox, 0, "lambox.fnptr_slot"));
            for (size_t i = 0; i < caps.size(); i++) {
                auto* g = builder->CreateStructGEP(envTy, envBox,
                                                   1 + i, "lambox." + caps[i].cname);
                builder->CreateStore(caps[i].cval, g);
            }
            {
                auto* fslot = builder->CreateStructGEP(envTy, envBox, 0, "lambox.fn");
                builder->CreateStore(fn, fslot);
            }
        }
        entry.lambdaEnv = envBox;
        entry.lambdaBoxTy = envTy;
        entry.lambdaHeapBox = heapBox;
        if (!symTable.declare(name, entry)) {
            codegenError("E0039", "cannot rebind '" + name + "' as lambda");
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
            if (!werr.empty()) { codegenError("E0141", werr); return; }
            for (auto& u : uses) {
                if (bound.count(u)) continue;
                if (globalSymTable.count(u)) continue;
                if (functionMap.count(u)) continue;
                if (externFunctions.count(u)) continue;
                codegenError("E0130", "unknown variable '" + u + "' in lambda body");
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
        {
            // The env parameter arrives first; capture values live in it.
            auto* envArgPtr = fn->arg_begin();
            envArgPtr->setName("__env");
            size_t ci = 0;
            for (auto& c : caps) {
                auto* g = builder->CreateStructGEP(envTy, envArgPtr,
                                                   1 + ci, "env." + c.cname);
                auto* v = builder->CreateLoad(llvmType(c.ctype, *ctx, mod.get()), g,
                                              c.cname + "_value");
                auto* slot = builder->CreateAlloca(llvmType(c.ctype, *ctx, mod.get()),
                                                   nullptr, c.cname);
                builder->CreateStore(v, slot);
                Symbol cg;
                cg.type = c.ctype;
                cg.alloca = slot;
                cg.isMutable = false; // captures are read-only
                symTable.declare(c.cname, cg);
                ci++;
            }
        }
        size_t pi = 0;
        for (auto& arg : fn->args()) {
            if (pi == 0) { pi = 1; continue; } // env pointer, handled above
            std::string pName;
            Type pType;
            if (pi - 1 < lam->params.size()) {
                pName = lam->params[pi - 1].first;
                pType = paramFlint[pi - 1];
            } else {
                pName = "env_extra";
                pType = Type::i64();
            }
            auto* alloca = builder->CreateAlloca(llvmType(pType, *ctx, mod.get()), nullptr, pName);
            builder->CreateStore(&arg, alloca);
            Symbol ps;
            ps.type = pType;
            ps.alloca = alloca;
            ps.isMutable = false; // params are read-only
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
                    codegenError("E0088", "lambda body type mismatch");
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
        // P0-NEW-3: a tensor literal init carries no LLVM type of its own
        // (the handle is an opaque i64), so provenance is recorded here —
        // the only place binding and source meet.
        bool initIsTensor = decl->init && decl->init->kind == NodeKind::Tensor;
        // Phase 2 / 2D: `let h = cb` makes h a second name for cb's env box.
        // Remember the source so the new Symbol carries the box metadata
        // (needed to promote h, and to know h is a closure at all).
        const Symbol* fnAliasSrc = nullptr;
        // Emit init first to get its LLVM type (may differ from declared type for builtins)
        auto* init = emitExpr(decl->init.get());
        // M4: binding through a lambda VALUE (`let g = f`). The source
        // symbol holds the fn type; an i64-inferred variable declaration
        // would silently demote the value (the i64-or-void ladder below
        // turns a stray ptr into Type::str()). Copy the fn type across
        // instead; an explicit fn annotation is checked against it.
        if (init && decl->init->kind == NodeKind::Variable) {
            auto* vn = static_cast<VariableExprAST*>(decl->init.get());
            auto* srcSym = symTable.lookup(vn->name);
            const Symbol* gs = nullptr;
            auto gvit = globalSymTable.find(vn->name);
            if (gvit != globalSymTable.end()) gs = &gvit->second;
            const Symbol* cs = srcSym ? srcSym : gs;
            if (cs && cs->isLambda) fnAliasSrc = cs;
            // M4 follow-up: `let h = inc` binds a NAMED function. The
            // source is a function, not a symbol, so the fn type comes from
            // the function's own definition.
            if (!cs && functionMap.count(vn->name)) {
                Type fnTy;
                if (namedFnThunk(vn->name, fnTy)) {
                    Type ann = resolveType(decl->varType);
                    if (ann.kind == TypeKind::Func) {
                        if (!(ann == fnTy)) {
                            codegenError("E0024", "cannot bind function '" + vn->name +
                                "': declared signature differs from the function's signature");
                            return;
                        }
                    } else if (ann.kind != TypeKind::I64 && ann.kind != TypeKind::Void) {
                        codegenError("E0023", "cannot bind a function value to a non-fn type");
                        return;
                    }
                    rt = fnTy;
                    lty = llvmType(rt, *ctx, mod.get());
                }
            }
            if (Symbol::isFunctionValue(cs)) {
                Type ann = resolveType(decl->varType);
                if (ann.kind == TypeKind::Func) {
                    if (!(ann == cs->type)) {
                        codegenError("E0025", "cannot bind function value '" + vn->name +
                            "': declared signature differs from the value's fn type");
                        return;
                    }
                } else if (ann.kind != TypeKind::I64 && ann.kind != TypeKind::Void) {
                    codegenError("E0023", "cannot bind a function value to a non-fn type");
                    return;
                }
                rt = cs->type;
                lty = llvmType(rt, *ctx, mod.get());
            }
        }
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
        // Phase 2 / 2A.0: compare the two inference engines.
        if (auditTypes) {
            Type parseTy = resolveType(decl->varType);
            std::string pt = typeTag(parseTy), ct = typeTag(rt);
            if (pt != ct) {
                TypeDrift d;
                d.name = decl->varName;
                d.line = decl->loc.line;
                d.parseType = pt;
                d.codegenType = ct;
                d.annotated = decl->typeWasAnnotated;
                typeDrift.push_back(d);
            }
        }
        llvm::AllocaInst* alloca = createEntryAlloca(lty, decl->varName);
        symTable.declare(decl->varName, {rt, alloca, nullptr, decl->isMutable, false, 0});
        // Phase 2 / 2D: an alias of a closure binding. The value in the
        // alloca and the source's box are the same pointer; carrying the
        // metadata lets a later escape promote h, and lets the promotion
        // update both names through forEachSymbol.
        if (fnAliasSrc && fnAliasSrc->isLambda) {
            auto* dst = symTable.lookup(decl->varName);
            if (dst) {
                dst->isLambda = true;
                dst->lambdaFn = fnAliasSrc->lambdaFn;
                dst->lambdaDefinedIn = fnAliasSrc->lambdaDefinedIn;
                dst->lambdaParamTypes = fnAliasSrc->lambdaParamTypes;
                dst->lambdaCapNames = fnAliasSrc->lambdaCapNames;
                dst->lambdaCapTypes = fnAliasSrc->lambdaCapTypes;
                dst->lambdaEnv = fnAliasSrc->lambdaEnv;
                dst->lambdaBoxTy = fnAliasSrc->lambdaBoxTy;
                dst->lambdaHeapBox = fnAliasSrc->lambdaHeapBox;
            }
        }
        // Phase 2 / 2A.1: an array LITERAL has a known exact length. The
        // runtime array is allocated with exactly that many elements, so
        // the fact is exact by construction.
        if (decl->init && decl->init->kind == NodeKind::Array) {
            auto* arrLit = static_cast<ArrayExprAST*>(decl->init.get());
            // A spread element (`...other`) makes the runtime length
            // DYNAMIC, so the fact is only exact when there is no spread.
            bool hasSpread = false;
            for (bool sp : arrLit->spreadFlags) if (sp) { hasSpread = true; break; }
            if (!hasSpread) {
                auto* arrSym = symTable.lookup(decl->varName);
                if (arrSym)
                    arrSym->knownArrayLenValue =
                        llvm::ConstantInt::get(i64Ty, (uint64_t)arrLit->elements.size());
            }
        }
        if (initIsTensor) {
            auto* tref = symTable.lookup(decl->varName);
            if (tref) tref->isTensor = true;
        }
        // `arr = flint_array_alloc(n)`: the runtime length is exactly n, and
        // n is the value the user's own loop compares against -- so the
        // bounds check and the loop use the SAME value and LLVM folds the
        // check away. Local bindings only (the value must dominate the use).
        if (decl->init && decl->init->kind == NodeKind::Call && init) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(init);
            if (call && call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "flint_array_alloc" &&
                call->arg_size() == 1) {
                auto* arrSym = symTable.lookup(decl->varName);
                if (arrSym && arrSym->alloca) {
                    llvm::Value* n = call->getArgOperand(0);
                    if (n->getType()->isIntegerTy(64))
                        arrSym->knownArrayLenValue = n;
                }
            }
        }
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
        // B2: track time-derived vars for time-diff elision.
        if (decl->init && decl->init->kind == NodeKind::Call) {
            auto* cc = static_cast<CallExprAST*>(decl->init.get());
            if (cc->callee == "flint_time_ns" || cc->callee == "flint_time_ns_monotonic")
                timeVars.insert(decl->varName);
        }
    }

    // Trap a zero divisor before `sdiv`/`srem`.
//
// Integer division by zero is undefined behaviour in LLVM -- not a panic, not
// a zero result -- so `100 / 0` silently produced a garbage exit status. The
// check mirrors the overflow check beside it: inline `icmp` + branch, with
// flint_panic only in the cold block, so the hot path keeps no call.
void emitDivByZeroCheck(llvm::Value* r) {
    if (!safety.divZero) return;
    auto* panicFn = functionMap["flint_panic"];
    if (!panicFn) return;
    safety.divZeroInserted++;
    auto* curF = builder->GetInsertBlock()->getParent();
    auto* okBB = llvm::BasicBlock::Create(*ctx, "div_ok", curF);
    auto* panicBB = llvm::BasicBlock::Create(*ctx, "div_panic", curF);
    auto* isZero = builder->CreateICmpEQ(r, llvm::ConstantInt::get(r->getType(), 0), "divzero");
    builder->CreateCondBr(isZero, panicBB, okBB, coldWeightsTrue(*ctx));
    builder->SetInsertPoint(panicBB);
    builder->CreateCall(panicFn, {builder->CreateGlobalString("division by zero", "divzmsg")});
    builder->CreateBr(okBB);
    builder->SetInsertPoint(okBB);
}

// Coerce `main`'s return value to the i32 the C ABI requires.
//
// `main` is declared `i32 main(i64, i8*)` (see emitFunction), so every `return`
// in it must produce an i32. The value's type comes from inference and is NOT
// guaranteed to be an integer: a `return <void call>` (implicit last-expr
// return in `fn main() { flint_println(...) }`) yields void, and `return
// flint_array_alloc(n)` yields {ptr,i64}. Emitting `trunc` on either is invalid
// IR -- LLVM folds it to a poison constant, so main's exit status became
// garbage that happened to work. Verified by the per-function verifier gate.
//
// Numbers convert (trunc/zext/fptosi). Anything else is not a meaningful exit
// code, so it yields 0 rather than invalid IR.
llvm::Value* coerceMainReturn(llvm::Value* v) {
    if (!v) return llvm::ConstantInt::get(i32Ty, 0);
    llvm::Type* t = v->getType();
    if (t->isIntegerTy(32)) return v;
    if (t->isIntegerTy())
        return t->getIntegerBitWidth() < 32 ? builder->CreateZExt(v, i32Ty, "mainret")
                                           : builder->CreateTrunc(v, i32Ty, "mainret");
    if (t->isFloatingPointTy()) return builder->CreateFPToSI(v, i32Ty, "mainret");
    return llvm::ConstantInt::get(i32Ty, 0);
}

    void emitReturn(ReturnStmtAST* ret) {
        if (currentFunc && currentFunc->getName() == "main" && hasPython) {
            auto it = functionMap.find("flint_py_fini");
            if (it != functionMap.end()) builder->CreateCall(it->second, {});
        }
        if (ret->value) {
            // Phase 2 / 2D: a function value may cross a return boundary.
            // A capture-less closure and a named function already have a
            // module-level box. A CAPTURING closure is promoted to the heap
            // here, and a lambda literal is created on the heap (ScopedEscape
            // sets the flag emitLambdaDef reads). Captures are snapshotted by
            // value, so the heap copy IS the closure. Refusing instead (the
            // pre-2D rule) was correct only while every box was frame-bound.
            if (ret->value->kind == NodeKind::Variable) {
                auto* vn = static_cast<VariableExprAST*>(ret->value.get());
                Symbol* srcSym = symTable.lookup(vn->name);
                if (!srcSym) {
                    auto gvit = globalSymTable.find(vn->name);
                    if (gvit != globalSymTable.end()) srcSym = &gvit->second;
                }
                if (srcSym && srcSym->isLambda) promoteLambdaBox(srcSym);
            }
            llvm::Value* v = nullptr;
            if (ret->value->kind == NodeKind::Lambda) {
                ScopedEscape esc(lambdaEscapesNow);
                v = emitExpr(ret->value.get());
            } else {
                v = emitExpr(ret->value.get());
            }
            if (v) {
                if (currentFunc && currentFunc->getName() == "main") {
                    builder->CreateRet(coerceMainReturn(v));
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
        // Same discipline as match: void-typed branches can never join a
        // PHI (LLVM aborts the compiler on those), and branches that
        // disagree on value type fail loudly instead of coercing silently.
        // A missing/valueless branch still coerces to null (legacy).
        // NOTE: incoming blocks must be the arm EXIT blocks (thenLastBB /
        // elseLastBB) — using an entry block here emits a PHI whose
        // predecessor edge doesn't exist, which segfaults SimplifyCFG.
        auto valType = [](llvm::Value* v) -> llvm::Type* {
            return (v && !v->getType()->isVoidTy()) ? v->getType() : nullptr;
        };
        llvm::Type* tTy = valType(thenVal);
        llvm::Type* eTy = valType(elseVal);
        bool thenVoid = thenVal && thenVal->getType()->isVoidTy();
        bool elseVoid = elseVal && elseVal->getType()->isVoidTy();
        if ((thenVoid && eTy) || (elseVoid && tTy)) {
            return codegenError("E0084", "if branches must yield the same type");
        }
        llvm::Type* phiTy = tTy ? tTy : eTy;
        if (!phiTy) return llvm::ConstantInt::get(i64Ty, 0);
        if (tTy && eTy && tTy != eTy) {
            return codegenError("E0084", "if branches must yield the same type");
        }
        auto* phi = builder->CreatePHI(phiTy, 2, "ifval");
        llvm::Value* tIn = tTy ? thenVal : llvm::Constant::getNullValue(phiTy);
        llvm::Value* eIn = eTy ? elseVal : llvm::Constant::getNullValue(phiTy);
        phi->addIncoming(tIn, thenLastBB);
        phi->addIncoming(eIn, elseLastBB ? elseLastBB : mergeBB);
        return phi;
    }

    // Phase 2 / 2C: the loop-range context.
    //
    // A recognised loop gives the accesses in its body a RANGE fact about
    // the induction variable, and the fact is handed to LLVM as an
    // assumption. It is what lets the optimizer fold a bounds check the
    // compiler cannot see through: for `while i * i <= limit`, LLVM cannot
    // derive `i <= limit` (the multiply may overflow in its reasoning),
    // but the DERIVATION is trivial and sound: i*c <= n with c >= 1 implies
    // i <= n, for negative i as well (i*c <= i when c >= 1 and i < 0).
    //
    // The preconditions are the whole point, because an unsound assumption
    // is a MISCOMPILATION:
    //   * the condition shape must be recognised exactly;
    //   * the induction variable must not be modified before the access
    //     (the first assignment to it clears the fact);
    //   * the induction variable must not have its address taken;
    //   * the fact is only used for accesses that read the variable
    //     itself.
    // If any of these does not hold, NOTHING is emitted.
    struct LoopRange {
        std::string var;              // the induction variable
        llvm::Value* bound = nullptr; // the bound's SSA value
        bool strict = false;          // true: i < bound, false: i <= bound
        bool active = false;          // cleared at the first assignment to i
    };
    std::vector<LoopRange> loopRanges;

    // The induction variable and the bound of a recognised loop condition,
    // or empty. `strict` is true for `i < n`, false for the derived
    // `i * c <= n` form (which yields the weaker i <= n).
    struct LoopShape {
        bool ok = false;
        std::string var;
        bool strict = false;
        ExprAST* bound = nullptr;
    };
    LoopShape loopShapeOf(ExprAST* cond) {
        LoopShape sh;
        if (!cond || cond->kind != NodeKind::Compare) return sh;
        auto* cmp = static_cast<CompareExprAST*>(cond);
        ExprAST* lhs = cmp->lhs.get();
        ExprAST* rhs = cmp->rhs.get();
        bool strict = (cmp->op == "<");
        bool weak = (cmp->op == "<=");
        if (!strict && !weak) return sh;
        if (lhs && lhs->kind == NodeKind::Variable) {
            sh.var = static_cast<VariableExprAST*>(lhs)->name;
            sh.bound = rhs;
            sh.strict = strict;
            sh.ok = true;
            return sh;
        }
        // The derived form: `i * c <= n` with c a POSITIVE CONSTANT.
        // i*c <= n and c >= 1 imply i <= n (for i < 0: i*c <= i <= n).
        if (lhs && lhs->kind == NodeKind::Binary) {
            auto* b = static_cast<BinaryExprAST*>(lhs);
            if (b->op == '*' && b->lhs && b->rhs) {
                ExprAST* varE = nullptr;
                ExprAST* cstE = nullptr;
                if (b->lhs->kind == NodeKind::Variable &&
                    b->rhs->kind == NodeKind::Number)
                    { varE = b->lhs.get(); cstE = b->rhs.get(); }
                else if (b->rhs->kind == NodeKind::Variable &&
                         b->lhs->kind == NodeKind::Number)
                    { varE = b->rhs.get(); cstE = b->lhs.get(); }
                if (varE && cstE) {
                    auto* num = static_cast<NumberExprAST*>(cstE);
                    if (num->isInt && num->intValue >= 1) {
                        sh.var = static_cast<VariableExprAST*>(varE)->name;
                        sh.bound = rhs;
                        sh.strict = false;   // i <= n
                        sh.ok = true;
                    }
                }
            }
        }
        return sh;
    }

    // Does the body take the address of the induction variable, or assign
    // to it? Either makes the range fact unusable (an assignment changes
    // the value; an address allows an alias to change it).
    bool bodyBreaksRange(ExprAST* body, const std::string& var, size_t* firstAssign) {
        if (firstAssign) *firstAssign = (size_t)-1;
        bool bad = false;
        std::function<void(ExprAST*)> walk = [&](ExprAST* e) {
            if (!e || bad) return;
            switch (e->kind) {
                case NodeKind::Assign: {
                    auto* a = static_cast<AssignExprAST*>(e);
                    if (a->varName == var) {
                        if (firstAssign && *firstAssign == (size_t)-1) {
                            // the index of the statement, filled in by the caller
                        }
                        bad = true;
                    }
                    walk(a->rhs.get());
                    break;
                }
                case NodeKind::Ref: {
                    auto* r = static_cast<RefExprAST*>(e);
                    if (r->target && r->target->kind == NodeKind::Variable &&
                        static_cast<VariableExprAST*>(r->target.get())->name == var)
                        bad = true;
                    walk(r->target.get());
                    break;
                }
                case NodeKind::Binary: {
                    auto* b = static_cast<BinaryExprAST*>(e);
                    walk(b->lhs.get()); walk(b->rhs.get()); break;
                }
                case NodeKind::Compare: {
                    auto* c = static_cast<CompareExprAST*>(e);
                    walk(c->lhs.get()); walk(c->rhs.get()); break;
                }
                case NodeKind::Index: {
                    auto* i2 = static_cast<IndexExprAST*>(e);
                    walk(i2->base.get()); walk(i2->index.get()); break;
                }
                case NodeKind::Call: {
                    auto* c = static_cast<CallExprAST*>(e);
                    for (auto& a : c->args) walk(a.get());
                    break;
                }
                case NodeKind::Block: {
                    auto* b = static_cast<BlockStmtAST*>(e);
                    for (auto& s : b->stmts) walk(s.get());
                    break;
                }
                case NodeKind::VarDecl: {
                    auto* v = static_cast<VarDeclAST*>(e);
                    walk(v->init.get()); break;
                }
                default: break;
            }
        };
        walk(body);
        return bad;
    }

    // Does this statement assign to `name`? (The first assignment to the
    // induction variable invalidates the range fact.)
    static bool assignsTo(ExprAST* e, const std::string& name) {
        if (!e) return false;
        if (e->kind == NodeKind::Assign) {
            auto* a = static_cast<AssignExprAST*>(e);
            if (a->varName == name) return true;
            return assignsTo(a->rhs.get(), name);
        }
        if (e->kind == NodeKind::Block) {
            auto* b = static_cast<BlockStmtAST*>(e);
            for (auto& s : b->stmts) if (assignsTo(s.get(), name)) return true;
            return false;
        }
        if (e->kind == NodeKind::Binary) {
            auto* b = static_cast<BinaryExprAST*>(e);
            return assignsTo(b->lhs.get(), name) || assignsTo(b->rhs.get(), name);
        }
        return false;
    }

    // Emits the range fact for an access indexed by the induction
    // variable, as an LLVM assumption. Called ONLY where the index
    // expression is the variable itself.
    void emitLoopRangeAssume(ExprAST* indexExpr) {
        if (loopRanges.empty()) return;
        LoopRange& r = loopRanges.back();
        if (!r.active || !r.bound) return;
        if (!indexExpr || indexExpr->kind != NodeKind::Variable) return;
        if (static_cast<VariableExprAST*>(indexExpr)->name != r.var) return;
        Symbol* sym = symTable.lookup(r.var);
        if (!sym || !sym->alloca) return;
        llvm::Value* iv = builder->CreateLoad(i64Ty, sym->alloca, "loop_iv");
        llvm::Value* cmp = r.strict
            ? (llvm::Value*)builder->CreateICmpSLT(iv, r.bound, "loop_range")
            : (llvm::Value*)builder->CreateICmpULE(iv, r.bound, "loop_range");
        auto* assumeFn = llvm::Intrinsic::getDeclaration(mod.get(), llvm::Intrinsic::assume);
        builder->CreateCall(assumeFn, {cmp});
    }

    void emitWhile(WhileStmtAST* ws) {
        auto* condBB = llvm::BasicBlock::Create(*ctx, "while_cond", currentFunc);
        auto* bodyBB = llvm::BasicBlock::Create(*ctx, "while_body", currentFunc);
        auto* endBB = llvm::BasicBlock::Create(*ctx, "while_end", currentFunc);

        builder->CreateBr(condBB);

        builder->SetInsertPoint(condBB);
        // Phase 2 / 2C: a recognised loop condition is built here (rather
        // than through emitExpr) so the BOUND's SSA value is available for
        // the range assumption. The comparison itself is the same one the
        // generic path would build.
        LoopShape sh = loopShapeOf(ws->condition.get());
        llvm::Value* boundVal = nullptr;
        llvm::Value* cond = nullptr;
        if (sh.ok) {
            // lhs = the induction variable, rhs = the bound
            ExprAST* lhsE = nullptr;
            if (ws->condition && ws->condition->kind == NodeKind::Compare) {
                auto* c = static_cast<CompareExprAST*>(ws->condition.get());
                lhsE = c->lhs.get();
            }
            llvm::Value* lhsVal = lhsE ? emitExpr(lhsE) : nullptr;
            boundVal = sh.bound ? emitExpr(sh.bound) : nullptr;
            if (!lhsVal || !boundVal) {
                cond = emitExpr(ws->condition.get());
                boundVal = nullptr;
            } else {
                cond = sh.strict ? builder->CreateICmpSLT(lhsVal, boundVal, "loop_cond")
                                 : builder->CreateICmpULE(lhsVal, boundVal, "loop_cond");
            }
        } else {
            cond = emitExpr(ws->condition.get());
        }
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
        // Phase 2 / 2C: the range fact, if the loop shape was recognised
        // AND the body does not assign to the induction variable or take
        // its address (either would make the fact stale).
        LoopRange range;
        bool haveRange = false;
        if (sh.ok && boundVal && !bodyBreaksRange(ws->body.get(), sh.var, nullptr)) {
            range.var = sh.var;
            range.bound = boundVal;
            range.strict = sh.strict;
            range.active = true;
            haveRange = true;
        }
        if (haveRange) loopRanges.push_back(range);
        if (haveRange) {
            // Emit the body statement by statement so the fact can be
            // cleared at the FIRST assignment to the induction variable.
            if (auto* blk = dynamic_cast<BlockStmtAST*>(ws->body.get())) {
                for (size_t i = 0; i < blk->stmts.size(); i++) {
                    if (i > 0) {
                        // an assignment before this statement invalidates it
                        if (assignsTo(blk->stmts[i].get(), sh.var))
                            loopRanges.back().active = false;
                    }
                    emitStmt(blk->stmts[i].get());
                }
            } else {
                emitStmt(ws->body.get());
            }
        } else {
            emitStmt(ws->body.get());
        }
        if (haveRange) loopRanges.pop_back();
        // Loop fixpoint for move state: a binding declared outside the loop and
        // moved inside the body is re-initialised on the next iteration, so the
        // move must not survive the iteration boundary.
        symTable.reviveMoved();
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
            return codegenError("E0094", "map runtime not registered");
        }
        llvm::Value* m = builder->CreateCall(itNew->second, {}, "map_new");
        for (size_t i = 0; i < ml->keys.size(); i++) {
            llvm::Value* val = emitExpr(ml->values[i].get());
            if (!val) return nullptr;
            // Ensure i64 (bool is i64-typed already)
            if (val->getType()->isDoubleTy()) {
                return codegenError("E0095", "map values must be i64 (got f64)");
            }
            llvm::Value* keyStr = builder->CreateGlobalString(ml->keys[i], "mapkey");
            builder->CreateCall(itSet->second, {m, keyStr, val});
        }
        return m;
    }

    // Phase-2 `as` cast codegen: direct sitofp/fptosi (no runtime call).
    // Same-type casts are a no-op passthrough (emitting sitofp i64->i64
    // would be invalid IR).
    // f64->i64 safety: fptosi on NaN/+-Inf is poison (surfaced as garbage
    // values), so a non-finite check panics first with a clear message;
    // finite values outside [-(2^63), 2^63) have no i64 image, so an
    // overflow check panics second ("cast overflow"). Cost: 3 fcmps (uno
    // + 2 oeq) + 2 ors + 1 br, then 2 fcmps (oge + olt) + 1 or + 1 br per
    // f64->i64 cast; the panic calls live only in the cold blocks, so the
    // hot path keeps no call (same inline-split shape as bounds/div
    // checks). f64 cannot represent anything in (2^63 - 1024, 2^63), so
    // OGE against exactly 2^63 misses no representable value below it;
    // exactly -(2^63) itself is in range (ocr fptosi -> INT64_MIN).
    llvm::Value* emitCast(CastExprAST* c) {
        auto* v = emitExpr(c->expr.get());
        if (!v) return nullptr;
        bool wantF64 = c->target.kind == TypeKind::F64;
        bool wantI64 = c->target.kind == TypeKind::I64;
        if (!wantF64 && !wantI64)
            return codegenError("E0013", "as-cast target must be 'i64' or 'f64'");
        llvm::Type* t = v->getType();
        auto* dblTy = llvm::Type::getDoubleTy(*ctx);
        if (wantF64) {
            if (t->isDoubleTy()) return v;
            if (t->isIntegerTy(64))
                return builder->CreateSIToFP(v, dblTy, "as_f64");
            return codegenError("E0011", "as f64 needs an i64 or f64 value");
        }
        if (t->isIntegerTy(64)) return v;
        if (t->isDoubleTy()) {
            auto* panicFn = functionMap["flint_panic"];
            if (panicFn) {
                auto* curF = builder->GetInsertBlock()->getParent();
                auto* okBB = llvm::BasicBlock::Create(*ctx, "cast_ok", curF);
                auto* panicBB = llvm::BasicBlock::Create(*ctx, "cast_panic", curF);
                auto* zero = llvm::ConstantFP::get(dblTy, 0.0);
                auto* posInf = llvm::ConstantFP::getInfinity(dblTy, false);
                auto* negInf = llvm::ConstantFP::getInfinity(dblTy, true);
                auto* isNan = builder->CreateFCmpUNO(v, zero, "cast_nan");
                auto* isPosInf = builder->CreateFCmpOEQ(v, posInf, "cast_posinf");
                auto* isNegInf = builder->CreateFCmpOEQ(v, negInf, "cast_neginf");
                auto* isInf = builder->CreateOr(isPosInf, isNegInf, "cast_inf");
                auto* bad = builder->CreateOr(isNan, isInf, "cast_nonfinite");
                builder->CreateCondBr(bad, panicBB, okBB, coldWeightsTrue(*ctx));
                builder->SetInsertPoint(panicBB);
                builder->CreateCall(panicFn,
                    {builder->CreateGlobalString("f64 to i64 cast of non-finite value", "castmsg")});
                builder->CreateBr(okBB);
                builder->SetInsertPoint(okBB);
                auto* two63 = llvm::ConstantFP::get(dblTy, 9223372036854775808.0);
                auto* geBig = builder->CreateFCmpOGE(v, two63, "cast_ge_2p63");
                auto* ltNeg = builder->CreateFCmpOLT(
                    v, llvm::ConstantFP::get(dblTy, -9223372036854775808.0), "cast_lt_neg2p63");
                auto* oob = builder->CreateOr(geBig, ltNeg, "cast_oob");
                auto* oobBB = llvm::BasicBlock::Create(*ctx, "cast_oob_panic", curF);
                auto* finBB = llvm::BasicBlock::Create(*ctx, "cast_finite", curF);
                builder->CreateCondBr(oob, oobBB, finBB, coldWeightsTrue(*ctx));
                builder->SetInsertPoint(oobBB);
                builder->CreateCall(panicFn,
                    {builder->CreateGlobalString("f64 to i64 cast overflow", "oobmsg")});
                builder->CreateBr(finBB);
                builder->SetInsertPoint(finBB);
            }
            return builder->CreateFPToSI(v, i64Ty, "as_i64");
        }
        return codegenError("E0012", "as i64 needs an i64 or f64 value");
    }

    // Phase-2 tensor literal codegen: desugar to the existing builtins
    // flint_create_1d/2d + flt_set (flat row-major). No runtime change.
    llvm::Value* emitTensorLiteral(TensorLiteralAST* t) {
        auto itC1 = functionMap.find("flt_create_1d");
        auto itC2 = functionMap.find("flt_create_2d");
        auto itSet = functionMap.find("flt_set");
        if (itC1 == functionMap.end() || itC2 == functionMap.end() ||
            itSet == functionMap.end())
            return codegenError("E0114", "tensor runtime not registered");
        llvm::Value* ten = nullptr;
        auto* dblTy = llvm::Type::getDoubleTy(*ctx);
        if (t->rank == 1) {
            ten = builder->CreateCall(itC1->second,
                {llvm::ConstantInt::get(i64Ty, (uint64_t)t->rows)}, "tensor1");
        } else {
            ten = builder->CreateCall(itC2->second,
                {llvm::ConstantInt::get(i64Ty, (uint64_t)t->rows),
                 llvm::ConstantInt::get(i64Ty, (uint64_t)t->cols)}, "tensor2");
        }
        for (size_t i = 0; i < t->values.size(); i++) {
            builder->CreateCall(itSet->second,
                {ten, llvm::ConstantInt::get(i64Ty, i),
                 llvm::ConstantFP::get(dblTy, t->values[i])});
        }
        return ten;
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
            // Same guard as Parser::emitArrayLiteralEmit: element types are
            // erased at the LLVM boundary, so a non-i64 element would be
            // reinterpreted as bits and read back as a wrong number with no
            // error. Both emit paths must agree, or the same program compiles
            // under one and fails under the other.
            if (val->getType() != i64Ty) {
                return codegenError("E0010", "array literals must hold i64 values, but element " +
                                    std::to_string(i + 1) + " is " +
                                    llvmTypeToString(val->getType()) +
                                    " (arrays of other types are not supported yet)");
            }
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

    // Bounds trap: inline `icmp ult` on the hot path, call the panic helper
    // only in the cold block. Same helper, same args, same trap message
    // (unsigned wrap catches negatives); the call simply leaves the hot
    // path. An opaque call here would block inlining, unrolling and
    // loop vectorization, so every array path must use this shape --
    // `a[i]` and `flint_array_get/set` included.
    void emitBoundsCheckInline(llvm::Value* idxVal, llvm::Value* len) {
        // --unsafe removes the bounds check as well as the overflow check:
        // a system programmer who has proven an index in range needs a way
        // to say so. The check is the ONLY thing standing between a
        // deliberate out-of-range access and a silent read of whatever is
        // there, so the opt-out is explicit (a build flag, never a
        // profile), visible (a warning), and auditable (the artifact is
        // stamped "flint:safety=unsafe").
        if (!safety.bounds) return;
        auto* boundsFn = functionMap["flint_bounds_check"];
        if (!boundsFn) return;
        safety.boundsInserted++;
        auto* curF = builder->GetInsertBlock()->getParent();
        auto* okBB = llvm::BasicBlock::Create(*ctx, "bounds_ok", curF);
        auto* panicBB = llvm::BasicBlock::Create(*ctx, "bounds_panic", curF);
        auto* ok = builder->CreateICmpULT(idxVal, len, "bounds_ok");
        builder->CreateCondBr(ok, okBB, panicBB, coldWeightsFalse(*ctx));
        builder->SetInsertPoint(panicBB);
        builder->CreateCall(boundsFn, {idxVal, len});
        builder->CreateBr(okBB);
        builder->SetInsertPoint(okBB);
    }

    // Bounds trap for the runtime-fn array builtins (`flint_array_get/set`).
    // Same inline split as `a[i]` -- see emitBoundsCheckInline.
    void emitLoopBoundsCheck(llvm::Value* idxVal, llvm::Value* len) {
        emitBoundsCheckInline(idxVal, len);
    }

    // Does this value DOMINATE the current insertion point?
    //
    // A fact that carries an SSA value (the argument of the
    // flint_array_alloc call) may only be used where that value is
    // available. A value defined inside a loop and used after the loop is
    // not available there, and using it would be invalid IR (LLVM would
    // reject the module -- or worse, read a stale register).
    //
    // The test is exact: a constant dominates everything, an instruction
    // dominates the point if it dominates the containing block and comes
    // before the insertion point inside it.
    bool valueDominatesHere(llvm::Value* v) {
        auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
        if (!inst) return true;              // constants, globals, arguments
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb) return false;
        llvm::Function* f = bb->getParent();
        if (!f || inst->getFunction() != f) return false;
        if (inst->getParent() == bb) {
            // Same block: it must come BEFORE the insertion point.
            for (auto& i : *bb) {
                if (&i == &*builder->GetInsertPoint()) return false;
                if (&i == inst) return true;
            }
            return false;
        }
        llvm::DominatorTree dt(*f);
        return dt.dominates(inst, bb);
    }

    // The length fact of an argument expression, when the argument is a
    // binding that holds an array whose length the compiler knows
    // (Phase 2 / 2A.1). null when the length is not known.
    llvm::Value* knownLenOfArg(ExprAST* arg) {
        if (!arg || arg->kind != NodeKind::Variable) return nullptr;
        auto* vn = static_cast<VariableExprAST*>(arg);
        Symbol* sym = symTable.lookup(vn->name);
        if (sym && sym->knownArrayLenValue) return sym->knownArrayLenValue;
        auto git = globalSymTable.find(vn->name);
        if (git != globalSymTable.end() && git->second.knownArrayLenValue)
            return git->second.knownArrayLenValue;
        return nullptr;
    }

    llvm::Value* emitIndexAccess(IndexExprAST* idx) {
        llvm::Value* base = emitExpr(idx->base.get());
        llvm::Value* index = emitExpr(idx->index.get());
        if (!base || !index) return nullptr;
        // Phase 2 / 2C: the loop-range fact for this access.
        emitLoopRangeAssume(idx->index.get());
        // The exact length of the base, when the compiler knows it.
        llvm::Value* knownLenValue = nullptr;

        // Map read: `m["k"]` / `m[k]`. Map keys are strings (LLVM ptr) while
        // array/str indices are i64, so the index type discriminates (map and
        // str bases are both LLVM ptr and otherwise indistinguishable).
        // Same call as `m.get(k)` (flint_map_get); missing keys report via
        // flint_set_err exactly like the method form.
        if (index->getType()->isPointerTy()) {
            if (base->getType()->isStructTy())
                return codegenError("E0009", "array index needs i64 (got str; map keys are strings)");
            auto it = functionMap.find("flint_map_get");
            if (it == functionMap.end())
                return codegenError("E0094", "map runtime not registered");
            return builder->CreateCall(it->second, {base, index}, "map_get");
        }
        // D1: string vs array — array is {ptr,i64}, string is ptr.
        // m[0]: a statically-known map base with an i64 key is a loud
        // compile-time error (map keys are strings); without static type
        // info the ptr bases are indistinguishable, so only known-map
        // bases get this error and everything else keeps the str-GEP path
        // (dynamic `f()[0]` / `(x)[0]` remains the documented gap).
        // Same lookup as the fa_* method lowering below.
        bool baseIsMap = false;
        ExprAST* be = idx->base.get();
        if (be->kind == NodeKind::MapLiteral) baseIsMap = true;
        else if (be->kind == NodeKind::Variable) {
            auto* bv = static_cast<VariableExprAST*>(be);
            auto* sym = symTable.lookup(bv->name);
            if (sym && sym->type.kind == TypeKind::Map) baseIsMap = true;
            else {
                auto git = globalSymTable.find(bv->name);
                if (git != globalSymTable.end() && git->second.type.kind == TypeKind::Map)
                    baseIsMap = true;
            }
            // P0-NEW-3 (B1 narrow stop-gap): a tensor handle is an opaque
            // i64. a[0] used to GEP float bits and print garbage (48/16)
            // with no diagnostic. The AST type system has no tensor kind,
            // so refuse on the flag the binding carries instead.
            if (sym && sym->isTensor) {
                return codegenError("E0115", "tensor values cannot be indexed with []; use flt_get(t, i)");
            }
            auto gsym2 = globalSymTable.find(bv->name);
            if (gsym2 != globalSymTable.end() && gsym2->second.isTensor)
                return codegenError("E0115", "tensor values cannot be indexed with []; use flt_get(t, i)");
            // The known length, when the compiler has it (Phase 2 / 2A.1).
            if (sym && sym->knownArrayLenValue) knownLenValue = sym->knownArrayLenValue;
            else if (gsym2 != globalSymTable.end() && gsym2->second.knownArrayLenValue)
                knownLenValue = gsym2->second.knownArrayLenValue;
        }
        if (index->getType()->isIntegerTy(64) && baseIsMap)
            return codegenError("E0093", "map index needs a str key (map keys are strings)");
        bool isArray = base->getType()->isStructTy();
        if (isArray) {
            llvm::Value* dataPtr = builder->CreateExtractValue(base, {0}, "arr_ptr");
            llvm::Value* len = builder->CreateExtractValue(base, {1}, "arr_len");
            // Phase 2 / 2A.1 -- use the KNOWN length when the compiler
            // has it: a constant for a literal array, the alloc's argument
            // for a runtime array. The check then compares the index
            // against the same value the user's loop uses, and LLVM folds
            // the check away. With the length loaded from the slice, the
            // bound and the loop condition were unrelated values and the
            // check always survived (measured: 3 of 6 checks survive in
            // sum_array).
            //
            // The fact is EXACT, so this is the same check, not a weaker
            // one -- and the safety direction is tested: an index that is
            // not proven in range is still checked.
            // Only use the fact if its value is available here.
            if (knownLenValue && valueDominatesHere(knownLenValue))
                len = knownLenValue;
            // The "R2 BCE-lite" proof, implemented for real this time (the
            // flag existed for years and was never set): a LITERAL index into
            // an array whose length is a KNOWN CONSTANT is provably in
            // bounds, so the check is unnecessary.
            if (!idx->unchecked && knownLenValue &&
                llvm::isa<llvm::ConstantInt>(knownLenValue) &&
                idx->index->kind == NodeKind::Number) {
                auto* num = static_cast<NumberExprAST*>(idx->index.get());
                auto* lenConst = llvm::cast<llvm::ConstantInt>(knownLenValue);
                if (!num->isFloat && num->isInt && num->intValue >= 0 &&
                    num->intValue < lenConst->getSExtValue())
                    idx->unchecked = true;
            }
            if (!idx->unchecked) {
                emitBoundsCheckInline(index, len);
            }
            llvm::Value* elemPtr = builder->CreateGEP(i64Ty, dataPtr, index, "arr_elem");
            return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
        } else {
            // string: ptr + i64 index → i8 load → zext i64 (char code)
            auto* lenFn = functionMap["flint_str_length"];
            llvm::Value* len = nullptr;
            if (lenFn) len = builder->CreateCall(lenFn, {base}, "str_len");
            else len = llvm::ConstantInt::get(i64Ty, 0);
            if (!idx->unchecked && len) {
                emitBoundsCheckInline(index, len);
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
        if (!sliceFn) { return codegenError("E0067", "flint_array_slice not linked"); }
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
            if (!sym) {
                // The suggestion needs the names in scope, which only the
                // codegen has (the parser is single-pass).
                std::string near = closestName(var->name, symTable.visibleNames());
                return codegenError("E0121", std::string("undefined var '") + var->name + "'", near);
            }
                if (sym->moved) {
                    return codegenError("E0027", std::string("cannot borrow moved variable '") + var->name + "'");
                }
            if (sym->isLambda) {
                return codegenError("E0026", std::string("cannot borrow lambda '") + var->name + "'");
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
            return codegenError("E0128", std::string("unknown struct '") + sl->structName + "'");
        }
        auto& def = it->second;
        llvm::Type* st = llvmType(Type::struct_(sl->structName), *ctx, mod.get());
        if (!st) { return codegenError("E0111", std::string("struct type not found '") + sl->structName + "'"); }
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
                return codegenError("E0098", std::string("missing field '") + def.fields[i].name + "' in struct literal");
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
                return codegenError("E0110", std::string("struct '") + name + "' has no field '" + fa->fieldName + "'");
            }
        }
        return codegenError("E0019", "cannot access field '" + fa->fieldName + "' on non-struct type");
    }

    llvm::Value* emitEnumConstruct(EnumConstructAST* ec) {
        auto it = enumRegistry.find(ec->enumName);
        if (it == enumRegistry.end()) { return codegenError("E0123", std::string("unknown enum '") + ec->enumName + "'"); }
        auto& ed = it->second;
        llvm::Type* enumTy = llvmType(Type::enum_(ec->enumName), *ctx, mod.get());
        // Find variant index
        int tag = -1;
        for (size_t i = 0; i < ed.variants.size(); i++) {
            if (ed.variants[i].name == ec->variantName) { tag = (int)i; break; }
        }
        if (tag < 0) { return codegenError("E0131", std::string("unknown variant '") + ec->variantName + "'"); }

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

    // Structural equality for struct and enum operands.
    //
    // Returns an i1. For an enum the tag is compared first and, when it
    // matches, the payload fields as well -- two different variants of the
    // same enum must not compare equal just because their payload bytes
    // happen to match. Recursion carries `depth` so a malformed self-referential
    // type cannot loop forever; Flint has no recursive struct values today, so
    // the cap is a guard rather than a real limit.
    // P0-NEW-2: enum equality without garbage-byte answers.
    //
    // An enum value is `{ i8, [N x i8] }` (tag + payload sized to the WIDEST
    // variant). Bytes a variant never writes are garbage, so byte-comparing
    // the payload array reported two identical `Pair.Some2(3, 4)` values as
    // UNEQUAL — which is why enum equality was refused rather than answered
    // wrongly. The refuse would still be the safe choice, but the width is
    // now threadable: the variant struct layout `{ i8, T0, T1, ... }` is
    // exactly the layout the construction path writes, so compare the TAG,
    // then — once the tags match — dispatch on the tag and compare THAT
    // variant's payload field by field, typed GEPs only. Unwritten tail
    // bytes are never read, and two identical payloads compare equal.
    llvm::Value* emitEnumEqual(llvm::Value* l, llvm::Value* r, EnumDef& ed, int depth) {
        if (!l || !r) return nullptr;
        llvm::Type* enumTy = l->getType();
        auto* tag0 = builder->CreateExtractValue(l, {0}, "eq_tag_l");
        auto* tag1 = builder->CreateExtractValue(r, {0}, "eq_tag_r");
        auto* tagEq = builder->CreateICmpEQ(tag0, tag1, "eq_tags");

        auto* allocaL = createEntryAlloca(enumTy, "eqcmp_l");
        auto* allocaR = createEntryAlloca(enumTy, "eqcmp_r");
        builder->CreateStore(l, allocaL);
        builder->CreateStore(r, allocaR);
        // Result accumulates as i64; a variant body ANDs its payload
        // comparison into it. Tags unequal or an unknown tag leave 0
        // (unequal) — never garbage-driven.
        auto* resultSlot = createEntryAlloca(i64Ty, "eqcmp_r1");
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), resultSlot);

        // Tags equal -> payload dispatch. Tags differ -> skip straight to
        // the end, where resultSlot still holds 0 (unequal).
        auto* bbDispatch = llvm::BasicBlock::Create(*ctx, "eq_dispatch", currentFunc);
        auto* bbEnd = llvm::BasicBlock::Create(*ctx, "eq_end", currentFunc);
        builder->CreateCondBr(tagEq, bbDispatch, bbEnd);

        // Tag chain: one block per variant; a matching tag falls into its
        // compare body, otherwise the chain moves to the next variant.
        std::vector<llvm::BasicBlock*> condBBs;
        for (size_t k = 0; k < ed.variants.size(); k++)
            condBBs.push_back(llvm::BasicBlock::Create(*ctx,
                "eq_try_" + ed.variants[k].name, currentFunc));
        builder->SetInsertPoint(bbDispatch);
        if (condBBs.empty()) {
            // Tag-only enum: tags already match here, so the result is 1.
            builder->CreateStore(llvm::ConstantInt::get(i64Ty, 1), resultSlot);
            builder->CreateBr(bbEnd);
        } else {
            builder->CreateBr(condBBs[0]);
        }

        for (size_t k = 0; k < ed.variants.size(); k++) {
            auto& var = ed.variants[k];
            llvm::BasicBlock* bodyBB =
                llvm::BasicBlock::Create(*ctx, "eq_body_" + var.name, currentFunc);
            llvm::BasicBlock* nextBB = (k + 1 < ed.variants.size())
                ? condBBs[k + 1] : bbEnd;
            builder->SetInsertPoint(condBBs[k]);
            auto* isK = builder->CreateICmpEQ(tag0,
                llvm::ConstantInt::get(i8Ty, (uint64_t)k), "eq_is_" + var.name);
            builder->CreateCondBr(isK, bodyBB, nextBB);
            builder->SetInsertPoint(bodyBB);

            std::vector<llvm::Type*> fldTys = {i8Ty};
            for (auto& pt : var.payloadTypes)
                fldTys.push_back(llvmType(pt, *ctx, mod.get()));
            auto* varTy = llvm::StructType::get(*ctx, fldTys);

            llvm::Value* varEq = nullptr;
            for (unsigned j = 1; j < fldTys.size(); j++) {
                llvm::Value* fL = builder->CreateLoad(fldTys[j],
                    builder->CreateStructGEP(varTy, allocaL, j, "eq_l." + var.name),
                    "eq_lf." + var.name);
                llvm::Value* fR = builder->CreateLoad(fldTys[j],
                    builder->CreateStructGEP(varTy, allocaR, j, "eq_r." + var.name),
                    "eq_rf." + var.name);
                llvm::Value* feq = nullptr;
                if (fldTys[j]->isFloatingPointTy())
                    feq = builder->CreateFCmp(llvm::CmpInst::FCMP_OEQ, fL, fR, "eq_f");
                else if (fldTys[j]->isPointerTy())
                    feq = builder->CreateICmpEQ(fL, fR, "eq_p");
                else if (fldTys[j]->isStructTy())
                    feq = emitAggregateEqual(fL, fR, depth + 1);
                else
                    feq = builder->CreateICmpEQ(fL, fR, "eq_i");
                if (!feq) return nullptr;
                varEq = varEq ? builder->CreateAnd(varEq, feq, "eq_and." + var.name) : feq;
            }
            auto* bodyBool = varEq ? varEq : llvm::ConstantInt::getTrue(*ctx);
            // OR-accumulate: each variant body runs at most once (the tag
            // dispatch chain), so the first body to match decides. ANDing
            // against the 0-initialized slot instead forced EVERY equality
            // to 0 — including None == None — while `!=` flanked by the
            // same bug still reported truth, making == and != disagree.
            auto* prev = builder->CreateLoad(i64Ty, resultSlot, "eq_prev");
            auto* both = builder->CreateOr(prev,
                builder->CreateZExt(bodyBool, i64Ty, "eq_ext." + var.name),
                "eq_acc." + var.name);
            builder->CreateStore(both, resultSlot);
            builder->CreateBr(nextBB);
        }

        builder->SetInsertPoint(bbEnd);
        auto* acc = builder->CreateLoad(i64Ty, resultSlot, "eq_acc_final");
        return builder->CreateICmpNE(acc, llvm::ConstantInt::get(i64Ty, 0), "eq_bool");
    }

    // P0-NEW-1: THE comparison core. The AST path and the single-pass Parser
    // path each carried a copy of this lowering, and the copies drifted: the
    // float-FCmp fix and the aggregate-equality lowering both went into one
    // path only. Both now call this; a future fix applies once.
    // Takes already-lowered LLVM values, returns the ZExted i64 result.
    llvm::Value* emitCompareValues(const std::string& op, llvm::Value* l,
                                   llvm::Value* r) {
        if (!l || !r) return nullptr;
        // Aggregate operands: `==` and `!=` lower to a field-by-field
        // comparison. ICmp on a struct is invalid IR and LLVM asserted,
        // aborting the compiler; comparing raw bytes would compare padding.
        // Field-by-field recurses so nested structs compare structurally.
        if (l->getType()->isStructTy() || r->getType()->isStructTy()) {
            if (op != "==" && op != "!=") {
                return codegenError("E0003", "'" + op +
                    "' is not defined for structs or enums; use == or !=");
            }
            llvm::Value* eq = emitAggregateEqual(l, r, 0);
            if (!eq) return nullptr;
            if (op == "==") return builder->CreateZExt(eq, i64Ty, "agg_eq");
            return builder->CreateZExt(builder->CreateNot(eq), i64Ty, "agg_ne");
        }
        llvm::CmpInst::Predicate pred;
        if (op == "==") pred = llvm::CmpInst::ICMP_EQ;
        else if (op == "!=") pred = llvm::CmpInst::ICMP_NE;
        else if (op == "<")  pred = llvm::CmpInst::ICMP_SLT;
        else if (op == ">")  pred = llvm::CmpInst::ICMP_SGT;
        else if (op == "<=") pred = llvm::CmpInst::ICMP_SLE;
        else if (op == ">=") pred = llvm::CmpInst::ICMP_SGE;
        else return codegenError("E0127", "unknown operator '" + op + "'");
        // String content comparison: ptr == ptr compares contents
        // via flint_str_compare (same convention as `+` -> concat).
        if (l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
            auto it = functionMap.find("flint_str_compare");
            if (it != functionMap.end()) {
                auto* sc = builder->CreateCall(it->second, {l, r}, "strcmp");
                auto* z = llvm::ConstantInt::get(i64Ty, 0);
                auto* cmp = builder->CreateICmp(pred, sc, z, "scmp");
                return builder->CreateZExt(cmp, i64Ty, "scmp_ext");
            }
        }
        // Normalize types: pointer vs integer zero -> null pointer comparison
        if (l->getType()->isPointerTy() && r->getType()->isIntegerTy())
            r = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l->getType()));
        else if (l->getType()->isIntegerTy() && r->getType()->isPointerTy())
            l = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(r->getType()));
        // Floating-point operands need FCmp, not ICmp. ICmp on doubles
        // is invalid IR and LLVM asserts -- the original drift bug.
        if (l->getType()->isFloatingPointTy() || r->getType()->isFloatingPointTy()) {
            llvm::Type* cmpTy = l->getType()->isFloatingPointTy() ? l->getType() : r->getType();
            llvm::Value* lf = l->getType() == cmpTy ? l : builder->CreateSIToFP(l, cmpTy, "cmpf_l");
            llvm::Value* rf = r->getType() == cmpTy ? r : builder->CreateSIToFP(r, cmpTy, "cmpf_r");
            llvm::CmpInst::Predicate fpred;
            switch (pred) {
                case llvm::CmpInst::ICMP_EQ:  fpred = llvm::CmpInst::FCMP_OEQ; break;
                case llvm::CmpInst::ICMP_NE:  fpred = llvm::CmpInst::FCMP_ONE; break;
                case llvm::CmpInst::ICMP_SLT: fpred = llvm::CmpInst::FCMP_OLT; break;
                case llvm::CmpInst::ICMP_SLE: fpred = llvm::CmpInst::FCMP_OLE; break;
                case llvm::CmpInst::ICMP_SGT: fpred = llvm::CmpInst::FCMP_OGT; break;
                case llvm::CmpInst::ICMP_SGE: fpred = llvm::CmpInst::FCMP_OGE; break;
                default: fpred = llvm::CmpInst::FCMP_OEQ; break;
            }
            auto* fcmp = builder->CreateFCmp(fpred, lf, rf, "fcmp");
            return builder->CreateZExt(fcmp, i64Ty, "fcmp_ext");
        }
        auto* cmp = builder->CreateICmp(pred, l, r, "cmp");
        return builder->CreateZExt(cmp, i64Ty, "cmp_ext");
    }

    // P0-NEW-1: THE call guard. Arity + argument types are checked before the
    // call is built, so a wrong call reads as a Flint diagnostic rather than
    // LLVM's raw "Call parameter type does not match function signature!".
    // Pointers compare loosely: Flint passes aggregate handles and raw
    // pointers interchangeably in places, and rejecting that would flag
    // correct code. The AST path and the Parser emit path both route through
    // here; instantiationMsg picks the generic-specific trailing text.
    llvm::Value* createCallChecked(const std::string& displayName, llvm::Function* f,
                                   std::vector<llvm::Value*>& args,
                                   bool instantiationMsg = false) {
        if (!f) return nullptr;
        if (llvm::FunctionType* fty = f->getFunctionType()) {
            // A variadic C function (printf) has a fixed prefix plus `...`:
            // require at least the fixed parameters and do not count the
            // variadic ones against the signature.
            const bool variadic = fty->isVarArg();
            const size_t fixed = fty->getNumParams();
            const bool arity_ok = variadic ? args.size() >= fixed
                                           : args.size() == fixed;
            if (!arity_ok) {
                return codegenError("E0001", "'" + displayName + "' expects " +
                    std::to_string(fixed) + (variadic ? " or more" : "") +
                    " argument(s), got " + std::to_string(args.size()));
            }
            for (size_t i = 0; i < args.size() && i < fixed; i++) {
                if (!args[i]) continue;
                llvm::Type* want = fty->getParamType(i);
                llvm::Type* got = args[i]->getType();
                bool bothPointers = want->isPointerTy() && got->isPointerTy();
                if (want == got || bothPointers) continue;
                return codegenError("E0008", "argument " + std::to_string(i + 1) + " of '" +
                    displayName + "' has type " + llvmTypeToString(got) +
                    (instantiationMsg ? ", but this instantiation expects "
                                      : ", but '" + displayName + "' expects ") +
                    llvmTypeToString(want));
            }
        }
        return builder->CreateCall(f, args);
    }

    // P0-NEW-1: print dispatch, keyed on the VALUE type. The Parser path used
    // to dispatch by token shape and fell through silently for unsupported
    // types; both paths share this now.
    llvm::Value* emitPrintCall(llvm::Value* arg) {
        if (!arg) return nullptr;
        if (arg->getType() == i64Ty) {
            auto it = functionMap.find("flint_println_i64");
            if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
        }
        if (arg->getType()->isDoubleTy()) {
            auto it = functionMap.find("flint_println_f64");
            if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
        }
        if (arg->getType()->isPointerTy()) {
            auto it = functionMap.find("flint_println_str");
            if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
        }
        return codegenError("E0103", "print() of unsupported type");
    }

    // P0-NEW-1: array-builtin cores. Values in; the struct guard lives HERE
    // (the Parser copy had lost it and extracted field 0 of a bare i64).
    // `knownLen` is the EXACT length of the array when the compiler knows
    // it (Phase 2 / 2A.1: the length fact on the binding). The check then
    // compares the index against the SAME value the user's loop compares
    // against, and LLVM folds it away. Without it the check compares
    // against the length LOADED FROM THE SLICE, which is an unrelated value
    // and therefore never folds (measured: 3 of 6 checks survive in
    // sum_array).
    llvm::Value* emitArrayGetCore(llvm::Value* arr, llvm::Value* idx,
                                  llvm::Value* knownLen = nullptr) {
        if (!arr || !idx) return nullptr;
        if (!arr->getType()->isStructTy()) {
            return codegenError("E0062", "flint_array_get needs an array");
        }
        auto* dataPtr = builder->CreateExtractValue(arr, {0}, "arr_data");
        auto* len = builder->CreateExtractValue(arr, {1}, "arr_len");
        if (knownLen && valueDominatesHere(knownLen)) len = knownLen;
        emitLoopBoundsCheck(idx, len);
        auto* elemPtr = builder->CreateGEP(i64Ty, dataPtr, idx, "arr_elem");
        return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
    }
    llvm::Value* emitArraySetCore(llvm::Value* arr, llvm::Value* idx,
                                  llvm::Value* val, llvm::Value* knownLen = nullptr) {
        if (!arr || !idx || !val) return nullptr;
        if (!arr->getType()->isStructTy()) {
            return codegenError("E0065", "flint_array_set needs an array");
        }
        auto* dataPtr = builder->CreateExtractValue(arr, {0}, "arr_data");
        auto* len = builder->CreateExtractValue(arr, {1}, "arr_len");
        if (knownLen && valueDominatesHere(knownLen)) len = knownLen;
        emitLoopBoundsCheck(idx, len);
        auto* elemPtr = builder->CreateGEP(i64Ty, dataPtr, idx, "arr_elem");
        builder->CreateStore(val, elemPtr);
        return llvm::ConstantInt::get(i64Ty, 0); // dummy return
    }
    llvm::Value* emitArrayDataCore(llvm::Value* arr) {
        if (!arr) return nullptr;
        return builder->CreateExtractValue(arr, {0}, "arr_data");
    }
    llvm::Value* emitArrayGetPtrCore(llvm::Value* ptr, llvm::Value* idx) {
        if (!ptr || !idx) return nullptr;
        auto* elemPtr = builder->CreateGEP(i64Ty, ptr, idx, "arr_elem");
        return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
    }
    llvm::Value* emitArraySetPtrCore(llvm::Value* ptr, llvm::Value* idx, llvm::Value* val) {
        if (!ptr || !idx || !val) return nullptr;
        auto* elemPtr = builder->CreateGEP(i64Ty, ptr, idx, "arr_elem");
        builder->CreateStore(val, elemPtr);
        return llvm::ConstantInt::get(i64Ty, 0);
    }

    llvm::Value* emitAggregateEqual(llvm::Value* l, llvm::Value* r, int depth) {
        if (!l || !r || depth > 16) return nullptr;
        llvm::Type* t = l->getType();
        // P0-NEW-2: named struct whose name is a registered ENUM -> the
        // per-variant payload compare. Kept at the top so the nested-struct
        // recursion below routes enum payloads the same way.
        if (t->isStructTy() && llvm::isa<llvm::StructType>(t)) {
            auto* st = llvm::cast<llvm::StructType>(t);
            // Enum types are identified (non-literal) structs; literal
            // aggregates from the struct path have no name to look up.
            if (st->isLiteral()) { /* not a registered enum */ }
            else {
                auto eit = enumRegistry.find(st->getName().str());
                if (eit != enumRegistry.end()) {
                    if (r->getType() != t)
                        return codegenError("E0034", "cannot compare enum '" + st->getName().str() + "' with a value of a different type");
                    return emitEnumEqual(l, r, eit->second, depth);
                }
            }
        }
        // Array fields are compared element-wise.
        //
        // An enum's LLVM type is `{ i8, [N x i8] }`: a tag followed by a PACKED
        // payload array, not a scalar. The scalar path below reached
        // CreateICmpEQ on that array, which is invalid IR and aborted the
        // compiler with
        //     assertion "Invalid operand types for ICmp instruction" failed
        // so `E.B(3) == E.B(3)` killed the compiler while struct equality --
        // whose fields are all scalars -- worked.
        if (t->isArrayTy() || r->getType()->isArrayTy()) {
            // Struct fields can still hold typed arrays ([N x i64] etc.) whose
            // representation is NOT the enum payload: byte-comparing those is
            // as wrong as before, and per-element lowering for struct fields
            // is separate work. Refuse until then.
            return nullptr;
        }
        if (!t->isStructTy()) {
            // Scalars nested inside an aggregate.
            llvm::Value* rv = (r->getType() == t) ? r
                : builder->CreateSIToFP(r, t, "agg_cvt");
            auto* cmp = t->isFloatingPointTy() ? builder->CreateFCmp(llvm::CmpInst::FCMP_OEQ, l, rv, "aggf")
                                               : builder->CreateICmpEQ(l, rv, "aggi");
            return cmp;
        }
        llvm::Value* result = nullptr;
        unsigned n = cast<llvm::StructType>(t)->getNumElements();
        for (unsigned i = 0; i < n; i++) {
            llvm::Value* lf = builder->CreateExtractValue(l, {i}, "agg_l");
            llvm::Value* rf = builder->CreateExtractValue(r, {i}, "agg_r");
            llvm::Value* eq = emitAggregateEqual(lf, rf, depth + 1);
            if (!eq) return nullptr;
            result = result ? builder->CreateAnd(result, eq, "agg_and") : eq;
        }
        return result ? result : llvm::ConstantInt::getTrue(*ctx);
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
        if (eit == enumRegistry.end()) { return codegenError("E0124", "unknown enum in match"); }
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
            if (!arm.bindNames.empty() && armTag >= 0 && !ed.variants[armTag].payloadTypes.empty()) {
                auto& vt = ed.variants[armTag];
                std::vector<llvm::Type*> fldTys = {i8Ty};
                for (auto& pt : ed.variants[armTag].payloadTypes)
                    fldTys.push_back(llvmType(pt, *ctx, mod.get()));
                auto* varTy = llvm::StructType::get(*ctx, fldTys);
                auto* bcPtr = builder->CreateBitCast(alloca, i8PtrTy);
                // The pattern must bind exactly the payloads the variant
                // carries. A mismatch used to be silently ignored, so `Node(a)`
                // on a two-payload variant compiled with `a` bound to nothing
                // and failed later with a confusing "undefined var".
                if (arm.bindNames.size() != vt.payloadTypes.size()) {
                    return codegenError("E0101", "pattern " + arm.enumName + "." + arm.variantName +
                        " binds " + std::to_string(arm.bindNames.size()) +
                        " value(s) but the variant has " +
                        std::to_string(vt.payloadTypes.size()));
                }
                // One binding per payload field, in order.
                for (size_t bi = 0; bi < arm.bindNames.size(); bi++) {
                    const std::string& name = arm.bindNames[bi];
                    auto* valPtr = builder->CreateStructGEP(varTy, bcPtr, (unsigned)(bi + 1));
                    auto* fieldTy = llvmType(vt.payloadTypes[bi], *ctx, mod.get());
                    auto* val = builder->CreateLoad(fieldTy, valPtr, name);
                    auto* bindAlloca = createEntryAlloca(val->getType(), name);
                    builder->CreateStore(val, bindAlloca);
                    symTable.declare(name, {vt.payloadTypes[bi], bindAlloca, nullptr, false, false, 0});
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
        // Match is an expression: reaching arms must agree on a value type.
        // Void-typed arm values (statement arms like print) can never join
        // a PHI — LLVM aborts the compiler on those. All-void/null means
        // statement context (return 0, the existing convention); anything
        // mixed is a source type error → fail loudly, never abort.
        llvm::Type* phiTy = nullptr;
        bool mixed = false;
        for (size_t i = 0; i < armVals.size(); i++) {
            if (!armReachesMerge[i]) continue;
            llvm::Value* v = armVals[i];
            if (!v) continue; // valueless arm: coerced to null below (legacy)
            if (v->getType()->isVoidTy()) {
                // Void-typed arm (statement like print): only OK when no
                // reaching arm yields a real value, else the arms disagree.
                for (size_t j = 0; j < armVals.size(); j++) {
                    if (j == i || !armReachesMerge[j]) continue;
                    llvm::Value* u = armVals[j];
                    if (u && !u->getType()->isVoidTy()) { mixed = true; break; }
                }
                if (mixed) break;
                continue;
            }
            if (!phiTy) phiTy = v->getType();
            else if (v->getType() != phiTy) { mixed = true; break; }
        }
        if (mixed) { return codegenError("E0096", "match arms must yield the same type"); }
        if (!phiTy) return llvm::ConstantInt::get(i64Ty, 0);
        auto* phi = builder->CreatePHI(phiTy, (unsigned)armVals.size() + 1, "matchval");
        for (size_t i = 0; i < armVals.size(); i++) {
            if (!armReachesMerge[i]) continue;
            llvm::Value* v = armVals[i];
            if (!v || v->getType()->isVoidTy()) v = llvm::Constant::getNullValue(phiTy);
            phi->addIncoming(v, armEndBBs[i]);
        }
        // Default edge (unmatched tag falls through to merge): incoming 0.
        phi->addIncoming(llvm::Constant::getNullValue(phiTy), switchBB);
        return phi;
    }

    void emitPythonBlock(PyBlockStmtAST* py) {
        hasPython = true; markUsesPython();
        for (auto& code : py->codeStrings) {
            auto* str = builder->CreateGlobalString(code, "pycode");
            auto it = functionMap.find("flint_py_run");
            if (it != functionMap.end()) builder->CreateCall(it->second, {str});
        }
    }

    // Mangles a generic instantiation into a unique symbol name.
    //
    // This MUST be injective: two different type arguments that produce the
    // same string would collide in `genericCache`, so one instantiation would
    // silently be served another one's body. The previous version had no case
    // for F64, Ptr, Void or TypeParam, so all four fell through to `default:
    // s += "any"` -- four distinct types sharing one cache key.
    //
    // Every TypeKind gets an explicit, distinct case. If a new kind is ever
    // added, the default below stays a *unique* marker rather than a shared
    // one, so an unhandled kind degrades into a missed cache hit (a
    // performance cost) instead of a wrong answer.
    // Injective kind tag for a Type (generic-instantiation mangles). fn types
    // carry a signature: encoding mirrors their structure so fn two distinct
    // signatures never collide.
    static void typeMangleTag(std::string& s, const Type& t) {
        switch (t.kind) {
            case TypeKind::I64: s += "i64"; return;
            case TypeKind::F64: s += "f64"; return;
            case TypeKind::Str: s += "str"; return;
            case TypeKind::Bool: s += "bool"; return;
            case TypeKind::Ptr: s += "ptr"; return;
            case TypeKind::Void: s += "void"; return;
            case TypeKind::TypeParam: s += "tp"; return;
            case TypeKind::Ref: s += "ref"; return;
            case TypeKind::Array: s += "arr"; return;
            case TypeKind::Map: s += "map"; return;
            case TypeKind::Struct:
                // Struct and enum names are user-supplied, so they must be
                // distinguishable from the fixed tags above: a struct named
                // "i64" would otherwise collide with an i64 instantiation.
                s += "s:" + (t.structName.empty() ? std::string("?") : t.structName); return;
            case TypeKind::Enum:
                s += "e:" + (t.structName.empty() ? std::string("?") : t.structName); return;
            case TypeKind::Func: {
                s += "fn<";
                for (size_t i = 0; i < t.funcParams.size(); i++) {
                    if (i) s += ",";
                    typeMangleTag(s, t.funcParams[i]);
                }
                s += ">->";
                if (t.funcRet) typeMangleTag(s, *t.funcRet);
                return;
            }
            default: s += "unknown_kind"; return;
        }
    }

    std::string mangle(const std::string& fnName, const std::vector<Type>& typeArgs) {
        std::string s = fnName;
        for (auto& ta : typeArgs) {
            s += ".";
            typeMangleTag(s, ta);
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

        // Activate the substitution map BEFORE resolving any parameter type.
        //
        // resolveType reads `typeSubstMap`, so computing signatures first
        // resolved every parameter to an unresolved TypeParam, which llvmType
        // rendered as the int64 placeholder -- every generic silently became
        // i64. The map must be live before the first resolveType call.
        auto savedMap = typeSubstMap;
        typeSubstMap = subst;

        // Build LLVM function type with concrete types.
        //
        // resolveType, not the inline lookup this used to do: the inline
        // version tested `rt.kind == TypeKind::TypeParam`, so a COMPOUND
        // parameter like `[T]` never matched and kept the bare type parameter.
        // resolveType recurses through elemType, so compound substitution works
        // by pointing at it.
        std::vector<llvm::Type*> paramTys;
        for (auto& p : generic->params) {
            paramTys.push_back(llvmType(resolveType(p.second), *ctx, mod.get()));
        }
        Type retRt = resolveType(generic->returnType);
        llvm::Type* retTy = llvmType(retRt, *ctx, mod.get());

        auto fTy = llvm::FunctionType::get(retTy, paramTys, false);
        auto f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, mangled, mod.get());
        size_t i = 0;
        for (auto& arg : f->args()) {
            if (i < generic->params.size()) arg.setName(generic->params[i].first);
            i++;
        }
        functionMap[mangled] = f;
        // Register before emitting the body. Previously this happened after,
        // at the bottom of the function, so a generic that calls itself
        // re-entered specialize forever and the compiler hung or blew the
        // stack. The signature is already correct here, which is all a
        // recursive call needs.
        genericCache[mangled] = f;

        // Emit the function body with substitution active
        llvm::Function* savedFunc = currentFunc;
        // saveIP/restoreIP, not GetInsertBlock/SetInsertPoint: SetInsertPoint
        // positions at the END of the block, so after a nested monomorphization
        // the outer function resumed emitting *after its terminator* and
        // produced invalid IR. The save/restore pair keeps the exact position.
        auto savedInsertPoint = builder->saveIP();
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
                // The null value of the return type: a generic returning
                // an fn(...) has a POINTER return type (see the note at the
                // function-terminator site).
                builder->CreateRet(llvm::Constant::getNullValue(retTy));
            else
                builder->CreateRetVoid();
        }

        symTable.exitScope();
        currentFunc = savedFunc;
        typeSubstMap = savedMap;
        builder->restoreIP(savedInsertPoint);

        return f;
    }

    llvm::Value* emitCall(CallExprAST* call) {
        // Phase 2 / 2D: every argument may be retained by the callee, so a
        // closure argument must not point into this frame. A bound closure
        // is promoted BEFORE its value is read; a lambda literal is created
        // on the heap for the duration of the call. This is what makes
        // `store_it(cb)` -- STATE.md §4.4's "done when" -- sound: the callee
        // may store the value in a global or return it, and it still points
        // at live storage.
        for (auto& a : call->args) {
            if (!a || a->kind != NodeKind::Variable) continue;
            auto* vn = static_cast<VariableExprAST*>(a.get());
            Symbol* s = symTable.lookup(vn->name);
            if (s && s->isLambda && !s->lambdaHeapBox) promoteLambdaBox(s);
        }
        ScopedEscape escapeArgs(lambdaEscapesNow);
        // Lambda call rewrite (P0-6): a direct call to a lambda bound in an
        // accessible function passes user args + frozen by-value captures.
        // Cross-function calls are rejected (closures don't escape).
        {
            auto* lsym = symTable.lookup(call->callee);
            auto* glsym = symTable.lookup(call->callee) ? nullptr
                : &globalSymTable[call->callee];
            if ((lsym && lsym->isLambda) || (glsym && glsym->isLambda)) {
                Symbol* Lsym = lsym ? lsym : glsym;
                bool selfRecurse = (Lsym->lambdaFn == currentFunc);
                if (Lsym->lambdaDefinedIn != currentFunc && !selfRecurse)
                    return codegenError("E0028", "cannot call lambda '" + call->callee + "' from a different function");
                if (call->args.size() != Lsym->lambdaParamTypes.size())
                    return codegenError("E0087", "lambda '" + call->callee + "' expects " +
                        std::to_string(Lsym->lambdaParamTypes.size()) + " argument(s), got " +
                        std::to_string(call->args.size()));
                std::vector<llvm::Value*> largs;
                auto* lamFnTy = Lsym->lambdaFn->getFunctionType();
                largs.push_back(Lsym->lambdaEnv);
                for (size_t i = 0; i < call->args.size(); i++) {
                    auto* v = emitExpr(call->args[i].get());
                    if (!v) return nullptr;
                    llvm::Type* want = lamFnTy->getParamType(i + 1);
                    if (v->getType() != want) {
                        if (v->getType()->isIntegerTy(64) && want->isDoubleTy())
                            v = builder->CreateSIToFP(v, want, "lamarg");
                        else
                            return codegenError("E0086", "lambda '" + call->callee + "' argument type mismatch");
                    }
                    largs.push_back(v);
                }
                // Pure indirect call: load the callee from env box field 0
                // and pass the box as the env argument. The outlined fn's
                // signature IS fn(env, userargs...) and no cap args exist.
                {
                    auto* fslotPtr = builder->CreateStructGEP(Lsym->lambdaBoxTy,
                                                              Lsym->lambdaEnv, 0, "lam.env.fnptr");
                    auto* fnPtr = builder->CreateLoad(llvm::PointerType::get(*ctx, 0),
                                                      fslotPtr, "lam.env.fn");
                    return builder->CreateCall(lamFnTy, fnPtr, largs, "lam_call");
                }
            }
        }
        // M4: `g(x)` where g is an fn-typed value (param or fn-typed
        // binding). The value is an env box: field 0 is the code pointer,
        // the box itself travels as the env argument. Pure indirect call —
        // looking g up in functionMap would be wrong (fn values have no
        // static name).
        {
            Symbol* vsym = symTable.lookup(call->callee);
            const Symbol* gvs = nullptr;
            auto gvit = globalSymTable.find(call->callee);
            if (gvit != globalSymTable.end()) gvs = &gvit->second;
            Symbol glCopy;
            Symbol* cs = vsym;
            if (!cs && gvs) { glCopy = *gvs; cs = &glCopy; }
            if (cs && !cs->isLambda && cs->type.kind == TypeKind::Func && cs->type.funcRet) {
                if (call->args.size() != cs->type.funcParams.size())
                    return codegenError("E0082", "function value '" + call->callee + "' expects " +
                        std::to_string(cs->type.funcParams.size()) + " argument(s), got " +
                        std::to_string(call->args.size()));
                VariableExprAST cvNode(call->callee);
                auto* calleeVal = emitExpr(&cvNode);
                if (!calleeVal) return nullptr;
                auto* fnPtr = builder->CreateLoad(llvm::PointerType::get(*ctx, 0),
                                                  calleeVal, "fncall.fn");
                auto* pty = llvm::PointerType::get(*ctx, 0);
                std::vector<llvm::Type*> ptys = {pty};
                std::vector<llvm::Value*> avals = {calleeVal};
                for (size_t i = 0; i < call->args.size(); i++) {
                    auto* v = emitExpr(call->args[i].get());
                    if (!v) return nullptr;
                    llvm::Type* want = llvmType(cs->type.funcParams[i], *ctx, mod.get());
                    ptys.push_back(want);
                    if (v->getType() != want) {
                        if (v->getType()->isIntegerTy(64) && want->isDoubleTy())
                            v = builder->CreateSIToFP(v, want, "fnarg");
                        else if (!(v->getType()->isPointerTy() && want->isPointerTy()))
                            return codegenError("E0081", "function value '" + call->callee +
                                "' argument type mismatch (parameter " + std::to_string(i + 1) + ")");
                    }
                    avals.push_back(v);
                }
                auto* cty = llvm::FunctionType::get(
                    llvmType(*cs->type.funcRet, *ctx, mod.get()), ptys, false);
                return builder->CreateCall(cty, fnPtr, avals, "fnval_call");
            }
        }
        // Handle generic function call
        auto git = genericFunctions.find(call->callee);
        if (git != genericFunctions.end()) {
            auto* generic = git->second;

            // M2b: explicit type arguments (`f::<T>(...)`) override
            // inference. They are concrete types by construction — a type
            // parameter would have nothing to resolve to.
            if (!call->typeArgs.empty()) {
                if (call->typeArgs.size() != generic->typeParams.size())
                    return codegenError("E0002", "'" + call->callee + "' expects " +
                        std::to_string(generic->typeParams.size()) +
                        " type argument(s), got " +
                        std::to_string(call->typeArgs.size()));
                for (auto& ta : call->typeArgs)
                    if (ta.kind == TypeKind::TypeParam)
                        return codegenError("E0117", "the explicit type argument for '" +
                            call->callee + "' must be a concrete type");
                llvm::Function* ef = nullptr;
                auto ecit = genericCache.find(mangle(call->callee, call->typeArgs));
                if (ecit != genericCache.end()) {
                    ef = ecit->second;
                } else {
                    ef = specialize(generic, call->typeArgs);
                }
                if (!ef) return codegenError("E0058", std::string("failed to specialize '") + call->callee + "'");
                call->instantiatedTypeArgs = call->typeArgs;
                recordCallFact(call, call->typeArgs, "");
                std::vector<llvm::Value*> eargs;
                for (auto& a : call->args) {
                    auto* v = emitExpr(a.get());
                    if (!v) return nullptr;
                    eargs.push_back(v);
                    if (a->kind == NodeKind::Variable) {
                        auto* varArg = static_cast<VariableExprAST*>(a.get());
                        auto* srcSym = symTable.lookup(varArg->name);
                        if (srcSym && !srcSym->type.isCopyType()) {
                            if (srcSym->borrowCount > 0)
                                return codegenError("E0037", "cannot move while borrowed");
                            srcSym->moved = true;
                        }
                    }
                }
                return createCallChecked(call->callee, ef, eargs, /*instantiationMsg=*/true);
            }

            // Infer each type parameter from the ARGUMENT that fills the
            // parameter which mentions it.
            //
            // The previous version walked type parameters and read
            // `call->args[ai]` -- indexing arguments by TYPE-PARAM position.
            // For `fn pair<T,U>(a: T, b: U)` that happens to work, but for
            // `fn swap<T,U>(a: U, b: T)` it infers T from the first argument,
            // which is U's argument, and silently instantiates the wrong
            // signature. Matching parameter to argument by index is the only
            // correct rule.
            //
            // Argument types come from exprFlintType, which knows about
            // numbers (including floats), strings, variables, calls, enums,
            // structs, struct field access and binary expressions. The old
            // inline switch handled four of those and defaulted the rest to
            // i64 -- so `first<T>(p.x)` and `first<T>(Other(i64))` were
            // silently wrong.
            std::vector<Type> typeArgs;
            bool inferenceFailed = false;
            std::string failedParam;
            for (size_t ai = 0; ai < generic->typeParams.size(); ai++) {
                // Locate the parameter that actually uses this type parameter.
                size_t argIndex = SIZE_MAX;
                for (size_t pi = 0; pi < generic->params.size(); pi++) {
                    if (generic->params[pi].second.kind == TypeKind::TypeParam &&
                        generic->params[pi].second.structName == generic->typeParams[ai]) {
                        argIndex = pi;
                        break;
                    }
                }
                Type inferred;
                bool known = false;
                if (argIndex != SIZE_MAX && argIndex < call->args.size()) {
                    ExprAST* arg = call->args[argIndex].get();
                    // M2c: the strict resolver. A variable argument carries
                    // its declared type; casts carry their target; a nested
                    // generic call resolves its return with the same
                    // by-parameter rule; a user function contributes its
                    // declared return type. When the shape is not one it
                    // knows, it says so instead of answering i64.
                    known = inferGenericArgType(arg, inferred);
                }
                if (!known || inferred.kind == TypeKind::Void ||
                    inferred.kind == TypeKind::TypeParam) {
                    // Un-inferrable. Defaulting to i64 here is how the old
                    // version produced a confidently wrong instantiation, so
                    // report it rather than guess.
                    if (!inferenceFailed) {
                        inferenceFailed = true;
                        failedParam = generic->typeParams[ai];
                    }
                    inferred = Type::i64();
                }
                typeArgs.push_back(inferred);
            }
            if (inferenceFailed) {
                return codegenError("E0035", "cannot infer type parameter '" + failedParam +
                    "' for '" + call->callee +
                    "'; the argument's type could not be determined");
            }

            call->instantiatedTypeArgs = typeArgs;
            recordCallFact(call, typeArgs, "");
            std::string mangled = mangle(call->callee, typeArgs);
            llvm::Function* f = nullptr;
            auto cit = genericCache.find(mangled);
            if (cit != genericCache.end()) {
                f = cit->second;
            } else {
                f = specialize(generic, typeArgs);
            }
            if (!f) { return codegenError("E0058", std::string("failed to specialize '") + call->callee + "'"); }

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
                            return codegenError("E0037", "cannot move while borrowed");
                        }
                        srcSym->moved = true;
                    }
                }
            }
            // A mismatch here means inference picked the wrong instantiation.
            // Checking first turns the whole class of generic-misfit errors
            // into a Flint diagnostic. P0-NEW-1: shared guard (both paths).
            return createCallChecked(call->callee, f, args, /*instantiationMsg=*/true);
        }

        if (!call->typeArgs.empty())
            return codegenError("E0057", "explicit type arguments are only allowed on a generic function; '" +
                call->callee + "' is not generic");

        if (call->callee == "print") {
            if (call->args.empty()) { return codegenError("E0102", "print() needs an arg"); }
            auto* arg = emitExpr(call->args[0].get());
            // P0-NEW-1: shared print dispatch (both paths).
            return emitPrintCall(arg);
        }

        if (call->callee == "flint_array_get") {
            if (call->args.size() != 2) { return codegenError("E0061", "flint_array_get needs 2 args"); }
            auto* arrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            // Phase 2 / 2C: the loop-range fact for this access.
            emitLoopRangeAssume(call->args[1].get());
            // P0-NEW-1: shared core (both paths).
            return emitArrayGetCore(arrVal, idxVal, knownLenOfArg(call->args[0].get()));
        }

        if (call->callee == "flint_array_set") {
            if (call->args.size() != 3) { return codegenError("E0064", "flint_array_set needs 3 args"); }
            auto* arrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            auto* valVal = emitExpr(call->args[2].get());
            // Phase 2 / 2C: the loop-range fact for this access.
            emitLoopRangeAssume(call->args[1].get());
            // P0-NEW-1: shared core (both paths).
            return emitArraySetCore(arrVal, idxVal, valVal, knownLenOfArg(call->args[0].get()));
        }

        if (call->callee == "flint_array_data") {
            if (call->args.size() != 1) { return codegenError("E0060", "flint_array_data needs 1 arg"); }
            auto* arrVal = emitExpr(call->args[0].get());
            // P0-NEW-1: shared core (both paths).
            return emitArrayDataCore(arrVal);
        }

        if (call->callee == "flint_array_get_ptr") {
            if (call->args.size() != 2) { return codegenError("E0063", "flint_array_get_ptr needs 2 args"); }
            auto* ptrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            // P0-NEW-1: shared core (both paths).
            return emitArrayGetPtrCore(ptrVal, idxVal);
        }

        if (call->callee == "flint_array_set_ptr") {
            if (call->args.size() != 3) { return codegenError("E0066", "flint_array_set_ptr needs 3 args"); }
            auto* ptrVal = emitExpr(call->args[0].get());
            auto* idxVal = emitExpr(call->args[1].get());
            auto* valVal = emitExpr(call->args[2].get());
            // P0-NEW-1: shared core (both paths).
            return emitArraySetPtrCore(ptrVal, idxVal, valVal);
        }

        if (call->callee == "flint_thread_spawn") {
            if (call->args.size() != 2) { return codegenError("E0075", "flint_thread_spawn needs 2 args: func_name, arg"); }
            if (call->args[0]->kind != NodeKind::String) { return codegenError("E0074", "flint_thread_spawn first arg must be a string literal (function name)"); }
            auto* nameLit = static_cast<StringExprAST*>(call->args[0].get());
            auto* argVal = emitExpr(call->args[1].get());
            if (!argVal) return nullptr;
            auto fIt = functionMap.find(nameLit->value);
            if (fIt == functionMap.end()) { return codegenError("E0078", std::string("flint_thread_spawn: unknown function '") + nameLit->value + "'"); }
            auto* targetFn = fIt->second;
            auto* wrapperPtr = builder->CreateBitCast(targetFn, llvm::PointerType::get(*ctx, 0), "thread_fn");
            auto* createFn = functionMap["flint_thread_create"];
            if (!createFn) { return codegenError("E0076", "flint_thread_spawn: runtime function 'flint_thread_create' not found"); }
            auto* voidArg = builder->CreateIntToPtr(argVal, i8PtrTy, "thread_arg");
            return builder->CreateCall(createFn, {wrapperPtr, voidArg}, "thread_id");
        }

        if (call->callee == "py_eval" || call->callee == "py_eval_float") {
            hasPython = true; markUsesPython();
            const bool wantFloat = (call->callee == "py_eval_float");
            if (call->args.size() != 1) {
                return codegenError("E0005", call->callee + "() needs one string arg");
            }
            auto* arg = emitExpr(call->args[0].get());
            if (!arg) return nullptr;
            auto it = functionMap.find(wantFloat ? "flint_py_eval_float" : "flint_py_eval_int");
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

        // D2: bare len() over array/str/map (mirrors self-host e_expr len:
        // array → extractvalue 1, str → flint_str_length, map → flint_map_len).
        // No new syntax; dispatch on static kind + LLVM struct fallback.
        if (call->callee == "len") {
            if (call->args.size() != 1)
                return codegenError("E0092", "len() needs 1 argument");
            ExprAST* a0 = call->args[0].get();
            // Static kind: 4=array,3=str,6=map,0=unknown (self-host ETYPE codes).
            int lenKind = 0;
            if (a0->kind == NodeKind::String) lenKind = 3;
            else if (a0->kind == NodeKind::Array) lenKind = 4;
            else if (a0->kind == NodeKind::MapLiteral) lenKind = 6;
            else if (a0->kind == NodeKind::Variable) {
                auto* v = static_cast<VariableExprAST*>(a0);
                TypeKind tk = TypeKind::Void;
                if (auto* sym = symTable.lookup(v->name)) tk = sym->type.kind;
                else {
                    auto git = globalSymTable.find(v->name);
                    if (git != globalSymTable.end()) tk = git->second.type.kind;
                }
                if (tk == TypeKind::Array) lenKind = 4;
                else if (tk == TypeKind::Str) lenKind = 3;
                else if (tk == TypeKind::Map) lenKind = 6;
            } else if (a0->kind == NodeKind::Call) {
                auto* c = static_cast<CallExprAST*>(a0);
                if (c->callee == "flint_map_new") lenKind = 6;
                else if (c->callee == "flint_array_alloc" || c->callee == "flint_array_concat") lenKind = 4;
                else if (c->callee == "flint_str_concat" || c->callee == "flint_str_substring" ||
                         c->callee == "flint_str_to_upper" || c->callee == "flint_str_to_lower" ||
                         c->callee == "flint_str_trim" || c->callee == "flint_str_reverse" ||
                         c->callee == "flint_str_replace" || c->callee == "flint_str_repeat" ||
                         c->callee == "flint_str_join" || c->callee == "flint_i64_to_string" ||
                         c->callee == "flint_f64_to_string" || c->callee == "flint_map_keys") lenKind = 3;
                else {
                    auto fit2 = functionDefs.find(c->callee);
                    if (fit2 != functionDefs.end()) {
                        TypeKind rk = fit2->second->returnType.kind;
                        if (rk == TypeKind::Array) lenKind = 4;
                        else if (rk == TypeKind::Str) lenKind = 3;
                        else if (rk == TypeKind::Map) lenKind = 6;
                    }
                }
            }
            auto* aval = emitExpr(a0);
            if (!aval) return nullptr;
            // Array is {ptr,i64} struct — reliable even when static kind unknown.
            if (aval->getType()->isStructTy())
                return builder->CreateExtractValue(aval, {1}, "len");
            if (lenKind == 6) {
                auto lit = functionMap.find("flint_map_len");
                if (lit != functionMap.end()) return builder->CreateCall(lit->second, {aval});
                return codegenError("E0068", "flint_map_len not linked");
            }
            if (lenKind == 3) {
                auto lit = functionMap.find("flint_str_length");
                if (lit != functionMap.end()) return builder->CreateCall(lit->second, {aval});
                return codegenError("E0072", "flint_str_length not linked");
            }
            return codegenError("E0091", "len needs array, str or map");
        }

        auto it = functionMap.find(call->callee);
        if (it == functionMap.end()) {
            // Leftover fa_ method call: the receiver type was unknown at
            // parse time (or the method doesn't exist). Before saying so,
            // try M3 name+arity dispatch: a method call may resolve to an
            // ordinary function taking the receiver as its first argument.
            if (call->callee.rfind("fa_", 0) == 0) {
                Type recv; bool recvKnown = ufcsReceiverType(call->args[0].get(), recv);
                std::string diag;
                FunctionAST* impl = resolveUfcsImpl(call->callee.substr(3),
                    call->args.size(), recv, recvKnown, diag);
                if (!impl) return codegenError("E0142", diag);
                // The FUNCTION NAME alone does not identify an impl: two
                // types may implement `area` with the same name, and the
                // receiver type is what name+arity dispatch selected on.
                // Record both, e.g. `area for Circle`.
                std::string implFact = impl->name;
                if (recvKnown)
                    implFact += " for " +
                        ((recv.kind == TypeKind::Struct || recv.kind == TypeKind::Enum)
                             ? recv.structName : typeTag(recv));
                call->resolvedImpl = implFact;
                recordCallFact(call, {}, implFact);
                it = functionMap.find(impl->name);
                if (it == functionMap.end())
                    return codegenError("E0099", "no type in scope implements '" + impl->name + "'");
            } else {
                // Suggest the closest declared function (the user's own
                // functions and the builtins are both in functionMap).
                std::vector<std::string> names;
                names.reserve(functionMap.size());
                for (const auto& kv : functionMap) names.push_back(kv.first);
                std::string near = closestName(call->callee, names);
                return codegenError("E0120", std::string("undefined function '") + call->callee + "'", near);
            }
        }
        std::vector<llvm::Value*> args;
        for (size_t ai = 0; ai < call->args.size(); ai++) {
            ExprAST* a = call->args[ai].get();
            // M4: a lambda argument type-checks against the callee's
            // declared parameter. fn-typed params accept any signature
            // match; anything else refuses the closure outright — with a
            // ptr-sized env box flowing under opaque pointers, LLVM could
            // not catch a bad signature at the call itself.
            if (a && a->kind == NodeKind::Lambda) {
                auto fit = functionDefs.find(call->callee);
                if (fit != functionDefs.end() && ai < fit->second->params.size()) {
                    Type wantTy = fit->second->params[ai].second;
                    auto* lam = static_cast<LambdaExprAST*>(a);
                    std::vector<Type> lamPs;
                    lamPs.reserve(lam->params.size());
                    for (auto& lp : lam->params) lamPs.push_back(lp.second);
                    Type lamTy = Type::fn_(std::move(lamPs), lam->retType);
                    if (wantTy.kind != TypeKind::Func)
                        return codegenError("E0038", "cannot pass a lambda to parameter '" + fit->second->params[ai].first +
                            "' of '" + call->callee + "': the parameter is not a fn type");
                    if (!(wantTy == lamTy))
                        return codegenError("E0089", "lambda does not match the fn type of parameter '" + fit->second->params[ai].first + "' of '" + call->callee + "'");
                }
            }
            llvm::Value* v = nullptr;
            // Array-to-pointer decay: passing an array where a pointer is
            // expected yields the address of its first element, as in C.
            //
            // Without this, every pointer+length builtin (PNG decode, HTTPS,
            // CSV, Argon2) is unreachable from an array literal: the array
            // would load as a {ptr, i64} slice, and LLVM rejects the call as a
            // parameter type mismatch.
            llvm::Type* want = nullptr;
            if (ai < it->second->getFunctionType()->getNumParams())
                want = it->second->getFunctionType()->getParamType(ai);
            if (want && want->isPointerTy() && a->kind == NodeKind::Variable) {
                auto* var = static_cast<VariableExprAST*>(a);
                auto* sym = symTable.lookup(var->name);
                if (sym && sym->type.kind == TypeKind::Array) {
                    // A Flint array is a {ptr, i64} slice; the C interop value
                    // for it is field 0, the data pointer. Extracting it here
                    // (rather than calling flint_array_data_ptr, which takes a
                    // pointer TO the slice) keeps the decay a pure IR rewrite
                    // with no runtime dependency.
                    if (auto* slice = emitExpr(a)) {
                        if (auto* st = llvm::dyn_cast<llvm::StructType>(slice->getType()))
                            if (st->getNumElements() == 2)
                                v = builder->CreateExtractValue(slice, 0, "arr_decay");
                    }
                }
            }
            if (!v) v = emitExpr(a);
            if (!v) return nullptr;
            args.push_back(v);
            // Passing a variable as an argument BORROWS it: the callee receives
            // the value, but the caller's binding stays usable. Marking it moved
            // here made every `flint_sb_append(sb, ..)` retire `sb`, so its
            // next use reported "use of moved variable" -- a false positive on
            // correct code, and the reason Str/Map could never be non-Copy.
            // Ownership transfer is expressed explicitly at assignment, which
            // is the only place a binding is actually consumed.
        }
        // Check the call against the declared signature before building it.
        //
        // Without this, a builtin invoked with the wrong argument type produced
        // IR that LLVM's own verifier rejected, and the user saw
        //
        //     { ptr, i64 }  call void @flint_array_free(ptr %s29)
        //     Call parameter type does not match function signature!
        //
        // -- a message about the compiler's internals, emitted after a first
        // error had already been reported, so it read as a cascade of compiler
        // faults rather than one mistake in the user's program. Call it what it
        // is: a type error, at the call, with the expected and actual types.
        //
        // Pointers are compared loosely: Flint passes aggregate handles by
        // value and raw pointers interchangeably in places, and rejecting that
        // would be a false positive on correct code.
        // P0-NEW-1: shared call guard (both paths).
        return createCallChecked(call->callee, it->second, args);
    }
};

// ============================================================================
// INTERFACE SYSTEM — emit and load .flint.bc declaration files
// ============================================================================

static bool emitInterface(ProgramAST& prog, const std::string& outputPath,
                          const std::string& targetTriple) {
    Codegen cg(targetTriple);
    if (!cg.generateDeclarations(prog)) return false;
    std::error_code ec;
    llvm::raw_fd_ostream out(outputPath, ec);
    if (ec) { std::cerr << "interface error: " << ec.message() << "\n"; return false; }
    llvm::WriteBitcodeToFile(*cg.mod, out);
    out.close();
    return true;
}

// ============================================================================
// PHASE 2E.5 — THE SIGNATURE QUERY MODE
// ============================================================================
// The self-hosted pipeline re-derived every function signature from the
// S-expression, and a parameter declared without a type (`fn show(x)`) has
// no type there: the pipeline defaulted it to i64 and passed a str as an
// integer (measured: it printed the pointer's decimal value where the C++
// compiler printed "hi"). The C++ compiler RESOLVES such a parameter from
// its call sites (`inferUntypedParams`), so it can hand the pipeline the
// answer instead of letting it guess.
//
// `flintc --emit-signatures in.fl out.sig` writes one line per function:
//
//     fn NAME (TPARAMS) (PARAM_TYPES) -> RET
//
// with comma-separated type words: i64, f64, str, bool, ptr, map, [T], a
// struct/enum name, or a type parameter. The query runs the same
// declaration pass as codegen (so untyped params are resolved exactly as
// the emitted function will see them) but emits no artifact.
static std::string signatureTypeName(const Type& t) {
    switch (t.kind) {
        case TypeKind::I64: return "i64";
        case TypeKind::F64: return "f64";
        case TypeKind::Str: return "str";
        case TypeKind::Bool: return "bool";
        case TypeKind::Void: return "void";
        case TypeKind::Ptr: return "ptr";
        case TypeKind::Map: return "map";
        case TypeKind::TypeParam: return t.structName;
        case TypeKind::Struct: return t.structName;
        case TypeKind::Enum: return t.structName;
        case TypeKind::Array:
            // The pipeline only implements [i64] (M7 is deferred), but the
            // file must say what the compiler actually resolved: a [f64]
            // parameter is a loud "not supported here" in the consumer,
            // never a silent i64.
            if (t.elemType) return "[" + signatureTypeName(*t.elemType) + "]";
            return "[i64]";
        case TypeKind::Ref: return "ref"; // no ref support in the pipeline
        case TypeKind::Func: return "fn"; // no first-class fn types there
        default: return "?";
    }
}

static bool emitSignatures(ProgramAST& prog, const std::string& outputPath,
                           const std::string& targetTriple) {
    // generateDeclarations runs inferUntypedParams (the call-site fixpoint)
    // and builds the declaration ladder; it is the exact view codegen uses.
    Codegen cg(targetTriple);
    if (!cg.generateDeclarations(prog)) return false;

    // A forward declaration can carry a different (unresolved) signature
    // than the definition; the definition is the truth. Prints one line per
    // name, in first-occurrence order.
    std::unordered_set<std::string> defined;
    for (auto& fn : prog.functions)
        if (fn && !fn->isDeclaration) defined.insert(fn->name);
    std::unordered_set<std::string> emitted;

    std::ofstream out(outputPath);
    if (!out) {
        std::cerr << "error: cannot write '" << outputPath << "'\n";
        return false;
    }
    out << "flint-signatures 1\n";
    for (auto& fn : prog.functions) {
        if (!fn) continue;
        if (fn->isDeclaration && defined.count(fn->name)) continue;
        if (!emitted.insert(fn->name).second) continue;
        out << "fn " << fn->name << " (";
        for (size_t i = 0; i < fn->typeParams.size(); i++) {
            if (i) out << ",";
            out << fn->typeParams[i];
        }
        out << ") (";
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (i) out << ",";
            out << signatureTypeName(fn->params[i].second);
        }
        out << ") -> " << signatureTypeName(fn->returnType) << "\n";
    }
    out.close();
    if (!out) {
        std::cerr << "error: cannot write '" << outputPath << "'\n";
        return false;
    }
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
            if (!sym->isMutable) { advance(); advance(); parseError("E0021", "cannot assign to immutable '" + name + "'"); return nullptr; }
            if (sym->borrowCount > 0) { advance(); advance(); parseError("E0020", "cannot assign to '" + name + "' while borrowed"); return nullptr; }
            advance(); advance();
            auto val = parseExpressionEmit();
            if (!val) return nullptr;
            auto* store = cg->builder->CreateStore(val, sym->alloca);
            if (sym->moved) sym->moved = false;
            return store;
        }
        auto git = cg->globalSymTable.find(name);
        if (git != cg->globalSymTable.end()) {
            if (!git->second.isMutable) { advance(); advance(); parseError("E0022", "cannot assign to immutable global '" + name + "'"); return nullptr; }
            advance(); advance();
            auto val = parseExpressionEmit();
            if (!val) return nullptr;
            return cg->builder->CreateStore(val, git->second.global);
        }
        parseError("E0136", "variable '" + name + "' not declared");
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
            parseError("E0136", "variable '" + name + "' not declared");
            return nullptr;
        }
        bool isGlobal = !sym;
        bool isMut = isGlobal ? git->second.isMutable : sym->isMutable;
        if (!isMut) { advance(); advance(); parseError("E0021", "cannot assign to immutable '" + name + "'"); return nullptr; }
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
    Token exprStartTok = peek(); // for static map-base index checks
    auto left = parseNudEmit();
    if (!left) return nullptr;

    while (true) {
        TokenType tt = peek().type;
        int p = tokPrec(tt);
        if (p < minPrec) break;

        if (tt == TokenType::LBRACKET) {
            advance();
            auto idx = parseExpressionEmit();
            if (!idx) return nullptr;
            // m[0] emit-mode mirror of Codegen::emitIndexAccess: a
            // statically-known map base with an i64 key is a loud error,
            // not the string byte-GEP path (silent garbage read).
            if (idx->getType()->isIntegerTy(64) &&
                exprStartTok.type == TokenType::IDENTIFIER) {
                auto mit = varTypeMap.find(exprStartTok.lexeme);
                if (mit != varTypeMap.end() && mit->second.kind == TypeKind::Map) {
                    parseError("E0093", "map index needs a str key (map keys are strings)");
                    return nullptr;
                }
            }
            consume(TokenType::RBRACKET, "expected ']' after index");
            // P0-NEW-3 (B1 narrow stop-gap, emit path): a tensor handle is
            // an opaque i64; indexing it printed garbage with no diagnostic
            // (a[0] over [1.5, 2.5] used to read 48). varTypeMap symbol
            // provenance, like the static map check above.
            if (exprStartTok.type == TokenType::IDENTIFIER) {
                auto* thsym = cg->symTable.lookup(exprStartTok.lexeme);
                if (thsym && thsym->isTensor) {
                    parseError("E0115", "tensor values cannot be indexed with []; use flt_get(t, i)");
                    return nullptr;
                }
            }
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
            } else if (tt == TokenType::IDENTIFIER && peek().lexeme == "as") {
                // Phase-2 `as` cast, emit-mode mirror of the AST path
                // (contextual IDENT, same trigger rule, same errors).
                bool notYet = false;
                int tk = castTargetKind(peek(1).type, peek(1).lexeme, &notYet);
                if (tk == 0) break;
                Token asTok = advance(); // 'as'
                Token tyTok = advance(); // target
                (void)asTok;
                if (tk < 0) {
                    if (notYet)
                        parseError("E0014", "as-cast to '" + tyTok.lexeme + "' is not yet supported (only 'i64' and 'f64'; f64->i64 truncates toward zero)");
                    else
                        parseError("E0015", "as-casts support only 'i64' and 'f64' (got '" + tyTok.lexeme + "')");
                    return nullptr;
                }
                bool wantF64 = (tk == 2);
                llvm::Type* lt = left->getType();
                auto* dblTy = llvm::Type::getDoubleTy(*cg->ctx);
                if (wantF64) {
                    if (lt->isDoubleTy()) { /* no-op */ }
                    else if (lt->isIntegerTy(64))
                        left = cg->builder->CreateSIToFP(left, dblTy, "as_f64");
                    else {
                        parseError("E0011", "as f64 needs an i64 or f64 value");
                        return nullptr;
                    }
                } else {
                    if (lt->isIntegerTy(64)) { /* no-op */ }
                    // f64->i64 truncates toward zero (LLVM fptosi); NaN/+-Inf
                    // is poison, so panic first (same inline-split + cost as
                    // the AST path: non-finite block, then an overflow block
                    // for |v| >= 2^63), panic calls only in cold.
                    else if (lt->isDoubleTy()) {
                        auto* panicFn = cg->functionMap["flint_panic"];
                        if (panicFn) {
                            auto* curF = cg->builder->GetInsertBlock()->getParent();
                            auto* okBB = llvm::BasicBlock::Create(*cg->ctx, "cast_ok", curF);
                            auto* panicBB = llvm::BasicBlock::Create(*cg->ctx, "cast_panic", curF);
                            auto* zero = llvm::ConstantFP::get(dblTy, 0.0);
                            auto* posInf = llvm::ConstantFP::getInfinity(dblTy, false);
                            auto* negInf = llvm::ConstantFP::getInfinity(dblTy, true);
                            auto* isNan = cg->builder->CreateFCmpUNO(left, zero, "cast_nan");
                            auto* isPosInf = cg->builder->CreateFCmpOEQ(left, posInf, "cast_posinf");
                            auto* isNegInf = cg->builder->CreateFCmpOEQ(left, negInf, "cast_neginf");
                            auto* isInf = cg->builder->CreateOr(isPosInf, isNegInf, "cast_inf");
                            auto* bad = cg->builder->CreateOr(isNan, isInf, "cast_nonfinite");
                            cg->builder->CreateCondBr(bad, panicBB, okBB, coldWeightsTrue(*cg->ctx));
                            cg->builder->SetInsertPoint(panicBB);
                            cg->builder->CreateCall(panicFn,
                                {cg->builder->CreateGlobalString("f64 to i64 cast of non-finite value", "castmsg")});
                            cg->builder->CreateBr(okBB);
                            cg->builder->SetInsertPoint(okBB);
                            auto* two63 = llvm::ConstantFP::get(dblTy, 9223372036854775808.0);
                            auto* geBig = cg->builder->CreateFCmpOGE(left, two63, "cast_ge_2p63");
                            auto* ltNeg = cg->builder->CreateFCmpOLT(
                                left, llvm::ConstantFP::get(dblTy, -9223372036854775808.0), "cast_lt_neg2p63");
                            auto* oob = cg->builder->CreateOr(geBig, ltNeg, "cast_oob");
                            auto* oobBB = llvm::BasicBlock::Create(*cg->ctx, "cast_oob_panic", curF);
                            auto* finBB = llvm::BasicBlock::Create(*cg->ctx, "cast_finite", curF);
                            cg->builder->CreateCondBr(oob, oobBB, finBB, coldWeightsTrue(*cg->ctx));
                            cg->builder->SetInsertPoint(oobBB);
                            cg->builder->CreateCall(panicFn,
                                {cg->builder->CreateGlobalString("f64 to i64 cast overflow", "oobmsg")});
                            cg->builder->CreateBr(finBB);
                            cg->builder->SetInsertPoint(finBB);
                        }
                        left = cg->builder->CreateFPToSI(left, cg->i64Ty, "as_i64");
                    }
                    else {
                        parseError("E0012", "as i64 needs an i64 or f64 value");
                        return nullptr;
                    }
                }
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

int Parser::parseTensorNumEmit(double& out, bool& isF) {
    int depth = 0;
    while (check(TokenType::LPAREN)) {
        advance();
        depth++;
    }
    bool neg = false;
    if (check(TokenType::MINUS) && peek(1).type == TokenType::NUMBER_LITERAL) {
        neg = true;
        advance();
    }
    if (!check(TokenType::NUMBER_LITERAL)) return 0;
    Token nt = advance();
    for (int i = 0; i < depth; i++)
        if (!match(TokenType::RPAREN)) return 0;
    if (check(TokenType::IDENTIFIER) && isTensorDtypeSuffix(peek().lexeme)) {
        parseError("E0100", "numeric dtype suffix '" + peek().lexeme + "' is not yet supported (tensor literals are f64-only; convert with `as f64` / `as i64`)");
        return -1;
    }
    std::string lit = nt.lexeme;
    bool f = false;
    if (!lit.empty() && lit.back() == 'd') {
        f = true;
        lit.pop_back();
    }
    double v = 0.0;
    int64_t iv = 0;
    if (!f && parseI64Exact(lit, iv)) {
        v = static_cast<double>(iv);
    } else {
        v = std::stod(lit);
        if (!f) {
            double ip;
            if (std::modf(v, &ip) != 0.0) f = true;
        }
    }
    out = neg ? -v : v;
    isF = f;
    return 1;
}

int Parser::tryParseTensorEmit(TensorLiteralAST& t) {
    size_t afterOpen = getPos();
    if (check(TokenType::RBRACKET)) return 0; // [] stays an array
    if (check(TokenType::ELLIPSIS)) return 0; // spread: array path
    struct Row { std::vector<double> v; bool f = false; };
    std::vector<double> flat;
    std::vector<Row> rows;
    bool flatFloat = false, flatNums = true, allRows = true, poisoned = false;
    size_t nelems = 0;
    auto soft = [&]() -> int {
        setPos(afterOpen);
        return 0;
    };
    auto hard = [&](const std::string& m) -> int {
        parseError("E0143", m);
        int depth = 1;
        while (depth > 0 && !isAtEnd()) {
            if (check(TokenType::LBRACKET)) depth++;
            else if (check(TokenType::RBRACKET)) depth--;
            advance();
        }
        return -1;
    };
    while (!check(TokenType::RBRACKET) && !isAtEnd()) {
        nelems++;
        if (check(TokenType::LBRACKET)) {
            poisoned = true;
            flatNums = false;
            advance(); // inner '['
            Row r;
            bool ok = !check(TokenType::RBRACKET);
            while (ok && !check(TokenType::RBRACKET) && !isAtEnd()) {
                if (check(TokenType::LBRACKET))
                    return hard("tensor literals support only rank 1 and 2 (f64)");
                if (check(TokenType::ELLIPSIS)) {
                    ok = false;
                    break;
                }
                double d = 0.0;
                bool f = false;
                int rc = parseTensorNumEmit(d, f);
                if (rc < 0) return -1; // suffix error already reported; ']' unconsumed but progress made
                if (rc == 0) {
                    ok = false;
                    break;
                }
                r.v.push_back(d);
                r.f = r.f || f;
                if (!match(TokenType::COMMA)) break;
            }
            if (!ok) return hard("tensor literal rows must be non-empty all-numeric f64 literal lists");
            consume(TokenType::RBRACKET, "expected ']' to close tensor row");
            rows.push_back(std::move(r));
        } else {
            double d = 0.0;
            bool f = false;
            int rc = parseTensorNumEmit(d, f);
            if (rc < 0) return -1;
            if (rc == 0) {
                // Non-numeric element: consume one expression so the later
                // array path (or error) starts at a sane position, then mark.
                // Consumption is harmless: soft rewinds, hard skips.
                auto sink = parseExpressionEmit();
                (void)sink;
                flatNums = false;
                allRows = false;
            } else {
                flat.push_back(d);
                flatFloat = flatFloat || f;
                allRows = false;
                if (f) poisoned = true;
            }
        }
        if (!match(TokenType::COMMA)) break;
    }
    if (!poisoned) return soft(); // pure int/var/str/call: old array path
    if (flatNums) {
        t.rank = 1;
        t.rows = (int64_t)flat.size();
        t.values = std::move(flat);
        consume(TokenType::RBRACKET, "expected ']' after tensor literal");
        return 1;
    }
    if (allRows && rows.size() == nelems) {
        size_t cols = rows[0].v.size();
        bool ragged = false, anyF = false;
        for (auto& r : rows) {
            if (r.v.size() != cols) ragged = true;
            anyF = anyF || r.f;
        }
        if (ragged) return hard("ragged tensor literal: all rows must have the same length");
        if (!anyF)
            return hard("tensor literal needs at least one f64 element (add a decimal point); nested i64 arrays are not supported");
        t.rank = 2;
        t.rows = (int64_t)rows.size();
        t.cols = (int64_t)cols;
        for (auto& r : rows)
            for (double v : r.v) t.values.push_back(v);
        consume(TokenType::RBRACKET, "expected ']' after tensor literal");
        return 1;
    }
    return hard("tensor-shaped array literal is neither a flat f64 list nor equal-length f64 rows (use bare f64 literals, or build dynamic tensors with flint_create_1d/2d + flt_set)");
}

llvm::Value* Parser::emitTensorLiteralEmit(TensorLiteralAST& t) {
    auto itC1 = cg->functionMap.find("flt_create_1d");
    auto itC2 = cg->functionMap.find("flt_create_2d");
    auto itSet = cg->functionMap.find("flt_set");
    if (itC1 == cg->functionMap.end() || itC2 == cg->functionMap.end() ||
        itSet == cg->functionMap.end()) {
        parseError("E0114", "tensor runtime not registered");
        return nullptr;
    }
    auto* dblTy = llvm::Type::getDoubleTy(*cg->ctx);
    llvm::Value* ten = nullptr;
    if (t.rank == 1) {
        ten = cg->builder->CreateCall(itC1->second,
            {llvm::ConstantInt::get(cg->i64Ty, (uint64_t)t.rows)}, "tensor1");
    } else {
        ten = cg->builder->CreateCall(itC2->second,
            {llvm::ConstantInt::get(cg->i64Ty, (uint64_t)t.rows),
             llvm::ConstantInt::get(cg->i64Ty, (uint64_t)t.cols)}, "tensor2");
    }
    for (size_t i = 0; i < t.values.size(); i++) {
        cg->builder->CreateCall(itSet->second,
            {ten, llvm::ConstantInt::get(cg->i64Ty, i),
             llvm::ConstantFP::get(dblTy, t.values[i])});
    }
    return ten;
}

llvm::Value* Parser::parseNudEmit() {
    lastEmitValueTensor = false;
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
        // B1 UB audit: see AST path above — huge whole doubles stay f64.
        if (!std::isfinite(v) || v >= 9223372036854775807.0 ||
            v < -9223372036854775808.0) {
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*cg->ctx), v);
        }
        return llvm::ConstantInt::get(cg->i64Ty, static_cast<int64_t>(v));
    }
    if (match(TokenType::STRING_LITERAL)) {
        return cg->builder->CreateGlobalString(previous().lexeme, "str");
    }
    if (match(TokenType::LBRACKET)) {
        // Phase-2: tensor-shaped float literals desugar to flint_create_*.
        // Soft-fail rewinds to the array path below (identical rule to the
        // AST path, so both agree exactly).
        size_t afterOpen = getPos();
        TensorLiteralAST tensor;
        int trc = tryParseTensorEmit(tensor);
        if (trc > 0) {
            lastEmitValueTensor = true;
            return emitTensorLiteralEmit(tensor);
        }
        if (trc < 0) return nullptr;
        lastEmitValueTensor = false;
        setPos(afterOpen);
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
            parseError("E0094", "map runtime not registered"); return nullptr;
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
        parseError("E0090", "lambdas are not supported in generic functions");
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
            if (!sym) { parseError("E0122", "undefined variable '" + vname + "'"); return nullptr; }
            if (sym->moved) { parseError("E0027", "cannot borrow moved variable '" + vname + "'"); return nullptr; }
            sym->borrowCount++;
            cg->symTable.recordBorrow(vname);
            return sym->alloca;
        }
        parseError("E0018", "can only reference variables");
        return nullptr;
    }
    if (match(TokenType::STAR)) {
        auto target = parseNudEmit();
        if (!target) return nullptr;
        return cg->builder->CreateLoad(cg->i64Ty, target, "deref");
    }
    parseError("E0052", "expected expression");
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
                parseError("E0073", "flint_thread_spawn first arg must be a string literal");
                return nullptr;
            }
            Token funcNameTok = advance(); // consume the string literal
            std::string funcName = funcNameTok.lexeme;
            // Strip quotes from the string literal
            if (funcName.size() >= 2 && funcName.front() == '"' && funcName.back() == '"')
                funcName = funcName.substr(1, funcName.size() - 2);
            if (!match(TokenType::COMMA)) {
                parseError("E0075", "flint_thread_spawn needs 2 args: func_name, arg");
                return nullptr;
            }
            auto* argVal = parseExpressionEmit();
            if (!argVal) return nullptr;
            consume(TokenType::RPAREN, "expected ')' after arguments");
            auto fIt = cg->functionMap.find(funcName);
            if (fIt == cg->functionMap.end()) { parseError("E0078", "flint_thread_spawn: unknown function '" + funcName + "'"); return nullptr; }
            auto* targetFn = fIt->second;
            auto* wrapperPtr = cg->builder->CreateBitCast(targetFn, llvm::PointerType::get(*cg->ctx, 0), "thread_fn");
            auto* createFn = cg->functionMap["flint_thread_create"];
            if (!createFn) { parseError("E0077", "flint_thread_spawn: runtime function not found"); return nullptr; }
            auto* voidArg = cg->builder->CreateIntToPtr(argVal, cg->i8PtrTy, "thread_arg");
            return cg->builder->CreateCall(createFn, {wrapperPtr, voidArg}, "thread_id");
        }
        // M2b: explicit type arguments in emit mode — the same syntax and
        // the same rules as the AST path (both paths must agree).
        std::vector<Type> explicitTypeArgs;
        if (check(TokenType::COLON) && peek(1).type == TokenType::COLON) {
            advance(); // ':'
            advance(); // ':'
            if (!match(TokenType::LT)) {
                parseError("E0047", "expected '<' after '::' before explicit type arguments");
                return nullptr;
            }
            do {
                explicitTypeArgs.push_back(parseType());
            } while (match(TokenType::COMMA));
            consume(TokenType::GT, "expected '>' after explicit type arguments");
        }
        return emitCallEmit(name, explicitTypeArgs);
    }
    if (structRegistry.count(name) && match(TokenType::LBRACE)) {
        return emitStructLiteralEmit(name);
    }
    auto* sym = cg->symTable.lookup(name);
    if (sym) {
        if (sym->moved) { parseError("E0134", "use of moved variable '" + name + "'"); return nullptr; }
        if (sym->borrowCount > 0 && !sym->type.isCopyType()) { parseError("E0036", "cannot move '" + name + "' while borrowed"); return nullptr; }
        llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
        return cg->builder->CreateLoad(cg->resolvedLlvmType(sym->type), ptr, name.c_str());
    }
    auto git = cg->globalSymTable.find(name);
    if (git != cg->globalSymTable.end()) {
        return cg->builder->CreateLoad(cg->resolvedLlvmType(git->second.type), git->second.global, name.c_str());
    }
    parseError("E0122", "undefined variable '" + name + "'");
    return nullptr;
}

llvm::Value* Parser::emitArithOpEmit(char op, llvm::Value* l, llvm::Value* r) {
    if (!cg->safety.overflow) {
        switch (op) {
            case '+': return cg->builder->CreateAdd(l, r, "add");
            case '-': return cg->builder->CreateSub(l, r, "sub");
            default:  return cg->builder->CreateMul(l, r, "mul");
        }
    }
    // B2 identity mirror (agreement with AST + Flint e_bin_*): x+0=x etc.
    if (auto* lc = llvm::dyn_cast<llvm::ConstantInt>(l)) {
        if (op == '+' && lc->isZero()) return r;
        if (op == '*' && lc->isZero()) return llvm::ConstantInt::get(cg->i64Ty, 0);
        if (op == '*' && lc->isOne()) return r;
    }
    if (auto* rc = llvm::dyn_cast<llvm::ConstantInt>(r)) {
        if ((op == '+' || op == '-') && rc->isZero()) return l;
        if (op == '*' && rc->isZero()) return llvm::ConstantInt::get(cg->i64Ty, 0);
        if (op == '*' && rc->isOne()) return l;
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
    cg->builder->CreateCondBr(ov, panicBB, okBB, coldWeightsTrue(*cg->ctx));
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
    // P0-NEW-1: comparisons AND every aggregate-operand use route through the
    // shared Codegen core, so both paths lower identically and fixes land
    // once. Aggregate operands short-circuit BEFORE the arithmetic and
    // boolean branches: `||` does ICmpNE against zero and `+` would build an
    // FAdd on a struct -- either aborts the compiler.
    //
    // A null operand means an earlier emit already failed (cgHadError); every
    // branch below dereferences l and r, so return before touching them.
    if (!l || !r) return nullptr;
    {
        const std::string& o = opTok.lexeme;
        bool isCompare = (o == "==" || o == "!=" || o == "<" || o == ">" ||
                          o == "<=" || o == ">=");
        if (isCompare || l->getType()->isStructTy() || r->getType()->isStructTy())
            return cg->emitCompareValues(o, l, r);
    }
    TokenType tt = opTok.type;
    if (tt == TokenType::AT) {
        auto it = cg->functionMap.find("fao_matmul");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {l, r});
        parseError("E0059", "fao_matmul not linked");
        return nullptr;
    }
    // String concatenation: str + str
    if (tt == TokenType::PLUS && l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
        auto it = cg->functionMap.find("flint_str_concat");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {l, r});
        parseError("E0069", "flint_str_concat not linked");
        return nullptr;
    }
    // Mixed str + i64/f64: convert scalar to string first
    if (tt == TokenType::PLUS && l->getType()->isPointerTy() && r->getType()->isIntegerTy(64)) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto iti = cg->functionMap.find("flint_i64_to_string");
        if (itc != cg->functionMap.end() && iti != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {l, cg->builder->CreateCall(iti->second, {r})});
        parseError("E0071", "flint_str_concat/flint_i64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS && l->getType()->isIntegerTy(64) && r->getType()->isPointerTy()) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto iti = cg->functionMap.find("flint_i64_to_string");
        if (itc != cg->functionMap.end() && iti != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {cg->builder->CreateCall(iti->second, {l}), r});
        parseError("E0071", "flint_str_concat/flint_i64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS && l->getType()->isPointerTy() && r->getType()->isDoubleTy()) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto itf = cg->functionMap.find("flint_f64_to_string");
        if (itc != cg->functionMap.end() && itf != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {l, cg->builder->CreateCall(itf->second, {r})});
        parseError("E0070", "flint_str_concat/flint_f64_to_string not linked");
        return nullptr;
    }
    if (tt == TokenType::PLUS && l->getType()->isDoubleTy() && r->getType()->isPointerTy()) {
        auto itc = cg->functionMap.find("flint_str_concat");
        auto itf = cg->functionMap.find("flint_f64_to_string");
        if (itc != cg->functionMap.end() && itf != cg->functionMap.end())
            return cg->builder->CreateCall(itc->second, {cg->builder->CreateCall(itf->second, {l}), r});
        parseError("E0070", "flint_str_concat/flint_f64_to_string not linked");
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
        if (tt == TokenType::SLASH) { cg->emitDivByZeroCheck(r); return cg->builder->CreateSDiv(l, r, "div"); }
        if (tt == TokenType::MODULO) { cg->emitDivByZeroCheck(r); return cg->builder->CreateSRem(l, r, "mod"); }
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
    parseError("E0127", "unknown operator '" + opTok.lexeme + "'");
    return nullptr;
}

llvm::Value* Parser::emitIndexEmit(llvm::Value* base, llvm::Value* index) {
    // Map read (emit-mode mirror of Codegen::emitIndexAccess): str index
    // (ptr) on a ptr base is `m["k"]` -> flint_map_get, same as `m.get(k)`.
    if (index->getType()->isPointerTy()) {
        if (base->getType()->isStructTy()) {
            parseError("E0009", "array index needs i64 (got str; map keys are strings)");
            return nullptr;
        }
        auto it = cg->functionMap.find("flint_map_get");
        if (it == cg->functionMap.end()) {
            parseError("E0094", "map runtime not registered");
            return nullptr;
        }
        return cg->builder->CreateCall(it->second, {base, index}, "map_get");
    }
    llvm::Value* dataPtr = cg->builder->CreateExtractValue(base, {0}, "arr_ptr");
    llvm::Value* len = cg->builder->CreateExtractValue(base, {1}, "arr_len");
    cg->emitBoundsCheckInline(index, len);
    llvm::Value* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, index, "arr_elem");
    return cg->builder->CreateLoad(cg->i64Ty, elemPtr, "arr_elem_val");
}

llvm::Value* Parser::emitIndexStoreEmit(llvm::Value* base, llvm::Value* index, llvm::Value* val) {
    if (!base || !index || !val) return nullptr;
    if (!base->getType()->isStructTy()) {
        parseError("E0085", "index assignment requires an array variable");
        return nullptr;
    }
    llvm::Value* dataPtr = cg->builder->CreateExtractValue(base, {0}, "arr_ptr");
    llvm::Value* len = cg->builder->CreateExtractValue(base, {1}, "arr_len");
    cg->emitBoundsCheckInline(index, len);
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
            parseError("E0110", "struct '" + name + "' has no field '" + field + "'");
            return nullptr;
        }
    }
    parseError("E0019", "cannot access field '" + field + "' on non-struct type");
    return nullptr;
}

llvm::Value* Parser::emitArrayLiteralEmit(const std::vector<llvm::Value*>& elems) {
    size_t count = elems.size();
    // Array element types are erased at the LLVM boundary: every array is a
    // {ptr, i64} slice and every element access GEPs with i64Ty. So a non-i64
    // element cannot be stored here -- it would be reinterpreted as bits and
    // read back as a wrong number with no error. `[1.5, 2.5]` used to print 48.
    //
    // Rejecting loudly is strictly better than emitting a plausible wrong
    // answer. Widening arrays to f64 is a separate, deliberately-gated
    // change; until then the language refuses instead of lying.
    for (size_t i = 0; i < count; i++) {
        if (elems[i] && elems[i]->getType() != cg->i64Ty) {
            parseError("E0010", "array literals must hold i64 values, but element " +
                       std::to_string(i + 1) + " is " +
                       llvmTypeToString(elems[i]->getType()) +
                       " (arrays of other types are not supported yet)");
            return nullptr;
        }
    }
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
    if (it == structRegistry.end()) { parseError("E0128", "unknown struct '" + name + "'"); return nullptr; }
    auto& def = it->second;
    auto* st = cg->resolvedLlvmType(Type::struct_(name));
    if (!st) { parseError("E0111", "struct type not found '" + name + "'"); return nullptr; }
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
    if (eit == enumRegistry.end()) { parseError("E0123", "unknown enum '" + enumName + "'"); return nullptr; }
    auto& ed = eit->second;
    auto* enumTy = cg->resolvedLlvmType(Type::enum_(enumName));
    int tag = -1;
    for (size_t i = 0; i < ed.variants.size(); i++)
        if (ed.variants[i].name == variantName) { tag = (int)i; break; }
    if (tag < 0) { parseError("E0131", "unknown variant '" + variantName + "'"); return nullptr; }

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

llvm::Value* Parser::emitCallEmit(const std::string& callee,
                                  const std::vector<Type>& explicitTypeArgs) {
    std::vector<llvm::Value*> args;
    if (!check(TokenType::RPAREN)) {
        do {
            auto a = parseExpressionEmit();
            if (a) args.push_back(a);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after arguments");
    return emitDirectCall(callee, args, explicitTypeArgs);
}

llvm::Value* Parser::emitDirectCall(const std::string& callee, std::vector<llvm::Value*>& args,
                                    const std::vector<Type>& explicitTypeArgs) {
    // Map methods (emit-mode): fa_has/fa_get/fa_set only used on maps.
    if (callee == "fa_has" || callee == "fa_get" || callee == "fa_set") {
        std::string target = callee == "fa_has" ? "flint_map_has"
            : callee == "fa_get" ? "flint_map_get" : "flint_map_set";
        auto it = cg->functionMap.find(target);
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, args);
        parseError("E0094", "map runtime not registered");
        return nullptr;
    }
    if (callee == "py_eval") {
        cg->hasPython = true; markUsesPython();
        if (args.size() != 1) { parseError("E0104", "py_eval() needs one string arg"); return nullptr; }
        auto it = cg->functionMap.find("flint_py_eval_int");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {args[0]});
        parseError("E0105", "py_eval_int not declared");
        return nullptr;
    }

    if (callee == "print") {
        if (args.empty()) { parseError("E0102", "print() needs an arg"); return nullptr; }
        // P0-NEW-1: shared dispatch — key on the VALUE type, and unsupported
        // types now get a diagnostic instead of a silent wrong branch.
        return cg->emitPrintCall(args[0]);
    }

    if (callee == "flint_array_get") {
        if (args.size() != 2) { parseError("E0061", "flint_array_get needs 2 args"); return nullptr; }
        // P0-NEW-1: shared core — the struct guard now applies here too.
        return cg->emitArrayGetCore(args[0], args[1]);
    }

    if (callee == "flint_array_set") {
        if (args.size() != 3) { parseError("E0064", "flint_array_set needs 3 args"); return nullptr; }
        // P0-NEW-1: shared core — the struct guard now applies here too.
        return cg->emitArraySetCore(args[0], args[1], args[2]);
    }

    if (callee == "flint_array_data") {
        if (args.size() != 1) { parseError("E0060", "flint_array_data needs 1 arg"); return nullptr; }
        // P0-NEW-1: shared core (both paths).
        return cg->emitArrayDataCore(args[0]);
    }

    if (callee == "flint_array_get_ptr") {
        if (args.size() != 2) { parseError("E0063", "flint_array_get_ptr needs 2 args"); return nullptr; }
        // P0-NEW-1: shared core (both paths).
        return cg->emitArrayGetPtrCore(args[0], args[1]);
    }

    if (callee == "flint_array_set_ptr") {
        if (args.size() != 3) { parseError("E0066", "flint_array_set_ptr needs 3 args"); return nullptr; }
        // P0-NEW-1: shared core (both paths).
        return cg->emitArraySetPtrCore(args[0], args[1], args[2]);
    }

    if (!explicitTypeArgs.empty()) {
        parseError("E0057", "explicit type arguments are only allowed on a generic function; '" +
            callee + "' is not generic");
        return nullptr;
    }
    auto fIt = cg->functionMap.find(callee);
    if (fIt != cg->functionMap.end()) {
        // P0-NEW-1: shared call guard — the Parser copy had none, so a
        // wrong-arity or wrong-type call built invalid IR here.
        return cg->createCallChecked(callee, fIt->second, args);
    }
    auto gIt = cg->genericFunctions.find(callee);
    if (gIt != cg->genericFunctions.end()) {
        cg->typeSubstMap.clear();
        auto* genFn = gIt->second;
        // M2b: explicit type arguments (`f::<T>(...)`) override inference,
        // exactly as in the AST path.
        if (!explicitTypeArgs.empty()) {
            if (explicitTypeArgs.size() != genFn->typeParams.size()) {
                parseError("E0002", "'" + callee + "' expects " +
                    std::to_string(genFn->typeParams.size()) + " type argument(s), got " +
                    std::to_string(explicitTypeArgs.size()));
                cg->typeSubstMap.clear();
                return nullptr;
            }
            for (auto& ta : explicitTypeArgs) {
                if (ta.kind == TypeKind::TypeParam) {
                    parseError("E0117", "the explicit type argument for '" + callee + "' must be a concrete type");
                    cg->typeSubstMap.clear();
                    return nullptr;
                }
            }
            auto* espec = cg->specialize(genFn, explicitTypeArgs);
            cg->typeSubstMap.clear();
            if (!espec) { parseError("E0058", "failed to specialize '" + callee + "'"); return nullptr; }
            return cg->createCallChecked(callee, espec, args, /*instantiationMsg=*/true);
        }
        // P0-NEW-1: the by-PARAMETER rule, matching the AST path: a type
        // parameter infers from the ARGUMENT filling the parameter that
        // mentions it. Indexing type params by argument position -- the old
        // copy of this loop -- instantiated `fn swap<T,U>(a: U, b: T)` with
        // T and U swapped, silently. Un-inferrable still defaults to i64
        // (emit mode has no AST to interrogate); the shared call guard
        // below catches any resulting misfit instead of letting LLVM
        // reject the module.
        auto inferFromArg = [&](const std::string& tp, size_t pi) {
            if (pi >= args.size()) return;
            auto* argTy = args[pi]->getType();
            for (auto& [sName, sd] : structRegistry) {
                auto* st = llvm::StructType::getTypeByName(*cg->ctx, sName);
                if (st && argTy == st) {
                    cg->typeSubstMap[tp] = Type::struct_(sName);
                    return;
                }
            }
            auto eit = enumRegistry.begin();
            while (eit != enumRegistry.end()) {
                auto* ety = llvm::StructType::getTypeByName(*cg->ctx, eit->second.name);
                if (ety && argTy == ety) {
                    cg->typeSubstMap[tp] = Type::enum_(eit->second.name);
                    return;
                }
                ++eit;
            }
            if (argTy == cg->i64Ty) { cg->typeSubstMap[tp] = Type::i64(); return; }
            if (argTy == llvm::Type::getDoubleTy(*cg->ctx)) { cg->typeSubstMap[tp] = Type::f64(); return; }
            if (argTy == cg->i8PtrTy) { cg->typeSubstMap[tp] = Type::str(); return; }
            // An array is a {ptr, i64} slice — a LITERAL struct, so the
            // named-struct scan above cannot see it. The AST path knows
            // this case (exprFlintType: NodeKind::Array), so without it the
            // two paths disagree on the same program: the AST path
            // instantiates T = array(i64) and the emit path fell back to
            // i64, which the call guard then rejected.
            if (argTy->isStructTy()) {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(argTy)) {
                    if (st->isLiteral() && st->getNumElements() == 2 &&
                        st->getElementType(0)->isPointerTy() &&
                        st->getElementType(1)->isIntegerTy(64)) {
                        cg->typeSubstMap[tp] = Type::array(Type::i64());
                        return;
                    }
                }
            }
        };
        for (size_t ti = 0; ti < genFn->typeParams.size(); ti++) {
            const std::string& tp = genFn->typeParams[ti];
            size_t pi = (size_t)-1;
            for (size_t pp = 0; pp < genFn->params.size(); pp++) {
                if (genFn->params[pp].second.kind == TypeKind::TypeParam &&
                    genFn->params[pp].second.structName == tp) {
                    pi = pp;
                    break;
                }
            }
            if (pi != (size_t)-1) inferFromArg(tp, pi);
        }
        std::string instanceKey = callee;
        for (auto& [k, v] : cg->typeSubstMap) instanceKey += ":" + k + "=" + std::to_string((int)v.kind);
        auto cacheIt = cg->genericCache.find(instanceKey);
        if (cacheIt != cg->genericCache.end()) {
            cg->typeSubstMap.clear();
            // P0-NEW-1: shared call guard (both paths).
            return cg->createCallChecked(callee, cacheIt->second, args, /*instantiationMsg=*/true);
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
            // P0-NEW-1: shared call guard (both paths).
            return cg->createCallChecked(callee, spec, args, /*instantiationMsg=*/true);
        }
    }
    // M3 dispatch: method syntax reaches here as fa_<name>; try to resolve
    // it as a plain function taking the receiver as its first argument, the
    // way the enum-qualification rewrite works. Only the arity and, when the
    // receiver's LLVM type is a named struct/enum or a known scalar, the
    // first parameter type are taken into account.
    if (callee.rfind("fa_", 0) == 0 && !args.empty()) {
        std::string mName = callee.substr(3);
        Type recv; bool recvKnown = false;
        llvm::Type* lt = args[0]->getType();
        if (auto* st = llvm::dyn_cast<llvm::StructType>(lt)) {
            std::string n = st->getStructName().str();
            if (cg->structRegistry.count(n)) { recv = Type::struct_(n); recvKnown = true; }
            else if (cg->enumRegistry.count(n)) { recv = Type::enum_(n); recvKnown = true; }
        } else if (lt == cg->i64Ty) { recv = Type::i64(); recvKnown = true; }
        else if (lt->isDoubleTy()) { recv = Type::f64(); recvKnown = true; }
        std::string diag;
        FunctionAST* impl = cg->resolveUfcsImpl(mName, args.size(), recv, recvKnown, diag);
        if (!impl) { parseError("E0144", diag); return nullptr; }
        auto fit = cg->functionMap.find(impl->name);
        if (fit == cg->functionMap.end()) {
            parseError("E0099", "no type in scope implements '" + impl->name + "'");
            return nullptr;
        }
        // P0-NEW-1: shared call guard (both paths).
        return cg->createCallChecked(impl->name, fit->second, args);
    }
    parseError("E0017", "call to unknown function '" + callee + "'");
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
        std::string enumName, variantName;
        // A vector, not a single name: a variant may carry several payloads
        // (`Node(i64, i64)` binds as `Node(a, b)`). The parser accepted exactly
        // one identifier here, so any multi-payload pattern was a parse error,
        // which the non-advancing `consume` turned into an infinite loop.
        std::vector<std::string> bindNames;
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
            if (!check(TokenType::RPAREN)) {
                do {
                    arm.bindNames.push_back(
                        consume(TokenType::IDENTIFIER, "expected binding name").lexeme);
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RPAREN, "expected ')' after pattern bindings");
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
    if (eit == enumRegistry.end()) { parseError("E0123", "unknown enum '" + arms[0].enumName + "'"); return nullptr; }
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
        if (!arm.bindNames.empty() && armTag >= 0 && !ed.variants[armTag].payloadTypes.empty()) {
            auto& vt = ed.variants[armTag];
            // The pattern must bind exactly the payloads the variant carries.
            // A mismatch used to be silently ignored, so `Node(a)` on a
            // two-payload variant compiled with `a` bound to nothing.
            if (arm.bindNames.size() != vt.payloadTypes.size()) {
                parseError("E0101", "pattern " + arm.enumName + "." + arm.variantName + " binds " +
                           std::to_string(arm.bindNames.size()) + " value(s) but the variant has " +
                           std::to_string(vt.payloadTypes.size()));
                cg->symTable.exitScope();
                return nullptr;
            }
            std::vector<llvm::Type*> fldTys = {cg->i8Ty};
            for (auto& pt : vt.payloadTypes)
                fldTys.push_back(cg->resolvedLlvmType(pt));
            auto* varTy = llvm::StructType::get(*cg->ctx, fldTys);
            auto* bcPtr = cg->builder->CreateBitCast(alloca, cg->i8PtrTy);
            // One binding per payload field, in order.
            for (size_t bi = 0; bi < arm.bindNames.size(); bi++) {
                const std::string& name = arm.bindNames[bi];
                auto* valPtr = cg->builder->CreateStructGEP(varTy, bcPtr, (unsigned)(bi + 1));
                auto* fieldTy = cg->resolvedLlvmType(vt.payloadTypes[bi]);
                auto* val = cg->builder->CreateLoad(fieldTy, valPtr, name);
                auto* bindAlloca = cg->createEntryAlloca(val->getType(), name);
                cg->builder->CreateStore(val, bindAlloca);
                cg->symTable.declare(name, {vt.payloadTypes[bi], bindAlloca, nullptr, false, false, 0});
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
    // Same discipline as AST emitMatch: void-typed arms can never join a
    // PHI (LLVM aborts); mixed/disagreeing arms fail loudly via parseError.
    llvm::Type* phiTyEmit = nullptr;
    bool mixedEmit = false;
    for (size_t i = 0; i < armValsEmit.size(); i++) {
        if (!armReachEmit[i]) continue;
        llvm::Value* v = armValsEmit[i];
        if (!v) continue; // valueless arm: coerced to null below (legacy)
        if (v->getType()->isVoidTy()) {
            for (size_t j = 0; j < armValsEmit.size(); j++) {
                if (j == i || !armReachEmit[j]) continue;
                llvm::Value* u = armValsEmit[j];
                if (u && !u->getType()->isVoidTy()) { mixedEmit = true; break; }
            }
            if (mixedEmit) break;
            continue;
        }
        if (!phiTyEmit) phiTyEmit = v->getType();
        else if (v->getType() != phiTyEmit) { mixedEmit = true; break; }
    }
    if (mixedEmit) { parseError("E0096", "match arms must yield the same type"); return nullptr; }
    if (!phiTyEmit) return llvm::ConstantInt::get(cg->i64Ty, 0);
    auto* phiEmit = cg->builder->CreatePHI(phiTyEmit, (unsigned)armValsEmit.size() + 1, "matchval");
    for (size_t i = 0; i < armValsEmit.size(); i++) {
        if (!armReachEmit[i]) continue;
        llvm::Value* v = armValsEmit[i];
        if (!v || v->getType()->isVoidTy()) v = llvm::Constant::getNullValue(phiTyEmit);
        phiEmit->addIncoming(v, armEndEmit[i]);
    }
    phiEmit->addIncoming(llvm::Constant::getNullValue(phiTyEmit), switchBB);
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
        parseError("E0135", "variable '" + nameTok.lexeme + "' already declared");
    }
    Type varType = Type::i64();
    bool hasTypeAnnotation = match(TokenType::COLON);
    if (hasTypeAnnotation) varType = parseType();
    if (!match(TokenType::COLON_EQ)) {
        consume(TokenType::ASSIGN, "expected '=' or ':=' in variable declaration");
    }
    size_t exprStart = getPos();
    auto initExpr = parseExpressionEmit();
    if (!initExpr) { parseError("E0053", "expected expression after '='"); return; }
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
    if (lastEmitValueTensor) {
        auto* tref = cg->symTable.lookup(nameTok.lexeme);
        if (tref) tref->isTensor = true;
        lastEmitValueTensor = false;
    }
    // Detect move source: if RHS was a single identifier token
    if (getPos() == exprStart + 1 && previous().type == TokenType::IDENTIFIER) {
        std::string srcName = previous().lexeme;
        auto* srcSym = cg->symTable.lookup(srcName);
        if (srcSym && !srcSym->type.isCopyType()) {
            if (srcSym->borrowCount > 0)
                parseError("E0036", "cannot move '" + srcName + "' while borrowed");
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
                cg->builder->CreateRet(cg->coerceMainReturn(val));
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
    if (cg->breakStack.empty()) { parseError("E0016", "break outside loop"); return; }
    cg->builder->CreateBr(cg->breakStack.back());
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseContinueEmit() {
    if (cg->continueStack.empty()) { parseError("E0045", "continue outside loop"); return; }
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
            parseError("E0108", "range() takes 1 or 2 arguments: range(n) or range(start, end)");
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
        parseError("E0079", "for-in needs an array or str collection");
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
    // B6 migration: see parseParallelForStmt. Same removal, same advice, on
    // the emit path.
    parseError("E0004", "'parallel for' was removed: it could not capture variables and had no result channel. Use a plain 'for' loop, or move the work into a Rust runtime function such as flint_parallel_reduce");
}

void Parser::parsePythonBlockEmit() {
    cg->hasPython = true; markUsesPython();
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
    if (!fileBuf) {
        emitFileError("E9001", "cannot open '" + path + "'");
        firstError = "cannot open '" + path + "'";
        return;
    }
    llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
    recordImportHash(absPath, sourceRef);

    int importFileId = gSourceMap.addFile(path, sourceRef.str());
    Lexer lexer(sourceRef);
    Parser parser(lexer);
    parser.fileId = importFileId;
    auto prog = parser.parseProgram();

    if (parser.hadError()) {
        // The diagnostic is already printed. The summary goes into the
        // import chain's error so the caller can say WHICH import failed.
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

    // Clear `moved` on every binding in scope. Called between loop passes:
    // re-running a loop body re-initialises the bindings it writes, so a move
    // from one iteration must not be visible in the next.
    void reviveMoved() {
        for (auto& sc : scopes)
            for (auto& kv : sc) kv.second.moved = false;
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
            case NodeKind::Cast:
                // The cast reads its operand (a moved non-copy value must
                // still be rejected here).
                checkExpr(static_cast<CastExprAST*>(node)->expr.get());
                break;
            // Tensor holds only doubles: nothing to check.
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
            // Kinds that used to fall through to `default` and go unchecked.
            // Each carries sub-expressions that must be walked or a subtree
            // goes unsound.
            case NodeKind::Match: {
                auto* m = static_cast<MatchExprAST*>(node);
                checkExpr(m->scrutinee.get());
                for (auto& arm : m->arms) checkExpr(arm.body.get());
                break;
            }
            case NodeKind::Lambda:
                if (auto* l = static_cast<LambdaExprAST*>(node)) checkExpr(l->body.get());
                break;
            case NodeKind::StructLiteral: {
                auto* sl = static_cast<StructLiteralAST*>(node);
                for (auto& f : sl->fields) checkExpr(f.second.get());
                break;
            }
            case NodeKind::Unwrap:
                if (auto* u = static_cast<UnwrapExprAST*>(node)) checkExpr(u->inner.get());
                break;
            case NodeKind::Destructure: {
                auto* d = static_cast<DestructureAST*>(node);
                checkExpr(d->init.get());
                for (auto& nm : d->names) declare(nm, Type::i64(), true);
                break;
            }
            case NodeKind::PyBlock:
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
                // Loop fixpoint: walk the body twice. The first pass may mark a
                // binding declared OUTSIDE the loop as moved; the next
                // iteration re-runs those statements and re-initialises it, so
                // the move must not carry across the iteration boundary.
                checkBlock(w->body.get());
                reviveMoved();
                checkBlock(w->body.get());
                reviveMoved();
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

// ===================== AEGIS Lease Checker =====================
// The compile-time half of the hybrid model documented in
// runtime/flint_aegis.h: the runtime lease table catches dynamic frees and
// FFI-escaped leases, while this pass rejects the ones that are provable
// statically. It was lost with the 2026-09-14 src/main.cpp wipe, leaving all
// three documented errors runtime-only.
//
// A lease is the int64 returned by flint_aegis_alloc / _move. Variables that
// receive one are tracked per scope; a lease freed or moved out from under a
// later read is rejected at compile time rather than panicking at runtime.
class AegisChecker {
    enum class LeaseState { Live, Freed, Moved };
    struct Sym {
        bool isLease = false; // holds an aegis lease (not a plain int/str/...)
        LeaseState state = LeaseState::Live;
        int borrowCount = 0;
        // Set when this binding is an alias of another binding's lease (a
        // _borrow result). The count then lives on the named original.
        std::string aliasOf;
    };
    std::vector<std::unordered_map<std::string, Sym>> scopes;
    std::string funcName;
    bool hadErr = false;
    std::string errMsg;

    // Leases that each branch may retire. A free on one path must not be
    // treated as a free on the other, so branch effects are recorded per
    // side and only committed when the branch joins.
    std::unordered_map<std::string, LeaseState> thenEffects;
    std::unordered_map<std::string, LeaseState> elseEffects;

    void error(const std::string& msg) {
        if (!hadErr) { hadErr = true; errMsg = msg; }
    }

    void enterScope() { scopes.emplace_back(); }
    void exitScope() { if (!scopes.empty()) scopes.pop_back(); }

    Sym* lookup(const std::string& n) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // Follow aliasOf to the binding that owns the borrow count, so freeing or
    // moving through an alias is still checked against the real lease.
    Sym* owner(const std::string& n, std::string* rootName = nullptr) {
        Sym* sym = lookup(n);
        if (!sym || !sym->isLease) return sym;
        std::string cur = n;
        int guard = 0;
        while (!sym->aliasOf.empty() && guard++ < 16) {
            Sym* nx = lookup(sym->aliasOf);
            if (!nx || !nx->isLease) break;
            cur = sym->aliasOf;
            sym = nx;
        }
        if (rootName) *rootName = cur;
        return sym;
    }

    Sym* lookupLocal(const std::string& n) {
        if (scopes.empty()) return nullptr;
        auto f = scopes.back().find(n);
        return f == scopes.back().end() ? nullptr : &f->second;
    }

    void declare(const std::string& n, bool isLease) {
        Sym s; s.isLease = isLease;
        scopes.back()[n] = s;
    }

    static bool isAegisAlloc(const std::string& callee) {
        return callee == "flint_aegis_alloc";
    }
    // A fresh id from _move is still a lease.
    static bool isAegisProducer(const std::string& callee) {
        return callee == "flint_aegis_alloc" || callee == "flint_aegis_move";
    }
    // _borrow returns the same lease, not a new one; it increments a count.
    static bool isAegisBorrow(const std::string& callee) {
        return callee == "flint_aegis_borrow";
    }
    static bool isAegisUnborrow(const std::string& callee) {
        return callee == "flint_aegis_unborrow";
    }
    static bool isAegisFree(const std::string& callee) {
        return callee == "flint_aegis_free";
    }
    static bool isAegisUse(const std::string& callee) {
        // flint_aegis_check is the documented weak-reference query: it
        // answers "is this lease still live?" and returns 0 rather than
        // panicking. Asking it about a freed lease is the whole point, so it
        // is exempt from use-after-free.
        if (callee == "flint_aegis_check") return false;
        // Anything else that dereferences or inspects a lease.
        return callee.rfind("flint_aegis_", 0) == 0;
    }

    // Apply a retirement to the symbol and, if inside a branch, to that
    // branch's pending effects rather than to the symbol outright.
    void retire(Sym* sym, const std::string& name, LeaseState st, const char* what) {
        if (st == LeaseState::Freed && sym->borrowCount > 0) {
            error("cannot free lease '" + name + "' while it is borrowed");
            return;
        }
        if (inThen) thenEffects[name] = st;
        else if (inElse) elseEffects[name] = st;
        else sym->state = st;
        (void)what;
    }

    bool inThen = false;
    bool inElse = false;

    // Effective state of `name` at this point: a retirement recorded by the
    // branch currently being walked takes precedence over the committed
    // state, so `free(a); use(a)` inside one arm is still a use-after-free.
    LeaseState effectiveState(const std::string& name, LeaseState committed) {
        if (inThen) { auto it = thenEffects.find(name); if (it != thenEffects.end()) return it->second; }
        if (inElse) { auto it = elseEffects.find(name); if (it != elseEffects.end()) return it->second; }
        return committed;
    }

    void checkExpr(ExprAST* e) {
        if (!e) return;
        if (e->kind == NodeKind::Call) {
            auto* c = static_cast<CallExprAST*>(e);
            const std::string& callee = c->callee;
            if (isAegisProducer(callee)) {
                // alloc/move both yield a fresh Live lease. _move consumes its
                // argument, so retire it -- that is what makes a second move
                // of the same lease (or a use after it) an error.
                for (auto& a : c->args) checkExpr(a.get());
                if (callee == "flint_aegis_move" && !c->args.empty()) {
                    if (auto* arg = c->args[0].get()) {
                        if (arg->kind == NodeKind::Variable) {
                            std::string from = static_cast<VariableExprAST*>(arg)->name;
                            if (Sym* sym = lookup(from); sym && sym->isLease) {
                                if (sym->borrowCount > 0)
                                    error("cannot move lease '" + from + "' while it is borrowed");
                                else if (effectiveState(from, sym->state) != LeaseState::Live)
                                    error("aegis_uaf: lease '" + from + "' moved twice");
                                else
                                    retire(sym, from, LeaseState::Moved, "move");
                            }
                        }
                    }
                }
                return;
            }
            if (isAegisBorrow(callee)) {
                // Counts on the lease the call names. An enclosing assignment
                // makes the target an alias of that same lease, so the count
                // must not be incremented twice for one borrow.
                if (!c->args.empty()) {
                    if (auto* arg = c->args[0].get()) {
                        if (arg->kind == NodeKind::Variable) {
                            std::string name = static_cast<VariableExprAST*>(arg)->name;
                            if (Sym* sym = lookup(name); sym && sym->isLease) {
                                if (effectiveState(name, sym->state) != LeaseState::Live)
                                    error("aegis_uaf: lease '" + name + "' borrowed after retirement");
                                else sym->borrowCount++;
                            }
                        }
                    }
                }
                return;
            }
            if (isAegisFree(callee) && !c->args.empty()) {
                if (auto* arg = c->args[0].get()) {
                    if (arg->kind == NodeKind::Variable) {
                        std::string name = static_cast<VariableExprAST*>(arg)->name;
                        std::string root = name;
                        Sym* sym = owner(name, &root);
                        if (sym && sym->isLease) {
                            if (sym->state == LeaseState::Freed) {
                                error("aegis_double_free: lease '" + name + "' is already freed");
                            } else if (sym->state == LeaseState::Moved) {
                                error("aegis_uaf: lease '" + name + "' was already moved");
                            } else {
                                // A borrow recorded on this binding, or on any
                                // alias of it, blocks the free.
                                int held = sym->borrowCount;
                                for (auto& sc : scopes)
                                    for (auto& kv : sc)
                                        if (kv.second.isLease && !kv.second.aliasOf.empty() &&
                                            kv.second.aliasOf == name)
                                            held += kv.second.borrowCount;
                                if (held > 0)
                                    error("cannot free lease '" + name + "' while it is borrowed");
                                else
                                    retire(sym, root, LeaseState::Freed, "free");
                            }
                        }
                    }
                }
                return; // free is handled here; do not re-report via isAegisUse
            }
            if (isAegisUnborrow(callee)) {
                // The count lives on the binding that was borrowed (an alias),
                // not on the root: the root's count describes borrows of it.
                if (!c->args.empty()) {
                    if (auto* arg = c->args[0].get()) {
                        if (arg->kind == NodeKind::Variable) {
                            std::string name = static_cast<VariableExprAST*>(arg)->name;
                            if (Sym* sym = lookup(name); sym && sym->borrowCount > 0)
                                sym->borrowCount--;
                        }
                    }
                }
                return;
            }
            if (isAegisUse(callee)) {
                for (auto& a : c->args) {
                    if (!a || a->kind != NodeKind::Variable) continue;
                    std::string name = static_cast<VariableExprAST*>(a.get())->name;
                    Sym* sym = lookup(name);
                    if (!sym || !sym->isLease) continue;
                    LeaseState st = effectiveState(name, sym->state);
                    if (st == LeaseState::Freed)
                        error((inThen || inElse ? "aegis_branch_uaf: lease '" + name + "' used after free on this path"
                                                : "aegis_uaf: lease '" + name + "' used after free"));
                    else if (st == LeaseState::Moved && callee != "flint_aegis_move")
                        error("aegis_uaf: lease '" + name + "' used after move");
                }
                return; // arguments already inspected above
            }
            // flint_aegis_check is the weak-reference query and takes a
            // possibly-dead lease by design, so its argument is not walked
            // as an ordinary use.
            if (callee != "flint_aegis_check")
                for (auto& a : c->args) checkExpr(a.get());
            return;
        }
        if (e->kind == NodeKind::VarDecl) {
            auto* n = static_cast<VarDeclAST*>(e);
            // `x = flint_aegis_alloc(..)` binds a fresh Live lease.
            if (n->init && n->init->kind == NodeKind::Call) {
                auto* ic = static_cast<CallExprAST*>(n->init.get());
                if (isAegisProducer(ic->callee)) {
                    // Walk the initializer FIRST: for _move it retires the
                    // source lease, and declaring the target before that walk
                    // would let a stale pending effect survive.
                    checkExpr(n->init.get());
                    declare(n->varName, true);
                    Sym* sym = lookupLocal(n->varName);
                    sym->state = LeaseState::Live;   // a fresh id, always
                    sym->borrowCount = 0;
                    sym->aliasOf.clear();
                    return;
                }
                // `c = flint_aegis_borrow(b)` binds an ALIAS of b's lease: the
                // runtime hands back the same id, so the borrow is recorded
                // once, on this binding.
                if (isAegisBorrow(ic->callee) && !ic->args.empty()) {
                    if (auto* arg = ic->args[0].get()) {
                        if (arg->kind == NodeKind::Variable) {
                            std::string from = static_cast<VariableExprAST*>(arg)->name;
                            if (Sym* src2 = lookup(from); src2 && src2->isLease) {
                                declare(n->varName, true);
                                Sym* tgt = lookupLocal(n->varName);
                                tgt->state = effectiveState(from, src2->state);
                                tgt->borrowCount = 1;
                                tgt->aliasOf = from;
                                return;
                            }
                        }
                    }
                }
            }
            declare(n->varName, false);
            checkExpr(n->init.get());
            return;
        }
        if (e->kind == NodeKind::Assign) {
            auto* n = static_cast<AssignExprAST*>(e);
            // `c = flint_aegis_borrow(b)` makes c an ALIAS of b's lease --
            // the runtime returns the same id -- so the two bindings must
            // share one borrow count rather than each holding their own.
            if (n->rhs && n->rhs->kind == NodeKind::Call) {
                auto* rc = static_cast<CallExprAST*>(n->rhs.get());
                if (isAegisBorrow(rc->callee) && !rc->args.empty()) {
                    if (auto* arg = rc->args[0].get()) {
                        if (arg->kind == NodeKind::Variable) {
                            std::string from = static_cast<VariableExprAST*>(arg)->name;
                            if (Sym* src2 = lookup(from); src2 && src2->isLease) {
                                // One borrow, recorded once on the alias. The
                                // source is deliberately NOT walked here: that
                                // would add a second count for the same borrow.
                                thenEffects.erase(n->varName);
                                elseEffects.erase(n->varName);
                                auto apply = [&](Sym* tgt) {
                                    tgt->isLease = true;
                                    tgt->state = effectiveState(from, src2->state);
                                    tgt->borrowCount = 1;
                                    tgt->aliasOf = from;
                                };
                                if (Sym* tgt = lookupLocal(n->varName)) apply(tgt);
                                else {
                                    declare(n->varName, true);
                                    apply(lookupLocal(n->varName));
                                }
                                return;
                            }
                        }
                    }
                }
            }
            checkExpr(n->rhs.get());
            if (n->rhs && n->rhs->kind == NodeKind::Call &&
                isAegisProducer(static_cast<CallExprAST*>(n->rhs.get())->callee)) {
                // The target now holds a brand-new Live lease, so any pending
                // retirement recorded for it in this branch is stale.
                thenEffects.erase(n->varName);
                elseEffects.erase(n->varName);
                if (Sym* sym = lookupLocal(n->varName)) {
                    sym->isLease = true; sym->state = LeaseState::Live; sym->borrowCount = 0;
                    sym->aliasOf.clear();
                } else declare(n->varName, true);
            }
            return;
        }
        for (auto* c : Codegen::exprChildren(e)) checkExpr(c);
    }

    void checkBlock(BlockStmtAST* b) {
        enterScope();
        if (b) for (auto& st : b->stmts) checkStmt(st.get());
        exitScope();
    }

    void checkStmt(ExprAST* s) {
        if (!s) return;
        switch (s->kind) {
            case NodeKind::Block: checkBlock(static_cast<BlockStmtAST*>(s)); return;
            case NodeKind::If: {
                auto* n = static_cast<IfStmtAST*>(s);
                checkExpr(n->condition.get());
                thenEffects.clear(); elseEffects.clear();
                bool pt = inThen, pe = inElse;
                inThen = true; inElse = false;
                checkStmt(n->thenBlock.get());
                inThen = pt; inElse = true;
                checkStmt(n->elseBlock.get());
                inThen = pt; inElse = pe;
                for (auto& kv : thenEffects) {
                    auto* sym = lookup(kv.first);
                    if (!sym || !sym->isLease) continue;
                    if (elseEffects.count(kv.first)) sym->state = kv.second;
                }
                thenEffects.clear(); elseEffects.clear();
                return;
            }
            case NodeKind::While: {
                auto* n = static_cast<WhileStmtAST*>(s);
                checkExpr(n->condition.get());
                enterScope();
                checkStmt(n->body.get());
                // Loop fixpoint: a lease retired on one path is retired on the
                // next iteration too, so walk the body twice.
                checkStmt(n->body.get());
                exitScope();
                return;
            }
            case NodeKind::Return:
                if (auto* r = static_cast<ReturnStmtAST*>(s)) checkExpr(r->value.get());
                return;
            case NodeKind::Break:
            case NodeKind::Continue:
                return;
            default: checkExpr(s); return;
        }
    }

public:
    bool check(FunctionAST* fn, std::string& errorOut) {
        hadErr = false; errMsg.clear();
        funcName = fn->name;
        scopes.clear();
        inThen = inElse = false;
        enterScope();
        for (auto& p : fn->params) declare(p.first, false);
        if (fn->body) {
            if (fn->body->kind == NodeKind::Block) checkBlock(static_cast<BlockStmtAST*>(fn->body.get()));
            else checkStmt(fn->body.get());
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
            case NodeKind::Cast:
                std::cerr << "QBE: as-casts not supported yet\n";
                return "0";
            case NodeKind::Tensor:
                std::cerr << "QBE: tensor literals not supported yet\n";
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
            // Bind every payload field. Previously only the first was bound,
            // at a hardcoded offset, so a multi-payload pattern bound one name
            // and left the rest undefined. Offsets are 8 + i*8: the struct is
            // a 1-byte tag followed by i64 payload fields.
            for (size_t bi = 0; bi < arm.bindNames.size(); bi++) {
                std::string bindSlot = getSlot(arm.bindNames[bi]);
                std::string pPtr = newTmp('l'), pVal = newTmp('l'), sp = newTmp('l');
                std::string off = std::to_string(8 + bi * 8);
                il << "\t" << sp << " =l loadl " << scrutSlot << "\n";
                il << "\t" << pPtr << " =l add " << sp << ", " << off << "\n";
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

// ============================================================================
// TYPE CHECKER — Phase 2 / 2A
//
// The compiler's first CHECKING phase: after parsing and import merging,
// after declaration generation (so the builtin registry exists), and before
// any function body is emitted. It checks only the DECIDABLE subset:
//
//   * an annotated variable declaration is a contract (the initialiser must
//     have the annotated type);
//   * an annotated return type is a contract (every `return`, including the
//     parser's synthetic one for a trailing expression, must have it);
//   * assignment to an annotated variable must keep its type;
//   * arguments to user functions and externs must match their declared
//     parameter types;
//   * a comparison may not mix i64 and f64 (LLVM has no FCmp for that; the
//     compiler used to crash with "Both operands to FCmp ... are not of the
//     same type").
//
// Everything it does not model answers UNKNOWN, and an UNKNOWN operand
// suppresses the check. That is the "never reject a program the compiler
// accepts" rule: the checker may MISS an error, never invent one. The
// differential gate (tests/test_type_check.sh) runs the corpus with and
// without this phase and requires identical accept/reject decisions.
//
// Types are compared through llvmType(), the same function the codegen's
// call guard uses. bool and i64 are therefore compatible (both LLVM i64),
// and all pointer-sized kinds are compatible (LLVM opaque pointers) exactly
// as the backend treats them; an f64-vs-i64 mismatch is still a mismatch.
//
// This is deliberately NOT a typed HIR. It records nothing and changes no
// codegen type; it only refuses what is provably wrong before LLVM sees it.
// ============================================================================

class TypeChecker {
public:
    TypeChecker(ProgramAST& prog, Codegen& cg, int fileId)
        : prog(prog), cg(cg), fileId(fileId) {}

    bool run() {
        indexDeclarations();
        checkGlobals();
        for (auto& fn : prog.functions) {
            if (fn->isDeclaration || !fn->body) continue;
            checkFunction(fn.get());
        }
        return !hadError;
    }

    int errorCount() const { return errors; }

private:
    // A checked expression type. `known` is the whole safety valve: an
    // expression shape the checker does not model is not a guess, it is
    // silence.
    struct CType {
        bool known = false;
        Type t;
    };

    struct VarInfo {
        bool known = true;
        bool annotated = false;
        Type type;
    };

    ProgramAST& prog;
    Codegen& cg;
    int fileId;
    bool hadError = false;
    int errors = 0;

    const FunctionAST* currentFn = nullptr;
    Type currentRet;
    bool currentRetAnnotated = false;

    std::vector<std::unordered_map<std::string, VarInfo>> scopes;
    std::unordered_map<std::string, VarInfo> globals;
    std::unordered_map<std::string, FunctionAST*> userFns;
    std::unordered_map<std::string, const ExternFn*> externFns;
    std::unordered_map<std::string, const StructDef*> structs;
    std::unordered_map<std::string, const EnumDef*> enums;

    static CType mkKnown(const Type& t) {
        CType c;
        c.known = true;
        c.t = t;
        return c;
    }
    static CType mkUnknown() { return CType{}; }

    static std::string typeName(const Type& t) { return Codegen::typeTag(t); }

    // A STORAGE context (declaration/global initialiser, assignment) stores
    // through an opaque-pointer slot, and the backend accepts an i64 there:
    // the runtime returns opaque handles as i64 (`flint_atomic_new`) while
    // the source annotates them `ptr`, and codegen's alloca is `ptr`. The
    // store is valid IR under opaque pointers and the load reinterprets the
    // same 8 bytes -- the handle convention, not a type error.
    //
    // Only `ptr` is relaxed this way. A `str`/`map` slot holding an
    // arbitrary integer would print/dereference a bogus address, and an
    // i64 ANNOTATION given a pointer triggers codegen's retyping ladder
    // (the variable silently becomes a str), which is the contract
    // violation E0290 exists to report.
    bool compatibleStore(const Type& dest, const Type& val) {
        if (compatible(dest, val)) return true;
        return dest.kind == TypeKind::Ptr &&
               (val.kind == TypeKind::I64 || val.kind == TypeKind::Bool);
    }

    // The one compatibility rule. It is intentionally the BACKEND's rule,
    // not a stricter one: rejecting something the codegen accepts would
    // break valid programs (the differential gate checks exactly that).
    bool compatible(const Type& a, const Type& b) {
        if (a.kind == TypeKind::TypeParam || b.kind == TypeKind::TypeParam)
            return true;   // generic: substituted before codegen, not modelled here
        if (a.kind == TypeKind::Void || b.kind == TypeKind::Void)
            return false;  // a void expression has no value to store or return
        llvm::Type* la = llvmType(a, *cg.ctx, cg.mod.get());
        llvm::Type* lb = llvmType(b, *cg.ctx, cg.mod.get());
        if (!la || !lb) return a == b;
        return la == lb;
    }

    // ---- diagnostics ----------------------------------------------------

    void typeError(const char* code, const std::string& message,
                   const ExprAST* at, const ExprAST* context = nullptr,
                   const std::string& help = "") {
        Diagnostic d;
        d.code = code ? code : "";
        d.level = "error";
        d.message = message;
        d.help = help;
        auto addLabel = [&](const ExprAST* n, bool primary,
                            const std::string& text) {
            if (!n) return;
            DiagLabel l;
            l.primary = primary;
            l.message = text;
            l.span.fileId = fileId;
            l.span.start.line = n->loc.line > 0 ? n->loc.line : 1;
            l.span.start.col = n->loc.col > 0 ? n->loc.col : 1;
            l.span.end = l.span.start;
            l.span.end.col += 1;
            d.labels.push_back(l);
        };
        addLabel(at, true, "");
        if (context && context != at) addLabel(context, false, "the declaration is here");
        emitDiagnostic(d);
        hadError = true;
        errors++;
    }

    // ---- scopes ---------------------------------------------------------

    void pushScope() { scopes.push_back({}); }
    void popScope() { if (!scopes.empty()) scopes.pop_back(); }

    void declare(const std::string& name, const VarInfo& v) {
        if (scopes.empty()) scopes.push_back({});
        scopes.back()[name] = v;
    }

    const VarInfo* lookup(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        auto g = globals.find(name);
        if (g != globals.end()) return &g->second;
        return nullptr;
    }

    // ---- declarations ---------------------------------------------------

    void indexDeclarations() {
        for (auto& s : prog.structs) structs[s.name] = &s;
        for (auto& e : prog.enums) enums[e.name] = &e;
        for (auto& fn : prog.functions)
            if (!userFns.count(fn->name)) userFns[fn->name] = fn.get();
        for (auto& ex : prog.externs) externFns[ex.name] = &ex;
    }

    void checkGlobals() {
        // Declare all globals first (a global may refer to a later one),
        // but trust only an ANNOTATED type until the initialiser is seen.
        for (auto& g : prog.globals) {
            VarInfo v;
            v.type = g->varType;
            v.annotated = g->typeWasAnnotated;
            v.known = g->typeWasAnnotated;
            globals[g->varName] = v;
        }
        for (auto& g : prog.globals) {
            CType init = g->init ? typeOf(g->init.get()) : mkUnknown();
            if (g->init && g->typeWasAnnotated &&
                g->varType.kind != TypeKind::TypeParam && init.known &&
                !compatibleStore(g->varType, init.t)) {
                typeError("E0290",
                    "cannot initialise '" + g->varName + "': the annotation is " +
                        typeName(g->varType) + ", but the value is " + typeName(init.t),
                    g->init.get(), g.get(),
                    "convert the value or change the annotation");
            }
            if (!g->typeWasAnnotated && init.known && init.t.kind != TypeKind::Void) {
                globals[g->varName].type = init.t;
                globals[g->varName].known = true;
            }
        }
    }

    void checkFunction(FunctionAST* fn) {
        scopes.clear();
        pushScope();
        for (size_t i = 0; i < fn->params.size(); i++) {
            VarInfo v;
            v.type = fn->params[i].second;
            bool untyped = false;
            for (size_t u : fn->untypedParams) if (u == i) { untyped = true; break; }
            // An untyped parameter is retyped at each call site; its
            // placeholder i64 is not a fact the checker may rely on.
            v.known = !untyped;
            v.annotated = !untyped;
            declare(fn->params[i].first, v);
        }
        currentFn = fn;
        currentRet = fn->returnType;
        currentRetAnnotated = fn->returnTypeWasAnnotated;
        if (fn->body->kind == NodeKind::Block)
            checkBlock(static_cast<BlockStmtAST*>(fn->body.get()));
        else
            typeOf(fn->body.get());
        currentFn = nullptr;
        popScope();
    }

    // ---- statements -----------------------------------------------------

    CType checkBlock(BlockStmtAST* b) {
        if (!b) return mkUnknown();
        pushScope();
        CType val = mkUnknown();
        for (size_t i = 0; i < b->stmts.size(); i++) {
            CType cur = checkStmt(b->stmts[i].get());
            bool last = (i + 1 == b->stmts.size());
            val = (last && b->stmts[i] && Parser::isValueExpr(b->stmts[i]->kind))
                      ? cur : mkUnknown();
        }
        popScope();
        return val;
    }

    CType checkStmt(ExprAST* s) {
        if (!s) return mkUnknown();
        switch (s->kind) {
            case NodeKind::Block:
                return checkBlock(static_cast<BlockStmtAST*>(s));
            case NodeKind::VarDecl:
                return checkVarDecl(static_cast<VarDeclAST*>(s));
            case NodeKind::Return:
                return checkReturn(static_cast<ReturnStmtAST*>(s));
            case NodeKind::Break:
            case NodeKind::Continue:
                return mkUnknown();
            default:
                return typeOf(s);
        }
    }

    CType checkVarDecl(VarDeclAST* d) {
        CType init = d->init ? typeOf(d->init.get()) : mkUnknown();
        if (d->init && d->typeWasAnnotated &&
            d->varType.kind != TypeKind::TypeParam && init.known &&
            !compatibleStore(d->varType, init.t)) {
            typeError("E0290",
                "cannot initialise '" + d->varName + "': the annotation is " +
                    typeName(d->varType) + ", but the value is " + typeName(init.t),
                d->init.get(), d,
                "convert the value or change the annotation");
        }
        VarInfo v;
        v.annotated = d->typeWasAnnotated;
        if (d->typeWasAnnotated && d->varType.kind != TypeKind::TypeParam) {
            v.type = d->varType;
            v.known = true;
        } else if (init.known && init.t.kind != TypeKind::Void) {
            // The initialiser's checked type is more trustworthy than the
            // parser's guess; it is also what codegen will have produced.
            v.type = init.t;
            v.known = true;
        } else {
            v.type = d->varType;
            v.known = false;
        }
        declare(d->varName, v);
        return init;
    }

    CType checkReturn(ReturnStmtAST* r) {
        CType v = r->value ? typeOf(r->value.get()) : mkUnknown();
        if (r->value && currentFn && currentRetAnnotated &&
            currentRet.kind != TypeKind::Void && v.known &&
            v.t.kind != TypeKind::Void && !compatible(currentRet, v.t)) {
            typeError("E0291",
                "cannot return a value of type " + typeName(v.t) + " from '" +
                    currentFn->name + "', which is declared to return " +
                    typeName(currentRet),
                r->value.get(), r,
                "convert the value or change the return type");
        }
        return v;
    }

    // ---- expressions ----------------------------------------------------

    CType typeOf(ExprAST* e) {
        if (!e) return mkUnknown();
        switch (e->kind) {
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(e);
                if (n->isFloat) return mkKnown(Type::f64());
                if (n->isInt) return mkKnown(Type::i64());
                double ip = 0;
                return mkKnown(std::modf(n->value, &ip) != 0.0 ? Type::f64()
                                                               : Type::i64());
            }
            case NodeKind::String: return mkKnown(Type::str());
            case NodeKind::Tensor: return mkKnown(Type::str());
            case NodeKind::Variable: {
                auto* ve = static_cast<VariableExprAST*>(e);
                const VarInfo* v = lookup(ve->name);
                // A DECLARED variable shadows a function of the same name,
                // even when its own type is unknown: silence, not the
                // function's signature.
                if (v) return v->known ? mkKnown(v->type) : mkUnknown();
                // A NAMED function used as a value (`h = inc`). Its fn type
                // is the declaration's signature + return type, which is
                // also the thunk codegen builds, so a later `h(5)` is
                // checked through the same value path a lambda uses. A
                // generic or void function has no fn value: stay unknown.
                auto uit = userFns.find(ve->name);
                if (uit != userFns.end() && uit->second->typeParams.empty() &&
                    uit->second->returnType.kind != TypeKind::Void) {
                    std::vector<Type> ps;
                    bool concrete = true;
                    for (auto& p : uit->second->params) {
                        if (p.second.kind == TypeKind::TypeParam) {
                            concrete = false;
                            break;
                        }
                        ps.push_back(p.second);
                    }
                    if (concrete)
                        return mkKnown(Type::fn_(ps, uit->second->returnType));
                }
                return mkUnknown();
            }
            case NodeKind::Array: {
                auto* a = static_cast<ArrayExprAST*>(e);
                for (auto& el : a->elements) typeOf(el.get());
                return mkKnown(Type::array(Type::i64()));
            }
            case NodeKind::MapLiteral: {
                auto* m = static_cast<MapLiteralAST*>(e);
                for (auto& v : m->values) typeOf(v.get());
                return mkKnown(Type::map());
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(e);
                CType l = typeOf(b->lhs.get());
                CType r = typeOf(b->rhs.get());
                if (b->op == '+' && ((l.known && l.t.kind == TypeKind::Str) ||
                                     (r.known && r.t.kind == TypeKind::Str)))
                    return mkKnown(Type::str());
                if ((l.known && l.t.kind == TypeKind::F64) ||
                    (r.known && r.t.kind == TypeKind::F64))
                    return mkKnown(Type::f64());
                if (l.known && r.known && isIntLike(l.t) && isIntLike(r.t))
                    return mkKnown(Type::i64());
                return mkUnknown();
            }
            case NodeKind::Compare: {
                auto* c = static_cast<CompareExprAST*>(e);
                CType l = typeOf(c->lhs.get());
                CType r = typeOf(c->rhs.get());
                if (l.known && r.known && l.t.kind != TypeKind::Void &&
                    r.t.kind != TypeKind::Void) {
                    bool lf = (l.t.kind == TypeKind::F64);
                    bool rf = (r.t.kind == TypeKind::F64);
                    if ((lf && isIntLike(r.t)) || (rf && isIntLike(l.t))) {
                        typeError("E0294",
                            "cannot compare " + typeName(l.t) + " with " +
                                typeName(r.t) + "; convert one side explicitly",
                            e, nullptr,
                            "write `as f64` on the integer, or `as i64` on the float");
                    }
                }
                return mkKnown(Type::i64());
            }
            case NodeKind::Call:
                return typeOfCall(static_cast<CallExprAST*>(e));
            case NodeKind::Cast: {
                auto* c = static_cast<CastExprAST*>(e);
                typeOf(c->expr.get());
                return mkKnown(c->target);
            }
            case NodeKind::Index: {
                auto* ix = static_cast<IndexExprAST*>(e);
                CType base = typeOf(ix->base.get());
                typeOf(ix->index.get());
                if (base.known) {
                    if (base.t.kind == TypeKind::Array) return mkKnown(Type::i64());
                    if (base.t.kind == TypeKind::Str ||
                        base.t.kind == TypeKind::Map) return mkKnown(Type::i64());
                }
                return mkUnknown();
            }
            case NodeKind::Slice: {
                auto* sl = static_cast<SliceExprAST*>(e);
                typeOf(sl->arr.get());
                typeOf(sl->start.get());
                if (sl->end) typeOf(sl->end.get());
                return mkUnknown();
            }
            case NodeKind::Ref: {
                auto* r = static_cast<RefExprAST*>(e);
                CType t = typeOf(r->target.get());
                if (t.known && t.t.kind != TypeKind::Void)
                    return mkKnown(Type::ref(t.t));
                return mkUnknown();
            }
            case NodeKind::Deref: {
                auto* d = static_cast<DerefExprAST*>(e);
                CType t = typeOf(d->target.get());
                if (t.known && t.t.kind == TypeKind::Ref && t.t.elemType)
                    return mkKnown(*t.t.elemType);
                return mkUnknown();
            }
            case NodeKind::StructLiteral: {
                auto* sl = static_cast<StructLiteralAST*>(e);
                for (auto& f : sl->fields) typeOf(f.second.get());
                return mkKnown(Type::struct_(sl->structName));
            }
            case NodeKind::EnumConstruct: {
                auto* ec = static_cast<EnumConstructAST*>(e);
                for (auto& a : ec->args) typeOf(a.get());
                return mkKnown(Type::enum_(ec->enumName));
            }
            case NodeKind::FieldAccess: {
                auto* fa = static_cast<FieldAccessAST*>(e);
                CType base = typeOf(fa->base.get());
                if (base.known && base.t.kind == TypeKind::Struct) {
                    auto sit = structs.find(base.t.structName);
                    if (sit != structs.end()) {
                        for (auto& fd : sit->second->fields)
                            if (fd.name == fa->fieldName) return mkKnown(fd.type);
                    }
                }
                return mkUnknown();
            }
            case NodeKind::If: {
                auto* st = static_cast<IfStmtAST*>(e);
                typeOf(st->condition.get());
                CType t = checkStmt(st->thenBlock.get());
                CType el = st->elseBlock ? checkStmt(st->elseBlock.get()) : mkUnknown();
                if (t.known && el.known && t.t.kind != TypeKind::Void &&
                    el.t.kind != TypeKind::Void && compatible(t.t, el.t))
                    return t;
                return mkUnknown();
            }
            case NodeKind::While: {
                auto* w = static_cast<WhileStmtAST*>(e);
                typeOf(w->condition.get());
                checkStmt(w->body.get());
                return mkUnknown();
            }
            case NodeKind::Match: {
                auto* m = static_cast<MatchExprAST*>(e);
                CType sc = typeOf(m->scrutinee.get());
                const EnumDef* ed = nullptr;
                if (sc.known && sc.t.kind == TypeKind::Enum) {
                    auto eit = enums.find(sc.t.structName);
                    if (eit != enums.end()) ed = eit->second;
                }
                CType result = mkUnknown();
                Type common;
                bool first = true;
                for (auto& arm : m->arms) {
                    pushScope();
                    if (ed) {
                        for (auto& vd : ed->variants) {
                            if (vd.name != arm.variantName) continue;
                            for (size_t i = 0; i < arm.bindNames.size(); i++) {
                                VarInfo v;
                                if (i < vd.payloadTypes.size()) {
                                    v.type = vd.payloadTypes[i];
                                    v.known = true;
                                } else {
                                    v.known = false;
                                }
                                declare(arm.bindNames[i], v);
                            }
                            break;
                        }
                    }
                    CType b = checkStmt(arm.body.get());
                    popScope();
                    if (b.known && b.t.kind != TypeKind::Void) {
                        if (first) { common = b.t; first = false; result = b; }
                        else if (!compatible(common, b.t)) result = mkUnknown();
                    }
                }
                return result;
            }
            case NodeKind::Lambda: {
                auto* lam = static_cast<LambdaExprAST*>(e);
                for (auto& p : lam->params) {
                    if (p.second.kind == TypeKind::TypeParam) return mkUnknown();
                }
                if (lam->retType.kind == TypeKind::Void) return mkUnknown();
                std::vector<Type> ps;
                for (auto& p : lam->params) ps.push_back(p.second);
                return mkKnown(Type::fn_(ps, lam->retType));
            }
            case NodeKind::Unwrap: {
                auto* u = static_cast<UnwrapExprAST*>(e);
                typeOf(u->inner.get());
                return mkUnknown();
            }
            case NodeKind::Destructure: {
                auto* d = static_cast<DestructureAST*>(e);
                typeOf(d->init.get());
                for (auto& n : d->names) {
                    VarInfo v;
                    v.known = false;
                    declare(n, v);
                }
                return mkUnknown();
            }
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(e);
                CType rhs = typeOf(a->rhs.get());
                const VarInfo* v = lookup(a->varName);
                if (v && v->known && v->annotated && rhs.known &&
                    !compatibleStore(v->type, rhs.t)) {
                    typeError("E0292",
                        "cannot assign a value of type " + typeName(rhs.t) +
                            " to '" + a->varName + "' of type " + typeName(v->type),
                        a->rhs.get(), a,
                        "convert the value or declare the variable with the new type");
                }
                return rhs;
            }
            case NodeKind::VarDecl:
                return checkVarDecl(static_cast<VarDeclAST*>(e));
            case NodeKind::Return:
                return checkReturn(static_cast<ReturnStmtAST*>(e));
            case NodeKind::PyBlock:
                return mkKnown(Type::void_());
            case NodeKind::Block:
                return checkBlock(static_cast<BlockStmtAST*>(e));
            default:
                return mkUnknown();
        }
    }

    static bool isIntLike(const Type& t) {
        return t.kind == TypeKind::I64 || t.kind == TypeKind::Bool;
    }

    void checkArgs(CallExprAST* call,
                   const std::vector<std::pair<std::string, Type>>& params,
                   const std::vector<size_t>& untyped,
                   const std::vector<CType>& argv) {
        if (argv.size() > params.size()) return;  // arity: codegen owns it
        for (size_t i = 0; i < argv.size(); i++) {
            bool skip = false;
            for (size_t u : untyped) if (u == i) { skip = true; break; }
            if (skip) continue;
            ExprAST* a = call->args[i].get();
            if (!a || a->kind == NodeKind::Lambda) continue;  // codegen checks signatures
            if (!argv[i].known || argv[i].t.kind == TypeKind::Void) continue;
            const Type& pt = params[i].second;
            if (pt.kind == TypeKind::TypeParam) continue;
            if (!compatible(pt, argv[i].t)) {
                typeError("E0293",
                    "call to '" + call->callee + "': argument " +
                        std::to_string(i + 1) + " is " + typeName(argv[i].t) +
                        ", but parameter " + std::to_string(i + 1) + " is " +
                        typeName(pt),
                    a, call,
                    "convert the argument or change parameter " + std::to_string(i + 1));
            }
        }
    }

    CType typeOfCall(CallExprAST* call) {
        std::vector<CType> argv;
        argv.reserve(call->args.size());
        for (auto& a : call->args) argv.push_back(typeOf(a.get()));

        const std::string& name = call->callee;
        // The printer accepts every value type; the array/thread builtins
        // are lowered by hand, so their registered LLVM signature is not
        // the call's source-level contract. Do not invent checks for them.
        if (name == "print" || name == "println") return mkKnown(Type::void_());
        if (name == "len") return mkKnown(Type::i64());
        if (name == "flint_array_get") return mkKnown(Type::i64());
        if (name == "flint_array_set" || name == "flint_array_data" ||
            name == "flint_array_get_ptr" || name == "flint_array_set_ptr" ||
            name == "flint_thread_spawn")
            return mkUnknown();
        if (name.rfind("fa_", 0) == 0) return mkUnknown();  // trait method dispatch

        auto uit = userFns.find(name);
        if (uit != userFns.end()) {
            FunctionAST* fn = uit->second;
            // A generic is instantiated at codegen; argument types there are
            // checked against the INSTANTIATED signature, not the template.
            if (fn->typeParams.empty())
                checkArgs(call, fn->params, fn->untypedParams, argv);
            if (fn->returnType.kind == TypeKind::TypeParam) return mkUnknown();
            return mkKnown(fn->returnType);
        }

        auto eit = externFns.find(name);
        if (eit != externFns.end()) {
            const ExternFn* ex = eit->second;
            // Always check the fixed parameters. The old condition skipped
            // every type check for a variadic call with extra arguments,
            // which is exactly the printf case.
            {
                for (size_t i = 0; i < argv.size() && i < ex->paramTypes.size(); i++) {
                    ExprAST* a = call->args[i].get();
                    if (!a || !argv[i].known || argv[i].t.kind == TypeKind::Void) continue;
                    const Type& pt = ex->paramTypes[i];
                    if (pt.kind == TypeKind::TypeParam) continue;
                    if (!compatible(pt, argv[i].t)) {
                        typeError("E0293",
                            "call to '" + call->callee + "': argument " +
                                std::to_string(i + 1) + " is " + typeName(argv[i].t) +
                                ", but parameter " + std::to_string(i + 1) + " is " +
                                typeName(pt),
                            a, call,
                            "convert the argument or change parameter " + std::to_string(i + 1));
                    }
                }
            }
            return mkKnown(ex->returnType);
        }

        // A function VALUE (`let g = f; g(5)`) carries its signature in the
        // binding.
        const VarInfo* v = lookup(name);
        if (v && v->known && v->type.kind == TypeKind::Func && v->type.funcRet) {
            const Type& ft = v->type;
            for (size_t i = 0; i < argv.size() && i < ft.funcParams.size(); i++) {
                ExprAST* a = call->args[i].get();
                if (!a || a->kind == NodeKind::Lambda) continue;
                if (!argv[i].known || argv[i].t.kind == TypeKind::Void) continue;
                if (!compatible(ft.funcParams[i], argv[i].t)) {
                    typeError("E0293",
                        "call to '" + name + "': argument " +
                            std::to_string(i + 1) + " is " + typeName(argv[i].t) +
                            ", but parameter " + std::to_string(i + 1) + " is " +
                            typeName(ft.funcParams[i]),
                        a, call,
                        "convert the argument or change the fn type");
                }
            }
            return mkKnown(*ft.funcRet);
        }

        auto bit = gBuiltinTypes.find(name);
        if (bit != gBuiltinTypes.end()) return mkKnown(bit->second);
        return mkUnknown();
    }
};

int main(int argc, char* argv[]) {
    PROFILE_BEGIN("total");
    // B3: explicit init phase — argv parsing + registry fetch. Lets the
    // profiler report process-init share (daemon decision hinges on init>15%).
    PROFILE_BEGIN("init");
    if (argc < 2) {
        std::cerr << "usage: flintc [options] <input.fl> [-o <output>] [-- args...]\n";
        std::cerr << "       flintc --emit-interface <input.fl> <output.flint.bc>\n";
        std::cerr << "       flintc --emit-signatures <input.fl> <output.sig>\n";
        std::cerr << "       flintc --use-interface <file.flint.bc> <input.fl> <output> [--use-interface ...]\n";
        std::cerr << "       flintc --parallel <N> <input.fl> <output> [--link ...]\n";
        std::cerr << "       flintc --opt-level <0|1|2|3> <input.fl> <output> [--link ...]\n";
        std::cerr << "       flintc --loop-opt <input.fl> <output> (loop-friendly check attrs, off by default)\n";
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
    std::string targetTripleSpec;
    bool debugFlag = false;
    bool helpFlag = false;
    bool versionFlag = false;
    bool emitInterfaceMode = false;
    bool emitSignaturesMode = false;
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
    bool checkBce = false;    // --check-bce: report emitted vs O2-surviving checks
    bool loopOpt = false;     // C1 --loop-opt: loop-friendly check attrs (off by default)
    bool testModeFlag = false; // --test: run test_ fns (must bypass cache hits)
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
        if (arg == "--help" || arg == "-h") {
            helpFlag = true;
        } else if (arg == "--version" || arg == "-V") {
            versionFlag = true;
        } else if (arg == "--") {
            progArgStart = i + 1;
            break;
        } else if (arg == "--unsafe") {
            releaseMode = true;  // disable overflow checks
            // The one escape hatch, and it is deliberately loud: a build
            // that removes the checks says so, on every compile.
            gSafetyPosture = "unsafe";
            if (!gUnsafeWarned) {
                gUnsafeWarned = true;
                std::cerr << "warning: --unsafe disables the overflow AND "
                             "bounds checks; this artifact is not memory-safe "
                             "by construction (ownership, borrow, escape, null "
                             "and divide-by-zero rules still apply)\n";
            }
        } else if (arg == "--offline") {
            offlineMode = true;  // registry: cache only, fail if missing
        } else if (arg == "--fast") {
            // Compat alias for -O0 builds; intentionally a no-op here:
            // the default pipeline already agrees with every opt level
            // (see test_opt_identity.sh). Accepted anywhere on the CLI so
            // flag order never shifts positional input/output.
        } else if (arg == "--safe") {
            releaseMode = false;  // enable overflow checks (default)
            gSafetyPosture = "safe";
        } else if (arg == "--release") {
            releaseMode = false;  // safe mode is default; --release is a no-op for compat
        } else if (arg == "--lib-path" && i + 1 < argc) {
            libPaths.push_back(argv[++i]);
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            outputPath = argv[++i];
            runMode = false;
        } else if (arg == "--link" && i + 1 < argc) {
            linkFlags = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            targetTripleSpec = argv[++i];
            if (targetTripleSpec.empty()) {
                std::cerr << "error: --target requires a triple\n";
                return 1;
            }
        } else if (arg == "--debug") {
            debugFlag = true; // DWARF line tables (JIT + emit)
        } else if (arg == "--emit-interface") {
            emitInterfaceMode = true;
        } else if (arg == "--emit-signatures") {
            emitSignaturesMode = true;
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
            // Handled after JIT compilation: run all test_ functions.
            // (testModeFlag also bypasses the cache-hit shortcuts below:
            // a cached binary would skip the tests silently.)
            testModeFlag = true;
            runMode = false;
        } else if (arg == "--type-audit") {
            gTypeAudit = true;
        } else if (arg == "--no-type-check") {
            gTypeCheck = false;
        } else if (arg == "--print-size") {
            gPrintSize = true;
        } else if (arg.rfind("--print-size-json=", 0) == 0) {
            gPrintSizeJsonPath = arg.substr(std::strlen("--print-size-json="));
        } else if (arg == "--check-bce") {
            checkBce = true; // diagnostic only: report check counts, change nothing
        } else if (arg == "--error-format" && i + 1 < argc) {
            // "human" (the default) or "json" (one JSON object per line --
            // the LSP's input, and the shape rustc's --error-format json
            // uses). The JSON stream is printed at the end of the run.
            gDiagFormat = argv[++i];
            if (gDiagFormat != "human" && gDiagFormat != "json") {
                std::cerr << "error: --error-format expects 'human' or 'json'\n";
                return 1;
            }
        } else if (arg.rfind("--error-format=", 0) == 0) {
            gDiagFormat = arg.substr(std::strlen("--error-format="));
            if (gDiagFormat != "human" && gDiagFormat != "json") {
                std::cerr << "error: --error-format expects 'human' or 'json'\n";
                return 1;
            }
        } else if (arg == "--loop-opt") {
            loopOpt = true; // C1: loop-friendly check attrs (off by default)
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

    if (versionFlag) {
        std::cout << "flintc " << kFlintVersion << "\n";
        return 0;
    }
    if (helpFlag) {
        std::cout
            << "flintc " << kFlintVersion << " — Flint compiler (LLVM backend)\n\n"
            << "USAGE\n"
            << "  flintc [options] <input.fl> [-o <output>] [-- prog args...]\n"
            << "  flintc --emit-interface <input.fl> <out.flint.bc>\n"
            << "  flintc --emit-signatures <input.fl> <out.sig>\n"
            << "  flintc --use-interface <in.flint.bc> <input.fl> <out> [--link flags]\n\n"
            << "Default mode (no -o) compiles and runs in-memory via the ORC JIT.\n"
            << "With -o, the output extension picks the form: .ll = LLVM IR text,\n"
            << ".o = object file, any other name = linked executable.\n\n"
            << "OUTPUT\n"
            << "  -o, --output <path>      Emit instead of JIT-running\n"
            << "  --link <flags>           Extra linker flags (with -o)\n"
            << "  --emit-interface         Emit a .flint.bc program interface\n"
            << "  --emit-signatures        Write resolved fn signatures (query mode)\n"
            << "  --use-interface <path>   Reuse an emitted interface (repeatable)\n\n"
            << "CODEGEN\n"
            << "  --opt-level <0|1|2|3>    Optimization level (default 2)\n"
            << "  --target <triple>        Emit for a target triple (requires -o)\n"
            << "                           e.g. x86_64-pc-linux-gnu, wasm32-unknown-wasi\n"
            << "  --debug                  Emit DWARF line tables (requires -o)\n"
            << "  --parallel <N>           Parallel codegen threads (capped at hw*4)\n"
            << "  --loop-opt               Loop-friendly check attributes\n"
            << "  --backend <llvm|qbe>     Backend (llvm default; qbe experimental)\n"
            << "  --lib-path <dir>         Extra runtime/library search dir\n\n"
            << "SAFETY\n"
            << "  (default)                Overflow checks ON\n"
            << "  --unsafe                 Disable overflow checks\n"
            << "  --safe, --release        Explicit safety posture (no-ops, compat)\n\n"
            << "DIAGNOSTICS AND MODES\n"
            << "  --check-bce              Report bounds-check counts (changes nothing)\n"
            << "  --error-format FORMAT    Diagnostic output: human (default) or json\n"
            << "  --no-type-check          Disable the 2A type checker (differential oracle)\n"
            << "  --type-audit             Report inference disagreements + 2A.1 call facts\n"
            << "  --print-size             Report the artifact's size attribution after linking\n"
            << "  --print-size-json=PATH   Write the same attribution as JSON to PATH\n"
            << "  --test                   Run test_ functions after JIT compile\n"
            << "  --dump-tokens <path>     Write the stable token dump and exit\n"
            << "  --run                    Explicit JIT run (the default)\n"
            << "  --offline                Registry: cache only, fail if missing\n"
            << "  --fast                   Compat no-op (kept for flag stability)\n"
            << "  -h, --help               This text\n"
            << "  -V, --version            Version\n";
        return 0;
    }
    if (inputPath.empty()) {
        std::cerr << "usage: flintc [options] <input.fl> [-o <output>]\n";
        std::cerr << "Default mode: compile & run via ORC JIT (no output file)\n";
        return 1;
    }

    // Diagnostics live in artifacts (.ll text, .o/linked binaries) — real
    // DWARF for llvm-dwarfdump/lldb. The ORC JIT with debug metadata
    // reliably hangs at mainFn (reproduced with and without only one
    // variable: the module carrying DIBuilder metadata), so bare-run mode
    // refuses loudly instead of wedging; -o gives a one-command artifact
    // (flintc file.fl --debug -o prog && ./prog).
    if (!targetTripleSpec.empty() && runMode) {
        std::cerr << "error: --target '" << targetTripleSpec
                  << "' requires an emit output (-o path.ll or path.o); JIT execution is host-triple only\n";
        return 1;
    }
    if (debugFlag && runMode) {
        std::cerr << "error: --debug builds an artifact with DWARF: use "
                  << "-o (e.g. 'flintc file.fl --debug -o prog' then ./prog); "
                  << "JIT run with debug metadata is unsupported\n";
        return 1;
    }

    // Registry: fetch flint.toml deps into the local cache (or verify the
    // cache in --offline mode) before any parsing/import resolution.
    if (!fetchRegistryDeps(inputPath, libPaths, offlineMode)) return 1;
    PROFILE_END(); // init (argv parsing + registry; LLVM/target init is in codegen_init)

    // --emit-signatures mode: the query mode. Parse the WHOLE file (the
    // untyped-parameter fixpoint needs the call sites), run the declaration
    // pass, write the resolved signatures. No artifact, no linking.
    if (emitSignaturesMode) {
        if (outputPath.empty()) {
            std::cerr << "error: --emit-signatures needs an output path "
                         "(flintc --emit-signatures <input.fl> <out.sig>)\n";
            return 1;
        }
        auto fileBuf = llvm::MemoryBuffer::getFile(inputPath);
        if (!fileBuf) { std::cerr << "error: cannot open '" << inputPath << "'\n"; return 1; }
        llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
        Lexer lexer(sourceRef);
        Parser parser(lexer);
        auto combined = parser.parseProgram();
        if (parser.hadError()) return 1;   // the diagnostic is already printed
        if (!emitSignatures(*combined, outputPath, targetTripleSpec)) {
            std::cerr << "error: failed to emit signatures\n";
            return 1;
        }
        PROFILE_END(); // total
        PROFILE_REPORT();
        return 0;
    }

    // --emit-interface mode: parse declarations only, emit .flint.bc
    if (emitInterfaceMode) {
        auto fileBuf = llvm::MemoryBuffer::getFile(inputPath);
        if (!fileBuf) { std::cerr << "error: cannot open '" << inputPath << "'\n"; return 1; }
        llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
        Lexer lexer(sourceRef);
        Parser parser(lexer);
        parser.setSkipBodies(true);
        auto combined = parser.parseProgram();
        if (parser.hadError()) return 1;   // the diagnostic is already printed
        if (!emitInterface(*combined, outputPath, targetTripleSpec)) {
            std::cerr << "error: failed to emit interface\n";
            return 1;
        }
        PROFILE_END(); // total
        PROFILE_REPORT();
        return 0;
    }

    bool emitLl = outputPath.size() >= 3 && outputPath.substr(outputPath.size() - 3) == ".ll";
    bool emitObj = outputPath.size() >= 2 && outputPath.substr(outputPath.size() - 2) == ".o";
    // Phase 3 / P1: bounded range facts are only sound for a whole-program
    // output. An object/IR artifact can be linked and called with an argument
    // outside the analyzed call sites, and an interface/link build can add
    // such a caller. Emit-only and interface/link builds therefore stay on
    // the fully checked path.
    //
    // This bit is part of the module-cache key: the cache stores an
    // OPTIMIZED module, so an executable's fact-elided module must never
    // satisfy a later .o/.ll request (or vice versa).
    bool closedWorldForRanges =
        !checkBce && !emitLl && !emitObj && useInterfaces.empty() &&
        linkFlags.empty() && targetTripleSpec.empty();

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
    // B3: --use-interface .bc bytes + --link flags/objects join the same
    // salted key. Previously the key was main-file bytes only, so an edited
    // header (re-emitted .bc) or a changed lib.o silently reused the stale
    // cached binary (wrong build — the rustc lesson). Hashing bytes (not
    // mtimes) keeps the check exact; cost is one extra file read per
    // interface/object (negligible vs link). Interface .bc files themselves
    // are explicit user artifacts, not cache entries, so they carry no
    // separate salt — version skew is caught here via cacheSalt.
    PROFILE_BEGIN("iface_hash");
    std::string ifaceFp;
    for (auto& iface : useInterfaces) {
        auto ibuf = llvm::MemoryBuffer::getFile(iface);
        if (ibuf) ifaceFp += std::to_string(fnv1a(ibuf.get()->getBuffer())) + "|";
        else ifaceFp += "missing|";
    }
    std::string linkFp = linkFlags;
    for (auto& tok : splitFlags(linkFlags)) {
        bool isObj = tok.size() > 2 &&
            (tok.substr(tok.size() - 2) == ".o" || tok.substr(tok.size() - 2) == ".a" ||
             tok.find(".so") != std::string::npos);
        if (isObj) {
            auto lbuf = llvm::MemoryBuffer::getFile(tok);
            if (lbuf) linkFp += "|" + std::to_string(fnv1a(lbuf.get()->getBuffer()));
            else linkFp += "|missing";
        }
    }
    PROFILE_END(); // iface_hash
    uint64_t sourceHash =
        fnv1a(cacheSalt(optLevel, releaseMode, backend, argv[0], loopOpt, targetTripleSpec, debugFlag,
                        closedWorldForRanges) +
              std::string(1, '\0') + (gTypeCheck ? "type-check=1" : "type-check=0") +
              std::string(1, '\0') + sourceRef.str() +
              std::string(1, '\0') + ifaceFp + std::string(1, '\0') + linkFp);
    if (getenv("FLINT_LOG_CACHE"))
        std::cerr << "cache: key iface=[" << ifaceFp << "] link=[" << linkFp << "]\n";
    ModuleCache cache;

    // For binary output, check if cached binary exists (skip lex/parse/codegen/link entirely)
    // But skip cache in --run mode: we always recompile for JIT.
    // And skip in --test mode: a cached binary would skip the tests silently.
    bool emitBinary = !emitLl && !emitObj && !runMode;

    if (emitBinary && !testModeFlag && !checkBce && !gTypeAudit && cache.hasBinary(sourceHash)) {
        if (cache.loadBinary(sourceHash, outputPath)) {
            if (getenv("FLINT_LOG_CACHE"))
                std::cerr << "cache: hit binary (skipped lex/parse/codegen/link)\n";
            PROFILE_BEGIN("total");
            PROFILE_END(); // total
            PROFILE_REPORT();
            return 0;
        }
        if (getenv("FLINT_LOG_CACHE")) std::cerr << "cache: miss binary\n";
    }

    PROFILE_BEGIN("cache_check");
    // --check-bce is a DIAGNOSTIC: it must report the same numbers every
    // time. The module cache stores the ALREADY-OPTIMIZED module, so on a
    // cache hit the "checks emitted" count is gone and the flag would
    // print nothing (or, worse, a misleading number). A diagnostic that
    // depends on cache state is not a diagnostic.
    // The type audit needs codegen to RUN, so it bypasses the module cache
    // (the same reasoning as --check-bce: a diagnostic that depends on
    // cache state is not a diagnostic).
    if (!runMode && !testModeFlag && !checkBce && !gTypeAudit && cache.has(sourceHash)) {
        llvm::LLVMContext ctx;
        if (auto cachedMod = cache.load(sourceHash, ctx)) {
            PROFILE_END(); // cache_check
            {
                PROFILE_BEGIN("llvm_opt");
                // --debug: cached modules follow the same O0-pass rule (see
                // the fresh-codegen comment). The cache salt already keys
                // on --debug, so a cached module was saved WITH the flag.
                llvm::OptimizationLevel ol = llvm::OptimizationLevel::O2;
                if (debugFlag) ol = llvm::OptimizationLevel::O0;
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
                maybeReportSize(outputPath);
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

    // The main file's id in the source map, so a diagnostic can quote
    // the source line it is about.
    int mainFileId = gSourceMap.addFile(inputPath, sourceRef.str());

    // Phase 2 / 2A: the compiler is constructed BEFORE the parse, so the
    // registry of builtin TYPES exists when the parser infers. The parser
    // and the codegen then read ONE registry and cannot drift on a
    // builtin's type.
    PROFILE_BEGIN("codegen_setup");
    Codegen codegen(targetTripleSpec);
    codegen.fileId = mainFileId;
    PROFILE_END(); // codegen_setup

    PROFILE_BEGIN("parse_decls");
    Parser parser(lexer);
    parser.fileId = mainFileId;
    parser.setSkipBodies(false);
    auto combined = parser.parseProgram();
    PROFILE_BEGIN("parse_error_check");
    if (parser.hadError()) {
        return 1;   // the diagnostic is already printed
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
                    if (firstError.empty()) {
                        emitFileError("E9001", "cannot open '" + impPath + "'");
                        firstError = "cannot open '" + impPath + "'";
                    }
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
            // The diagnostic is already printed; the summary is only the
            // bookkeeping that says WHICH import failed.
            return 1;
        }
    } else {
        // Sequential import processing (original behavior)
        for (auto& imp : combined->imports) {
            auto impPath = resolveImportPath(inputPath, imp, libPaths);
            processFile(impPath, combined, seen, firstError, libPaths);
            if (!firstError.empty()) {
                // The diagnostic is already printed (see above).
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
                    return 1;   // the diagnostic is already printed
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
                maybeReportSize(outputPath);
                PROFILE_END(); // link
                unlink(objectPath.c_str());
            }
        }

        PROFILE_END(); // total
        PROFILE_REPORT();
        return 0;
    }

    // ===== LLVM Backend (kept for --backend llvm) =====
    //
    // Static safety passes. These used to run only under `--backend qbe`,
    // which is not installed on this device -- so the default path compiled
    // and ran a use-after-free with no diagnostic at all. They belong to the
    // language, not to one backend, so they run here too.
    //
    // Bodies are reparsed from their token ranges when the single-pass emit
    // mode left them unbuilt, exactly as the QBE path does, so the walkers
    // below always see a tree.
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
                return 1;   // the diagnostic is already printed
                PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                return 1;
            }
        }
    }

    // Borrow/move checking: use-after-move, free-while-borrowed.
    {
        BorrowChecker borrowChecker;
        for (auto& fn : combined->functions) {
            if (fn->isDeclaration) continue;
            std::string errorOut;
            if (!borrowChecker.check(fn.get(), errorOut)) {
                std::cerr << "borrow/move error in '" << fn->name << "': " << errorOut << "\n";
                PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                return 1;
            }
        }
    }

    // AEGIS lease checking: the compile-time half of the hybrid model that
    // runtime/flint_aegis.h documents but that no longer existed.
    {
        AegisChecker aegisChecker;
        for (auto& fn : combined->functions) {
            if (fn->isDeclaration) continue;
            std::string errorOut;
            if (!aegisChecker.check(fn.get(), errorOut)) {
                std::cerr << "aegis error in '" << fn->name << "': " << errorOut << "\n";
                PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                return 1;
            }
        }
    }

    PROFILE_BEGIN("codegen");
    // (the Codegen was constructed before the parse -- see above)
    // Phase 2 / 2A.0: the type audit must be enabled BEFORE codegen
    // runs -- it compares the two inference engines as it goes.
    codegen.auditTypes = gTypeAudit;
    // Phase 2 / 2B: the safety posture becomes the safety POLICY, in one
    // place. `--unsafe` removes the two PERFORMANCE checks (overflow and
    // bounds); the divide-by-zero trap, the ownership rules and the
    // one-error-per-program contract are NOT part of the performance
    // posture and stay.
    {
        bool unsafeBuild = (gSafetyPosture == "unsafe");
        codegen.safety.bounds = !unsafeBuild;
        codegen.safety.overflow = !unsafeBuild;
        codegen.safety.divZero = true;
    }
    if (debugFlag) codegen.initDebugSource(inputPath);
    codegen.releaseMode = releaseMode;
    codegen.loopOpt = loopOpt;
    codegen.setParser(&parser);

    // Generate declarations from the combined AST
    if (!codegen.generateDeclarations(*combined)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "declaration generation failed\n";
        return 1;
    }

    // Load pre-compiled interfaces (--use-interface)
    // These add declarations NOT in the AST (e.g., pre-compiled libraries).
    // B3: timed as iface_load so init-share breakdowns show the reuse cost;
    // truncated/corrupt .bc fails cleanly here (never a wrong build).
    PROFILE_BEGIN("iface_load");
    for (auto& iface : useInterfaces) {
        auto ifaceMod = loadInterface(iface, *codegen.ctx);
        if (!ifaceMod) { PROFILE_END(); PROFILE_END(); PROFILE_REPORT(); std::cerr << "error: cannot load interface '" << iface << "'\n"; return 1; }
        if (getenv("FLINT_LOG_CACHE"))
            std::cerr << "interface: loaded '" << iface << "'\n";
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
    PROFILE_END(); // iface_load
    // Sync LLVM declarations into C++ symbol tables for call resolution
    codegen.syncInterfaceSymbols();

    // Phase 2 / 2A: the type checker runs as a compiler PHASE -- after
    // parsing/imports and declaration generation (so the builtin registry
    // exists), and before any body is emitted. A provable type error stops
    // the compile here, with a span and a code, instead of reaching LLVM's
    // verifier with a message about the compiler's own IR.
    if (gTypeCheck) {
        TypeChecker checker(*combined, codegen, codegen.fileId);
        if (!checker.run()) {
            PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
            std::cerr << "type check failed\n";
            return 1;
        }
    }

    // Phase 3 / P1: bounded range facts (closedWorldForRanges is computed
    // before the cache key and is part of it).
    if (closedWorldForRanges) {
        PROFILE_BEGIN("range_facts");
        BoundedRangeAnalysis rangeAnalysis(*combined, codegen.rangeFacts);
        rangeAnalysis.run();
        PROFILE_END();
    }

    // Emit function bodies and output
    if (!codegen.emitFunctionBodies(*combined)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "function body emission failed\n";
        return 1;
    }

    // --debug: seal the DIBuilder BEFORE any consumer (bitcode, .ll text, .o
    // object, JIT). finalize() replaces temporary subprogram nodes and is
    // also what makes the CU worth emitting at all.
    codegen.finishDebug();

    // Run LLVM IR optimization passes — O2 by default with overflow checks for safety
    long checksBeforeOpt = -1;
    if (checkBce) checksBeforeOpt = countChecks(codegen.mod.get());
    {
        PROFILE_BEGIN("llvm_opt");
        llvm::OptimizationLevel ol = llvm::OptimizationLevel::O2;
        if (!releaseMode) { /* safe mode: overflow checks added at codegen level */ }
        // --debug opts the module passes down to O0: O2 constant-folds and
        // merges statements, which erases per-statement DILocations from the
        // IR and the line table. A debuggable build keeps the source-line
        // mapping exact; use FLINT_NO_OPT semantics deliberately here.
        if (debugFlag) ol = llvm::OptimizationLevel::O0;
        if (!getenv("FLINT_NO_OPT")) runLLVMOptimizations(codegen.mod.get(), ol);
        PROFILE_END(); // llvm_opt
    }
    if (gTypeAudit) {
        // The report: annotated declarations are contracts (a disagreement
        // is a BUG), inferred ones are guesses (a disagreement is DRIFT).
        long contracts = 0, drift = 0;
        for (const auto& d : codegen.typeDrift) {
            if (d.annotated) {
                contracts++;
                std::cerr << "type-audit: CONTRACT VIOLATED at line " << d.line
                          << ": '" << d.name << "' is annotated "
                          << d.parseType << " but compiled as " << d.codegenType
                          << "\n";
            } else {
                drift++;
            }
        }
        std::cerr << "type-audit: " << codegen.typeDrift.size()
                  << " declaration(s) where the two inference engines"
                     " disagree (" << contracts << " annotated, " << drift
                  << " inferred)\n";
        // Show the drift: a count alone is not actionable. The first few
        // are enough to see the shape of the disagreement.
        int shown = 0;
        for (const auto& d : codegen.typeDrift) {
            if (d.annotated) continue;
            if (shown++ >= 5) break;
            std::cerr << "type-audit:   line " << d.line << ": '" << d.name
                      << "' parse=" << d.parseType
                      << " codegen=" << d.codegenType << "\n";
        }
        // Phase 2 / 2A.1: the call facts. A generic call's concrete type
        // arguments and a trait method's resolved implementation are the
        // two facts the compiler used to keep implicit; print them so a
        // gate (and a human) can compare them with the source.
        if (!codegen.callFacts.empty()) {
            std::cerr << "type-audit: " << codegen.callFacts.size()
                      << " call fact(s)\n";
            for (const auto& f : codegen.callFacts) {
                std::cerr << "type-audit: call line " << f.line
                          << ": '" << f.callee << "'";
                if (!f.typeArgs.empty()) {
                    std::cerr << " type-args=[";
                    for (size_t i = 0; i < f.typeArgs.size(); i++) {
                        if (i) std::cerr << ",";
                        std::cerr << Codegen::typeTag(f.typeArgs[i]);
                    }
                    std::cerr << "]";
                }
                if (!f.impl.empty())
                    std::cerr << " impl='" << f.impl << "'";
                std::cerr << "\n";
            }
        }
        if (contracts > 0) { std::cerr << "type-audit: FAILED\n"; return 1; }
    }
    if (checkBce) {
        long checksAfterOpt = countChecks(codegen.mod.get());
        std::cout << "bce: " << checksBeforeOpt << " checks emitted, "
                  << checksAfterOpt << " survive O2 ("
                  << (checksBeforeOpt - checksAfterOpt) << " elided)\n";
        // Phase 2 / 2B: the policy's own counters are exact and
        // pre-optimization, so "emitted" is not inferred from the IR.
        std::cout << "bce: inserted by policy -- bounds "
                  << codegen.safety.boundsInserted
                  << ", overflow " << codegen.safety.overflowInserted
                  << ", div-zero " << codegen.safety.divZeroInserted << "\n";
        // Phase 3 / P1: --check-bce is a diagnostic, so it keeps the
        // checked path (the facts are disabled above). Report what the
        // closed-world analysis WOULD prove, as a portable signal that the
        // analysis ran and how much it can remove.
        std::unordered_map<const ExprAST*, IntRangeFact> diagnosticFacts;
        BoundedRangeAnalysis diagnosticAnalysis(*combined, diagnosticFacts);
        diagnosticAnalysis.run();
        long wouldElide = 0;
        for (auto& kv : diagnosticFacts) {
            if (!kv.first || kv.first->kind != NodeKind::Binary) continue;
            auto* b = static_cast<const BinaryExprAST*>(kv.first);
            if (b->op == '+' || b->op == '-' || b->op == '*') wouldElide++;
        }
        std::cout << "bce: bounded-range facts prove " << wouldElide
                  << " arithmetic overflow checks safe\n";
    }

    // --test mode: compile & run all test_ functions via JIT, report results
    {
        if (testModeFlag) {
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
            // Same JIT setup as normal runs (targets + runtime objects):
            // the old bare LLJITBuilder().create() failed here every time.
            preloadPython();
            initTargets();
            auto JTMB = llvm::orc::JITTargetMachineBuilder::detectHost();
            if (!JTMB) {
                std::cerr << "JIT error: " << toString(JTMB.takeError()) << "\n";
                return 1;
            }
            JTMB->setCodeGenOptLevel(llvm::CodeGenOptLevel::Default);
            auto jit = llvm::orc::LLJITBuilder()
                .setJITTargetMachineBuilder(std::move(*JTMB)).create();
            if (!jit) { std::cerr << "JIT creation failed\n"; return 1; }
            if (!loadJITObjects(**jit, extraObjs)) return 1;
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
            maybeReportSize(outputPath);
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
