#!/bin/bash
# setup.sh end to end with the fake CrossOver.app, a wine stub, file://
# archives and no Plutonium download; in a folder whose path has a space.
set -u
test_name=setup  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"

make_tmp
r="$tmp/aio repo"
mkdir -p "$r"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$r"
app="$tmp/CrossOver.app"
cp -R "$repo_root/tests/fixtures/CrossOver.app" "$app"
dl="$tmp/fixture downloads"
mkdir -p "$dl/tree/bin" "$dl/tree/lib/wine/x86_64-unix" "$dl/tree/lib/wine/i386-windows" "$dl/tree/tests/bin"
echo "patched wineserver" > "$dl/tree/bin/wineserver"
echo "ntdll fixed" > "$dl/tree/lib/wine/x86_64-unix/ntdll.so.fixed"
echo "ntdll legacy" > "$dl/tree/lib/wine/x86_64-unix/ntdll.so.legacy"
echo "dxvk" > "$dl/tree/lib/wine/i386-windows/d3d9.dll.dxvk"
echo "test program" > "$dl/tree/tests/bin/t.exe"
tar -czf "$dl/bin.tar.gz" -C "$dl/tree" .
mkdir -p "$dl/stage/v0.80/i386-windows" "$dl/stage/v0.80/x86_64-windows" "$dl/stage/v0.80/x86_64-unix"
for a in i386-windows x86_64-windows; do for d in d3d11 dxgi d3d10core winemetal; do echo "$a $d" > "$dl/stage/v0.80/$a/$d.dll"; done; done
echo "winemetal.so" > "$dl/stage/v0.80/x86_64-unix/winemetal.so"
tar -czf "$dl/dxmt.tar.gz" -C "$dl/stage" v0.80
mkdir -p "$dl/mvk/MoltenVK/dynamic/dylib/macOS"
echo 'int mvk_probe(void) { return 1; }' > "$dl/probe.c"
clang -arch x86_64 -arch arm64 -dynamiclib -o "$dl/mvk/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$dl/probe.c" 2>/dev/null
tar -cf "$dl/mvk.tar" -C "$dl/mvk" MoltenVK
u() { echo "file://${1// /%20}"; }
s() { shasum -a 256 "$1" | cut -d' ' -f1; }
cat > "$tmp/manifest.txt" <<EOM
dxmt-v0.80-builtin.tar.gz $(u "$dl/dxmt.tar.gz") $(s "$dl/dxmt.tar.gz")
MoltenVK-macos.tar $(u "$dl/mvk.tar") $(s "$dl/mvk.tar")
plutonium-mac-aio-bin.tar.gz $(u "$dl/bin.tar.gz") $(s "$dl/bin.tar.gz")
EOM
lib_two="$tmp/External/SteamLibrary"
mkdir -p "$tmp/Steam/steamapps/common" "$lib_two/steamapps/common/Call of Duty Black Ops II"
sed "s|LIBRARY_ONE|$tmp/Steam|; s|LIBRARY_TWO|$lib_two|" "$repo_root/tests/fixtures/libraryfolders.vdf" > "$tmp/libraryfolders.vdf"
export CROSSOVER_APP=$app AIO_MANIFEST=$tmp/manifest.txt AIO_DOWNLOADS="$tmp/cache"
export AIO_WINE=$repo_root/tests/fixtures/wine-stub.sh AIO_WINESERVER=$repo_root/tests/fixtures/wine-stub.sh
export AIO_STUB_LOG=$tmp/stub.log AIO_SKIP_PLUTONIUM=1 STEAM_VDF=$tmp/libraryfolders.vdf
export AIO_DXMT_SHA
AIO_DXMT_SHA=$(s "$dl/dxmt.tar.gz")
unset BO1_DIR BO2_DIR
app_sums() { (cd "$app" && find . -type f -exec shasum {} + | sort); }
before=$(app_sums)

