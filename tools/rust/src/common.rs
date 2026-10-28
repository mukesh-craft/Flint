//! Shared helpers for the tool subcommands.
//!
//! The repository root is found by walking up from the current directory
//! rather than from argv[0], because these tools are invoked from gates that
//! run from the root anyway, and a relative binary path would make argv[0]
//! unreliable.

use std::path::{Path, PathBuf};

/// The repository root, marked by the files that must exist for this to be
/// the right directory.
pub fn repo_root() -> PathBuf {
    let cwd = std::env::current_dir().expect("cannot read the current directory");
    let mut dir: &Path = &cwd;
    loop {
        if dir.join("VERSION").is_file() && dir.join("src").join("main.cpp").is_file() {
            return dir.to_path_buf();
        }
        match dir.parent() {
            Some(parent) => dir = parent,
            None => panic!(
                "could not find the Flint repository root from {}",
                cwd.display()
            ),
        }
    }
}

pub fn read(path: &Path) -> Option<String> {
    std::fs::read_to_string(path).ok()
}