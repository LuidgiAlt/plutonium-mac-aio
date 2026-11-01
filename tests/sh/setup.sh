#!/bin/bash
# setup.sh end to end with a fake engine and template, a wine stub, file://
# archives and no Plutonium download; in a folder whose path has a space.
set -u
test_name=setup  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"

make_tmp
r="$tmp/aio repo"
mkdir -p "$r"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$r"
dl="$tmp/fixture downloads"
engine="$tmp/engine"
mkdir -p "$engine/bin" "$engine/lib/wine/x86_64-unix" "$engine/lib/wine/i386-windows"
echo "engine wineserver" > "$engine/bin/wineserver"
echo "engine ntdll" > "$engine/lib/wine/x86_64-unix/ntdll.so"
echo "ntdll fixed" > "$engine/lib/wine/x86_64-unix/ntdll.so.fixed"
echo "ntdll legacy" > "$engine/lib/wine/x86_64-unix/ntdll.so.legacy"
echo "build 1" > "$engine/version"
mkdir -p "$dl/tpl/Template-1.0.24.app/Contents/Frameworks"
echo "template lib" > "$dl/tpl/Template-1.0.24.app/Contents/Frameworks/libfreetype.6.dylib"
tar -cJf "$dl/template.tar.xz" -C "$dl/tpl" Template-1.0.24.app
mkdir -p "$dl/stage/v0.80/i386-windows" "$dl/stage/v0.80/x86_64-windows" "$dl/stage/v0.80/x86_64-unix"
for a in i386-windows x86_64-windows; do for d in d3d11 dxgi d3d10core winemetal; do echo "$a $d" > "$dl/stage/v0.80/$a/$d.dll"; done; done
echo "winemetal.so" > "$dl/stage/v0.80/x86_64-unix/winemetal.so"
tar -czf "$dl/dxmt.tar.gz" -C "$dl/stage" v0.80
u() { echo "file://${1// /%20}"; }
s() { shasum -a 256 "$1" | cut -d' ' -f1; }
cat > "$tmp/manifest.txt" <<EOM
dxmt-v0.80-builtin.tar.gz $(u "$dl/dxmt.tar.gz") $(s "$dl/dxmt.tar.gz")
Sikarugir-Template.tar.xz $(u "$dl/template.tar.xz") $(s "$dl/template.tar.xz")
EOM
lib_two="$tmp/External/SteamLibrary"
mkdir -p "$tmp/Steam/steamapps/common" "$lib_two/steamapps/common/Call of Duty Black Ops II"
sed "s|LIBRARY_ONE|$tmp/Steam|; s|LIBRARY_TWO|$lib_two|" "$repo_root/tests/fixtures/libraryfolders.vdf" > "$tmp/libraryfolders.vdf"
export AIO_ENGINE=$engine AIO_MANIFEST=$tmp/manifest.txt AIO_DOWNLOADS="$tmp/cache"
export AIO_WINE=$repo_root/tests/fixtures/wine-stub.sh AIO_WINESERVER=$repo_root/tests/fixtures/wine-stub.sh
export AIO_STUB_LOG=$tmp/stub.log AIO_SKIP_PLUTONIUM=1 STEAM_VDF=$tmp/libraryfolders.vdf
export AIO_DXMT_SHA
AIO_DXMT_SHA=$(s "$dl/dxmt.tar.gz")
unset BO1_DIR BO2_DIR

# First run: the runner is the template with the engine, linked as runner/engine.
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "first run failed: $(echo "$out" | tail -5)"
rc="$r/runner/engine"
assert_eq "$(readlink "$rc")" "Sikarugir.app/Contents/Frameworks/wswine.bundle"
assert_file "$r/runner/Sikarugir.app/Contents/Frameworks/libfreetype.6.dylib"
assert_eq "$(cat "$rc/bin/wineserver")" "engine wineserver"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll fixed"
assert_eq "$(cat "$rc/lib/wine/i386-windows/d3d11.dll")" "i386-windows d3d11" "DXMT not installed"
assert_file "$r/prefix/system.reg"
assert_file "$r/logs/setup.log"
assert_contains "$(cat "$tmp/stub.log")" 'reg add HKCU\Environment /v WINE_ROSETTA_EXEC_BREAKPOINTS /d 1 /f'
assert_contains "$(cat "$tmp/stub.log")" 'Hidraw /t REG_DWORD /d 0 /f'
assert_contains "$out" "BO1 not found"
assert_contains "$out" "Z:${lib_two//\//\\}\\steamapps\\common\\Call of Duty Black Ops II"

