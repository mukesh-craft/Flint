//! flint-lsp: the Flint language server (LSP 3.17 over stdio).
//!
//! Design decisions worth stating once:
//!   * **Diagnostics come from the real compiler**, never from a re-implemented
//!     parser. A language server that guesses is a second source of truth, and
//!     two sources of truth drift.
//!   * **Debounce, don't throttle.** Every keystroke is saved immediately (so
//!     features feel instant) while compilation is coalesced on a worker
//!     thread, because `flintc` costs far more than the editor frame budget.
//!   * **stdout is the protocol channel and nothing else.** Logs go to a file.
//!     One stray `println!` corrupts the stream and the client hangs.

mod analyzer;
mod builtins;
mod diagnostics;
mod protocol;

use protocol::{codes, error_response, notification, read_message, response, write_message};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{self, BufReader, Write};
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

/// Debounce window for compilation. 300 ms is under the threshold where a
/// typist perceives lag, and long enough to avoid recompiling per keystroke.
const DEBOUNCE: Duration = Duration::from_millis(300);

struct Doc {
    text: String,
    /// Monotonic per-document version; used to drop stale diagnostics.
    generation: u64,
}

#[derive(Clone)]
struct ServerState {
    docs: Arc<Mutex<HashMap<String, Doc>>>,
    flintc: PathBuf,
    next_generation: Arc<Mutex<u64>>,
    diag_tx: Sender<CompileJob>,
}

struct CompileJob {
    uri: String,
    text: String,
    generation: u64,
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    // A client may pass the compiler path so that the extension can point at a
    // sibling build; otherwise discover it next to the server or on PATH.
    let flintc = resolve_flintc(&args);

    let (diag_tx, diag_rx) = mpsc::channel::<CompileJob>();
    let state = ServerState {
        docs: Arc::new(Mutex::new(HashMap::new())),
        flintc,
        next_generation: Arc::new(Mutex::new(0)),
        diag_tx,
    };

    // Worker: coalesces bursts of edits, then publishes diagnostics.
    {
        let state = state.clone();
        thread::spawn(move || diagnostics_worker(state, diag_rx));
    }

    let stdin = io::stdin();
    let mut reader = BufReader::new(stdin.lock());
    let mut stdout = io::stdout();

    loop {
        match read_message(&mut reader) {
            Ok(None) => break, // client closed the pipe: shut down cleanly
            Ok(Some(msg)) => {
                if let Some(reply) = handle(&state, &msg) {
                    if write_message(&mut stdout, &reply).is_err() {
                        break;
                    }
                }
                // A reply that cannot be written means the client is gone.
                if let Some(method) = msg.get("method").and_then(|m| m.as_str()) {
                    if method == "exit" {
                        break;
                    }
                }
            }
            Err(e) => {
                // Framing/JSON error: report per spec and keep the session.
                // Silently ignoring a bad message strands the client waiting.
                log(&format!("protocol error: {e}"));
                let err = error_response(&Value::Null, codes::PARSE_ERROR, &e.to_string());
                if write_message(&mut stdout, &err).is_err() {
                    break;
                }
            }
        }
    }
}

fn resolve_flintc(args: &[String]) -> PathBuf {
    if let Some(i) = args.iter().position(|a| a == "--flintc") {
        if let Some(p) = args.get(i + 1) {
            return PathBuf::from(p);
        }
    }
    if let Ok(p) = std::env::var("FLINT_LSP_FLINTC") {
        if !p.is_empty() {
            return PathBuf::from(p);
        }
    }
    // Sibling layout: <prefix>/bin/flint-lsp next to <prefix>/bin/flintc.
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let candidate = dir.join("flintc");
            if candidate.exists() {
                return candidate;
            }
        }
    }
    PathBuf::from("flintc")
}

