#!/bin/bash
# build/build.sh --pack <version> packs build/out for a GitHub Release and
# prints the manifest.txt line with its SHA-256; it refuses files that carry
# the builder's home folder path.
set -u
test_name=release  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"

make_tmp
r="$tmp/repo"
mkdir -p "$r"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$r"
o=$r/build/out
mkdir -p "$o/bin" "$o/lib/wine/x86_64-unix" "$o/tests/bin"
echo ws > "$o/bin/wineserver"
echo nt > "$o/lib/wine/x86_64-unix/ntdll.so.fixed"
echo t > "$o/tests/bin/t.exe"

out=$("$r/build/build.sh" --pack 0.1.0 2>&1) || fail "--pack failed: $out"
a=$r/build/plutonium-mac-aio-bin-0.1.0.tar.gz
assert_file "$a"
list=$(tar -tzf "$a")
assert_contains "$list" "bin/wineserver"
assert_contains "$list" "lib/wine/x86_64-unix/ntdll.so.fixed"
assert_contains "$list" "tests/bin/t.exe"
case "$(tar -tvzf "$a")" in *"$(id -un)"*) fail "the archive names the builder as file owner" ;; esac
sha=$(shasum -a 256 "$a" | cut -d' ' -f1)
assert_contains "$out" "plutonium-mac-aio-bin.tar.gz https://github.com/LuidgiAlt/plutonium-mac-aio/releases/download/v0.1.0/plutonium-mac-aio-bin-0.1.0.tar.gz $sha"

# A public archive must not carry the builder's home folder (debug info,
# assert file names).
printf 'built in %s/build/tree\n' "$HOME" > "$o/lib/wine/x86_64-unix/ntdll.so.fixed"
if out=$("$r/build/build.sh" --pack 0.1.0 2>&1); then fail "--pack accepted a file with the home folder path"; fi
assert_contains "$out" "lib/wine/x86_64-unix/ntdll.so.fixed"
echo nt > "$o/lib/wine/x86_64-unix/ntdll.so.fixed"

if "$r/build/build.sh" --pack 2>/dev/null; then fail "--pack without a version accepted"; fi
rm -rf "$o"
if "$r/build/build.sh" --pack 0.1.0 2>/dev/null; then fail "--pack without build/out accepted"; fi
pass
