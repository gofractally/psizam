//! Record the actual fork source in the native artifact/cache identity.
//!
//! Cargo.lock provides an immutable git revision for release dependencies.
//! Local path builds instead require the caller's HEAD + source-state receipt.
//! An upstream package version alone must never identify the patched runtime.

use std::{env, fs, path::Path};

fn locked_wasmtime_source(lock: &str) -> Option<&str> {
    let mut source = None;
    for package in lock.split("[[package]]").skip(1) {
        if !package
            .lines()
            .any(|line| line.trim() == "name = \"wasmtime\"")
        {
            continue;
        }
        // More than one Wasmtime package would make the selected dependency
        // ambiguous. The shim deliberately pins one runtime version.
        if source.is_some() {
            return None;
        }
        let value = package.lines().find_map(|line| {
            line.trim()
                .strip_prefix("source = \"")
                .and_then(|v| v.strip_suffix('"'))
        })?;
        let (_, revision) = value.rsplit_once('#')?;
        if !value.starts_with("git+")
            || revision.len() != 40
            || !revision.bytes().all(|b| b.is_ascii_hexdigit())
        {
            return None;
        }
        source = Some(value);
    }
    source
}

fn main() {
    println!("cargo:rerun-if-env-changed=PSIZAM_WASMTIME_BUILD_ID");
    println!("cargo:rerun-if-changed=Cargo.lock");
    let explicit = env::var("PSIZAM_WASMTIME_BUILD_ID").ok();
    let lock = fs::read_to_string(
        Path::new(&env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("Cargo.lock"),
    )
    .expect("read the shim's locked Wasmtime dependency");
    let identity = explicit
        .as_deref()
        .or_else(|| locked_wasmtime_source(&lock))
        .expect(
            "Wasmtime must be a pinned git fork, or a local path build must set \
         PSIZAM_WASMTIME_BUILD_ID to the fork HEAD plus its source-state receipt",
        );
    assert!(
        !identity.is_empty() && identity.len() <= 4096 && !identity.chars().any(char::is_control),
        "PSIZAM_WASMTIME_BUILD_ID must be a nonempty single-line source identity"
    );
    println!("cargo:rustc-env=PSIZAM_WASMTIME_BUILD_ID={identity}");
}

#[cfg(test)]
mod tests {
    use super::locked_wasmtime_source;

    #[test]
    fn identity_is_the_pinned_runtime_source() {
        let source = "git+https://github.com/gofractally/wasmtime?rev=0123456789abcdef0123456789abcdef01234567#0123456789abcdef0123456789abcdef01234567";
        let lock = format!(
            "version = 4\n[[package]]\nname = \"other\"\nsource = \"ignored\"\n\
             [[package]]\nname = \"wasmtime\"\nversion = \"51.0.0\"\nsource = \"{source}\"\n"
        );
        assert_eq!(locked_wasmtime_source(&lock), Some(source));
    }

    #[test]
    fn versions_and_unpinned_or_ambiguous_sources_are_insufficient() {
        for source in [
            "registry+https://github.com/rust-lang/crates.io-index",
            "git+https://github.com/gofractally/wasmtime#main",
            "git+https://github.com/gofractally/wasmtime#0123456789abcdef0123456789abcdef0123456x",
        ] {
            let lock = format!("[[package]]\nname = \"wasmtime\"\nsource = \"{source}\"\n");
            assert_eq!(locked_wasmtime_source(&lock), None);
        }
        assert_eq!(
            locked_wasmtime_source("[[package]]\nname = \"wasmtime\"\n"),
            None
        );
        let package = "[[package]]\nname = \"wasmtime\"\nsource = \"git+https://example.com/fork#0123456789abcdef0123456789abcdef01234567\"\n";
        assert_eq!(locked_wasmtime_source(&format!("{package}{package}")), None);
    }
}
