// flint — the high-level Flint command driver.
//
// The compiler (flintc) is the low-level interface: one file, explicit
// output, explicit flags. This driver is the beginner path:
//
//   flint run app.fl        build if needed, then run the native artifact
//   flint build app.fl      build only
//   flintc app.fl -o app    lower-level compiler interface
//
// `flint` deliberately delegates every build to flintc, so the existing
// content-addressed module/binary cache, the source-overwrite guard, the
// warning/cache behaviour and every codegen flag stay in one implementation.
// It only chooses a default artifact path (.flint/bin/<name>) and splits
// `run` from `build`.
#include "flint_os.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define FLINT_GETCWD _getcwd
static const char* kExeSuffix = ".exe";
#else
#include <unistd.h>
#define FLINT_GETCWD getcwd
static const char* kExeSuffix = "";
#endif

#ifndef FLINT_VERSION
#define FLINT_VERSION "dev"
#endif

static const char* kFlintVersion = FLINT_VERSION;

static void printUsage(std::ostream& os) {
    os << "usage: flint run <file.fl> [options] [-- args...]\n"
       << "       flint build <file.fl> [options]\n"
       << "\n"
       << "  run    build the native artifact if needed, then run it\n"
       << "  build  build the native artifact without running it\n"
       << "\n"
       << "Artifacts default to .flint/bin/<name>; use -o <path> to choose\n"
       << "another path. All other options are passed to flintc\n"
       << "(see `flintc --help`).\n";
}

static void printHelp() {
    std::cout << "flint " << kFlintVersion << " — Flint command driver\n\n";
    printUsage(std::cout);
    std::cout << "\nEXAMPLES\n"
              << "  flint run hello.fl             build and run .flint/bin/hello\n"
              << "  flint build hello.fl           build .flint/bin/hello only\n"
              << "  flint run app.fl --release     pass compiler flags through\n"
              << "  flint run app.fl -- one two    forward arguments to the program\n"
              << "  flint build app.fl -o app      choose the artifact path\n"
              << "  flint build app.fl --target x86_64-pc-linux-gnu\n"
              << "                                 cross-target emit (.flint/bin/app.o)\n"
              << "\nENVIRONMENT\n"
              << "  FLINTC    path to flintc (default: sibling of this binary, then PATH)\n";
}

static bool isAbsolute(const std::string& p) {
    if (p.empty()) return false;
#ifdef _WIN32
    if (p.size() >= 2 && p[1] == ':') return true;
    if (p[0] == '\\' || p[0] == '/') return true;
#else
    if (p[0] == '/') return true;
#endif
    return false;
}

static std::string absolutePath(const std::string& p) {
    if (p.empty() || isAbsolute(p)) return p;
    char buf[4096];
    if (!FLINT_GETCWD(buf, sizeof buf)) return p;
    return std::string(buf) + "/" + p;
}

static std::string dirName(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return p.substr(0, 1);
    return p.substr(0, slash);
}

