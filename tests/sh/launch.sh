#!/bin/bash
# launch.sh in dry-run mode (LAUNCH_DRY_RUN=1 prints the actions) against a
# fake runner and prefix, in a folder whose path has a space.
set -u
test_name=launch  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
# shellcheck source=lib/env.sh
. "$repo_root/lib/env.sh"
# shellcheck source=lib/runner.sh
. "$repo_root/lib/runner.sh"

make_tmp
r="$tmp/aio repo"
mkdir -p "$r"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$r"
export CROSSOVER_APP=$repo_root/tests/fixtures/CrossOver.app AIO_NO_DIALOG=1 LAUNCH_DRY_RUN=1
export AIO_WINE=$repo_root/tests/fixtures/wine-stub.sh AIO_WINESERVER=$repo_root/tests/fixtures/wine-stub.sh
unset D3D9 NTDLL XAUDIO MAXFPS LAZY_EXEC SOCKFIX HUD WINDOWED FULLSCREEN TWEAKS VERBOSE PLUTONIUM_START

# No runner yet.
if out=$("$r/launch.sh" 2>&1); then fail "launch without setup succeeded"; fi
assert_contains "$out" "Run ./setup.sh first"

runner_create "$r" "$CROSSOVER_APP" || fail "runner_create failed"
w32=$r/runner/CrossOver/lib/wine
mkdir -p "$w32/x86_64-unix" "$w32/i386-windows"
echo fixed > "$w32/x86_64-unix/ntdll.so.fixed"
echo dxvk > "$w32/i386-windows/d3d9.dll.dxvk"
echo vk > "$w32/i386-windows/wined3d.dll.vk"
mkdir -p "$r/prefix/drive_c/users/crossover/AppData/Local/Plutonium/bin"
echo reg > "$r/prefix/system.reg"
touch "$r/prefix/drive_c/users/crossover/AppData/Local/Plutonium/plutonium.exe"

out=$("$r/launch.sh" 2>&1) || fail "default launch failed: $out"
assert_contains "$out" "select d3d9 dxvk"
assert_contains "$out" "select ntdll fixed"
assert_contains "$out" 'reg add HKCU\Environment /v WINE_ROSETTA_LAZY_EXEC /d 1 /f'
assert_contains "$out" 'reg add HKCU\Environment /v WINE_ROSETTA_EXEC_BREAKPOINTS /d 1 /f'
# wined3d's no3d renderer (DirectDraw only) reports a 128 MB GPU; BO1 reads
# its video memory there and capped textures at 112 MB.
assert_contains "$out" 'reg add HKCU\Software\Wine\AppDefaults\plutonium-bootstrapper-win32.exe\Direct3D /v VideoMemorySize /d 4096 /f'
assert_contains "$out" "exec "
assert_contains "$out" "Plutonium/plutonium.exe"

out=$(SOCKFIX=0 "$r/launch.sh" 2>&1) || fail "SOCKFIX=0 launch failed"
assert_contains "$out" 'reg add HKCU\Environment /v WINE_SOCK_FASTPATH /d 0 /f'

out=$(D3D9=vulkan PLUTONIUM_START=launcher "$r/launch.sh" 2>&1) || fail "vulkan launch failed"
assert_contains "$out" "select d3d9 vulkan"
assert_contains "$out" "plutonium-launcher-win32.exe"

if out=$(D3D9=foo "$r/launch.sh" 2>&1); then fail "D3D9=foo accepted"; fi
assert_contains "$out" "bad option: D3D9=foo"
case "$out" in *select*|*"reg add"*) fail "actions ran before the options were checked" ;; esac

# A launch already running (Review Focus 4: two clicks in quick succession).
mkdir -p "$r/logs/.launch.lock" && echo $$ > "$r/logs/.launch.lock/pid"
out=$("$r/launch.sh" 2>&1)
assert_contains "$out" "Plutonium is running. Quit it and start again?"
rm -rf "$r/logs/.launch.lock"
# The game still running after the first launch finished (no lock left).
out=$(AIO_PREFIX_RUNNING=1 "$r/launch.sh" 2>&1)
assert_contains "$out" "Plutonium is running. Quit it and start again?"
# A lock another launch has just taken (its pid not written yet) is live.
mkdir -p "$r/logs/.launch.lock" && : > "$r/logs/.launch.lock/pid"
out=$("$r/launch.sh" 2>&1)
assert_contains "$out" "Plutonium is running. Quit it and start again?"
rm -rf "$r/logs/.launch.lock"

# A missing file in the runner stops the launch instead of starting on
# CrossOver's own files (NTDLL=legacy is the documented safety net).
if out=$(NTDLL=legacy "$r/launch.sh" 2>&1); then fail "NTDLL=legacy without ntdll.so.legacy succeeded"; fi
assert_contains "$out" "ntdll.so.legacy is missing"

# A launcher that stops at once with an error shows the log dialog.
out=$(LAUNCH_DRY_RUN=0 AIO_STUB_EXIT=3 AIO_PREFIX_RUNNING=0 "$r/launch.sh" 2>&1)
assert_contains "$out" "Plutonium stopped right away"

# selftest runs the Wine tests on both renderers, the legacy ntdll's
# breakpoint check, then puts the selected renderer and ntdll back.
out=$(AIO_TEST_RUN="echo run" "$r/launch.sh" selftest 2>&1) || fail "selftest failed: $out"
assert_contains "$out" "run --ntdll fixed --d3d9 both"
assert_contains "$out" "run --ntdll legacy breakpoint-test self"
assert_contains "$out" "select d3d9 dxvk"

# A runner made from another CrossOver build.
echo 26.3.0.00001 > "$r/runner/.version"
if out=$("$r/launch.sh" 2>&1); then fail "launch with a stale runner succeeded"; fi
assert_contains "$out" "CrossOver was updated: run ./setup.sh"
pass
