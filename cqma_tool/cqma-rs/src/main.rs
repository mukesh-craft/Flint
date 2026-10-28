//! cqma — Code Quality & Memory Safety Analyzer for C and C++.
//!
//! Rust port of tools/cqma.py. Behaviour is intentionally identical:
//!   1. compiler warnings (clang/gcc, -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
//!   2. cppcheck --enable=all (unused functions, style, null pointers)
//!   3. ASan + UBSan, with a valgrind fallback
//!   4. one Markdown report (+ optional JSON) with a health score
//!
//! Exit codes: 0 = no findings, 1 = findings, 2 = configuration error.

use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::env;
use std::fmt::Write as FmtWrite;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const VERSION: &str = "1.0.0";

/// Allocator-hook leak tracker linked into ASan builds when LeakSanitizer is
/// unavailable (Android/Termux). See src/leak_hooks.c.
const LEAK_HOOKS_C: &str = include_str!("leak_hooks.c");

const C_EXTS: &[&str] = &["c"];
const CXX_EXTS: &[&str] = &["cc", "cpp", "cxx", "c++", "C"];

const SKIP_DIRS: &[&str] = &[
    ".git", ".hg", ".svn", ".cache", ".venv", "venv", "__pycache__",
    "node_modules", "build", "build-asan", "cmake-build-debug",
    "cmake-build-release", "out", "target", "dist", "third_party",
];

const BASE_WARNINGS: &[&str] = &[
    "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wconversion",
];

const SANITIZER_FLAGS: &[&str] = &[
    "-fsanitize=address,undefined", "-g", "-fno-omit-frame-pointer",
];

fn severity_weight(severity: &str) -> u32 {
    match severity {
        "error" => 10,
        "warning" => 3,
        "style" => 1,
        "performance" => 1,
        "portability" => 1,
        _ => 0,
    }
}

/// A process/coverage warning: visible in the report, excluded from the
/// defect score, and (via the stage status) able to make the run incomplete.
fn operational_finding(tool: &str, check: &str, message: String, file: String) -> Finding {
    Finding {
        tool: tool.to_string(),
        severity: "warning".to_string(),
        check: check.to_string(),
        message,
        file,
        weight: 0,
        operational: true,
        ..Default::default()
    }
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, Default)]
struct Finding {
    tool: String,
    severity: String,
    check: String,
    message: String,
    file: String,
    line: Option<i64>,
    column: Option<i64>,
    detail: String,
    weight: u32,
    /// True for process/coverage problems (timeout, analyzer failed, valgrind
    /// error-summary, truncated output). Operational warnings stay visible in
    /// the report and JSON but are NOT scored as memory-safety defects: a
    /// timeout does not prove a bug, and a missing analysis is not a clean run.
    operational: bool,
}

impl Finding {
    fn key(&self) -> String {
        format!(
            "{}|{}|{:?}|{:?}|{}|{}|{}",
            self.tool, self.file, self.line, self.column, self.check, self.message, self.detail
        )
    }
}

#[derive(Clone, Debug)]
struct Stage {
    name: String,
    status: String, // ran | skipped | failed | unavailable
    note: String,
    seconds: f64,
    command: String,
    /// Translation units the stage was asked to process / could not finish.
    files_total: usize,
    files_failed: usize,
    /// The stage produced more output than the cap; later diagnostics may be
    /// missing, so the run is not a clean bill of health.
    truncated: bool,
    /// Coverage limitation that does not change `status` (e.g. leak detection
    /// unavailable on this host).
    coverage_note: String,
}

impl Stage {
    fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            status: "skipped".to_string(),
            note: String::new(),
            seconds: 0.0,
            command: String::new(),
            files_total: 0,
            files_failed: 0,
            truncated: false,
            coverage_note: String::new(),
        }
    }
}

fn merge_stage(stage: &mut Stage, status: &str, note: &str) {
    fn rank(s: &str) -> u8 {
        match s {
            "skipped" => 0,
            "ran" => 1,
            "unavailable" => 2,
            "failed" => 3,
            _ => 0,
        }
    }
    if rank(status) >= rank(&stage.status) {
        stage.status = status.to_string();
    }
    if !note.is_empty() {
        if stage.note.is_empty() {
            stage.note = note.to_string();
        } else {
            stage.note.push_str("; ");
            stage.note.push_str(note);
        }
    }
}

struct Config {
    root: PathBuf,
    files: Vec<PathBuf>,
    cc: String,
    cxx: String,
    std_c: String,
    std_cxx: String,
    includes: Vec<String>,
    defines: Vec<String>,
    cflags: Vec<String>,
    jobs: usize,
    timeout_compile: u64,
    timeout_run: u64,
    timeout_cppcheck: u64,
    cppcheck_level: String,
    timeout_analyzer: u64,
    cc_commands: HashMap<PathBuf, Vec<String>>,
    run_binary: Option<PathBuf>,
    run_args: Vec<String>,
    run_cwd: PathBuf,
    build_cmd: Option<String>,
    sanitizer_ok: bool,
    leaks_ok: bool,
    sanitizer_note: String,
    loc: usize,
    skipped_files: Vec<String>,
}

struct Proc {
    rc: i32,
    out: String,
    err: String,
    timed_out: bool,
    out_truncated: bool,
    err_truncated: bool,
}

impl Proc {
    fn text(&self) -> String {
        let joined = format!("{}\n{}", self.out, self.err);
        joined.trim().to_string()
    }
    fn truncated(&self) -> bool {
        self.out_truncated || self.err_truncated
    }
    fn last_line(&self) -> String {
        self.text()
            .lines()
            .rev()
            .find(|l| !l.trim().is_empty())
            .unwrap_or("no output")
            .chars()
            .take(300)
            .collect()
    }
}

/// Hard cap on retained child output. A compiler or a program that prints
/// forever must not be able to exhaust cqma's memory; the pipe is still
/// drained after the cap so the child cannot deadlock on a full buffer.
const MAX_PIPE_BYTES: usize = 8 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

fn read_pipe<R: Read + Send + 'static>(mut reader: R) -> mpsc::Receiver<(String, bool)> {
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 8192];
        let mut truncated = false;
        loop {
            match reader.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => {
                    let room = MAX_PIPE_BYTES.saturating_sub(buf.len());
                    if room > 0 {
                        let keep = room.min(n);
                        buf.extend_from_slice(&chunk[..keep]);
                        if keep < n {
                            truncated = true;
                        }
                    } else {
                        truncated = true;
                    }
                }
                Err(_) => break,
            }
        }
        let mut text = String::from_utf8_lossy(&buf).into_owned();
        if truncated {
            text.push_str("\n[cqma: output truncated at 8 MiB]");
        }
        let _ = tx.send((text, truncated));
    });
    rx
}

/// Kill the child and everything it spawned.
///
/// `run_cmd` puts each child in its own process group, so a negative pid
/// reaches build tools that fork (`make -j`, shell pipelines). A plain
/// `child.kill()` would leave those grandchildren running.
fn kill_tree(child: &mut std::process::Child) {
    #[cfg(unix)]
    {
        extern "C" {
            fn kill(pid: i32, sig: i32) -> i32;
        }
        const SIGKILL: i32 = 9;
        let pid = child.id() as i32;
        if pid > 1 {
            // SAFETY: plain libc call; pid is this process's own child, which
            // was made a process-group leader by process_group(0).
            unsafe {
                kill(-pid, SIGKILL);
            }
        }
    }
    let _ = child.kill();
}

/// Run a command with a hard timeout. Output pipes are drained concurrently,
/// so a chatty compiler cannot deadlock us.
fn run_cmd(cmd: &[String], cwd: Option<&Path>, timeout: Duration, envs: &[(&str, &str)]) -> Proc {
    if cmd.is_empty() {
        return Proc {
            rc: 126,
            out: String::new(),
            err: "empty command".into(),
            timed_out: false,
            out_truncated: false,
            err_truncated: false,
        };
    }
    let started = Instant::now();
    let mut command = Command::new(&cmd[0]);
    command.args(&cmd[1..]);
    if let Some(dir) = cwd {
        command.current_dir(dir);
    }
    for (key, value) in envs {
        command.env(key, value);
    }
    command.stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }

    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(err) => {
            return Proc {
                rc: 127,
                out: String::new(),
                err: err.to_string(),
                timed_out: false,
                out_truncated: false,
                err_truncated: false,

            }
        }
    };

    let out_rx = child.stdout.take().map(read_pipe);
    let err_rx = child.stderr.take().map(read_pipe);

    let mut timed_out = false;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) => {
                if started.elapsed() >= timeout {
                    kill_tree(&mut child);
                    let status = child.wait().ok();
                    timed_out = true;
                    break status;
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(_) => {
                kill_tree(&mut child);
                break child.wait().ok();
            }
        }
    };

    let rc = match status {
        Some(status) => {
            #[cfg(unix)]
            {
                use std::os::unix::process::ExitStatusExt;
                status.code().unwrap_or_else(|| -status.signal().unwrap_or(0))
            }
            #[cfg(not(unix))]
            {
                status.code().unwrap_or(-1)
            }
        }
        None => -1,
    };

    // Do not block forever if a grandchild inherited the pipes.
    let (out, out_truncated) = out_rx
        .and_then(|rx| rx.recv_timeout(Duration::from_secs(3)).ok())
        .unwrap_or_default();
    let (err, err_truncated) = err_rx
        .and_then(|rx| rx.recv_timeout(Duration::from_secs(3)).ok())
        .unwrap_or_default();

    Proc { rc, out, err, timed_out, out_truncated, err_truncated }
}

