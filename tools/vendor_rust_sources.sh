#!/usr/bin/env bash
# Vendor the glint C++ sources into the glint-audio-sys crate.
#
# Why this exists: `cargo package` only archives files under the crate root, so
# the `../../../src` paths the in-tree build uses are unreachable from the
# published tarball. Without a vendored copy, `cargo publish` uploads a crate
# that cannot build for anyone. Run this before `cargo package`/`cargo publish`
# (the publish workflow does it automatically).
#
# The vendored tree is generated, not authored — it is gitignored, and build.rs
# prefers the repository copy whenever it is present so an in-tree build never
# compiles a stale snapshot.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
vendor="$repo_root/bindings/rust/glint-sys/vendor"

rm -rf "$vendor"
mkdir -p "$vendor/src" "$vendor/include"

# build.rs compiles every .cpp in the source dir, so copy the whole set plus
# the headers they include. src/ is flat (no subdirectories).
cp "$repo_root"/src/*.cpp "$repo_root"/src/*.hpp "$vendor/src/"
cp -R "$repo_root"/include/. "$vendor/include/"

cpp=$(find "$vendor/src" -name '*.cpp' | wc -l | tr -d ' ')
hpp=$(find "$vendor/src" -name '*.hpp' | wc -l | tr -d ' ')
echo "vendored $cpp .cpp + $hpp .hpp + include/ -> ${vendor#"$repo_root"/}"

# Guard against a partial copy shipping a crate that cannot build.
if [ ! -f "$vendor/src/encoder.cpp" ] || [ ! -f "$vendor/include/glint/glint.h" ]; then
    echo "error: vendored tree is incomplete" >&2
    exit 1
fi
