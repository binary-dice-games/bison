// MIT License © 2025 Binary Dice Games
//! Build script that locates and links against the precompiled `bison_abi`
//! shared library.
//!
//! Rust, like C++ (see `bindings/cpp/`), links against `bison_abi` at
//! *build* time rather than `dlopen`/P-Invoke-ing it at run time the way the
//! Python and C# bindings do. Resolution order mirrors every other
//! binding's `_find_library()` / `Native.ResolveLibrary()`:
//!
//!   1. `BISON_LIB` environment variable — a full path to
//!      `libbison_abi.{so,dylib}` / `bison_abi.dll`. Both the link-search
//!      directory and the library name are derived from it.
//!   2. A sibling `build/` directory next to `bindings/rust/` (the repo's
//!      own `cmake -B build` output), trying `Debug`/`Release`
//!      subdirectories on Windows.
//!   3. Ordinary system library search (`-lbison_abi`, no explicit `-L`).
//!
//! On Linux an rpath is also emitted for the resolved library directory (in
//! cases 1 and 2) so built binaries find the `.so` at run time without
//! requiring `LD_LIBRARY_PATH` — there is no install step for this binding.

use std::env;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=BISON_LIB");

    if let Ok(env_path) = env::var("BISON_LIB") {
        if !env_path.is_empty() {
            let path = PathBuf::from(&env_path);
            let dir = path
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_else(|| PathBuf::from("."));
            let lib_name = library_name_from_path(&path);

            println!("cargo:rustc-link-search=native={}", dir.display());
            println!("cargo:rustc-link-lib=dylib={lib_name}");
            emit_rpath(&dir);
            return;
        }
    }

    if let Some(dir) = find_build_dir() {
        println!("cargo:rustc-link-search=native={}", dir.display());
        println!("cargo:rustc-link-lib=dylib=bison_abi");
        emit_rpath(&dir);
        return;
    }

    // Fall back to the OS's normal library search path (no -L directive).
    println!("cargo:rustc-link-lib=dylib=bison_abi");
}

/// Extracts the link-time library name from a full path, stripping the
/// platform-specific `lib`/`.so`/`.dylib`/`.dll` decoration the way a
/// linker's `-l` flag expects (e.g. `/path/libbison_abi.so` -> `bison_abi`).
fn library_name_from_path(path: &Path) -> String {
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("bison_abi");
    stem.strip_prefix("lib").unwrap_or(stem).to_string()
}

/// Looks for `bison_abi`'s shared library in `<repo_root>/build/`, the
/// same conventional location `bison/_native.py`'s `_find_library()` and
/// C#'s `Native.ResolveLibrary()` check. `bindings/rust/build.rs` lives two
/// directories below the repo root.
fn find_build_dir() -> Option<PathBuf> {
    let here = env::var("CARGO_MANIFEST_DIR").ok()?;
    let repo_root = Path::new(&here).parent()?.parent()?;
    let build_dir = repo_root.join("build");

    let candidates = [
        build_dir.join("libbison_abi.so"),
        build_dir.join("libbison_abi.dylib"),
        build_dir.join("Release").join("bison_abi.dll"),
        build_dir.join("Debug").join("bison_abi.dll"),
    ];

    for candidate in &candidates {
        if candidate.is_file() {
            return Some(candidate.parent().unwrap().to_path_buf());
        }
    }
    None
}

fn emit_rpath(dir: &Path) {
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
    }
}