fn sh(cmd: &[String]) -> String {
    cmd.iter()
        .map(|part| {
            if part.is_empty() || part.chars().any(|c| c.is_whitespace() || "'\"$`\\".contains(c)) {
                format!("'{}'", part.replace('\'', "'\\''"))
            } else {
                part.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// Include directories for a C extension that embeds Python.
///
/// `runtime/pyruntime.c` includes `Python.h`, which lives outside the project.
/// Without this the file fails to compile and its real defects are never
/// analyzed — a silent coverage hole.
fn python_include_dirs() -> Option<Vec<String>> {
    let output = Command::new("python3-config")
        .arg("--includes")
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let dirs: Vec<String> = text
        .split_whitespace()
        .filter_map(|token| token.strip_prefix("-I"))
        .filter(|path| Path::new(path).is_dir())
        .map(|path| path.to_string())
        .collect();
    if dirs.is_empty() {
        None
    } else {
        Some(dirs)
    }
}

fn which(name: &str) -> Option<PathBuf> {
    if name.contains('/') {
        let path = PathBuf::from(name);
        return if path.is_file() { Some(path) } else { None };
    }
    let path_var = env::var_os("PATH")?;
    for dir in env::split_paths(&path_var) {
        let candidate = dir.join(name);
        if !candidate.is_file() {
            continue;
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if let Ok(meta) = candidate.metadata() {
                if meta.permissions().mode() & 0o111 != 0 {
                    return Some(candidate);
                }
            }
        }
        #[cfg(not(unix))]
        {
            return Some(candidate);
        }
    }
    None
}

fn tool_version(exe: &str) -> String {
    static CACHE: OnceLock<Mutex<HashMap<String, String>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    if let Some(hit) = cache.lock().unwrap().get(exe) {
        return hit.clone();
    }
    let result = run_cmd(
        &[exe.to_string(), "--version".to_string()],
        None,
        Duration::from_secs(15),
        &[],
    );
    let version = result
        .text()
        .lines()
        .find(|line| !line.trim().is_empty())
        .unwrap_or("")
        .trim()
        .chars()
        .take(120)
        .collect::<String>();
    cache.lock().unwrap().insert(exe.to_string(), version.clone());
    version
}

fn is_clang(exe: &str) -> bool {
    tool_version(exe).to_lowercase().contains("clang")
}

fn version_tuple(text: &str) -> (u32, u32, u32) {
    let mut numbers = Vec::new();
    let mut current = String::new();
    for ch in text.chars() {
        if ch.is_ascii_digit() {
            current.push(ch);
        } else if !current.is_empty() {
            numbers.push(current.parse::<u32>().unwrap_or(0));
            current.clear();
        }
    }
    if !current.is_empty() {
        numbers.push(current.parse::<u32>().unwrap_or(0));
    }
    (
        *numbers.first().unwrap_or(&0),
        *numbers.get(1).unwrap_or(&0),
        *numbers.get(2).unwrap_or(&0),
    )
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

fn is_c(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| C_EXTS.contains(&e))
        .unwrap_or(false)
}

fn is_source(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| C_EXTS.contains(&e) || CXX_EXTS.contains(&e))
        .unwrap_or(false)
}

fn walk_dir(
    dir: &Path,
    excludes: &[String],
    files: &mut Vec<PathBuf>,
    skipped: &mut Vec<String>,
) {
    let mut entries: Vec<PathBuf> = match fs::read_dir(dir) {
        Ok(reader) => reader.filter_map(|e| e.ok().map(|x| x.path())).collect(),
        Err(_) => return,
    };
    entries.sort();
    for path in entries {
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        if path.is_dir() {
            if name.starts_with('.') || SKIP_DIRS.contains(&name.as_str()) {
                continue;
            }
            if excludes.iter().any(|ex| path.to_string_lossy().contains(ex)) {
                continue;
            }
            walk_dir(&path, excludes, files, skipped);
        } else if is_source(&path) {
            if excludes.iter().any(|ex| path.to_string_lossy().contains(ex)) {
                skipped.push(path.display().to_string());
            } else {
                files.push(path);
            }
        }
    }
}

fn discover(
    targets: &[PathBuf],
    excludes: &[String],
    max_files: usize,
) -> (Vec<PathBuf>, Vec<String>, PathBuf) {
    let mut files: Vec<PathBuf> = Vec::new();
    let mut skipped: Vec<String> = Vec::new();
    let mut roots: Vec<PathBuf> = Vec::new();

    for target in targets {
        let target = fs::canonicalize(target).unwrap_or_else(|_| target.clone());
        if target.is_file() {
            if is_source(&target) {
                files.push(target);
            } else {
                skipped.push(format!("{} (not a C/C++ source)", target.display()));
            }
            continue;
        }
        roots.push(target.clone());
        walk_dir(&target, excludes, &mut files, &mut skipped);
    }

    files.sort();
    files.dedup();

    if max_files > 0 && files.len() > max_files {
        skipped.extend(files[max_files..].iter().map(|p| p.display().to_string()));
        files.truncate(max_files);
    }

    let root = if files.is_empty() {
        roots.first().cloned().unwrap_or_else(|| env::current_dir().unwrap_or_else(|_| PathBuf::from(".")))
    } else {
        common_root(&files)
    };

    (files, skipped, root)
}

fn common_root(files: &[PathBuf]) -> PathBuf {
    let first = match files.first().and_then(|f| f.parent()) {
        Some(parent) => parent.to_path_buf(),
        None => return PathBuf::from("."),
    };
    for ancestor in first.ancestors() {
        if files.iter().all(|file| file.starts_with(ancestor)) {
            return ancestor.to_path_buf();
        }
    }
    first
}

fn count_loc(files: &[PathBuf]) -> usize {
    files
        .iter()
        .map(|path| {
            fs::read_to_string(path)
                .map(|text| text.lines().count())
                .unwrap_or(0)
        })
        .sum()
}

// ---------------------------------------------------------------------------
// compile_commands.json (serde_json)
// ---------------------------------------------------------------------------

const STRIP_PREFIXES: &[&str] = &[
    "-Wno-", "-fsanitize", "-flto", "-fprofile", "-fcoverage", "-save-temps", "-MJ",
];

fn shell_split(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    let mut escaped = false;
    for ch in text.chars() {
        if escaped {
            current.push(ch);
            escaped = false;
            continue;
        }
        match quote {
            Some(q) => {
                if ch == q {
                    quote = None;
                } else if ch == '\\' && q == '"' {
                    escaped = true;
                } else {
                    current.push(ch);
                }
            }
            None => {
                if ch == '\'' || ch == '"' {
                    quote = Some(ch);
                } else if ch == '\\' {
                    escaped = true;
                } else if ch.is_whitespace() {
                    if !current.is_empty() {
                        out.push(std::mem::take(&mut current));
                    }
                } else {
                    current.push(ch);
                }
            }
        }
    }
    if !current.is_empty() {
        out.push(current);
    }
    out
}

fn looks_like_compiler(name: &str) -> bool {
    let trimmed = name.trim_end_matches(|c: char| c.is_ascii_digit());
    matches!(trimmed, "cc" | "c++" | "clang" | "clang++" | "gcc" | "g++" | "clang-cl")
        || trimmed.starts_with("clang")
        || trimmed.starts_with("gcc")
        || trimmed.starts_with("g++")
}

fn same_path(arg: &str, source: &Path) -> bool {
    let path = Path::new(arg);
    if path == source {
        return true;
    }
    match (fs::canonicalize(path), fs::canonicalize(source)) {
        (Ok(a), Ok(b)) => a == b,
        _ => false,
    }
}

fn find_compile_commands(root: &Path, depth: usize) -> Option<PathBuf> {
    if depth > 4 {
        return None;
    }
    let direct = root.join("compile_commands.json");
    if direct.is_file() {
        return Some(direct);
    }
    let reader = fs::read_dir(root).ok()?;
    let mut entries: Vec<PathBuf> = reader.filter_map(|e| e.ok().map(|x| x.path())).collect();
    entries.sort();
    for path in entries {
        if !path.is_dir() {
            continue;
        }
        let name = path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
        if name.starts_with('.') || SKIP_DIRS.contains(&name.as_str()) {
            continue;
        }
        if let Some(found) = find_compile_commands(&path, depth + 1) {
            return Some(found);
        }
    }
    None
}

fn load_compile_commands(
    root: &Path,
    override_arg: Option<&str>,
) -> (HashMap<PathBuf, Vec<String>>, Vec<String>, Vec<String>) {
    if override_arg == Some("none") {
        return (HashMap::new(), Vec::new(), Vec::new());
    }
    let candidate = match override_arg {
        Some(path) if path != "auto" => PathBuf::from(path),
        _ => match find_compile_commands(root, 0) {
            Some(path) => path,
            None => return (HashMap::new(), Vec::new(), Vec::new()),
        },
    };
    if !candidate.is_file() {
        return (HashMap::new(), Vec::new(), Vec::new());
    }

    let text = match fs::read_to_string(&candidate) {
        Ok(text) => text,
        Err(_) => return (HashMap::new(), Vec::new(), Vec::new()),
    };
    let parsed: Value = match serde_json::from_str(&text) {
        Ok(value) => value,
        Err(_) => return (HashMap::new(), Vec::new(), Vec::new()),
    };
    let entries = match parsed.as_array() {
        Some(entries) => entries,
        None => return (HashMap::new(), Vec::new(), Vec::new()),
    };

    let mut flags_by_file: HashMap<PathBuf, Vec<String>> = HashMap::new();
    let mut includes: Vec<String> = Vec::new();
    let mut defines: Vec<String> = Vec::new();

    for entry in entries {
        let directory = entry
            .get("directory")
            .and_then(|v| v.as_str())
            .map(PathBuf::from)
            .unwrap_or_else(|| root.to_path_buf());
        let source = match entry.get("file").and_then(|v| v.as_str()) {
            Some(source) if !source.is_empty() => source,
            _ => continue,
        };
        let source_path = fs::canonicalize(directory.join(source))
            .unwrap_or_else(|_| directory.join(source));

        let mut argv: Vec<String> = if let Some(args) = entry.get("arguments").and_then(|v| v.as_array()) {
            args.iter()
                .filter_map(|item| item.as_str().map(|s| s.to_string()))
                .collect()
        } else {
            entry
                .get("command")
                .and_then(|v| v.as_str())
                .map(shell_split)
                .unwrap_or_default()
        };

        while let Some(first) = argv.first() {
            let name = Path::new(first)
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_default();
            if matches!(name.as_str(), "ccache" | "distcc" | "sccache" | "env") {
                argv.remove(0);
            } else {
                break;
            }
        }
        if let Some(first) = argv.first() {
            let name = Path::new(first)
                .file_name()
                .map(|n| n.to_string_lossy().to_string())
                .unwrap_or_default();
            if looks_like_compiler(&name) {
                argv.remove(0);
            }
        }

        let mut cleaned: Vec<String> = Vec::new();
        let mut index = 0;
        while index < argv.len() {
            let arg = &argv[index];
            if arg == "-o" || arg == "-MF" || arg == "-MT" || arg == "-MQ" || arg == "-MJ" {
                index += 2;
                continue;
            }
            if arg == "-c" || arg == "-w" || arg == "-Werror"
                || STRIP_PREFIXES.iter().any(|prefix| arg.starts_with(prefix))
            {
                index += 1;
                continue;
            }
            if same_path(arg, &source_path) {
                index += 1;
                continue;
            }
            if let Some(path) = arg.strip_prefix("-I") {
                if !path.is_empty() {
                    includes.push(path.to_string());
                }
            } else if arg == "-I" && index + 1 < argv.len() {
                includes.push(argv[index + 1].clone());
            }
            if let Some(define) = arg.strip_prefix("-D") {
                if !define.is_empty() {
                    defines.push(define.to_string());
                }
            }
            cleaned.push(arg.clone());
            index += 1;
        }
        flags_by_file.insert(source_path, cleaned);
    }

    includes.sort();
    includes.dedup();
    defines.sort();
    defines.dedup();
    (flags_by_file, includes, defines)
}

// ---------------------------------------------------------------------------
// Stage 1 — compiler warnings
// ---------------------------------------------------------------------------

fn inside(path: &str, root: &Path) -> bool {
    if path.is_empty() {
        return false;
    }
    let candidate = Path::new(path);
    let absolute = if candidate.is_absolute() {
        candidate.to_path_buf()
    } else {
        root.join(candidate)
    };
    match fs::canonicalize(&absolute) {
        Ok(resolved) => resolved.starts_with(root),
        Err(_) => absolute.starts_with(root),
    }
}

fn path_inside(path: &Path, root: &Path) -> bool {
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        root.join(path)
    };
    absolute.starts_with(root)
}

fn promote_system_includes(flags: &[String], root: &Path) -> Vec<String> {
    let mut out = Vec::new();
    let mut index = 0;
    while index < flags.len() {
        let arg = &flags[index];
        let mut value: Option<String> = None;
        let mut consumed = 1;
        if arg == "-I" && index + 1 < flags.len() {
            value = Some(flags[index + 1].clone());
            consumed = 2;
        } else if let Some(path) = arg.strip_prefix("-I") {
            if !path.is_empty() {
                value = Some(path.to_string());
            }
        }
        match value {
            Some(path) if Path::new(&path).is_absolute() && !path_inside(Path::new(&path), root) => {
                // -idirafter, not -isystem. A system directory from a build
                // database (Termux: -I$PREFIX/include) would otherwise be
                // searched BEFORE the toolchain's own C++ wrappers: with
                // -isystem, `#include <math.h>` picks the C header, `isfinite`
                // stays a macro, and `std::isfinite` expands to
                // `std::__builtin_isfinite` — 8 phantom errors in main.cpp.
                // -idirafter appends the directory after the system paths, so
                // third-party headers are still found and nothing is shadowed.
                out.push("-idirafter".to_string());
                out.push(path);
            }
            _ => out.extend(flags[index..index + consumed].iter().cloned()),
        }
        index += consumed;
    }
    out
}

fn compiler_for<'a>(path: &Path, cfg: &'a Config) -> &'a str {
    if is_c(path) { &cfg.cc } else { &cfg.cxx }
}

fn compile_command(path: &Path, cfg: &Config, syntax_only: bool) -> Vec<String> {
    let exe = compiler_for(path, cfg);
    let std = if is_c(path) { &cfg.std_c } else { &cfg.std_cxx };
    let db_flags = promote_system_includes(
        cfg.cc_commands.get(&path.to_path_buf()).cloned().unwrap_or_default().as_slice(),
        &cfg.root,
    );

    let mut cmd: Vec<String> = vec![exe.to_string()];
    cmd.extend(db_flags);
    cmd.extend(cfg.cflags.iter().cloned());
    if syntax_only {
        cmd.push("-fsyntax-only".to_string());
    } else {
        cmd.push("-O1".to_string());
        cmd.extend(SANITIZER_FLAGS.iter().map(|f| f.to_string()));
    }
    cmd.push(format!("-std={}", std));
    cmd.extend(BASE_WARNINGS.iter().map(|f| f.to_string()));

    cmd.push(format!("-I{}", cfg.root.display()));
    if let Some(parent) = path.parent() {
        cmd.push(format!("-I{}", parent.display()));
    }
    for include in &cfg.includes {
        cmd.push(format!("-I{}", include));
    }
    for define in &cfg.defines {
        cmd.push(format!("-D{}", define));
    }

    if is_clang(exe) {
        cmd.push("-ferror-limit=0".to_string());
        cmd.push("-fno-caret-diagnostics".to_string());
        cmd.push("-fno-color-diagnostics".to_string());
    } else {
        cmd.push("-fmax-errors=0".to_string());
        cmd.push("-fno-diagnostics-show-caret".to_string());
    }

    cmd.push(path.display().to_string());
    cmd
}

fn parse_location(prefix: &str) -> Option<(String, i64, Option<i64>)> {
    let parts: Vec<&str> = prefix.rsplitn(3, ':').collect();
    if parts.len() == 3 {
        if let (Ok(column), Ok(line)) = (parts[0].parse::<i64>(), parts[1].parse::<i64>()) {
            return Some((parts[2].to_string(), line, Some(column)));
        }
    }
    if parts.len() >= 2 {
        if let Ok(line) = parts[0].parse::<i64>() {
            let file = parts[1..].iter().rev().cloned().collect::<Vec<_>>().join(":");
            return Some((file, line, None));
        }
    }
    None
}

fn split_flag(text: &str) -> (String, String) {
    let trimmed = text.trim_end();
    if trimmed.ends_with(']') {
        if let Some(open) = trimmed.rfind(" [-W") {
            let message = trimmed[..open].trim().to_string();
            let check = trimmed[open + 2..trimmed.len() - 1].to_string();
            return (message, check);
        }
    }
    (trimmed.to_string(), String::new())
}

fn parse_diag(line: &str) -> Option<Finding> {
    let markers = [
        (": fatal error: ", "error"),
        (": error: ", "error"),
        (": warning: ", "warning"),
        (": remark: ", "info"),
    ];
    for (marker, severity) in markers {
        if let Some(position) = line.find(marker) {
            let prefix = &line[..position];
            let rest = &line[position + marker.len()..];
            if let Some((file, line_no, column)) = parse_location(prefix) {
                let (message, check) = split_flag(rest);
                return Some(Finding {
                    tool: "compiler".to_string(),
                    severity: severity.to_string(),
                    check: if check.is_empty() { severity.to_string() } else { check },
                    message,
                    file,
                    line: Some(line_no),
                    column,
                    ..Default::default()
                });
            }
        }
    }
    None
}

fn parse_compiler_output(text: &str, path: &Path) -> Vec<Finding> {
    text.lines()
        .filter_map(|line| {
            parse_diag(line).map(|mut finding| {
                if finding.file.is_empty() {
                    finding.file = path.display().to_string();
                }
                finding
            })
        })
        .collect()
}

fn stage_compiler(cfg: &Config) -> (Vec<Finding>, Stage) {
    let mut stage = Stage::new("compiler warnings");
    stage.files_total = cfg.files.len();
    let started = Instant::now();
    let failures = AtomicUsize::new(0);
    let truncated = AtomicUsize::new(0);

    let check = |path: &Path| -> Vec<Finding> {
        let cmd = compile_command(path, cfg, true);
        let result = run_cmd(&cmd, Some(&cfg.root), Duration::from_secs(cfg.timeout_compile), &[]);
        if result.truncated() {
            truncated.fetch_add(1, Ordering::Relaxed);
        }
        let mut local: Vec<Finding> = parse_compiler_output(&result.text(), path)
            .into_iter()
            .filter(|finding| inside(&finding.file, &cfg.root))
            .collect();
        if result.timed_out {
            failures.fetch_add(1, Ordering::Relaxed);
            local.push(operational_finding(
                "compiler",
                "timeout",
                format!("compilation exceeded {}s", cfg.timeout_compile),
                path.display().to_string(),
            ));
        } else if result.rc != 0 && !local.iter().any(|f| f.severity == "error") {
            // A failed command with no parsed diagnostic is not "clean"; it
            // usually means an unsupported flag or a toolchain problem.
            failures.fetch_add(1, Ordering::Relaxed);
            local.push(operational_finding(
                "compiler",
                "stage-failed",
                format!("compiler exited {}: {}", result.rc, result.last_line()),
                path.display().to_string(),
            ));
        }
        local
    };

    let next = AtomicUsize::new(0);
    let results: Mutex<Vec<Finding>> = Mutex::new(Vec::new());
    let workers = cfg.jobs.max(1);
    thread::scope(|scope| {
        for _ in 0..workers {
            scope.spawn(|| loop {
                let index = next.fetch_add(1, Ordering::Relaxed);
                if index >= cfg.files.len() {
                    break;
                }
                let local = check(&cfg.files[index]);
                results.lock().unwrap().extend(local);
            });
        }
    });

    stage.files_failed = failures.load(Ordering::Relaxed);
    stage.truncated = truncated.load(Ordering::Relaxed) > 0;
    stage.seconds = started.elapsed().as_secs_f64();
    stage.command = cfg
        .files
        .first()
        .map(|path| sh(&compile_command(path, cfg, true)))
        .unwrap_or_default();
    if cfg.files.is_empty() {
        stage.status = "skipped".to_string();
        stage.note = "no C/C++ files were discovered".to_string();
    } else if stage.files_failed > 0 {
        stage.status = "failed".to_string();
        stage.note = format!(
            "{} of {} file(s) could not be checked",
            stage.files_failed, stage.files_total
        );
    } else {
        stage.status = "ran".to_string();
    }
    if stage.truncated {
        merge_stage(&mut stage, "ran", "compiler output truncated at 8 MiB");
    }
    (results.into_inner().unwrap(), stage)
}