/// Routes one message. Returns a reply for requests, `None` for
/// notifications and for methods that reply some other way.
fn handle(state: &ServerState, msg: &Value) -> Option<Value> {
    let method = msg.get("method").and_then(|m| m.as_str()).unwrap_or("");
    let id = msg.get("id").cloned();
    let params = msg.get("params").cloned().unwrap_or(Value::Null);

    // Notifications have no id and must never get a response.
    let is_request = id.is_some();
    let reply = match method {
        "initialize" => Some(response(
            id.as_ref().unwrap(),
            json!({
                "capabilities": {
                    "textDocumentSync": {"openClose": true, "change": 1},
                    "completionProvider": {"triggerCharacters": [".", ":", ">", "|"]},
                    "hoverProvider": true,
                    "definitionProvider": true,
                    "referencesProvider": true,
                    "documentSymbolProvider": true
                },
                "serverInfo": {"name": "flint-lsp-rs", "version": env!("CARGO_PKG_VERSION")}
            }),
        )),
        "shutdown" => Some(response(id.as_ref().unwrap(), Value::Null)),
        "initialized" => None,
        "exit" => None,
        "textDocument/didOpen" => {
            if let Some((uri, text)) = open_params(&params) {
                store(state, &uri, text);
                schedule(state, &uri);
            }
            None
        }
        "textDocument/didChange" => {
            if let Some((uri, text)) = change_params(&params) {
                store(state, &uri, text);
                schedule(state, &uri);
            }
            None
        }
        "textDocument/didClose" => {
            if let Some(uri) = document_uri(&params) {
                state.docs.lock().unwrap().remove(&uri);
            }
            None
        }
        "textDocument/completion" => {
            let (_, text) = doc_of(state, &params);
            Some(response(
                id.as_ref().unwrap(),
                json!({"isIncomplete": false, "items": analyzer::completion_items(&text)}),
            ))
        }
        "textDocument/hover" => {
            let (_, text) = doc_of(state, &params);
            let (line, ch) = position(&params);
            let result = analyzer::hover_for(&text, line, ch).unwrap_or(Value::Null);
            Some(response(id.as_ref().unwrap(), result))
        }
        "textDocument/definition" => {
            let (uri, text) = doc_of(state, &params);
            let (line, ch) = position(&params);
            let result = analyzer::definition_for(&text, &uri, line, ch).unwrap_or(Value::Null);
            Some(response(id.as_ref().unwrap(), result))
        }
        "textDocument/references" => {
            let (uri, text) = doc_of(state, &params);
            let (line, ch) = position(&params);
            Some(response(
                id.as_ref().unwrap(),
                json!(analyzer::references_for(&text, &uri, line, ch)),
            ))
        }
        "textDocument/documentSymbol" => {
            let (uri, text) = doc_of(state, &params);
            Some(response(id.as_ref().unwrap(), json!(analyzer::document_symbols(&text, &uri))))
        }
        // Unknown method: answer per spec so the client shows an error instead
        // of waiting forever. This is also what the gate asserts.
        _ => {
            if is_request {
                Some(error_response(
                    id.as_ref().unwrap(),
                    codes::METHOD_NOT_FOUND,
                    &format!("method not found: {method}"),
                ))
            } else {
                None
            }
        }
    };
    reply
}

fn store(state: &ServerState, uri: &str, text: String) {
    let mut gen = state.next_generation.lock().unwrap();
    *gen += 1;
    let generation = *gen;
    state.docs.lock().unwrap().insert(
        uri.to_string(),
        Doc {
            text,
            generation,
        },
    );
}

fn schedule(state: &ServerState, uri: &str) {
    if let Some(doc) = state.docs.lock().unwrap().get(uri) {
        let job = CompileJob {
            uri: uri.to_string(),
            text: doc.text.clone(),
            generation: doc.generation,
        };
        let _ = state.diag_tx.send(job);
    }
}

/// Coalescing compile worker. Sleeps out the debounce window, drops the job if
/// a newer one already arrived, then publishes.
fn diagnostics_worker(state: ServerState, rx: Receiver<CompileJob>) {
    let stdout = io::stdout();
    loop {
        let Ok(job) = rx.recv() else { break };
        thread::sleep(DEBOUNCE);
        // Drain: only the newest generation for a URI is worth compiling.
        let mut pending: Option<CompileJob> = Some(job);
        while let Ok(next) = rx.try_recv() {
            let keep_newest = match (&pending, &next) {
                (Some(cur), _) => next.generation >= cur.generation,
                (None, _) => true,
            };
            if keep_newest {
                pending = Some(next);
            }
        }
        let Some(job) = pending else { continue };

        // If a newer edit landed while we slept, this result is stale.
        let current = state.docs.lock().unwrap().get(&job.uri).map(|d| d.generation);
        if current != Some(job.generation) {
            continue;
        }

        let diags = diagnostics::check(&state.flintc, &job.text);
        let payload: Vec<Value> = diags.iter().map(|d| d.to_json()).collect();
        let msg = notification("textDocument/publishDiagnostics", json!({"uri": job.uri, "diagnostics": payload}));
        let mut out = stdout.lock();
        if write_message(&mut out, &msg).is_err() {
            break;
        }
    }
}

