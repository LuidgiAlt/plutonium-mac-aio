#!/bin/bash
# lib/runner.sh and put() against a fake runner/engine.
set -u
test_name=runner  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
# shellcheck source=lib/env.sh
. "$repo_root/lib/env.sh"
# shellcheck source=lib/fetch.sh
. "$repo_root/lib/fetch.sh"
# shellcheck source=lib/runner.sh
. "$repo_root/lib/runner.sh"

make_tmp
root=$tmp/project
rc=$root/runner/engine
w32=$rc/lib/wine/i386-windows
mkdir -p "$rc/bin" "$w32" "$rc/lib/wine/x86_64-unix"
echo "engine bin/wineserver" > "$rc/bin/wineserver"
echo "engine ntdll.so" > "$rc/lib/wine/x86_64-unix/ntdll.so"

# A build tree: one file replacing a runner file, the rest new.
tree=$tmp/out
mkdir -p "$tree/bin" "$tree/lib/wine/i386-windows" "$tree/lib/wine/x86_64-unix"
echo "patched wineserver 1" > "$tree/bin/wineserver"
echo "ntdll fixed" > "$tree/lib/wine/x86_64-unix/ntdll.so.fixed"
echo "ntdll legacy" > "$tree/lib/wine/x86_64-unix/ntdll.so.legacy"
echo "dxvk d3d9" > "$tree/lib/wine/i386-windows/d3d9.dll"
mkdir -p "$tree/tests/bin" && echo "test program" > "$tree/tests/bin/t.exe"

runner_install "$root" "$tree" || fail "runner_install failed"
assert_eq "$(cat "$rc/bin/wineserver")" "patched wineserver 1"
assert_eq "$(cat "$rc/bin/wineserver.orig")" "engine bin/wineserver"
assert_no_file "$rc/tests"
echo "patched wineserver 2" > "$tree/bin/wineserver"
runner_install "$root" "$tree" || fail "second runner_install failed"
assert_eq "$(cat "$rc/bin/wineserver")" "patched wineserver 2"
assert_eq "$(cat "$rc/bin/wineserver.orig")" "engine bin/wineserver" "second install overwrote .orig"

runner_select_ntdll "$root" legacy || fail "select legacy failed"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll legacy"
runner_select_ntdll "$root" fixed || fail "select fixed failed"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll fixed"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so.orig")" "engine ntdll.so"
if runner_select_ntdll "$root" old 2>/dev/null; then fail "select ntdll old accepted"; fi

# runner_thirdparty installs DXMT's builtin DLLs.
dl=$tmp/downloads
mkdir -p "$dl/stage/v0.80/i386-windows" "$dl/stage/v0.80/x86_64-windows" "$dl/stage/v0.80/x86_64-unix"
for a in i386-windows x86_64-windows; do
  for d in d3d11 dxgi d3d10core winemetal nvapi64; do echo "dxmt $a $d" > "$dl/stage/v0.80/$a/$d.dll"; done
done
echo "dxmt winemetal.so" > "$dl/stage/v0.80/x86_64-unix/winemetal.so"
tar -czf "$dl/dxmt-v0.80-builtin.tar.gz" -C "$dl/stage" v0.80
AIO_DXMT_SHA=$(shasum -a 256 "$dl/dxmt-v0.80-builtin.tar.gz" | cut -d' ' -f1)
export AIO_DXMT_SHA
runner_thirdparty "$root" "$dl" || fail "runner_thirdparty failed"
assert_eq "$(cat "$rc/lib/wine/i386-windows/d3d11.dll")" "dxmt i386-windows d3d11"
assert_eq "$(cat "$rc/lib/wine/x86_64-windows/winemetal.dll")" "dxmt x86_64-windows winemetal"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/winemetal.so")" "dxmt winemetal.so"
assert_no_file "$rc/lib/wine/x86_64-windows/nvapi64.dll"
if AIO_DXMT_SHA=0000 runner_thirdparty "$root" "$dl" 2>/dev/null; then fail "a DXMT archive with another SHA-256 was accepted"; fi

# put() leaves an identical file alone.
touch -t 202001010000 "$w32/d3d9.dll"
put "$tree/lib/wine/i386-windows/d3d9.dll" "$w32/d3d9.dll"
assert_eq "$(stat -f %m "$w32/d3d9.dll")" "$(date -j -f %Y%m%d%H%M%S 20200101000000 +%s)" "put rewrote an identical file"
assert_eq "$(find "$w32" -name 'd3d9.dll.??????' | wc -l | tr -d ' ')" "0" "put left a temp file"
pass
