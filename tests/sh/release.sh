#!/bin/bash
# build/build.sh --pack <version> packs build/out (engine and test programs)
# for a GitHub Release and prints the manifest.txt line with its SHA-256; it
# refuses files that carry the builder's home folder path.
set -u
test_name=release  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"

make_tmp
r="$tmp/repo"
mkdir -p "$r"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$r"
o=$r/build/out
mkdir -p "$o/wswine.bundle/lib/wine/x86_64-unix" "$o/tests/bin"
echo nt > "$o/wswine.bundle/lib/wine/x86_64-unix/ntdll.so.fixed"
echo t > "$o/tests/bin/t.exe"

out=$("$r/build/build.sh" --pack 0.2.0 2>&1) || fail "--pack failed: $out"
archive=$r/build/plutonium-mac-aio-engine-0.2.0.tar.xz
assert_file "$archive"
sha=$(shasum -a 256 "$archive" | cut -d' ' -f1)
assert_contains "$out" "plutonium-mac-aio-engine.tar.xz https://github.com/LuidgiAlt/plutonium-mac-aio/releases/download/v0.2.0/plutonium-mac-aio-engine-0.2.0.tar.xz $sha"
list=$(tar -tJf "$archive")
assert_contains "$list" "wswine.bundle/lib/wine/x86_64-unix/ntdll.so.fixed"
assert_contains "$list" "tests/bin/t.exe"
assert_eq "$(tar -tvJf "$archive" | awk 'NR==1 {print $3}')" "root" "archive not owned by root"

# A file naming the builder's home is refused.
echo "$HOME/x" > "$o/wswine.bundle/leak"
if out=$("$r/build/build.sh" --pack 0.2.1 2>&1); then fail "--pack accepted a file naming $HOME"; fi
assert_contains "$out" "contain $HOME"
rm "$o/wswine.bundle/leak"
if "$r/build/build.sh" --pack 2>/dev/null; then fail "--pack without a version succeeded"; fi
pass