static std::string baseName(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

static std::string stripFl(const std::string& p) {
    if (p.size() > 3 && p.compare(p.size() - 3, 3, ".fl") == 0)
        return p.substr(0, p.size() - 3);
    return p;
}

static bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string findOnPath(const std::string& name) {
    const char* env = getenv("PATH");
    if (!env || !*env) return "";
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string path(env);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(sep, start);
        std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (dir.empty()) dir = ".";
        std::string cand = dir + "/" + name;
        if (flintos::hasExe(cand)) return absolutePath(cand);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

// Resolve flintc to an absolute path. The compiler resolves its runtime
// objects relative to its own argv[0], so passing a bare "flintc" from PATH
// would make it look in the caller's project directory instead of its install
// directory.
static std::string findFlintc(const char* argv0) {
    const char* env = getenv("FLINTC");
    if (env && *env) {
        std::string p(env);
        bool hasSlash = p.find('/') != std::string::npos;
#ifdef _WIN32
        hasSlash = hasSlash || p.find('\\') != std::string::npos;
#endif
        if (hasSlash) return absolutePath(p);
        return findOnPath(p);
    }

    std::string self = argv0 ? argv0 : "";
    std::string beside = dirName(absolutePath(self)) + "/flintc" + kExeSuffix;
    if (flintos::hasExe(beside)) return beside;

    return findOnPath(std::string("flintc") + kExeSuffix);
}

static bool isValueOption(const std::string& a) {
    return a == "--link" || a == "--parallel" || a == "--opt-level" ||
           a == "--backend" || a == "--lib-path" || a == "--error-format";
}

static bool isLowLevelMode(const std::string& a) {
    return a == "--emit-interface" || a == "--emit-signatures" ||
           a == "--use-interface" || a == "--dump-tokens" || a == "--test";
}

int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? (size_t)(argc - 1) : 0);
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);

    if (args.empty()) {
        printUsage(std::cerr);
        return 1;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        printHelp();
        return 0;
    }
    if (args[0] == "--version" || args[0] == "-V") {
        std::cout << "flint " << kFlintVersion << "\n";
        return 0;
    }

    std::string command = args[0];
    if (command != "run" && command != "build") {
        std::cerr << "flint: unknown command '" << command << "'\n\n";
        printUsage(std::cerr);
        return 1;
    }

    std::vector<std::string> rest(args.begin() + 1, args.end());
    // `flint run --help` is this command's help, not flintc's. Everything
    // after `--` belongs to the program, including a literal --help.
    for (auto& a : rest) {
        if (a == "--") break;
        if (a == "--help" || a == "-h") { printHelp(); return 0; }
        if (a == "--version" || a == "-V") {
            std::cout << "flint " << kFlintVersion << "\n";
            return 0;
        }
    }

    std::string input, output;
    bool haveInput = false, haveOutput = false;
    bool haveTarget = false, haveNoRun = false, haveRun = false;
    std::vector<std::string> compilerArgs;
    std::vector<std::string> progArgs;
    bool afterDash = false;

    for (size_t i = 0; i < rest.size(); i++) {
        const std::string& a = rest[i];
        if (afterDash) { progArgs.push_back(a); continue; }
        if (a == "--") { afterDash = true; continue; }

        if (isLowLevelMode(a)) {
            std::cerr << "flint: " << a
                      << " is a flintc mode; use flintc directly\n";
            return 1;
        }
        if (a == "-o" || a == "--output") {
            if (i + 1 >= rest.size()) {
                std::cerr << "flint: " << a << " requires a path\n";
                return 1;
            }
            output = rest[++i];
            haveOutput = true;
            continue;
        }
        if (a == "--target") {
            if (i + 1 >= rest.size()) {
                std::cerr << "flint: --target requires a triple\n";
                return 1;
            }
            compilerArgs.push_back(a);
            compilerArgs.push_back(rest[++i]);
            haveTarget = true;
            continue;
        }
        if (a == "--no-run" || a == "--build-only") {
            haveNoRun = true;
            compilerArgs.push_back(a);
            continue;
        }
        if (a == "--run") {
            haveRun = true;
            compilerArgs.push_back(a);
            continue;
        }
        if (isValueOption(a)) {
            if (i + 1 >= rest.size()) {
                std::cerr << "flint: " << a << " requires a value\n";
                return 1;
            }
            compilerArgs.push_back(a);
            compilerArgs.push_back(rest[++i]);
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            compilerArgs.push_back(a);
            continue;
        }
        if (!haveInput) {
            input = a;
            haveInput = true;
            continue;
        }
        if (command == "run") {
            // go/dart-style direct program arguments; `--` also works.
            progArgs.push_back(a);
            continue;
        }
        std::cerr << "flint: unexpected argument '" << a
                  << "' for build (use -o <path> for the artifact)\n";
        return 1;
    }

    if (!haveInput) {
        std::cerr << "flint: no input file\n\n";
        printUsage(std::cerr);
        return 1;
    }

    if (command == "run") {
        if (haveNoRun) {
            std::cerr << "flint: run always executes; use `flint build` to build only\n";
            return 1;
        }
        if (haveTarget) {
            std::cerr << "flint: run cannot execute a cross-target artifact; "
                         "use `flint build --target ...`\n";
            return 1;
        }
        // flintc treats .ll/.o as emit-only, so accepting them here would
        // make `flint run` exit 0 without ever running the program.
        if (haveOutput && (endsWith(output, ".ll") || endsWith(output, ".o"))) {
            std::cerr << "flint: run cannot execute an emit-only artifact; "
                         "use `flint build -o " << output << "`\n";
            return 1;
        }
    } else {
        if (haveRun) {
            std::cerr << "flint: build does not run; use `flint run`\n";
            return 1;
        }
        if (!progArgs.empty()) {
            std::cerr << "flint: build does not accept program arguments\n";
            return 1;
        }
        if (!haveNoRun) compilerArgs.push_back("--no-run");
    }

    if (!haveOutput) {
        std::string stem = stripFl(baseName(input));
        bool crossTarget = command == "build" && haveTarget;
        output = std::string(".flint/bin/") + stem + (crossTarget ? ".o" : kExeSuffix);
        if (!flintos::makeDir(".flint") || !flintos::makeDir(".flint/bin")) {
            std::cerr << "flint: cannot create .flint/bin\n";
            return 1;
        }
    } else if (command == "build" && haveTarget &&
               !endsWith(output, ".ll") && !endsWith(output, ".o")) {
        std::cerr << "flint: --target emits .ll or .o; "
                     "use -o path.ll or -o path.o\n";
        return 1;
    }

    std::string flintc = findFlintc(argv[0]);
    if (flintc.empty() || !flintos::hasExe(flintc)) {
        std::cerr << "flint: cannot find flintc"
                     " (set FLINTC to the compiler path)\n";
        return 127;
    }

    std::vector<std::string> cmd;
    cmd.reserve(4 + compilerArgs.size() + progArgs.size() + 1);
    cmd.push_back(flintc);
    cmd.push_back(input);
    cmd.push_back("-o");
    cmd.push_back(output);
    for (auto& a : compilerArgs) cmd.push_back(a);
    if (!progArgs.empty()) {
        cmd.push_back("--");
        for (auto& a : progArgs) cmd.push_back(a);
    }

    int rc = flintos::spawn(cmd);
    if (rc < 0) {
        std::cerr << "flint: failed to execute '" << flintc << "'\n";
        return 127;
    }
    if (command == "build" && rc == 0)
        std::cerr << "flint: built " << output << "\n";
    return rc;
}
