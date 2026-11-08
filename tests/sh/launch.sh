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
export AIO_NO_DIALOG=1 LAUNCH_DRY_RUN=1
export AIO_WINE=$repo_root/tests/fixtures/wine-stub.sh AIO_WINESERVER=$repo_root/tests/fixtures/wine-stub.sh
unset NTDLL XAUDIO MAXFPS LAZY_EXEC SOCKFIX MOUSE_COALESCING X87 HUD WINDOWED FULLSCREEN TWEAKS VERBOSE PLUTONIUM_START GAME

# No runner yet.
if out=$("$r/launch.sh" 2>&1); then fail "launch without setup succeeded"; fi
assert_contains "$out" "Run ./setup.sh first"

w32=$r/runner/engine/lib/wine
mkdir -p "$w32/x86_64-unix" "$w32/i386-windows" "$r/runner/engine/bin"
echo wineserver > "$r/runner/engine/bin/wineserver"
echo fixed > "$w32/x86_64-unix/ntdll.so.fixed"
mkdir -p "$r/prefix/drive_c/users/crossover/AppData/Local/Plutonium/bin"
echo reg > "$r/prefix/system.reg"
touch "$r/prefix/drive_c/users/crossover/AppData/Local/Plutonium/plutonium.exe"

out=$("$r/launch.sh" 2>&1) || fail "default launch failed: $out"
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
assert_contains "$out" 'reg delete HKCU\Environment /v WINE_MOUSE_COALESCING /f'
out=$(MOUSE_COALESCING=1 "$r/launch.sh" 2>&1) || fail "MOUSE_COALESCING=1 launch failed"
assert_contains "$out" 'reg add HKCU\Environment /v WINE_MOUSE_COALESCING /d 1 /f'
assert_contains "$out" 'reg add HKCU\Environment /v WINE_X87_SIDECAR /d plutonium-bootstrapper-win32.exe /f'
out=$(X87=0 "$r/launch.sh" 2>&1) || fail "X87=0 launch failed"
assert_contains "$out" 'reg delete HKCU\Environment /v WINE_X87_SIDECAR /f'

out=$(PLUTONIUM_START=launcher "$r/launch.sh" 2>&1) || fail "launcher start failed"
assert_contains "$out" "plutonium-launcher-win32.exe"

if out=$(NTDLL=foo "$r/launch.sh" 2>&1); then fail "NTDLL=foo accepted"; fi
assert_contains "$out" "bad option: NTDLL=foo"
case "$out" in *select*|*"reg add"*) fail "actions ran before the options were checked" ;; esac

# GAME=BO2_SP: the campaign under steam.exe, checked step by step.
g="$tmp/BO2 game"; mkdir -p "$g"; touch "$g/t6sp.exe"
client="$tmp/Steam client"; mkdir -p "$client"
export BO2_DIR=$g AIO_STEAM_CLIENT=$client AIO_STEAM_RUNNING=1
if out=$(GAME=BO2_SP "$r/launch.sh" 2>&1); then fail "BO2_SP without the engine's steam.exe succeeded"; fi
assert_contains "$out" "This engine cannot start the campaign"
mkdir -p "$r/runner/engine/share/steam" "$w32/x86_64-windows"
touch "$r/runner/engine/share/steam/steam.exe" "$w32/x86_64-unix/lsteamclient.so"
if out=$(GAME=BO2_SP "$r/launch.sh" 2>&1); then fail "BO2_SP without steamclient.dll succeeded"; fi
assert_contains "$out" "needs Valve's steamclient.dll"
mkdir -p "$r/prefix/drive_c/Program Files (x86)/Steam"
touch "$r/prefix/drive_c/Program Files (x86)/Steam/steamclient.dll"
if out=$(GAME=BO2_SP "$r/launch.sh" 2>&1); then fail "BO2_SP without Steam for Mac succeeded"; fi
assert_contains "$out" "needs Steam for Mac"
touch "$client/steamclient.dylib"
if out=$(GAME=BO2_SP AIO_STEAM_RUNNING=0 "$r/launch.sh" 2>&1); then fail "BO2_SP without Steam running succeeded"; fi
assert_contains "$out" "Start Steam and sign in"
out=$(GAME=BO2_SP "$r/launch.sh" 2>&1) || fail "BO2_SP launch failed: $out"
assert_contains "$out" "/Steam/steam.exe"
assert_contains "$out" "syswow64/lsteamclient.dll"
assert_contains "$out" 'reg add HKCU\Environment /v WINE_X87_SIDECAR /d t6sp.exe /f'
assert_contains "$out" 'exec '
assert_contains "$out" '\t6sp.exe'
case "$out" in *Plutonium/plutonium.exe*) fail "BO2_SP started Plutonium" ;; esac
if out=$(GAME=BO2_MP "$r/launch.sh" 2>&1); then fail "GAME=BO2_MP accepted"; fi
assert_contains "$out" "bad option: GAME=BO2_MP"
unset BO2_DIR AIO_STEAM_CLIENT AIO_STEAM_RUNNING

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
# the wrong ntdll (NTDLL=legacy is the documented safety net).
if out=$(NTDLL=legacy "$r/launch.sh" 2>&1); then fail "NTDLL=legacy without ntdll.so.legacy succeeded"; fi
assert_contains "$out" "ntdll.so.legacy is missing"

# A launcher that stops at once with an error shows the log dialog.
out=$(LAUNCH_DRY_RUN=0 AIO_STUB_EXIT=3 AIO_PREFIX_RUNNING=0 "$r/launch.sh" 2>&1)
assert_contains "$out" "Plutonium stopped right away"
# A game the x87sidecar ended, or one it could not hook, is reported.
out=$(LAUNCH_DRY_RUN=0 AIO_STUB_EXIT=137 AIO_PREFIX_RUNNING=0 \
  AIO_STUB_OUTPUT="rosettax87 stub abort kind=0 val=0x10000003 idx=0x00000012" "$r/launch.sh" 2>&1)
assert_contains "$out" "The x87 acceleration ended the game"
out=$(LAUNCH_DRY_RUN=0 AIO_PREFIX_RUNNING=0 \
  AIO_STUB_OUTPUT="wine: no x87 acceleration for this process: no reply from the sidecar" "$r/launch.sh" 2>&1)
assert_contains "$out" "ran without x87 acceleration"

# selftest runs the Wine tests, the legacy ntdll's breakpoint check, then
# puts the selected ntdll back.
out=$(AIO_TEST_RUN="echo run" "$r/launch.sh" selftest 2>&1) || fail "selftest failed: $out"
assert_contains "$out" "run --ntdll fixed"
assert_contains "$out" "run --ntdll legacy breakpoint-test self"
assert_contains "$out" "select ntdll fixed"
pass