// ---------------------------------------------------------------------------
// Stage 2 — cppcheck
// ---------------------------------------------------------------------------

fn cppcheck_severity(severity: &str) -> &'static str {
    match severity {
        "error" => "error",
        "warning" => "warning",
        "style" => "style",
        "performance" => "performance",
        "portability" => "portability",
        _ => "info",
    }
}

fn parse_cppcheck(text: &str) -> Vec<Finding> {
    let mut findings = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    for line in text.lines() {
        let parts: Vec<&str> = line.splitn(6, '\t').collect();
        if parts.len() != 6 {
            continue;
        }
        let line_no = parts[1].parse::<i64>().unwrap_or(0);
        let column = parts[2].parse::<i64>().ok();
        let check = parts[4].trim();
        if check.is_empty() || check == "checkersReport" {
            continue;
        }
        let finding = Finding {
            tool: "cppcheck".into(),
            severity: cppcheck_severity(parts[3]).to_string(),
            check: check.to_string(),
            message: parts[5].to_string(),
            file: parts[0].to_string(),
            line: if line_no > 0 { Some(line_no) } else { None },
            column,
            ..Default::default()
        };
        if seen.insert(finding.key()) {
            findings.push(finding);
        }
    }
    findings
}

fn stage_cppcheck(cfg: &Config) -> (Vec<Finding>, Stage) {
    let mut stage = Stage::new("cppcheck");
    stage.files_total = cfg.files.len();
    let exe = match which("cppcheck") {
        Some(path) => path.display().to_string(),
        None => {
            stage.status = "unavailable".into();
            stage.note = "cppcheck not installed (see dependencies in the report)".into();
            return (Vec::new(), stage);
        }
    };

    let version = tool_version(&exe);
    let started = Instant::now();
    let mut findings: Vec<Finding> = Vec::new();
    let temp = make_temp("cqma-cppcheck");
    let groups: [(&str, Vec<&PathBuf>, String); 2] = [
        (
            "c",
            cfg.files.iter().filter(|p| is_c(p)).collect(),
            format!("--std={}", cfg.std_c),
        ),
        (
            "c++",
            cfg.files.iter().filter(|p| !is_c(p)).collect(),
            format!("--std={}", cfg.std_cxx),
        ),
    ];

    for (language, files, std) in groups {
        if files.is_empty() {
            continue;
        }
        let mut cmd: Vec<String> = vec![
            exe.clone(),
            "--enable=all".into(),
            "--inconclusive".into(),
            "--force".into(),
            "--inline-suppr".into(),
            format!("--language={}", language),
            std,
            // Missing includes are a build concern, not a code-quality defect:
            // stage 1 already fails on them. Without this, a C++ project that
            // includes a third-party SDK emits one "information" finding per
            // header cppcheck cannot open, which buries the real results.
            "--suppress=missingInclude".into(),
            "--suppress=missingIncludeSystem".into(),
            "--suppress=unmatchedSuppression".into(),
            "--template={file}\t{line}\t{column}\t{severity}\t{id}\t{message}".into(),
        ];
        // NOTE: no -j. cppcheck disables the whole-program unusedFunction check
        // when -j > 1 (verified: 6 findings at -j1 vs 4 at -j4), and that check
        // is a core requirement.
        if version_tuple(&version) >= (2, 14, 0) {
            // "exhaustive" finds more but can take many minutes on large
            // translation units; expose the choice instead of hardcoding it.
            cmd.push(format!("--check-level={}", cfg.cppcheck_level));
        }
        let mut include_dirs: Vec<String> = vec![cfg.root.display().to_string()];
        for file in &files {
            if let Some(parent) = file.parent() {
                include_dirs.push(parent.display().to_string());
            }
        }
        for sub in ["include", "src", "runtime"] {
            let candidate = cfg.root.join(sub);
            if candidate.is_dir() {
                include_dirs.push(candidate.display().to_string());
            }
        }
        include_dirs.extend(cfg.includes.iter().cloned());
        include_dirs.sort();
        include_dirs.dedup();
        // cppcheck is not a compiler: feeding it an SDK include tree often
        // breaks preprocessing (Termux limits.h uses __has_include_next,
        // which cppcheck 2.22 cannot evaluate) and silently drops whole files.
        // Analyze project headers only; libc is built in.
        include_dirs.retain(|dir| inside(dir, &cfg.root));
        for include in include_dirs {
            cmd.push(format!("-I{}", include));
        }
        for define in &cfg.defines {
            cmd.push(format!("-D{}", define));
        }
        for file in &files {
            cmd.push(file.display().to_string());
        }
        stage.command = sh(&cmd);

        let result = run_cmd(&cmd, Some(&cfg.root), Duration::from_secs(cfg.timeout_cppcheck), &[]);
        if result.truncated() {
            stage.truncated = true;
        }
        if result.timed_out {
            merge_stage(&mut stage, "failed", &format!("cppcheck exceeded {}s", cfg.timeout_cppcheck));
        } else if result.rc != 0 {
            merge_stage(&mut stage, "failed", &format!("cppcheck exit {}", result.rc));
        } else {
            merge_stage(&mut stage, "ran", "");
        }
        // Findings go to stderr; parse stdout too in case of redirection changes.
        findings.extend(parse_cppcheck(&result.err));
        findings.extend(parse_cppcheck(&result.out));
    }

    let _ = fs::remove_dir_all(&temp);
    stage.seconds = started.elapsed().as_secs_f64();
    if stage.truncated {
        merge_stage(&mut stage, "ran", "cppcheck output truncated at 8 MiB");
    }
    if cfg.files.is_empty() {
        stage.status = "skipped".to_string();
        stage.note = "no C/C++ files were discovered".to_string();
    }
    let findings = dedupe(findings);
    let missing = unanalyzed_files(&findings);
    if !missing.is_empty() {
        merge_stage(&mut stage, "ran", &format!("{} file(s) not analyzed", missing.len()));
    }
    (findings, stage)
}

// ---------------------------------------------------------------------------
// Stage 3 — clang static analyzer
// ---------------------------------------------------------------------------

fn analyzer_exe(path: &Path, cfg: &Config) -> Option<String> {
    // `--analyze` is Clang's driver flag. GCC's equivalent is `-fanalyzer`
    // and it is a different engine with different output; silently passing
    // `--analyze` to gcc produces a failed command that used to look clean.
    let primary = compiler_for(path, cfg);
    if is_clang(primary) {
        return Some(primary.to_string());
    }
    let fallback = if is_c(path) { "clang" } else { "clang++" };
    which(fallback).map(|p| p.display().to_string())
}

fn analyzer_command(path: &Path, cfg: &Config, exe: &str) -> Vec<String> {
    let std = if is_c(path) { &cfg.std_c } else { &cfg.std_cxx };
    let db_flags = promote_system_includes(
        cfg.cc_commands.get(&path.to_path_buf()).cloned().unwrap_or_default().as_slice(),
        &cfg.root,
    );

    let mut cmd: Vec<String> = vec![exe.to_string()];
    cmd.push("--analyze".to_string());
    cmd.push("-Xanalyzer".to_string());
    cmd.push("-analyzer-output=text".to_string());
    cmd.extend(db_flags);
    cmd.extend(cfg.cflags.iter().cloned());
    cmd.push(format!("-std={}", std));

    cmd.push(format!("-I{}", cfg.root.display()));
    if let Some(parent) = path.parent() {
        cmd.push(format!("-I{}", parent.display()));
    }
    for include in &cfg.includes {
        cmd.push(format!("-I{}", include));
    }
    for define in &cfg.defines {
        cmd.push(format!("-D{}", define));
    }
    cmd.push(path.display().to_string());
    cmd
}

fn split_analyzer_check(text: &str) -> (String, String) {
    let trimmed = text.trim_end();
    if trimmed.ends_with(']') {
        if let Some(open) = trimmed.rfind(" [") {
            let message = trimmed[..open].trim().to_string();
            let check = trimmed[open + 2..trimmed.len() - 1].to_string();
            return (message, check);
        }
    }
    (trimmed.to_string(), "clang-analyzer".to_string())
}

fn parse_analyzer_output(text: &str) -> Vec<Finding> {
    let mut findings = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    for line in text.lines() {
        for (marker, severity) in [(": warning: ", "warning"), (": error: ", "error")] {
            let position = match line.find(marker) {
                Some(position) => position,
                None => continue,
            };
            let prefix = &line[..position];
            let rest = &line[position + marker.len()..];
            if let Some((file, line_no, column)) = parse_location(prefix) {
                let (message, check) = split_analyzer_check(rest);
                let finding = Finding {
                    tool: "clang-analyzer".to_string(),
                    severity: severity.to_string(),
                    check,
                    message,
                    file,
                    line: Some(line_no),
                    column,
                    ..Default::default()
                };
                if seen.insert(finding.key()) {
                    findings.push(finding);
                }
            }
        }
    }
    findings
}

fn stage_analyzer(cfg: &Config) -> (Vec<Finding>, Stage) {
    let mut stage = Stage::new("clang static analyzer");
    stage.files_total = cfg.files.len();
    let started = Instant::now();
    let failures = AtomicUsize::new(0);
    let truncated = AtomicUsize::new(0);

    let check = |path: &Path| -> Vec<Finding> {
        let exe = match analyzer_exe(path, cfg) {
            Some(exe) => exe,
            None => {
                failures.fetch_add(1, Ordering::Relaxed);
                return vec![operational_finding(
                    "clang-analyzer",
                    "unavailable",
                    "clang/clang++ not found; GCC's -fanalyzer is a different engine and was not substituted".into(),
                    path.display().to_string(),
                )];
            }
        };
        let cmd = analyzer_command(path, cfg, &exe);
        let result = run_cmd(&cmd, Some(&cfg.root), Duration::from_secs(cfg.timeout_analyzer), &[]);
        if result.truncated() {
            truncated.fetch_add(1, Ordering::Relaxed);
        }
        let mut local = parse_analyzer_output(&result.text());
        local.retain(|finding| inside(&finding.file, &cfg.root));
        if result.timed_out {
            failures.fetch_add(1, Ordering::Relaxed);
            local.push(operational_finding(
                "clang-analyzer",
                "timeout",
                format!("clang analyzer exceeded {}s", cfg.timeout_analyzer),
                path.display().to_string(),
            ));
        } else if result.rc != 0 {
            failures.fetch_add(1, Ordering::Relaxed);
            local.push(operational_finding(
                "clang-analyzer",
                "stage-failed",
                format!("clang analyzer exited {}: {}", result.rc, result.last_line()),
                path.display().to_string(),
            ));
        }
        local
    };

    let next = AtomicUsize::new(0);
    let results: Mutex<Vec<Finding>> = Mutex::new(Vec::new());
    let workers = cfg.jobs.max(1);
    thread::scope(|scope| {
        for _ in 0..workers {
            scope.spawn(|| loop {
                let index = next.fetch_add(1, Ordering::Relaxed);
                if index >= cfg.files.len() {
                    break;
                }
                let local = check(&cfg.files[index]);
                results.lock().unwrap().extend(local);
            });
        }
    });

    stage.files_failed = failures.load(Ordering::Relaxed);
    stage.truncated = truncated.load(Ordering::Relaxed) > 0;
    stage.seconds = started.elapsed().as_secs_f64();
    stage.command = cfg
        .files
        .first()
        .and_then(|path| analyzer_exe(path, cfg))
        .map(|exe| sh(&analyzer_command(&cfg.files[0], cfg, &exe)))
        .unwrap_or_default();
    if cfg.files.is_empty() {
        stage.status = "skipped".to_string();
        stage.note = "no C/C++ files were discovered".to_string();
    } else if stage.files_failed > 0 {
        stage.status = "failed".to_string();
        stage.note = format!(
            "{} of {} file(s) could not be analyzed",
            stage.files_failed, stage.files_total
        );
    } else {
        stage.status = "ran".to_string();
    }
    if stage.truncated {
        merge_stage(&mut stage, "ran", "analyzer output truncated at 8 MiB");
    }
    (results.into_inner().unwrap(), stage)
}

// ---------------------------------------------------------------------------
// Stage 4 — sanitizers / valgrind
// ---------------------------------------------------------------------------

fn make_temp(prefix: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let dir = env::temp_dir().join(format!("{}-{}-{}", prefix, std::process::id(), nanos));
    let _ = fs::create_dir_all(&dir);
    dir
}