fn document_uri(params: &Value) -> Option<String> {
    params
        .get("textDocument")?
        .get("uri")?
        .as_str()
        .map(|s| s.to_string())
}

fn open_params(params: &Value) -> Option<(String, String)> {
    let uri = document_uri(params)?;
    let text = params.get("textDocument")?.get("text")?.as_str()?.to_string();
    Some((uri, text))
}

fn change_params(params: &Value) -> Option<(String, String)> {
    let uri = document_uri(params)?;
    let changes = params.get("contentChanges")?.as_array()?;
    // Full sync (capability `change: 1`): the last change carries the whole
    // document. An empty array means the client sent nothing to apply.
    let last = changes.last()?;
    let text = last.get("text")?.as_str()?.to_string();
    Some((uri, text))
}

fn doc_of(state: &ServerState, params: &Value) -> (String, String) {
    let uri = document_uri(params).unwrap_or_default();
    let text = state
        .docs
        .lock()
        .unwrap()
        .get(&uri)
        .map(|d| d.text.clone())
        .unwrap_or_default();
    (uri, text)
}

fn position(params: &Value) -> (usize, usize) {
    let p = params.get("position");
    let line = p
        .and_then(|p| p.get("line"))
        .and_then(|l| l.as_u64())
        .unwrap_or(0) as usize;
    let ch = p
        .and_then(|p| p.get("character"))
        .and_then(|c| c.as_u64())
        .unwrap_or(0) as usize;
    (line, ch)
}

/// Logging goes to a file, never to stdout (stdout is the protocol channel).
fn log(msg: &str) {
    let path = diagnostics::scratch_dir().join("flint-lsp.log");
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "[{}] {msg}", chrono_now());
    }
}

