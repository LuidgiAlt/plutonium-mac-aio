#!/bin/bash
# build/build.sh --check applies every patch to pristine CrossOver 26.3 sources,
# and reports the patch that no longer applies.
set -u
test_name=build-check  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
export AIO_DOWNLOADS="${AIO_DOWNLOADS:-$repo_root/downloads}"

out=$("$repo_root/build/build.sh" --check 2>&1) || fail "--check failed on the real patches: $out"

make_tmp
mkdir "$tmp/repo"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$tmp/repo"
patch=$(ls "$tmp/repo/patches/wine/"0015-*.patch)
# Change one character of the first context line (a line starting with a space).
line=$(grep -n '^ [^ ]' "$patch" | head -1 | cut -d: -f1)
sed -i '' "${line}s/^ \(.\)/ X\1/" "$patch"
if out=$("$tmp/repo/build/build.sh" --check 2>&1); then fail "--check passed with a broken 0015"; fi
assert_contains "$out" "0015"

# Wine's build system cannot handle spaces in paths: from a folder whose
# path has one, build.sh works through a space-free link.
assert_contains "$tmp" " "
out=$(bash -c '. "$1/build/common.sh"; build_path "$1/build"' _ "$tmp/repo" 2>&1) || fail "build_path failed: $out"
case "$out" in *" "*) fail "build_path kept a space: $out" ;; esac
[ "$(cd "$out" && pwd -P)" = "$(cd "$tmp/repo/build" && pwd -P)" ] || fail "build_path does not lead to build/: $out"
assert_eq "$(bash -c '. "$1/build/common.sh"; build_path "$1/build"' _ "$tmp/repo")" "$out" "build_path is not stable"

# --check must not touch build/src, where build/series.sh keeps work in progress.
mkdir -p "$repo_root/build/src/wine"
probe=$repo_root/build/src/wine/.check-probe
echo keep > "$probe"
"$repo_root/build/build.sh" --check > /dev/null 2>&1 || fail "--check failed"
assert_file "$probe"
rm -f "$probe"
pass