fn probe_sanitizers(cfg: &Config, workdir: &Path) -> (bool, bool, String) {
    let overflow_src = workdir.join("probe_overflow.c");
    if fs::write(&overflow_src, "int main(void){char a[4]; a[4]=1; return a[0];}\n").is_err() {
        return (false, false, "cannot write sanitizer probe".into());
    }
    let overflow_bin = workdir.join("probe_overflow");
    let mut build_cmd: Vec<String> = vec![cfg.cc.clone()];
    build_cmd.push("-O0".into());
    build_cmd.extend(SANITIZER_FLAGS.iter().map(|f| f.to_string()));
    build_cmd.push(overflow_src.display().to_string());
    build_cmd.push("-o".into());
    build_cmd.push(overflow_bin.display().to_string());

    let build = run_cmd(&build_cmd, None, Duration::from_secs(90), &[]);
    if build.rc != 0 {
        return (false, false, format!("sanitizer probe failed to compile: {}", build.last_line()));
    }

    let crash = run_cmd(
        &[overflow_bin.display().to_string()],
        None,
        Duration::from_secs(30),
        &[("ASAN_OPTIONS", "detect_leaks=0")],
    );
    if !crash.text().contains("AddressSanitizer") {
        return (false, false, "sanitizer probe produced no AddressSanitizer report".into());
    }

    let leak_src = workdir.join("probe_leak.c");
    let _ = fs::write(
        &leak_src,
        "#include <stdlib.h>\nint main(void){void *p=malloc(64);(void)p;return 0;}\n",
    );
    let leak_bin = workdir.join("probe_leak");
    let mut leak_cmd: Vec<String> = vec![cfg.cc.clone()];
    leak_cmd.push("-O0".into());
    leak_cmd.extend(SANITIZER_FLAGS.iter().map(|f| f.to_string()));
    leak_cmd.push(leak_src.display().to_string());
    leak_cmd.push("-o".into());
    leak_cmd.push(leak_bin.display().to_string());

    let build = run_cmd(&leak_cmd, None, Duration::from_secs(90), &[]);
    if build.rc != 0 {
        return (true, false, "ASan works; leak probe failed to compile".into());
    }
    let leak_run = run_cmd(
        &[leak_bin.display().to_string()],
        None,
        Duration::from_secs(30),
        &[("ASAN_OPTIONS", "detect_leaks=1")],
    );
    let text = leak_run.text();
    if text.contains("detected memory leaks") {
        (true, true, "ASan + LeakSanitizer operational".into())
    } else if text.contains("detect_leaks is not supported") {
        (true, false, "ASan works; LeakSanitizer unsupported on this platform".into())
    } else {
        (true, false, "ASan works; leak detection produced no report".into())
    }
}

fn parse_frame(line: &str) -> Option<(String, i64, String)> {
    let marker = line.find(" in ")?;
    let rest = &line[marker + 4..];
    let function = rest.split_whitespace().next()?.to_string();
    let location = rest.split_whitespace().last()?;
    // ASan frames are file:line:column; parse_location handles both shapes.
    let (file, line_no, _column) = parse_location(location)?;
    Some((file, line_no, function))
}

fn parse_runtime_output(text: &str) -> Vec<Finding> {
    let mut findings = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    let lines: Vec<&str> = text.lines().collect();

    for (index, line) in lines.iter().enumerate() {
        if let Some(position) = line.find("ERROR: AddressSanitizer: ") {
            let kind: String = line[position + 24..]
                .split_whitespace()
                .next()
                .unwrap_or("unknown")
                .to_string();
            let mut file = String::new();
            let mut line_no = None;
            let mut detail = String::new();
            for frame_line in lines.iter().skip(index).take(40) {
                if let Some((frame_file, frame_line_no, function)) = parse_frame(frame_line) {
                    if frame_file.contains('/') {
                        file = frame_file;
                        line_no = Some(frame_line_no);
                        detail = format!("{} at {}:{}", function, file, frame_line_no);
                        break;
                    }
                }
            }
            let key = format!("asan|{}|{}|{:?}", kind, file, line_no);
            if seen.insert(key) {
                findings.push(Finding {
                    tool: "asan".into(),
                    severity: "error".into(),
                    check: kind.clone(),
                    message: format!("AddressSanitizer: {}", kind),
                    file,
                    line: line_no,
                    detail,
                    weight: 25,
                    ..Default::default()
                });
            }
        }

        if let Some(position) = line.find(": runtime error: ") {
            let prefix = &line[..position];
            let message = line[position + 17..].to_string();
            if let Some((file, line_no, column)) = parse_location(prefix) {
                let key = format!("ubsan|{}|{}|{}", file, line_no, message);
                if seen.insert(key) {
                    findings.push(Finding {
                        tool: "ubsan".into(),
                        severity: "error".into(),
                        check: "undefined-behavior".into(),
                        message,
                        file,
                        line: Some(line_no),
                        column,
                        weight: 15,
                        ..Default::default()
                    });
                }
            }
        }

        for kind in ["Direct", "Indirect"] {
            let marker = format!("{} leak of ", kind);
            if let Some(position) = line.find(&marker) {
                let rest = &line[position + marker.len()..];
                let tokens: Vec<&str> = rest.split_whitespace().collect();
                let size = tokens.first().and_then(|t| t.parse::<u64>().ok()).unwrap_or(0);
                let objects = tokens.get(3).and_then(|t| t.parse::<u64>().ok()).unwrap_or(0);
                let mut detail = String::new();
                let mut file = String::new();
                let mut line_no = None;
                for frame_line in lines.iter().skip(index).take(12) {
                    if let Some((frame_file, frame_line_no, function)) = parse_frame(frame_line) {
                        detail = format!("{} at {}:{}", function, frame_file, frame_line_no);
                        file = frame_file;
                        line_no = Some(frame_line_no);
                        break;
                    }
                }
                let key = format!(
                    "leak|{}|{}|{}|{}|{:?}|{}",
                    kind, size, objects, file, line_no, detail
                );
                if seen.insert(key) {
                    findings.push(Finding {
                        tool: "lsan".into(),
                        severity: "error".into(),
                        check: format!("{}-leak", kind.to_lowercase()),
                        message: format!("{} leak: {} byte(s) in {} object(s)", kind, size, objects),
                        file,
                        line: line_no,
                        detail,
                        weight: 8,
                        ..Default::default()
                    });
                }
            }
        }
    }

    findings
}