# Second run: every step is already done.
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "second run failed: $(echo "$out" | tail -5)"
for step in runner thirdparty prefix; do assert_contains "$out" "skip $step"; done
# Without the engine folder (build/out removed), the runner's engine is kept.
out=$(AIO_ENGINE=$tmp/no-engine "$r/setup.sh" < /dev/null 2>&1) || fail "run without the engine folder failed: $(echo "$out" | tail -3)"
assert_contains "$out" "skip runner"

# A newer engine build replaces the runner's; DXMT goes in again, the prefix stays.
echo "build 2" > "$engine/version"
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "run with a new engine failed: $(echo "$out" | tail -3)"
case "$out" in *"skip runner"*) fail "a new engine build was skipped" ;; esac
assert_contains "$out" "skip prefix"
assert_eq "$(cat "$rc/version")" "build 2"
assert_eq "$(cat "$rc/lib/wine/i386-windows/d3d11.dll")" "i386-windows d3d11" "DXMT not installed again"

# A new DXMT in manifest.txt installs it again.
echo "newer" > "$dl/stage/v0.80/x86_64-unix/winemetal.so"
tar -czf "$dl/dxmt2.tar.gz" -C "$dl/stage" v0.80
AIO_DXMT_SHA=$(s "$dl/dxmt2.tar.gz")
sed -i '' "s|^dxmt-v0.80-builtin.tar.gz .*|dxmt-v0.80-builtin.tar.gz $(u "$dl/dxmt2.tar.gz") $AIO_DXMT_SHA|" "$tmp/manifest.txt"
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "run with a new DXMT failed: $(echo "$out" | tail -3)"
case "$out" in *"skip thirdparty"*) fail "a new DXMT in manifest.txt was skipped" ;; esac
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/winemetal.so")" "newer"

# BO1 found only on a later run: its XAudio2 is added then (the prefix
# itself is already done).
bo1="$lib_two/steamapps/common/Call of Duty Black Ops"
mkdir -p "$bo1/Redist/DirectX"
echo "xaudio2 dll" > "$tmp/XAudio2_7.dll"
python3 -I "$repo_root/tests/fixtures/make-cab.py" "$bo1/Redist/DirectX/Jun2010_XAudio_x86.cab" XAudio2_7.dll "$tmp/XAudio2_7.dll"
assert_no_file "$r/prefix/xaudio2_7.native.dll"
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "run with BO1 failed: $(echo "$out" | tail -3)"
assert_contains "$out" "skip prefix"
assert_eq "$(cat "$r/prefix/xaudio2_7.native.dll" 2>/dev/null)" "xaudio2 dll"
assert_contains "$(cat "$tmp/stub.log")" 'DllOverrides /v xaudio2_7 /d native /f'

# No local engine: the release's archive (manifest.txt) is downloaded once,
# with its test programs.
mkdir -p "$dl/rel/tests/bin"
cp -R "$engine" "$dl/rel/wswine.bundle"
echo "release build" > "$dl/rel/wswine.bundle/version"
echo "test program" > "$dl/rel/tests/bin/t.exe"
tar -cJf "$dl/engine.tar.xz" -C "$dl/rel" wswine.bundle tests
echo "plutonium-mac-aio-engine.tar.xz $(u "$dl/engine.tar.xz") $(s "$dl/engine.tar.xz")" >> "$tmp/manifest.txt"
out=$(AIO_ENGINE=$tmp/no-engine "$r/setup.sh" < /dev/null 2>&1) || fail "run with the release's engine failed: $(echo "$out" | tail -3)"
assert_contains "$out" "downloading the engine"
assert_eq "$(cat "$rc/version")" "release build"
assert_eq "$(cat "$r/tests/bin/t.exe")" "test program"
assert_contains "$out" "skip prefix"
out=$(AIO_ENGINE=$tmp/no-engine "$r/setup.sh" < /dev/null 2>&1) || fail "second run with the release's engine failed"
case "$out" in *"downloading the engine"*) fail "the engine was downloaded again" ;; esac
assert_contains "$out" "skip runner"

# Uninstall removes exactly what setup made; the engine build is untouched.
out=$(echo y | "$r/setup.sh" uninstall 2>&1) || fail "uninstall failed: $out"
assert_no_file "$r/runner"
assert_no_file "$r/prefix"
assert_no_file "$r/logs"
assert_file "$r/setup.sh"
assert_file "$r/tests/sh/setup.sh"
assert_file "$engine/version"
pass