# First run.
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "first run failed: $(echo "$out" | tail -5)"
rc="$r/runner/CrossOver"
assert_eq "$(cat "$r/runner/.version")" "26.3.0.39832"
assert_eq "$(cat "$rc/bin/wineserver")" "patched wineserver"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll fixed"
assert_eq "$(cat "$rc/lib/wine/i386-windows/d3d9.dll")" "dxvk"
assert_file "$rc/lib64/libMoltenVK.dylib.142"
assert_eq "$(cat "$r/tests/bin/t.exe")" "test program"
assert_file "$r/prefix/system.reg"
assert_file "$r/logs/setup.log"
assert_contains "$(cat "$tmp/stub.log")" 'reg add HKCU\Environment /v WINE_ROSETTA_EXEC_BREAKPOINTS /d 1 /f'
assert_contains "$(cat "$tmp/stub.log")" 'Hidraw /t REG_DWORD /d 0 /f'
assert_contains "$out" "BO1 not found"
assert_contains "$out" "Z:${lib_two//\//\\}\\steamapps\\common\\Call of Duty Black Ops II"

# Second run: every step is already done.
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "second run failed: $(echo "$out" | tail -5)"
for step in runner binaries thirdparty prefix; do assert_contains "$out" "skip $step"; done

# DXMT and MoltenVK are recorded by their checksums: an install recorded the
# old way ("done") or a new archive in manifest.txt installs them again.
assert_eq "$(cat "$r/runner/.thirdparty")" "$(s "$dl/dxmt.tar.gz") $(s "$dl/mvk.tar")"
echo "done" > "$r/runner/.thirdparty"
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "run after an old mark failed: $(echo "$out" | tail -3)"
case "$out" in *"skip thirdparty"*) fail "an old thirdparty mark was trusted" ;; esac
assert_eq "$(cat "$r/runner/.thirdparty")" "$(s "$dl/dxmt.tar.gz") $(s "$dl/mvk.tar")"
echo "newer MoltenVK" > "$dl/mvk/MoltenVK/README"
tar -cf "$dl/mvk2.tar" -C "$dl/mvk" MoltenVK
sed -i '' "s|^MoltenVK-macos.tar .*|MoltenVK-macos.tar $(u "$dl/mvk2.tar") $(s "$dl/mvk2.tar")|" "$tmp/manifest.txt"
out=$("$r/setup.sh" < /dev/null 2>&1) || fail "run with a new MoltenVK failed: $(echo "$out" | tail -3)"
case "$out" in *"skip thirdparty"*) fail "a new MoltenVK in manifest.txt was skipped" ;; esac
assert_contains "$out" "skip binaries"
assert_eq "$(cat "$r/runner/.thirdparty")" "$(s "$dl/dxmt.tar.gz") $(s "$dl/mvk2.tar")"
assert_file "$rc/lib64/libMoltenVK.dylib.142"

# A runner made from another CrossOver build: setup asks before rebuilding.
echo 26.3.0.00001 > "$r/runner/.version"
if out=$(echo n | "$r/setup.sh" 2>&1); then fail "rebuild refused but setup succeeded"; fi
assert_contains "$out" "CrossOver changed: rebuild runner? [y/N]"
assert_eq "$(cat "$r/runner/.version")" "26.3.0.00001"
out=$(echo y | "$r/setup.sh" 2>&1) || fail "rebuild accepted but setup failed: $(echo "$out" | tail -3)"
assert_eq "$(cat "$r/runner/.version")" "26.3.0.39832"
# The rebuilt runner has the patched files again, not CrossOver's own.
assert_eq "$(cat "$rc/bin/wineserver")" "patched wineserver"
assert_eq "$(cat "$rc/lib/wine/x86_64-unix/ntdll.so")" "ntdll fixed"
assert_file "$rc/lib64/libMoltenVK.dylib.142"

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

# Another CrossOver version: setup stops before touching anything.
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion 26.3.0.00000" "$app/Contents/Info.plist"
if out=$("$r/setup.sh" < /dev/null 2>&1); then fail "setup accepted CrossOver 26.3.0.00000"; fi
assert_contains "$out" "26.3.0.39832"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion 26.3.0.39832" "$app/Contents/Info.plist"

# Uninstall removes exactly what setup made; CrossOver.app is untouched.
out=$(echo y | "$r/setup.sh" uninstall 2>&1) || fail "uninstall failed: $out"
assert_no_file "$r/runner"
assert_no_file "$r/prefix"
assert_no_file "$r/logs"
assert_file "$r/setup.sh"
assert_file "$r/tests/sh/setup.sh"
assert_eq "$(app_sums)" "$before" "CrossOver.app changed"
pass