fn parse_valgrind_log(text: &str) -> Vec<Finding> {
    let mut findings = Vec::new();
    let lines: Vec<&str> = text.lines().collect();

    for (index, line) in lines.iter().enumerate() {
        if let Some(position) = line.find("Invalid ") {
            let rest = &line[position..];
            let kind = if rest.starts_with("Invalid read") {
                "read"
            } else if rest.starts_with("Invalid write") {
                "write"
            } else {
                continue;
            };
            let size = rest
                .split(" of size ")
                .nth(1)
                .and_then(|s| s.split_whitespace().next())
                .unwrap_or("?");
            let mut file = String::new();
            let mut line_no = None;
            let mut function = String::new();
            for at in lines.iter().skip(index).take(8) {
                if let Some(at_pos) = at.find(" at 0x") {
                    let tail = &at[at_pos + 4..];
                    if let Some(colon) = tail.find(": ") {
                        let after = &tail[colon + 2..];
                        if let Some((func, location)) = after.split_once(" (") {
                            let location = location.trim_end_matches(')');
                            if let Some((frame_file, frame_line)) = location.rsplit_once(':') {
                                if let Ok(parsed) = frame_line.parse::<i64>() {
                                    function = func.to_string();
                                    file = frame_file.to_string();
                                    line_no = Some(parsed);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            findings.push(Finding {
                tool: "valgrind".into(),
                severity: "error".into(),
                check: format!("invalid-{}", kind),
                message: format!("Invalid {} of size {}", kind, size),
                file,
                line: line_no,
                detail: function,
                weight: 25,
                ..Default::default()
            });
        }

        for kind in ["definitely", "indirectly"] {
            let marker = format!("{} lost: ", kind);
            if let Some(position) = line.find(&marker) {
                let rest = &line[position + marker.len()..];
                let bytes = rest
                    .split_whitespace()
                    .next()
                    .unwrap_or("0")
                    .replace(',', "")
                    .parse::<u64>()
                    .unwrap_or(0);
                let blocks = rest
                    .split(" bytes in ")
                    .nth(1)
                    .and_then(|s| s.split_whitespace().next())
                    .unwrap_or("0")
                    .replace(',', "")
                    .parse::<u64>()
                    .unwrap_or(0);
                // A leak's identity is its allocation site, not only its size:
                // two 64-byte leaks at different lines are two findings.
                let mut file = String::new();
                let mut line_no = None;
                let mut function = String::new();
                for at in lines.iter().skip(index).take(24) {
                    if let Some(at_pos) = at.find(" at 0x").or_else(|| at.find(" by 0x")) {
                        let tail = &at[at_pos + 4..];
                        if let Some(colon) = tail.find(": ") {
                            let after = &tail[colon + 2..];
                            if let Some((func, location)) = after.split_once(" (") {
                                let location = location.trim_end_matches(')');
                                if let Some((frame_file, frame_line)) = location.rsplit_once(':') {
                                    if let Ok(parsed) = frame_line.parse::<i64>() {
                                        function = func.to_string();
                                        file = frame_file.to_string();
                                        line_no = Some(parsed);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                findings.push(Finding {
                    tool: "valgrind".into(),
                    severity: "error".into(),
                    check: format!("{}-leak", kind),
                    message: format!("{} lost: {} bytes in {} blocks", kind, bytes, blocks),
                    file,
                    line: line_no,
                    detail: function,
                    weight: 8,
                    ..Default::default()
                });
            }
        }
    }
    findings
}

fn has_main(path: &Path) -> bool {
    fs::read_to_string(path)
        .map(|text| text.contains("main(") || text.contains("main ("))
        .unwrap_or(false)
}

fn compile_leak_hooks(cfg: &Config, workdir: &Path) -> Option<PathBuf> {
    let source = workdir.join("cqma_leak_hooks.c");
    fs::write(&source, LEAK_HOOKS_C).ok()?;
    let object = workdir.join("cqma_leak_hooks.o");
    let build = |extra: &[&str]| -> bool {
        let mut cmd = vec![
            cfg.cc.clone(),
            "-O2".to_string(),
            "-g".to_string(),
            "-fno-sanitize=all".to_string(),
            "-fno-omit-frame-pointer".to_string(),
        ];
        cmd.extend(extra.iter().map(|flag| flag.to_string()));
        cmd.extend([
            "-c".to_string(),
            source.display().to_string(),
            "-o".to_string(),
            object.display().to_string(),
        ]);
        run_cmd(&cmd, None, Duration::from_secs(120), &[]).rc == 0
    };
    // Termux/Android clang defaults to emulated TLS, whose accessor allocates;
    // that would recurse through the malloc hook. Native TLS is allocation-free.
    if !build(&["-fno-emulated-tls"]) && !build(&[]) {
        return None;
    }
    if object.is_file() {
        Some(object)
    } else {
        None
    }
}

fn build_sanitized_binary(
    cfg: &Config,
    workdir: &Path,
    hooks: Option<&Path>,
) -> (Option<PathBuf>, String, Option<PathBuf>) {
    // Order matters: --build-cmd must run BEFORE the user-supplied-binary
    // shortcut, otherwise the documented
    //   cqma . --build-cmd "make" --binary ./app
    // never builds anything and runs the stale (or missing) binary.
    if cfg.build_cmd.is_none() {
        if let Some(binary) = &cfg.run_binary {
            return (Some(binary.clone()), "using user-supplied binary".into(), None);
        }
    }
    if let Some(build_cmd) = &cfg.build_cmd {
        let cflags = SANITIZER_FLAGS.join(" ");
        let mut ldflags = "-fsanitize=address,undefined".to_string();
        if let Some(object) = hooks {
            ldflags.push(' ');
            ldflags.push_str(&object.display().to_string());
        }
        let envs = [
            ("CFLAGS", cflags.as_str()),
            ("CXXFLAGS", cflags.as_str()),
            ("LDFLAGS", ldflags.as_str()),
        ];
        let result = run_cmd(
            &["sh".to_string(), "-c".to_string(), build_cmd.clone()],
            Some(&cfg.root),
            Duration::from_secs(900),
            &envs,
        );
        if result.rc != 0 {
            return (
                None,
                format!("build command failed (exit {}): {}", result.rc, result.last_line()),
                None,
            );
        }
        return (
            cfg.run_binary.clone(),
            "built via --build-cmd (pass --binary to run it)".into(),
            hooks.map(|path| path.to_path_buf()),
        );
    }
    if cfg.files.len() == 1 && has_main(&cfg.files[0]) {
        let source = &cfg.files[0];
        let binary = workdir.join(format!(
            "{}-cqma",
            source.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "app".into())
        ));
        let mut cmd = compile_command(source, cfg, false);
        if let Some(object) = hooks {
            cmd.push(object.display().to_string());
        }
        cmd.push("-o".into());
        cmd.push(binary.display().to_string());
        let result = run_cmd(&cmd, Some(&cfg.root), Duration::from_secs(cfg.timeout_compile), &[]);
        if result.rc != 0 {
            return (
                None,
                format!("sanitized build failed: {}", result.last_line()),
                None,
            );
        }
        return (
            Some(binary),
            "built single-file sanitized binary".into(),
            hooks.map(|path| path.to_path_buf()),
        );
    }
    (
        None,
        "project has multiple translation units; use --binary or --build-cmd".into(),
        None,
    )
}

fn parse_leak_tracker(text: &str, _cfg: &Config) -> (Vec<Finding>, String) {
    let mut findings = Vec::new();
    let mut summary = String::new();
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("summary ") {
            summary = rest.to_string();
            continue;
        }
        let rest = match line.strip_prefix("LEAK|") {
            Some(rest) => rest,
            None => continue,
        };
        let parts: Vec<&str> = rest.splitn(3, '|').collect();
        if parts.len() < 3 {
            continue;
        }
        let bytes: u64 = parts[0].parse().unwrap_or(0);
        let count: u64 = parts[1].parse().unwrap_or(0);
        let (top, detail) = match parts[2].split_once(';') {
            Some((top, detail)) => (top, detail),
            None => (parts[2], ""),
        };
        let (location, function) = match top.split_once(' ') {
            Some((location, function)) => (location, function),
            None => (top, ""),
        };
        let (file, line_no, _column) =
            parse_location(location).unwrap_or((String::new(), 0, None));
        let stack = if detail.is_empty() {
            function.to_string()
        } else {
            format!("{} <- {}", function, detail.replace(';', " <- "))
        };
        findings.push(Finding {
            tool: "leak-tracker".into(),
            severity: "error".into(),
            check: "leak".into(),
            message: format!(
                "{} byte(s) in {} allocation(s) still allocated and unreachable at exit",
                bytes, count
            ),
            file,
            line: if line_no > 0 { Some(line_no) } else { None },
            detail: stack,
            weight: 8,
            ..Default::default()
        });
    }
    (findings, summary)
}

fn stage_runtime(cfg: &Config) -> (Vec<Finding>, Stage) {
    let mut stage = Stage::new("runtime memory analysis");
    let workdir = make_temp("cqma-asan");

    if !cfg.sanitizer_ok {
        let valgrind = which("valgrind").map(|p| p.display().to_string());
        let binary = match &cfg.run_binary {
            Some(binary) => binary.clone(),
            None => {
                stage.status = "unavailable".into();
                stage.note = if cfg.sanitizer_note.is_empty() {
                    "sanitizers unavailable".into()
                } else {
                    cfg.sanitizer_note.clone()
                };
                let _ = fs::remove_dir_all(&workdir);
                return (Vec::new(), stage);
            }
        };
        let valgrind = match valgrind {
            Some(exe) => exe,
            None => {
                stage.status = "unavailable".into();
                stage.note = format!(
                    "{}valgrind not installed, so runtime analysis was skipped",
                    if cfg.sanitizer_note.is_empty() { String::new() } else { format!("{}; ", cfg.sanitizer_note) }
                );
                let _ = fs::remove_dir_all(&workdir);
                return (Vec::new(), stage);
            }
        };

        let log = workdir.join("valgrind.log");
        let mut cmd = vec![
            valgrind,
            "--leak-check=full".into(),
            "--show-leak-kinds=definite,indirect".into(),
            "--track-origins=yes".into(),
            "--error-exitcode=99".into(),
            format!("--log-file={}", log.display()),
            binary.display().to_string(),
        ];
        cmd.extend(cfg.run_args.iter().cloned());
        stage.command = sh(&cmd);
        let started = Instant::now();
        let result = run_cmd(&cmd, Some(&cfg.run_cwd), Duration::from_secs(cfg.timeout_run), &[]);
        stage.seconds = started.elapsed().as_secs_f64();
        stage.files_total = 1;

        let text = fs::read_to_string(&log).unwrap_or_default();
        let valid_log = text.contains("ERROR SUMMARY") || text.contains("HEAP SUMMARY");
        let mut findings = parse_valgrind_log(&text);
        if result.timed_out {
            stage.status = "failed".into();
            stage.files_failed = 1;
            stage.note = format!("valgrind exceeded {}s and was killed", cfg.timeout_run);
            findings.push(operational_finding(
                "valgrind",
                "timeout",
                stage.note.clone(),
                binary.display().to_string(),
            ));
        } else if !valid_log {
            stage.status = "failed".into();
            stage.files_failed = 1;
            stage.note = if text.is_empty() {
                format!(
                    "valgrind exited {} without writing a log: {}",
                    result.rc,
                    result.last_line()
                )
            } else {
                format!(
                    "valgrind exited {} without a summary log: {}",
                    result.rc,
                    result.last_line()
                )
            };
            findings.push(operational_finding(
                "valgrind",
                "stage-failed",
                stage.note.clone(),
                binary.display().to_string(),
            ));
        } else {
            stage.status = "ran".into();
            if result.rc != 0 && result.rc != 99 {
                stage.note = format!("program exited {} under valgrind", result.rc);
            }
        }
        if let Some(position) = text.find("ERROR SUMMARY: ") {
            let count = text[position + 15..]
                .split_whitespace()
                .next()
                .and_then(|s| s.parse::<usize>().ok())
                .unwrap_or(0);
            if count > findings.iter().filter(|f| !f.operational).count() {
                findings.push(operational_finding(
                    "valgrind",
                    "error-summary",
                    format!("valgrind reported {} error context(s); the detail was not fully parsed", count),
                    String::new(),
                ));
                // The log exists, but its detail was not fully understood.
                // That is a coverage gap, not a clean run: the summary count
                // says there ARE errors the report cannot show.
                stage.coverage_note =
                    "valgrind reported errors whose detail was not fully parsed".into();
            }
        }
        let _ = fs::remove_dir_all(&workdir);
        return (findings, stage);
    }

    let hooks = compile_leak_hooks(cfg, &workdir);
    let (binary, note, linked_hooks) = build_sanitized_binary(cfg, &workdir, hooks.as_deref());
    let binary = match binary {
        Some(binary) if binary.is_file() => binary,
        _ => {
            stage.status = "failed".into();
            stage.note = note;
            stage.files_total = 1;
            stage.files_failed = 1;
            let _ = fs::remove_dir_all(&workdir);
            return (Vec::new(), stage);
        }
    };
    stage.files_total = 1;

    let asan_options = if cfg.leaks_ok {
        "detect_leaks=1:symbolize=1:print_summary=1:detect_stack_use_after_return=1"
    } else {
        "detect_leaks=0:symbolize=1:print_summary=1"
    };
    let leak_report = workdir.join("leaks.txt");
    let leak_report_text = leak_report.display().to_string();
    // The root filter exists to hide startup allocations made by libc/ASan
    // when cqma builds the binary from sources it knows. For a user-supplied
    // --binary the sources may live anywhere, so filtering by the analyzed
    // directory would silently drop real leaks (and report CLEAN). The
    // tracker already discards sanitizer/libc frames by name.
    let root_text = if linked_hooks.is_some() {
        cfg.root.display().to_string()
    } else {
        String::new()
    };
    let envs = [
        ("ASAN_OPTIONS", asan_options),
        // halt_on_error=1 is required: UBSan recovery can spin forever on some
        // runtimes (observed on Android/bionic).
        ("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1"),
        ("LSAN_OPTIONS", "report_objects=0"),
        ("CQMA_LEAK_REPORT", leak_report_text.as_str()),
        ("CQMA_LEAK_ROOT", root_text.as_str()),
    ];

    let mut cmd = vec![binary.display().to_string()];
    cmd.extend(cfg.run_args.iter().cloned());
    stage.command = sh(&cmd);
    let started = Instant::now();
    let result = run_cmd(&cmd, Some(&cfg.run_cwd), Duration::from_secs(cfg.timeout_run), &envs);
    stage.seconds = started.elapsed().as_secs_f64();
    stage.status = "ran".into();

    let mut findings = parse_runtime_output(&result.text());

    let per_leak = findings
        .iter()
        .any(|f| f.tool == "lsan" && f.check.ends_with("-leak"));
    if !per_leak {
        if let Some(position) = result.text().find("SUMMARY: AddressSanitizer: ") {
            let rest = &result.text()[position + 25..];
            if let Some(bytes) = rest.split_whitespace().next() {
                if rest.contains("byte(s) leaked") {
                    findings.push(Finding {
                        tool: "lsan".into(),
                        severity: "error".into(),
                        check: "leak-total".into(),
                        message: format!("LeakSanitizer: {} bytes leaked in total", bytes),
                        weight: 8,
                        ..Default::default()
                    });
                }
            }
        }
    }

    // Our allocator-hook tracker writes a report at exit. It runs in the same
    // ASan pass, so memory errors and leaks come from one execution.
    let mut tracker_summary = String::new();
    // A user-supplied --binary may already contain the tracker (cqma's
    // hooks are linkable on their own). Honour any report the run produced,
    // whether cqma linked the hooks or the caller did.
    if leak_report.is_file() {
        match fs::read_to_string(&leak_report) {
            Ok(text) => {
                let (leak_findings, summary) = parse_leak_tracker(&text, cfg);
                findings.extend(leak_findings);
                tracker_summary = summary;
            }
            Err(err) => {
                stage.coverage_note = format!("leak tracker report was unreadable: {}", err);
            }
        }
    }
    if linked_hooks.is_some() || !tracker_summary.is_empty() {
        if tracker_summary.is_empty() {
            stage.note = "cqma leak tracker linked but wrote no report (program exited before destructors?)".into();
            stage.coverage_note = "leak tracker linked but wrote no report".into();
        } else {
            stage.note = format!("cqma leak tracker active: {}", tracker_summary);
        }
    } else if !cfg.leaks_ok {
        stage.note = "LeakSanitizer unavailable on this platform; leak coverage was not possible".into();
        stage.coverage_note = "leaks not covered on this host".into();
    }
    if result.truncated() {
        stage.truncated = true;
        merge_stage(&mut stage, "ran", "program output truncated at 8 MiB");
    }

    if result.timed_out {
        stage.status = "failed".into();
        stage.files_failed = 1;
        findings.push(operational_finding(
            "asan",
            "timeout",
            format!("program exceeded {}s and was killed", cfg.timeout_run),
            binary.display().to_string(),
        ));
    } else if result.rc < 0 {
        // A signal without a parsed sanitizer report is not a clean run: it
        // may be a memory bug the parser did not recognize, or a crash that
        // prevented the leak report from being written.
        stage.status = "failed".into();
        stage.files_failed = 1;
        findings.push(operational_finding(
            "asan",
            "signal",
            format!("program died with signal {} and produced no parsed sanitizer finding", -result.rc),
            binary.display().to_string(),
        ));
    } else if result.rc != 0 && findings.is_empty() {
        stage.status = "failed".into();
        stage.files_failed = 1;
        findings.push(operational_finding(
            "asan",
            "nonzero-exit",
            format!(
                "binary exited {} and produced no sanitizer finding; the execution was not clean",
                result.rc
            ),
            binary.display().to_string(),
        ));
    } else {
        stage.status = "ran".into();
        if result.rc != 0 {
            stage.note = format!("{}binary exited {}", if stage.note.is_empty() { String::new() } else { format!("{}; ", stage.note) }, result.rc);
        }
    }

    let _ = fs::remove_dir_all(&workdir);
    (findings, stage)
}

// ---------------------------------------------------------------------------
// Scoring & report
// ---------------------------------------------------------------------------

fn dedupe(findings: Vec<Finding>) -> Vec<Finding> {
    let mut seen: HashSet<String> = HashSet::new();
    findings
        .into_iter()
        .filter(|finding| seen.insert(finding.key()))
        .collect()
}

fn is_runtime_tool(tool: &str) -> bool {
    matches!(tool, "asan" | "ubsan" | "lsan" | "valgrind" | "leak-tracker")
}

struct Score {
    value: Option<u32>,
    grade: String,
    weighted: u32,
}

fn compute_score(findings: &[Finding], loc: usize, complete: bool) -> Score {
    // Operational warnings (timeouts, failed analyzers, truncated output) are
    // not evidence of a memory defect and must not move the defect score.
    let scored: Vec<&Finding> = findings.iter().filter(|f| !f.operational).collect();
    let weighted: u32 = scored
        .iter()
        .map(|f| if f.weight > 0 { f.weight } else { severity_weight(&f.severity) })
        .sum();
    if !complete {
        return Score { value: None, grade: "INCOMPLETE".into(), weighted };
    }
    if scored.is_empty() {
        return Score { value: Some(100), grade: "CLEAN".into(), weighted: 0 };
    }
    let line_unit = (loc as f64 / 1000.0).max(1.0);
    let raw = 100.0 - weighted as f64 / line_unit;
    let mut score = raw.round().clamp(0.0, 100.0) as u32;
    // A confirmed memory-safety defect is not style debt: no amount of code
    // volume makes a buffer overflow, use-after-free or leak acceptable. The
    // density model above rewards large trees, so it gets a hard ceiling.
    let runtime = scored.iter().filter(|f| is_runtime_tool(&f.tool)).count();
    let static_errors = scored
        .iter()
        .filter(|f| f.severity == "error" && !is_runtime_tool(&f.tool))
        .count();
    let ceiling = if runtime >= 3 {
        49
    } else if runtime >= 1 {
        69
    } else if static_errors >= 1 {
        79
    } else {
        100
    };
    score = score.min(ceiling);
    // 100 is reserved for a run with no confirmed findings. A low-weight
    // warning in a large tree must not round up to a perfect score: "100"
    // and "2 confirmed defect(s)" in the same report is a contradiction.
    if !scored.is_empty() {
        score = score.min(99);
    }
    let grade = if score >= 90 {
        "A"
    } else if score >= 80 {
        "B"
    } else if score >= 70 {
        "C"
    } else if score >= 60 {
        "D"
    } else {
        "F"
    };
    Score { value: Some(score), grade: grade.to_string(), weighted }
}

fn esc(text: &str) -> String {
    text.replace('|', "\\|").replace('\n', " ").replace('\r', " ")
}

fn md_table(headers: &[&str], rows: &[Vec<String>]) -> String {
    let mut out = String::new();
    out.push_str("| ");
    out.push_str(&headers.join(" | "));
    out.push_str(" |\n|");
    for _ in headers {
        out.push_str("---|");
    }
    out.push('\n');
    for row in rows {
        out.push_str("| ");
        out.push_str(
            &row.iter()
                .map(|cell| esc(cell))
                .collect::<Vec<_>>()
                .join(" | "),
        );
        out.push_str(" |\n");
    }
    out
}

fn rel(path: &str, root: &Path) -> String {
    if path.is_empty() {
        return String::new();
    }
    let candidate = Path::new(path);
    let absolute = if candidate.is_absolute() {
        candidate.to_path_buf()
    } else {
        root.join(candidate)
    };
    match absolute.strip_prefix(root) {
        Ok(relative) => relative.display().to_string(),
        Err(_) => path.to_string(),
    }
}

fn stage_tools(name: &str) -> &'static [&'static str] {
    if name.starts_with("compiler") {
        &["compiler"]
    } else if name.starts_with("cppcheck") {
        &["cppcheck"]
    } else if name.starts_with("clang") {
        &["clang-analyzer"]
    } else if name.starts_with("runtime") {
        &["asan", "ubsan", "lsan", "valgrind", "leak-tracker"]
    } else {
        &[]
    }
}

/// Files cppcheck reported as unparseable (no working preprocessor config).
fn unanalyzed_files(findings: &[Finding]) -> Vec<String> {
    let mut files: Vec<String> = findings
        .iter()
        .filter(|f| f.check == "noValidConfiguration")
        .map(|f| f.file.clone())
        .collect();
    files.sort();
    files.dedup();
    files
}

/// Reasons a run is not a clean bill of health.
fn coverage_gaps(cfg: &Config, findings: &[Finding], stages: &[Stage]) -> Vec<String> {
    let mut gaps = Vec::new();
    if stages.is_empty() {
        gaps.push("no analysis stages were requested (all disabled)".into());
    }
    if !cfg.skipped_files.is_empty() {
        gaps.push(format!(
            "{} input file(s) were skipped before analysis (see report section 8)",
            cfg.skipped_files.len()
        ));
    }
    let missing = unanalyzed_files(findings);
    if !missing.is_empty() {
        gaps.push(format!("{} file(s) not analyzed by cppcheck", missing.len()));
    }
    for stage in stages {
        if stage.status != "ran" {
            let detail = if stage.note.is_empty() {
                stage.status.clone()
            } else {
                format!("{}: {}", stage.status, stage.note)
            };
            gaps.push(format!("{} did not complete ({})", stage.name, detail));
        }
        if stage.files_failed > 0 && stage.status == "ran" {
            gaps.push(format!(
                "{}: {} of {} file(s) failed",
                stage.name, stage.files_failed, stage.files_total
            ));
        }
        if stage.truncated {
            gaps.push(format!("{}: output truncated at 8 MiB", stage.name));
        }
        if !stage.coverage_note.is_empty()
            && (stage.note.is_empty()
                || !(stage.note.contains(&stage.coverage_note)
                    || stage.coverage_note.contains(&stage.note)))
        {
            gaps.push(format!("{}: {}", stage.name, stage.coverage_note));
        }
    }
    gaps.sort();
    gaps.dedup();
    gaps
}

fn build_report(cfg: &Config, findings: &[Finding], stages: &[Stage], elapsed: f64) -> String {
    let gaps = coverage_gaps(cfg, findings, stages);
    let complete = gaps.is_empty();
    let score = compute_score(findings, cfg.loc, complete);
    let confirmed: Vec<&Finding> = findings.iter().filter(|f| !f.operational).collect();
    let operational_count = findings.len() - confirmed.len();

    let mut by_tool: BTreeMap<String, Vec<&Finding>> = BTreeMap::new();
    for finding in findings {
        by_tool.entry(finding.tool.clone()).or_default().push(finding);
    }
    let runtime_findings: Vec<&Finding> = findings
        .iter()
        .filter(|f| matches!(f.tool.as_str(), "asan" | "ubsan" | "lsan" | "valgrind" | "leak-tracker"))
        .collect();
    let confirmed_runtime: Vec<&&Finding> = runtime_findings
        .iter()
        .filter(|f| !f.operational)
        .collect();
    let leak_lines = confirmed
        .iter()
        .filter(|f| f.check.to_lowercase().contains("leak"))
        .count();

    let missing = unanalyzed_files(findings);

    let verdict = if !complete {
        if confirmed.is_empty() {
            "⚠️ **ANALYSIS INCOMPLETE — no confirmed defects in the stages that completed**".to_string()
        } else {
            format!(
                "⚠️ **ANALYSIS INCOMPLETE — {} confirmed finding(s) so far**",
                confirmed.len()
            )
        }
    } else if findings.is_empty() {
        "✅ **CLEAN — no issues found**".to_string()
    } else if confirmed.is_empty() {
        format!(
            "✅ **No confirmed defects — {} operational warning(s)**",
            operational_count
        )
    } else {
        format!(
            "**{}/100 (grade {})**",
            score.value.unwrap_or(0),
            score.grade
        )
    };

    let mut out = String::new();
    let _ = writeln!(out, "# Code Quality & Memory Safety Report\n");
    let _ = writeln!(out, "**Target:** `{}`  ", cfg.root.display());
    let _ = writeln!(out, "**Files analyzed:** {} ({} lines of C/C++)  ", cfg.files.len(), cfg.loc);
    if !cfg.skipped_files.is_empty() {
        let _ = writeln!(out, "**Inputs skipped:** {} (see section 8)  ", cfg.skipped_files.len());
    }
    let _ = writeln!(
        out,
        "**Generated:** {}  ",
        chrono_like_timestamp()
    );
    let _ = writeln!(out, "**Analyzer:** cqma {} (Rust)  ", VERSION);
    let _ = writeln!(out, "**Analysis complete:** {}  ", if complete { "yes" } else { "NO" });
    let _ = writeln!(out, "**Duration:** {:.1}s\n", elapsed);
    let _ = writeln!(out, "## Verdict: {}\n", verdict);

    if findings.is_empty() {
        let _ = writeln!(out, "No issues were reported by the stages that ran.");
        if !gaps.is_empty() {
            let _ = writeln!(out, "\n> ⚠️ **This is not a clean bill of health.** {}.\n", gaps.join("; "));
        }
    } else {
        let mut counts: BTreeMap<&str, usize> = BTreeMap::new();
        for finding in findings {
            *counts.entry(finding.severity.as_str()).or_insert(0) += 1;
        }
        let breakdown = counts
            .iter()
            .map(|(severity, count)| format!("{} {}", count, severity))
            .collect::<Vec<_>>()
            .join(", ");
        let _ = writeln!(
            out,
            "{} finding(s): {} ({} confirmed defect(s), {} operational warning(s)).\n",
            findings.len(),
            breakdown,
            confirmed.len(),
            operational_count
        );
        if complete && !confirmed.is_empty() {
            let _ = writeln!(
                out,
                "Score = 100 − weighted/(lines/1000), weighted = {}, floor 0. **Score: {}/100 (grade {})**\n",
                score.weighted,
                score.value.unwrap_or(0),
                score.grade
            );
        } else if complete {
            let _ = writeln!(
                out,
                "**Score: 100/100 (CLEAN)** — operational warnings are reported separately and do not count as defects.\n"
            );
        } else {
            let _ = writeln!(
                out,
                "**Score: unavailable (INCOMPLETE)** — a clean score is reserved for runs where every requested stage completed.\n"
            );
        }
        let runtime = confirmed_runtime.len();
        if runtime > 0 {
            let _ = writeln!(
                out,
                "> Grade ceiling applied: **{} confirmed memory-safety defect(s)** cap the score at {} — a leak, overflow or use-after-free is not style debt, so code volume cannot dilute it.\n",
                runtime,
                if runtime >= 3 { "49 (F)" } else { "69 (D)" }
            );
        }
    }

    if !confirmed_runtime.is_empty() {
        let errors = confirmed_runtime.iter().filter(|f| f.severity == "error").count();
        let _ = writeln!(
            out,
            "> ⚠️ **{} confirmed runtime memory-safety finding(s)** — these are real failures, not style.\n",
            errors
        );
    }

    if !gaps.is_empty() {
        let _ = writeln!(
            out,
            "> ⚠️ **Coverage is incomplete:** {}. The score is unavailable; findings above are only what the completed stages reported.\n",
            gaps.join("; ")
        );
    }

    let _ = writeln!(out, "## Summary\n");
    let mut summary_rows: Vec<Vec<String>> = Vec::new();
    for stage in stages {
        let tools = stage_tools(&stage.name);
        let count = findings
            .iter()
            .filter(|f| tools.contains(&f.tool.as_str()))
            .count();
        let icon = match stage.status.as_str() {
            "ran" => "✅",
            "failed" => "❌",
            "skipped" | "unavailable" => "⏭️",
            _ => "•",
        };
        summary_rows.push(vec![
            stage.name.clone(),
            format!("{} {}", icon, stage.status),
            count.to_string(),
            format!("{:.1}s", stage.seconds),
            stage.note.clone(),
        ]);
    }
    out.push_str(&md_table(&["Stage", "Status", "Findings", "Time", "Notes"], &summary_rows));
    out.push('\n');

    let _ = writeln!(out, "## 1. Compilation Warnings\n");
    let compiler_findings = by_tool.get("compiler").cloned().unwrap_or_default();
    if compiler_findings.is_empty() {
        let _ = writeln!(out, "No compiler warnings. ✅\n");
    } else {
        let mut by_file: BTreeMap<String, Vec<&Finding>> = BTreeMap::new();
        for finding in &compiler_findings {
            by_file.entry(finding.file.clone()).or_default().push(finding);
        }
        let _ = writeln!(
            out,
            "{} diagnostic(s) across {} file(s). Flags: `{}`.\n",
            compiler_findings.len(),
            by_file.len(),
            BASE_WARNINGS.join(" ")
        );
        let mut files: Vec<(&String, &Vec<&Finding>)> = by_file.iter().collect();
        files.sort_by_key(|(_, items)| std::cmp::Reverse(items.len()));
        for (file, items) in files.into_iter().take(40) {
            let _ = writeln!(out, "### `{}` — {}\n", rel(file, &cfg.root), items.len());
            let rows: Vec<Vec<String>> = items
                .iter()
                .take(40)
                .map(|finding| {
                    vec![
                        finding.line.map(|n| n.to_string()).unwrap_or_default(),
                        finding.severity.clone(),
                        finding.check.clone(),
                        finding.message.clone(),
                    ]
                })
                .collect();
            out.push_str(&md_table(&["Line", "Severity", "Flag", "Message"], &rows));
            if items.len() > 40 {
                let _ = writeln!(out, "*… {} more in JSON output.*", items.len() - 40);
            }
            out.push('\n');
        }
    }

    let _ = writeln!(out, "## 2. Static Analysis (cppcheck)\n");
    let all_cpp_findings = by_tool.get("cppcheck").cloned().unwrap_or_default();
    let cpp_findings: Vec<&Finding> = all_cpp_findings
        .iter()
        .copied()
        .filter(|f| f.check != "noValidConfiguration")
        .collect();
    if !missing.is_empty() {
        let _ = writeln!(
            out,
            "> ⚠️ **{} file(s) were NOT analyzed** (cppcheck could not extract a working preprocessor configuration). Defects in these files are unknown.\n",
            missing.len()
        );
        for file in missing.iter().take(10) {
            let _ = writeln!(out, "> - `{}`", rel(file, &cfg.root));
        }
        if missing.len() > 10 {
            let _ = writeln!(out, "> - … and {} more", missing.len() - 10);
        }
        out.push('\n');
    }
    if cpp_findings.is_empty() {
        let _ = writeln!(out, "No static-analysis defects. ✅\n");
    } else {
        let _ = writeln!(
            out,
            "{} defect(s). Null-pointer and unused-function checks are included.\n",
            cpp_findings.len()
        );
        for severity in ["error", "warning", "performance", "portability", "style", "info"] {
            let items: Vec<&&Finding> = cpp_findings
                .iter()
                .filter(|f| f.severity == severity)
                .collect();
            if items.is_empty() {
                continue;
            }
            let _ = writeln!(out, "### {} ({})\n", capitalize(severity), items.len());
            let rows: Vec<Vec<String>> = items
                .iter()
                .take(80)
                .map(|finding| {
                    vec![
                        format!(
                            "{}:{}",
                            rel(&finding.file, &cfg.root),
                            finding.line.map(|n| n.to_string()).unwrap_or_else(|| "?".into())
                        ),
                        finding.check.clone(),
                        finding.message.clone(),
                    ]
                })
                .collect();
            out.push_str(&md_table(&["Location", "Check", "Message"], &rows));
            if items.len() > 80 {
                let _ = writeln!(out, "*… {} more in JSON output.*", items.len() - 80);
            }
            out.push('\n');
        }
    }

    let _ = writeln!(out, "## 3. Clang Static Analyzer\n");
    let analyzer_findings = by_tool.get("clang-analyzer").cloned().unwrap_or_default();
    let analyzer_confirmed: Vec<&&Finding> =
        analyzer_findings.iter().filter(|f| !f.operational).collect();
    if analyzer_findings.is_empty() {
        let _ = writeln!(out, "No path-sensitive defects. ✅\n");
    } else {
        let _ = writeln!(
            out,
            "{} defect(s) from clang's path-sensitive engine (null derefs, leaks, dead stores, uninitialized reads); {} operational warning(s) are listed separately.\n",
            analyzer_confirmed.len(),
            analyzer_findings.len() - analyzer_confirmed.len()
        );
        let rows: Vec<Vec<String>> = analyzer_findings
            .iter()
            .take(120)
            .map(|finding| {
                vec![
                    format!(
                        "{}:{}",
                        rel(&finding.file, &cfg.root),
                        finding.line.map(|n| n.to_string()).unwrap_or_else(|| "?".into())
                    ),
                    finding.check.clone(),
                    format!(
                        "{}{}",
                        finding.message,
                        if finding.operational { " (operational)" } else { "" }
                    ),
                ]
            })
            .collect();
        out.push_str(&md_table(&["Location", "Checker", "Message"], &rows));
        if analyzer_findings.len() > 120 {
            let _ = writeln!(out, "*… {} more in JSON output.*", analyzer_findings.len() - 120);
        }
        out.push('\n');
    }

    let _ = writeln!(out, "## 4. Runtime Memory Analysis\n");
    if runtime_findings.is_empty() {
        let runtime = stages.iter().find(|s| s.name.starts_with("runtime"));
        match runtime {
            Some(stage) if stage.status == "ran" => {
                let _ = writeln!(out, "No runtime memory errors or leaks were observed. ✅");
                if !stage.note.is_empty() {
                    let _ = writeln!(out, "\n> ℹ️ {}", stage.note);
                }
                if !cfg.leaks_ok && !stage.note.contains("leak tracker active") {
                    let _ = writeln!(
                        out,
                        "\n> ⚠️ **Leaks were not covered on this run.** Build a single-file target, pass `--build-cmd`, or install valgrind for leak tracking."
                    );
                }
            }
            Some(stage) => {
                let _ = writeln!(out, "Runtime analysis did not run. ⏭️");
                if !stage.note.is_empty() {
                    let _ = writeln!(out, "\nReason: {}", stage.note);
                }
                let _ = writeln!(
                    out,
                    "\nEnable it with `--binary ./program` (single-file targets are built automatically)."
                );
            }
            None => {
                let _ = writeln!(out, "Runtime analysis was not requested. ⏭️");
            }
        }
        out.push('\n');
    } else {
        let memory_errors = confirmed_runtime.iter().filter(|f| f.severity == "error").count();
        let _ = writeln!(
            out,
            "{} runtime finding(s): {} confirmed memory/UB error(s), {} leak-related, {} operational warning(s).\n",
            runtime_findings.len(),
            memory_errors,
            leak_lines,
            runtime_findings.len() - confirmed_runtime.len()
        );
        let rows: Vec<Vec<String>> = runtime_findings
            .iter()
            .map(|finding| {
                vec![
                    finding.tool.clone(),
                    finding.check.clone(),
                    if finding.file.is_empty() {
                        "—".to_string()
                    } else {
                        format!(
                            "{}:{}",
                            rel(&finding.file, &cfg.root),
                            finding.line.map(|n| n.to_string()).unwrap_or_else(|| "?".into())
                        )
                    },
                    finding.message.clone(),
                    finding.detail.clone(),
                ]
            })
            .collect();
        out.push_str(&md_table(&["Tool", "Kind", "Location", "Message", "Detail"], &rows));
        out.push('\n');
    }

    if !confirmed.is_empty() {
        let _ = writeln!(out, "## 5. Hotspots (fix these first)\n");
        let mut per_file: HashMap<String, u32> = HashMap::new();
        for finding in &confirmed {
            let key = if finding.file.is_empty() {
                "(runtime / no location)".to_string()
            } else {
                rel(&finding.file, &cfg.root)
            };
            let weight = if finding.weight > 0 {
                finding.weight
            } else {
                severity_weight(&finding.severity)
            };
            *per_file.entry(key).or_insert(0) += weight;
        }
        let mut ranked: Vec<(String, u32)> = per_file.into_iter().collect();
        ranked.sort_by_key(|(_, weight)| std::cmp::Reverse(*weight));
        let rows: Vec<Vec<String>> = ranked
            .into_iter()
            .take(15)
            .map(|(path, weight)| vec![path, weight.to_string()])
            .collect();
        out.push_str(&md_table(&["File", "Weighted issues"], &rows));
        out.push('\n');
    }

    let _ = writeln!(out, "## 6. Toolchain & Environment\n");
    let mut tool_rows: Vec<Vec<String>> = Vec::new();
    for (name, exe) in [
        ("C compiler", Some(cfg.cc.clone())),
        ("C++ compiler", Some(cfg.cxx.clone())),
        ("cppcheck", which("cppcheck").map(|p| p.display().to_string())),
        ("valgrind", which("valgrind").map(|p| p.display().to_string())),
    ] {
        match exe {
            Some(exe) => tool_rows.push(vec![
                name.to_string(),
                exe.clone(),
                tool_version(&exe),
                "ready".to_string(),
            ]),
            None => tool_rows.push(vec![
                name.to_string(),
                "not found".to_string(),
                "—".to_string(),
                "stage skipped/fallback".to_string(),
            ]),
        }
    }
    out.push_str(&md_table(&["Tool", "Path", "Version", "Status"], &tool_rows));
    let _ = writeln!(
        out,
        "\n- Sanitizers: {} ({})",
        if cfg.sanitizer_ok { "available" } else { "unavailable" },
        if cfg.sanitizer_note.is_empty() { "probed at startup" } else { &cfg.sanitizer_note }
    );
    let _ = writeln!(
        out,
        "- Leak detection: {}\n",
        if cfg.leaks_ok {
            "LeakSanitizer".to_string()
        } else if cfg.sanitizer_ok {
            "cqma allocator-hook tracker (ASan builds); LeakSanitizer unavailable".to_string()
        } else {
            "not available on this host".to_string()
        }
    );

    let _ = writeln!(out, "## 7. Reproduce\n");
    let _ = writeln!(out, "```sh");
    let _ = writeln!(out, "# compiler diagnostics");
    let _ = writeln!(
        out,
        "{} -fsyntax-only {} -std={} <file.c>",
        cfg.cc,
        BASE_WARNINGS.join(" "),
        cfg.std_c
    );
    let _ = writeln!(out, "\n# static analysis");
    let _ = writeln!(
        out,
        "cppcheck --enable=all --inconclusive --force --std={} <files>",
        cfg.std_c
    );
    let _ = writeln!(out, "\n# runtime");
    let _ = writeln!(
        out,
        "{} {} {} -O1 <file.c> -o app && ./app",
        cfg.cc,
        BASE_WARNINGS.join(" "),
        SANITIZER_FLAGS.join(" ")
    );
    let _ = writeln!(out, "```\n");

    if !cfg.skipped_files.is_empty() {
        let _ = writeln!(out, "## 8. Skipped Inputs\n");
        for item in cfg.skipped_files.iter().take(20) {
            let _ = writeln!(out, "- `{}`", item);
        }
        if cfg.skipped_files.len() > 20 {
            let _ = writeln!(out, "- … and {} more", cfg.skipped_files.len() - 20);
        }
        out.push('\n');
    }

    out
}

fn capitalize(text: &str) -> String {
    let mut chars = text.chars();
    match chars.next() {
        Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
        None => String::new(),
    }
}

fn chrono_like_timestamp() -> String {
    // std has no date formatting; ask `date` once (portable on Linux/macOS).
    let result = run_cmd(
        &["date".to_string(), "+%Y-%m-%d %H:%M:%S".to_string()],
        None,
        Duration::from_secs(5),
        &[],
    );
    let text = result.text();
    if text.is_empty() {
        "unknown".to_string()
    } else {
        text
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Args {
    targets: Vec<String>,
    cc: Option<String>,
    cxx: Option<String>,
    std_c: String,
    std_cxx: String,
    includes: Vec<String>,
    defines: Vec<String>,
    cflags: Vec<String>,
    compile_commands: Option<String>,
    jobs: usize,
    timeout_compile: u64,
    timeout_run: u64,
    timeout_cppcheck: u64,
    cppcheck_level: String,
    timeout_analyzer: u64,
    max_files: usize,
    excludes: Vec<String>,
    no_compile: bool,
    no_cppcheck: bool,
    no_analyzer: bool,
    no_runtime: bool,
    binary: Option<String>,
    build_cmd: Option<String>,
    run_args: String,
    output: String,
    json_out: Option<String>,
    quiet: bool,
    exit_zero: bool,
}

impl Default for Args {
    fn default() -> Self {
        let jobs = thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(1)
            .min(4)
            .max(1);
        Self {
            targets: Vec::new(),
            cc: None,
            cxx: None,
            std_c: "c11".into(),
            std_cxx: "c++17".into(),
            includes: Vec::new(),
            defines: Vec::new(),
            cflags: Vec::new(),
            compile_commands: None,
            jobs,
            timeout_compile: 120,
            timeout_run: 60,
            timeout_cppcheck: 900,
            cppcheck_level: "exhaustive".into(),
            timeout_analyzer: 300,
            max_files: 0,
            excludes: Vec::new(),
            no_compile: false,
            no_cppcheck: false,
            no_analyzer: false,
            no_runtime: false,
            binary: None,
            build_cmd: None,
            run_args: String::new(),
            output: "cqma-report.md".into(),
            json_out: None,
            quiet: false,
            exit_zero: false,
        }
    }
}

fn usage() {
    println!(
        "cqma {} — Code Quality & Memory Safety Analyzer for C/C++

USAGE:
    cqma <file.c|project-dir> [more targets] [options]

STAGES:
    1. compiler warnings  -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    2. cppcheck           --enable=all (unused functions, style, null pointers)
    3. clang analyzer     --analyze (path-sensitive null/leak/dead-store checks)
    4. runtime            ASan+UBSan, or valgrind --leak-check=full

OPTIONS:
    --cc <path>                 C compiler (default: clang, then gcc)
    --cxx <path>                C++ compiler
    --std-c <std>               C standard (default c11)
    --std-cxx <std>             C++ standard (default c++17)
    -I, --include <dir>         extra include directory (repeatable)
    -D, --define <name[=val]>   extra preprocessor define (repeatable)
    --cflag <flag>              extra compiler flag (repeatable)
    --compile-commands <p|auto|none>
    --jobs <n>                  parallel compiler jobs
    --timeout-compile <s>       per-file compile timeout (default 120)
    --timeout-run <s>           program runtime timeout (default 60)
    --timeout-cppcheck <s>      cppcheck timeout (default 900)
    --cppcheck-level <l>        normal|exhaustive (default exhaustive)
    --max-files <n>             cap analyzed files
    --exclude <substring>       skip paths containing this (repeatable)
    --no-compile                skip stage 1
    --no-cppcheck               skip stage 2
    --no-analyzer               skip stage 3 (clang static analyzer)
    --timeout-analyzer <s>      per-file analyzer timeout (default 300)
    --no-runtime                skip stage 4
    --binary <path>             run an existing binary under ASan/valgrind
    --build-cmd <cmd>           build with sanitizer flags before running
    --run-args \"...\"            arguments for the analyzed binary
    --output <path>             Markdown report (default cqma-report.md)
    --json <path>               also write machine-readable JSON
    --quiet                     suppress progress
    --exit-zero                 always exit 0
    -h, --help                  show this help

EXIT CODES: 0 clean (all requested stages completed, no findings),
            1 findings, 2 configuration/toolchain error,
            3 analysis incomplete (a requested stage did not finish)",
        VERSION
    );
}

fn take_value(
    argv: &[String],
    index: &mut usize,
    inline: Option<String>,
    name: &str,
) -> Result<String, String> {
    if let Some(value) = inline {
        return Ok(value);
    }
    *index += 1;
    if *index >= argv.len() {
        return Err(format!("{} requires a value", name));
    }
    Ok(argv[*index].clone())
}

fn parse_args() -> Result<Args, String> {
    let argv: Vec<String> = env::args().skip(1).collect();
    let mut args = Args::default();
    let mut index = 0;

    while index < argv.len() {
        let arg = argv[index].clone();
        if arg == "-h" || arg == "--help" {
            usage();
            std::process::exit(0);
        }
        let (key, inline) = match arg.split_once('=') {
            Some((key, value)) => (key.to_string(), Some(value.to_string())),
            None => (arg.clone(), None),
        };

        match key.as_str() {
            "--cc" => args.cc = Some(take_value(&argv, &mut index, inline, "--cc")?),
            "--cxx" => args.cxx = Some(take_value(&argv, &mut index, inline, "--cxx")?),
            "--std-c" => args.std_c = take_value(&argv, &mut index, inline, "--std-c")?,
            "--std-cxx" => args.std_cxx = take_value(&argv, &mut index, inline, "--std-cxx")?,
            "-I" | "--include" => {
                args.includes.push(take_value(&argv, &mut index, inline, "--include")?)
            }
            "-D" | "--define" => {
                args.defines.push(take_value(&argv, &mut index, inline, "--define")?)
            }
            "--cflag" => args.cflags.push(take_value(&argv, &mut index, inline, "--cflag")?),
            "--compile-commands" => {
                args.compile_commands = Some(take_value(&argv, &mut index, inline, "--compile-commands")?)
            }
            "--jobs" => {
                args.jobs = take_value(&argv, &mut index, inline, "--jobs")?
                    .parse()
                    .map_err(|_| "--jobs expects a number".to_string())?
            }
            "--timeout-compile" => {
                args.timeout_compile = take_value(&argv, &mut index, inline, "--timeout-compile")?
                    .parse()
                    .map_err(|_| "--timeout-compile expects seconds".to_string())?
            }
            "--timeout-run" => {
                args.timeout_run = take_value(&argv, &mut index, inline, "--timeout-run")?
                    .parse()
                    .map_err(|_| "--timeout-run expects seconds".to_string())?
            }
            "--timeout-cppcheck" => {
                args.timeout_cppcheck = take_value(&argv, &mut index, inline, "--timeout-cppcheck")?
                    .parse()
                    .map_err(|_| "--timeout-cppcheck expects seconds".to_string())?
            }
            "--cppcheck-level" => {
                let value = take_value(&argv, &mut index, inline, "--cppcheck-level")?;
                if value != "normal" && value != "exhaustive" {
                    return Err("--cppcheck-level expects normal or exhaustive".to_string());
                }
                args.cppcheck_level = value;
            }
            "--timeout-analyzer" => {
                args.timeout_analyzer = take_value(&argv, &mut index, inline, "--timeout-analyzer")?
                    .parse()
                    .map_err(|_| "--timeout-analyzer expects seconds".to_string())?
            }
            "--max-files" => {
                args.max_files = take_value(&argv, &mut index, inline, "--max-files")?
                    .parse()
                    .map_err(|_| "--max-files expects a number".to_string())?
            }
            "--exclude" => args.excludes.push(take_value(&argv, &mut index, inline, "--exclude")?),
            "--skip-compile" | "--no-compile" => args.no_compile = true,
            "--no-cppcheck" => args.no_cppcheck = true,
            "--no-analyzer" => args.no_analyzer = true,
            "--no-runtime" => args.no_runtime = true,
            "--binary" => args.binary = Some(take_value(&argv, &mut index, inline, "--binary")?),
            "--build-cmd" => args.build_cmd = Some(take_value(&argv, &mut index, inline, "--build-cmd")?),
            "--run-args" => args.run_args = take_value(&argv, &mut index, inline, "--run-args")?,
            "--output" => args.output = take_value(&argv, &mut index, inline, "--output")?,
            "--json" => args.json_out = Some(take_value(&argv, &mut index, inline, "--json")?),
            "--quiet" => args.quiet = true,
            "--exit-zero" => args.exit_zero = true,
            _ if key.starts_with("-I") && key.len() > 2 => args.includes.push(key[2..].to_string()),
            _ if key.starts_with("-D") && key.len() > 2 => args.defines.push(key[2..].to_string()),
            _ if key.starts_with('-') => return Err(format!("unknown option: {}", key)),
            _ => args.targets.push(arg),
        }
        index += 1;
    }
    Ok(args)
}

fn expand_tilde(path: &str) -> PathBuf {
    if path == "~" || path.starts_with("~/") {
        if let Some(home) = env::var_os("HOME") {
            let mut full = PathBuf::from(home);
            if path.len() > 2 {
                full.push(&path[2..]);
            }
            return full;
        }
    }
    PathBuf::from(path)
}

fn main() {
    let args = match parse_args() {
        Ok(args) => args,
        Err(message) => {
            eprintln!("cqma: {}", message);
            std::process::exit(2);
        }
    };
    if args.targets.is_empty() {
        usage();
        std::process::exit(2);
    }

    let started = Instant::now();
    let targets: Vec<PathBuf> = args.targets.iter().map(|t| expand_tilde(t)).collect();
    if let Some(missing) = targets.iter().find(|t| !t.exists()) {
        eprintln!("cqma: target not found: {}", missing.display());
        std::process::exit(2);
    }

    let cc = args
        .cc
        .clone()
        .or_else(|| which("clang").map(|p| p.display().to_string()))
        .or_else(|| which("gcc").map(|p| p.display().to_string()))
        .or_else(|| which("cc").map(|p| p.display().to_string()));
    let cxx = args
        .cxx
        .clone()
        .or_else(|| which("clang++").map(|p| p.display().to_string()))
        .or_else(|| which("g++").map(|p| p.display().to_string()))
        .or_else(|| which("c++").map(|p| p.display().to_string()));
    let cc = match cc {
        Some(cc) => cc,
        None => {
            eprintln!("cqma: no C compiler found (install clang or gcc)");
            std::process::exit(2);
        }
    };
    let cxx = cxx.unwrap_or_else(|| cc.clone());

    let (files, skipped, root) = discover(&targets, &args.excludes, args.max_files);
    if files.is_empty() {
        eprintln!("cqma: no C/C++ sources found in the given targets");
        std::process::exit(2);
    }
    let loc = count_loc(&files);

    let (cc_commands, db_includes, db_defines) =
        load_compile_commands(&root, args.compile_commands.as_deref());

    let mut includes = args.includes.clone();
    includes.extend(db_includes);
    // A project's own header layout is a fact about the project, not a user
    // preference: every stage needs it. Without this the compiler stage cannot
    // resolve `#include "flint_blas.h"` from tests/ and reports a phantom
    // fatal error for files that build fine.
    for sub in ["include", "src", "runtime"] {
        let candidate = root.join(sub);
        if candidate.is_dir() {
            includes.push(candidate.display().to_string());
        }
    }
    // C extensions that embed a Python runtime need Python's headers, which
    // live outside the project. Ask the interpreter instead of guessing.
    if let Some(flags) = python_include_dirs() {
        includes.extend(flags);
    }
    includes.sort();
    includes.dedup();
    let mut defines = args.defines.clone();
    defines.extend(db_defines);
    defines.sort();
    defines.dedup();

    let run_binary = args.binary.as_ref().map(|path| {
        let expanded = expand_tilde(path);
        if expanded.is_absolute() {
            expanded
        } else {
            env::current_dir().unwrap_or_else(|_| PathBuf::from(".")).join(expanded)
        }
    });
    if let Some(binary) = &run_binary {
        // With --build-cmd the binary does not exist yet: cqma is about to
        // build it. Validating here made the documented
        // `--build-cmd "make" --binary ./app` invocation fail before the
        // build ever ran.
        if args.build_cmd.is_none() && !binary.is_file() {
            eprintln!("cqma: --binary not found: {}", binary.display());
            std::process::exit(2);
        }
    }

    let mut cfg = Config {
        root: fs::canonicalize(&root).unwrap_or(root.clone()),
        files,
        cc,
        cxx,
        std_c: args.std_c.clone(),
        std_cxx: args.std_cxx.clone(),
        includes,
        defines,
        cflags: args.cflags.clone(),
        jobs: args.jobs.max(1),
        timeout_compile: args.timeout_compile,
        timeout_run: args.timeout_run,
        timeout_cppcheck: args.timeout_cppcheck,
        cppcheck_level: args.cppcheck_level.clone(),
        timeout_analyzer: args.timeout_analyzer,
        cc_commands,
        run_binary,
        run_args: shell_split(&args.run_args),
        run_cwd: root.clone(),
        build_cmd: args.build_cmd.clone(),
        sanitizer_ok: false,
        leaks_ok: false,
        sanitizer_note: String::new(),
        loc,
        skipped_files: skipped,
    };

    let probe_dir = make_temp("cqma-probe");
    let (sanitizer_ok, leaks_ok, sanitizer_note) = probe_sanitizers(&cfg, &probe_dir);
    cfg.sanitizer_ok = sanitizer_ok;
    cfg.leaks_ok = leaks_ok;
    cfg.sanitizer_note = sanitizer_note;
    let _ = fs::remove_dir_all(&probe_dir);

    if !args.quiet {
        eprintln!(
            "cqma: {} file(s), {} lines, root {}",
            cfg.files.len(),
            cfg.loc,
            cfg.root.display()
        );
        eprintln!(
            "cqma: sanitizers={}, leaks={}",
            if cfg.sanitizer_ok { "yes" } else { "no" },
            if cfg.leaks_ok { "yes" } else { "no" }
        );
    }

    let mut findings: Vec<Finding> = Vec::new();
    let mut stages: Vec<Stage> = Vec::new();

    if !args.no_compile {
        if !args.quiet {
            eprintln!("cqma: [1/4] compiler warnings ...");
        }
        let (result, stage) = stage_compiler(&cfg);
        findings.extend(result);
        stages.push(stage);
    }
    if !args.no_cppcheck {
        if !args.quiet {
            eprintln!("cqma: [2/4] cppcheck ...");
        }
        let (result, stage) = stage_cppcheck(&cfg);
        findings.extend(result);
        stages.push(stage);
    }
    if !args.no_analyzer {
        if !args.quiet {
            eprintln!("cqma: [3/4] clang static analyzer ...");
        }
        let (result, stage) = stage_analyzer(&cfg);
        findings.extend(result);
        stages.push(stage);
    }
    if !args.no_runtime {
        if !args.quiet {
            eprintln!("cqma: [4/4] runtime ...");
        }
        let (result, stage) = stage_runtime(&cfg);
        findings.extend(result);
        stages.push(stage);
    }

    let mut findings = dedupe(findings);
    findings.sort_by(|a, b| {
        let rank = |f: &Finding| match f.severity.as_str() {
            "error" => 0,
            "warning" => 1,
            "performance" => 2,
            "portability" => 3,
            "style" => 4,
            _ => 5,
        };
        rank(a)
            .cmp(&rank(b))
            .then_with(|| a.file.cmp(&b.file))
            .then_with(|| a.line.cmp(&b.line))
    });

    let elapsed = started.elapsed().as_secs_f64();
    let gaps = coverage_gaps(&cfg, &findings, &stages);
    let complete = gaps.is_empty();
    let score = compute_score(&findings, cfg.loc, complete);
    let confirmed = findings.iter().filter(|f| !f.operational).count();
    let exit_code = if !complete {
        3
    } else if findings.is_empty() {
        0
    } else {
        1
    };
    let report = build_report(&cfg, &findings, &stages, elapsed);

    let output = expand_tilde(&args.output);
    if let Some(parent) = output.parent() {
        if !parent.as_os_str().is_empty() {
            let _ = fs::create_dir_all(parent);
        }
    }
    if let Err(err) = fs::write(&output, &report) {
        eprintln!("cqma: cannot write report {}: {}", output.display(), err);
        std::process::exit(2);
    }

    if let Some(json_path) = &args.json_out {
        let payload = json!({
            "version": VERSION,
            "target": cfg.root.display().to_string(),
            "files": cfg.files.len(),
            "lines": cfg.loc,
            "complete": complete,
            "coverage_gaps": gaps,
            "score": score.value,
            "grade": score.grade,
            "exit_code": exit_code,
            "sanitizers": cfg.sanitizer_ok,
            "leak_detection": cfg.leaks_ok,
            "stages": stages.iter().map(|stage| json!({
                "name": stage.name,
                "status": stage.status,
                "note": stage.note,
                "seconds": stage.seconds,
                "command": stage.command,
                "files_total": stage.files_total,
                "files_failed": stage.files_failed,
                "truncated": stage.truncated,
                "coverage_note": stage.coverage_note,
            })).collect::<Vec<_>>(),
            "findings": findings.iter().map(|finding| json!({
                "tool": finding.tool,
                "severity": finding.severity,
                "check": finding.check,
                "message": finding.message,
                "file": finding.file,
                "line": finding.line,
                "column": finding.column,
                "detail": finding.detail,
                "weight": finding.weight,
                "operational": finding.operational,
            })).collect::<Vec<_>>(),
        });
        let json_path = expand_tilde(json_path);
        if let Some(parent) = json_path.parent() {
            if !parent.as_os_str().is_empty() {
                let _ = fs::create_dir_all(parent);
            }
        }
        match serde_json::to_string_pretty(&payload) {
            Ok(text) => {
                if let Err(err) = fs::write(&json_path, text) {
                    eprintln!("cqma: cannot write JSON {}: {}", json_path.display(), err);
                }
            }
            Err(err) => eprintln!("cqma: cannot serialize JSON: {}", err),
        }
    }

    if !args.quiet {
        let missing = unanalyzed_files(&findings).len();
        if !complete {
            eprintln!(
                "cqma: {} finding(s) ({} confirmed), coverage incomplete ({}); score unavailable; exit {}",
                findings.len(),
                confirmed,
                gaps.join("; "),
                exit_code
            );
        } else if confirmed == 0 {
            eprintln!(
                "cqma: 0 confirmed finding(s), {} operational warning(s), score 100/100 (CLEAN)",
                findings.len()
            );
        } else if missing > 0 {
            eprintln!(
                "cqma: {} confirmed finding(s), {} file(s) NOT analyzed, score {}/100 ({})",
                confirmed,
                missing,
                score.value.unwrap_or(0),
                score.grade
            );
        } else {
            eprintln!(
                "cqma: {} confirmed finding(s), score {}/100 ({})",
                confirmed,
                score.value.unwrap_or(0),
                score.grade
            );
        }
        eprintln!("cqma: report -> {}", output.display());
    }

    if args.exit_zero {
        return;
    }
    std::process::exit(exit_code);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_config() -> Config {
        Config {
            root: PathBuf::from("."),
            files: vec![PathBuf::from("a.c")],
            cc: "clang".into(),
            cxx: "clang++".into(),
            std_c: "c11".into(),
            std_cxx: "c++17".into(),
            includes: Vec::new(),
            defines: Vec::new(),
            cflags: Vec::new(),
            jobs: 1,
            timeout_compile: 10,
            timeout_run: 10,
            timeout_cppcheck: 10,
            cppcheck_level: "normal".into(),
            timeout_analyzer: 10,
            cc_commands: HashMap::new(),
            run_binary: None,
            run_args: Vec::new(),
            run_cwd: PathBuf::from("."),
            build_cmd: None,
            sanitizer_ok: true,
            leaks_ok: true,
            sanitizer_note: String::new(),
            loc: 1000,
            skipped_files: Vec::new(),
        }
    }

    #[test]
    fn all_stages_disabled_is_a_coverage_gap() {
        let cfg = test_config();
        let gaps = coverage_gaps(&cfg, &[], &[]);
        assert!(gaps.iter().any(|g| g.contains("all disabled")), "{:?}", gaps);
        assert_eq!(compute_score(&[], cfg.loc, false).value, None);
    }

    #[test]
    fn skipped_files_are_a_coverage_gap() {
        let mut cfg = test_config();
        cfg.skipped_files = vec!["b.c".into(), "c.c".into()];
        let gaps = coverage_gaps(&cfg, &[], &[Stage::new("compiler warnings")]);
        assert!(gaps.iter().any(|g| g.contains("2 input file(s)")), "{:?}", gaps);
    }

    #[test]
    fn failed_stage_is_a_coverage_gap() {
        let cfg = test_config();
        let mut stage = Stage::new("clang static analyzer");
        stage.status = "failed".into();
        stage.note = "1 of 1 file(s) could not be analyzed".into();
        stage.coverage_note = "could not be analyzed".into();
        stage.files_total = 1;
        stage.files_failed = 1;
        let gaps = coverage_gaps(&cfg, &[], &[stage]);
        assert!(gaps.iter().any(|g| g.contains("did not complete")), "{:?}", gaps);
        assert_eq!(
            gaps.iter().filter(|g| g.contains("could not be analyzed")).count(),
            1,
            "the stage note and coverage note must not duplicate: {:?}",
            gaps
        );
    }

    #[test]
    fn a_coverage_note_alone_is_a_gap() {
        let cfg = test_config();
        let mut stage = Stage::new("runtime memory analysis");
        stage.status = "ran".into();
        stage.coverage_note =
            "valgrind reported errors whose detail was not fully parsed".into();
        let gaps = coverage_gaps(&cfg, &[], &[stage]);
        assert_eq!(gaps.len(), 1, "{:?}", gaps);
        assert!(gaps[0].contains("not fully parsed"), "{:?}", gaps);
    }

    #[test]
    fn operational_warnings_do_not_lower_the_score() {
        let finding = operational_finding(
            "asan",
            "timeout",
            "program exceeded 60s".into(),
            "a.c".into(),
        );
        let score = compute_score(&[finding], 1000, true);
        assert_eq!(score.value, Some(100));
        assert_eq!(score.grade, "CLEAN");
        assert_eq!(score.weighted, 0);
    }

    #[test]
    fn a_confirmed_warning_cannot_score_100() {
        let finding = Finding {
            tool: "clang-analyzer".into(),
            severity: "warning".into(),
            check: "unix.Malloc".into(),
            message: "Potential memory leak".into(),
            file: "big.cpp".into(),
            ..Default::default()
        };
        // 3 weighted points in a 16K-line file would round to 100; a perfect
        // score is reserved for zero confirmed findings.
        let score = compute_score(&[finding], 16232, true);
        assert_eq!(score.value, Some(99));
        assert_eq!(score.grade, "A");
    }

    #[test]
    fn incomplete_analysis_has_no_score() {
        let finding = Finding {
            tool: "compiler".into(),
            severity: "error".into(),
            check: "compile".into(),
            message: "boom".into(),
            file: "a.c".into(),
            ..Default::default()
        };
        let score = compute_score(&[finding], 1000, false);
        assert_eq!(score.value, None);
        assert_eq!(score.grade, "INCOMPLETE");
    }

    #[test]
    fn same_size_leaks_at_different_sites_stay_separate() {
        let cfg = test_config();
        let text = "\
summary live=2 live_bytes=128 reachable=0 reachable_bytes=0 unreachable=2 unreachable_bytes=128 tracked=2
LEAK|64|1|/tmp/a.c:10 leaky; /tmp/a.c:5 main
LEAK|64|1|/tmp/b.c:20 leaky2; /tmp/b.c:5 main
";
        let (findings, _summary) = parse_leak_tracker(text, &cfg);
        let deduped = dedupe(findings);
        assert_eq!(deduped.len(), 2, "{:#?}", deduped);
        assert_eq!(deduped[0].file, "/tmp/a.c");
        assert_eq!(deduped[1].file, "/tmp/b.c");
    }

    #[test]
    fn dedupe_key_includes_the_allocation_stack() {
        let base = Finding {
            tool: "leak-tracker".into(),
            severity: "error".into(),
            check: "leak".into(),
            message: "64 byte(s) in 1 allocation(s)".into(),
            file: "a.c".into(),
            line: Some(10),
            weight: 8,
            ..Default::default()
        };
        let mut other = base.clone();
        other.detail = "other_stack".into();
        assert_eq!(dedupe(vec![base, other]).len(), 2);
    }

    #[test]
    fn read_pipe_caps_retained_output() {
        struct Big {
            remaining: usize,
        }
        impl Read for Big {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                if self.remaining == 0 {
                    return Ok(0);
                }
                let n = buf.len().min(self.remaining);
                for byte in &mut buf[..n] {
                    *byte = b'x';
                }
                self.remaining -= n;
                Ok(n)
            }
        }
        let reader = Big { remaining: MAX_PIPE_BYTES + 1024 };
        let (text, truncated) = read_pipe(reader)
            .recv_timeout(Duration::from_secs(30))
            .expect("pipe thread");
        assert!(truncated);
        assert!(text.contains("output truncated"));
        assert!(text.len() <= MAX_PIPE_BYTES + 64);
    }
}
