use std::path::PathBuf;

/// Locate the glint C++ sources.
///
/// In-tree they live at the repository root; in the crate published to
/// crates.io they live in `vendor/` (cargo only packages files under the crate
/// root, so `../../../src` does not exist there). The repository copy wins when
/// present, so a development build never compiles a stale vendored snapshot.
fn resolve_sources() -> (PathBuf, PathBuf) {
    let repo = (
        PathBuf::from("../../../src"),
        PathBuf::from("../../../include"),
    );
    let vendored = (PathBuf::from("vendor/src"), PathBuf::from("vendor/include"));

    // Probe a known translation unit rather than just the directory: this crate
    // has its own `src/` (Rust), and cargo's publish-verification step unpacks
    // the package to `target/package/<pkg>-<ver>/`, from where `../../../src`
    // resolves back to that Rust `src/` and would otherwise look like a hit.
    if repo.0.join("encoder.cpp").is_file() {
        return repo;
    }
    if vendored.0.join("encoder.cpp").is_file() {
        return vendored;
    }
    panic!(
        "glint C++ sources not found: expected ../../../src/encoder.cpp (in-tree \
         build) or vendor/src/encoder.cpp (packaged crate). Run \
         tools/vendor_rust_sources.sh before `cargo package`/`cargo publish`."
    );
}

fn main() {
    let (src, include) = resolve_sources();

    // Rebuild when any C++ source or header changes. cc's own rerun-if-changed
    // proved unreliable across the target volume, so watch the whole
    // src/include trees (cargo scans dirs recursively).
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", include.display());
    println!("cargo:rerun-if-changed=build.rs");

    // Every .cpp in the source dir belongs to the library, so discover them
    // rather than keeping a hand-maintained list in sync with a vendored copy.
    let mut files: Vec<PathBuf> = std::fs::read_dir(&src)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", src.display()))
        .filter_map(|entry| entry.ok().map(|e| e.path()))
        .filter(|p| p.extension().and_then(|s| s.to_str()) == Some("cpp"))
        .collect();
    files.sort(); // deterministic compile order
    assert!(!files.is_empty(), "no .cpp sources in {}", src.display());

    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .include(&include)
        .include(&src)
        .files(&files)
        .opt_level(2)
        .compile("glint");
}
