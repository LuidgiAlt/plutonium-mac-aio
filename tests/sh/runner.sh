#!/bin/bash
# lib/runner.sh and put() against the fake CrossOver.app in tests/fixtures.
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
app=$repo_root/tests/fixtures/CrossOver.app
root=$tmp/project
mkdir -p "$root"
rc=$root/runner/CrossOver
w32=$rc/lib/wine/i386-windows

runner_create "$root" "$app" || fail "runner_create failed"
assert_file "$rc/bin/wineserver"
assert_eq "$(cat "$root/runner/.version")" "26.3.0.39832"

# A build tree: one file replacing a CrossOver file, the rest new.
tree=$tmp/out
mkdir -p "$tree/bin" "$tree/lib/wine/i386-windows" "$tree/lib/wine/x86_64-unix"
echo "patched wineserver 1" > "$tree/bin/wineserver"
echo "dxvk d3d9" > "$tree/lib/wine/i386-windows/d3d9.dll.dxvk"
echo "patched wined3d" > "$tree/lib/wine/i386-windows/wined3d.dll.vk"
echo "ntdll fixed" > "$tree/lib/wine/x86_64-unix/ntdll.so.fixed"
echo "ntdll legacy" > "$tree/lib/wine/x86_64-unix/ntdll.so.legacy"
mkdir -p "$tree/tests/bin" && echo "test program" > "$tree/tests/bin/t.exe"
mkdir -p "$tree/lib64" && echo "moltenvk 1.4.2" > "$tree/lib64/libMoltenVK.dylib.142"
mvk=$rc/lib64/libMoltenVK.dylib

runner_install "$root" "$tree" || fail "runner_install failed"
assert_eq "$(cat "$rc/bin/wineserver")" "patched wineserver 1"
assert_eq "$(cat "$rc/bin/wineserver.orig")" "crossover original bin/wineserver"
assert_no_file "$rc/tests"
echo "patched wineserver 2" > "$tree/bin/wineserver"
runner_install "$root" "$tree" || fail "second runner_install failed"
assert_eq "$(cat "$rc/bin/wineserver")" "patched wineserver 2"
assert_eq "$(cat "$rc/bin/wineserver.orig")" "crossover original bin/wineserver" "second install overwrote .orig"

runner_select_d3d9 "$root" vulkan || fail "select vulkan failed"
assert_eq "$(cat "$w32/d3d9.dll")" "crossover original lib/wine/i386-windows/d3d9.dll"
assert_eq "$(cat "$w32/wined3d.dll")" "patched wined3d"
assert_eq "$(cat "$mvk")" "crossover original lib64/libMoltenVK.dylib" "vulkan needs CrossOver's MoltenVK"
runner_select_d3d9 "$root" dxvk || fail "select dxvk failed"
assert_eq "$(cat "$w32/d3d9.dll")" "dxvk d3d9"
assert_eq "$(cat "$mvk")" "moltenvk 1.4.2" "dxvk needs MoltenVK 1.4.2"
cmp -s "$w32/wined3d.dll" "$app/Contents/SharedSupport/CrossOver/lib/wine/i386-windows/wined3d.dll" ||
  fail "dxvk did not restore CrossOver's wined3d.dll"
runner_select_d3d9 "$root" vulkan && runner_select_d3d9 "$root" dxvk
assert_eq "$(cat "$w32/d3d9.dll.orig")" "crossover original lib/wine/i386-windows/d3d9.dll" ".orig changed by switching"
if runner_select_d3d9 "$root" metal 2>/dev/null; then fail "select d3d9 metal accepted"; fi

runner_select_ntdll "$root" legacy || fail "select legacy failed"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll legacy"
runner_select_ntdll "$root" fixed || fail "select fixed failed"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll fixed"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so.orig")" "crossover original lib/wine/x86_64-unix/ntdll.so"

# runner_version_ok compares the runner's stamp with the installed CrossOver.
runner_version_ok "$root" "$app" || fail "runner_version_ok rejected a matching version"
echo 26.3.0.00001 > "$root/runner/.version"
if runner_version_ok "$root" "$app"; then fail "runner_version_ok accepted a different version"; fi
echo 26.3.0.39832 > "$root/runner/.version"

# runner_thirdparty installs DXMT's builtin DLLs and MoltenVK's x86_64 slice.
dl=$tmp/downloads
mkdir -p "$dl/stage/v0.80/i386-windows" "$dl/stage/v0.80/x86_64-windows" "$dl/stage/v0.80/x86_64-unix"
for a in i386-windows x86_64-windows; do
  for d in d3d11 dxgi d3d10core winemetal nvapi64; do echo "dxmt $a $d" > "$dl/stage/v0.80/$a/$d.dll"; done
done
echo "dxmt winemetal.so" > "$dl/stage/v0.80/x86_64-unix/winemetal.so"
tar -czf "$dl/dxmt-v0.80-builtin.tar.gz" -C "$dl/stage" v0.80
mkdir -p "$dl/mvk/MoltenVK/dynamic/dylib/macOS"
echo 'int mvk_probe(void) { return 1; }' > "$dl/probe.c"
clang -arch x86_64 -arch arm64 -dynamiclib -o "$dl/mvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$dl/probe.c" 2>/dev/null ||
  fail "cannot build the MoltenVK stand-in"
tar -cf "$dl/MoltenVK-macos.tar" -C "$dl/mvk" MoltenVK
AIO_DXMT_SHA=$(shasum -a 256 "$dl/dxmt-v0.80-builtin.tar.gz" | cut -d' ' -f1)
export AIO_DXMT_SHA
runner_thirdparty "$root" "$dl" || fail "runner_thirdparty failed"
assert_eq "$(cat "$rc/lib/wine/i386-windows/d3d11.dll")" "dxmt i386-windows d3d11"
assert_eq "$(cat "$rc/lib/wine/x86_64-windows/winemetal.dll")" "dxmt x86_64-windows winemetal"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/winemetal.so")" "dxmt winemetal.so"
assert_no_file "$rc/lib/wine/x86_64-windows/nvapi64.dll"
assert_contains "$(lipo -archs "$rc/lib64/libMoltenVK.dylib.142")" "x86_64"
# A MoltenVK without an x86_64 slice is refused (the archive itself is
# pinned in manifest.txt and checked when it is downloaded).
clang -arch arm64 -dynamiclib -o "$dl/mvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$dl/probe.c" 2>/dev/null
tar -cf "$dl/MoltenVK-macos.tar" -C "$dl/mvk" MoltenVK
if runner_thirdparty "$root" "$dl" 2>/dev/null; then fail "a MoltenVK without an x86_64 slice was accepted"; fi

# put() leaves an identical file alone.
touch -t 202001010000 "$w32/d3d9.dll"
put "$tree/lib/wine/i386-windows/d3d9.dll.dxvk" "$w32/d3d9.dll"
assert_eq "$(stat -f %m "$w32/d3d9.dll")" "$(date -j -f %Y%m%d%H%M%S 20200101000000 +%s)" "put rewrote an identical file"
assert_eq "$(find "$w32" -name 'd3d9.dll.??????' | wc -l | tr -d ' ')" "0" "put left a temp file"
pass