fn chrono_now() -> u128 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_state() -> ServerState {
        let (tx, rx) = mpsc::channel();
        // Keep the receiver alive for the lifetime of the state.
        std::mem::forget(rx);
        ServerState {
            docs: Arc::new(Mutex::new(HashMap::new())),
            flintc: PathBuf::from("flintc"),
            next_generation: Arc::new(Mutex::new(0)),
            diag_tx: tx,
        }
    }

    #[test]
    fn initialize_advertises_capabilities() {
        let state = test_state();
        let reply = handle(&state, &json!({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})).unwrap();
        let caps = &reply["result"]["capabilities"];
        assert_eq!(caps["hoverProvider"], true);
        assert_eq!(caps["definitionProvider"], true);
        assert_eq!(caps["referencesProvider"], true);
        assert_eq!(caps["documentSymbolProvider"], true);
        assert_eq!(caps["textDocumentSync"]["change"], 1);
        assert_eq!(reply["result"]["serverInfo"]["name"], "flint-lsp-rs");
    }

    #[test]
    fn unknown_method_returns_method_not_found() {
        let state = test_state();
        let reply = handle(&state, &json!({"jsonrpc":"2.0","id":7,"method":"textDocument/rename","params":{}})).unwrap();
        assert_eq!(reply["error"]["code"], codes::METHOD_NOT_FOUND);
        assert!(reply["error"]["message"].as_str().unwrap().contains("rename"));
    }

    #[test]
    fn notifications_get_no_reply() {
        let state = test_state();
        let reply = handle(&state, &json!({"jsonrpc":"2.0","method":"initialized","params":{}}));
        assert!(reply.is_none(), "notifications must not be answered");
    }

    #[test]
    fn did_open_stores_document_and_completion_sees_it() {
        let state = test_state();
        handle(
            &state,
            &json!({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{
                "textDocument": {"uri": "file:///t.fl", "text": "fn helper() -> i64 {\n  return 1\n}\nfn main() {\n  helper()\n}\n"}
            }}),
        );
        let reply = handle(&state, &json!({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{
            "textDocument": {"uri": "file:///t.fl"}, "position": {"line": 4, "character": 2}
        }})).unwrap();
        let items = reply["result"]["items"].as_array().unwrap();
        assert!(items.iter().any(|i| i["label"] == "helper"), "user fn must be completed");
        assert!(items.iter().any(|i| i["label"] == "continue"), "keyword table must be complete");
        assert!(items.iter().any(|i| i["label"] == "flint_vec_new"), "runtime builtins must be completed");
    }

    #[test]
    fn definition_and_references_find_user_symbols() {
        let state = test_state();
        let uri = "file:///d.fl";
        let text = "fn twice(x: i64) -> i64 {\n  return x * 2\n}\nfn main() {\n  print(twice(21))\n}\n";
        state.docs.lock().unwrap().insert(uri.to_string(), Doc { text: text.into(), generation: 1 });
        let def = handle(&state, &json!({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{
            "textDocument": {"uri": uri}, "position": {"line": 4, "character": 9}
        }})).unwrap();
        assert_eq!(def["result"][0]["range"]["start"]["line"], 0, "definition points at `fn twice`");

        let refs = handle(&state, &json!({"jsonrpc":"2.0","id":4,"method":"textDocument/references","params":{
            "textDocument": {"uri": uri}, "position": {"line": 4, "character": 9}
        }})).unwrap();
        assert_eq!(refs["result"].as_array().unwrap().len(), 2, "declaration + one use");
    }

    #[test]
    fn hover_includes_doc_comment_and_signature() {
        let state = test_state();
        let uri = "file:///h.fl";
        let text = "# Adds two numbers.\nfn add(a: i64, b: i64) -> i64 {\n  return a + b\n}\nfn main() {\n  print(add(1, 2))\n}\n";
        state.docs.lock().unwrap().insert(uri.to_string(), Doc { text: text.into(), generation: 1 });
        let hover = handle(&state, &json!({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{
            "textDocument": {"uri": uri}, "position": {"line": 5, "character": 10}
        }})).unwrap();
        let value = hover["result"]["contents"]["value"].as_str().unwrap();
        assert!(value.contains("Adds two numbers."), "doc comment must be in hover: {value}");
        assert!(value.contains("fn add(a: i64, b: i64)"), "signature must be in hover: {value}");
    }

    #[test]
    fn document_symbols_are_in_document_order() {
        let state = test_state();
        let uri = "file:///s.fl";
        let text = "fn alpha() -> i64 {\n  return 1\n}\nstruct Pair {\n  a: i64,\n}\nenum Color {\n  Red,\n}\nfn main() {\n  let x: i64 = 1\n}\n";
        state.docs.lock().unwrap().insert(uri.to_string(), Doc { text: text.into(), generation: 1 });
        let syms = handle(&state, &json!({"jsonrpc":"2.0","id":6,"method":"textDocument/documentSymbol","params":{
            "textDocument": {"uri": uri}
        }})).unwrap();
        let arr = syms["result"].as_array().unwrap();
        let names: Vec<&str> = arr.iter().map(|s| s["name"].as_str().unwrap()).collect();
        assert_eq!(names, vec!["alpha", "Pair", "Color", "main", "x"]);
    }

    #[test]
    fn clean_and_broken_documents_diagnose_correctly() {
        let state = test_state();
        let uri = "file:///diag.fl";
        // The compiler lives one level up from the crate; a bare "flintc"
        // only resolves if the crate dir happens to be on PATH.
        let flintc = std::path::PathBuf::from("../flintc");
        assert!(flintc.exists(), "run this test from the lsp/ crate dir");

        let broken = "fn main() {\n  nope_undefined()\n}\n";
        state.docs.lock().unwrap().insert(uri.to_string(), Doc { text: broken.into(), generation: 1 });
        let diags = diagnostics::check(&flintc, broken);
        assert!(!diags.is_empty(), "undefined function must produce a diagnostic");

        let clean = "fn main() {\n  print(1)\n}\n";
        state.docs.lock().unwrap().insert(uri.to_string(), Doc { text: clean.into(), generation: 2 });
        let none = diagnostics::check(&flintc, clean);
        assert!(none.is_empty(), "clean program must produce no diagnostics: {none:?}");

        // A compiler that cannot be executed must be visible, not reported as
        // an all-clear: silently returning zero diagnostics is the worst
        // failure mode an editor can have.
        let missing = diagnostics::check(std::path::Path::new("./definitely-not-a-compiler"), clean);
        assert_eq!(missing.len(), 1);
        assert!(missing[0].message.contains("compiler unavailable"), "{}", missing[0].message);
    }
}