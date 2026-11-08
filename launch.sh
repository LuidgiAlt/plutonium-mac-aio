#!/bin/bash
# Start Plutonium for BO1 and BO2. See README.md.
#   ./launch.sh            start (through Plutonium's updater)
#   ./launch.sh kill       stop everything in this project's prefix
#   ./launch.sh update     run Plutonium's updater only
#   ./launch.sh log        open the last run's log
#   ./launch.sh selftest   check this installation without a game
#   GAME=BO2_SP ./launch.sh  BO2's campaign, which Plutonium does not start
# Options (environment variables) are listed in README.md.
set -u
root=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/env.sh
. "$root/lib/env.sh"
# shellcheck source=lib/runner.sh
. "$root/lib/runner.sh"
# shellcheck source=lib/options.sh
. "$root/lib/options.sh"
# shellcheck source=lib/games.sh
. "$root/lib/games.sh"
# shellcheck source=lib/steam.sh
. "$root/lib/steam.sh"

plutonium=$root/prefix/drive_c/users/crossover/AppData/Local/Plutonium
logs=$root/logs
lock=$logs/.launch.lock
dry=${LAUNCH_DRY_RUN:-0}

stop() { dialog "$1" OK > /dev/null; exit 1; }

# The actions, printed instead of done with LAUNCH_DRY_RUN=1.
do_reg() { if [ "$dry" = 1 ]; then echo "reg $*"; else reg "$@"; fi; }
set_env() { export "$1=$2"; do_reg add 'HKCU\Environment' /v "$1" /d "$2" /f; }
clear_env() { unset "$1"; do_reg delete 'HKCU\Environment' /v "$1" /f; }
do_select() { if [ "$dry" = 1 ]; then echo "select $1 $2"; else "runner_select_$1" "$root" "$2"; fi; }
do_put() { if [ "$dry" = 1 ]; then echo "put $1 $2"; else put "$1" "$2"; fi; }
do_rm() { if [ "$dry" = 1 ]; then echo "rm $*"; else rm -rf "$@"; fi; }

use_env() {
  aio_env "$root"
  WINE=${AIO_WINE:-$WINE}
  WINESERVER=${AIO_WINESERVER:-$WINESERVER}
}

# Whether a wineserver serves this project's prefix: it keeps the "lock"
# file open in the directory it names after the prefix's device and inode.
# AIO_PREFIX_RUNNING=0|1 answers for tests.
prefix_running() {
  local lockfile dev ino
  if [ -n "${AIO_PREFIX_RUNNING:-}" ]; then [ "$AIO_PREFIX_RUNNING" = 1 ]; return; fi
  read -r dev ino < <(stat -f '%d %i' "$root/prefix") || return 1
  lockfile=/tmp/.wine-$(id -u)/server-$(printf '%x-%x' "$dev" "$ino")/lock
  [ -f "$lockfile" ] && lsof -t -- "$lockfile" > /dev/null 2>&1
}

# Whether another launch holds the lock: its pid is alive, or it has just
# made the lock and not written its pid yet.
lock_live() {
  local pid
  [ -d "$lock" ] || return 1
  pid=$(cat "$lock/pid" 2>/dev/null)
  if [ -n "$pid" ]; then kill -0 "$pid" 2>/dev/null; return; fi
  [ $(($(date +%s) - $(stat -f %m "$lock"))) -lt 10 ]
}

# One launch at a time, and never on top of a game that is still running
# (the lock is held until the prefix's wineserver ends).
take_lock() {
  mkdir -p "$logs"
  if lock_live || prefix_running; then
    [ "$(dialog "Plutonium is running. Quit it and start again?" "Quit and restart" "Cancel")" = "Quit and restart" ] || exit 0
    if [ "$dry" != 1 ]; then
      "$WINESERVER" -k 2>/dev/null
      "$WINESERVER" -w 2>/dev/null
    fi
  fi
  rm -rf "$lock"
  mkdir "$lock" || stop "Another launch is starting; try again."
  echo $$ > "$lock/pid"
  trap 'rm -rf "$lock"' EXIT
}

# The files the selected options need, before anything is switched.
check_runner_files() {
  local rc=$root/runner/engine f
  for f in "lib/wine/x86_64-unix/ntdll.so.${NTDLL:-fixed}" bin/wineserver; do
    [ -f "$rc/$f" ] || stop "$(basename "$f") is missing from the runner: run ./setup.sh again (in $root)."
  done
}

apply_switches() {
  local w32=$root/runner/engine/lib/wine/i386-windows syswow64=$WINEPREFIX/drive_c/windows/syswow64
  local bootstrapper='HKCU\Software\Wine\AppDefaults\plutonium-bootstrapper-win32.exe'
  do_select ntdll "${NTDLL:-fixed}"
  do_reg add "$bootstrapper\\Direct3D" /v renderer /d no3d /f
  # no3d reports a 128 MB GPU; BO1 reads its video memory through DirectDraw
  # and capped textures to it. 4096 MB is what the GPU renderers report.
  do_reg add "$bootstrapper\\Direct3D" /v VideoMemorySize /d 4096 /f
  # Plutonium starts the games with an environment built from HKCU\Environment.
  set_env WINE_ROSETTA_EXEC_BREAKPOINTS 1
  set_env WINE_ROSETTA_WOW64_BREAKPOINTS 1
  set_env WINE_ROSETTA_CALL_WORKAROUND 1
  if [ "${LAZY_EXEC:-1}" = 1 ]; then set_env WINE_ROSETTA_LAZY_EXEC 1; else clear_env WINE_ROSETTA_LAZY_EXEC; fi
  if [ "${SOCKFIX:-1}" = 0 ]; then set_env WINE_SOCK_FASTPATH 0; else clear_env WINE_SOCK_FASTPATH; fi
  if [ "${MOUSE_COALESCING:-0}" = 1 ]; then set_env WINE_MOUSE_COALESCING 1; else clear_env WINE_MOUSE_COALESCING; fi
  # x87 maths through the x87sidecar (patch 0023), only in the process both
  # games run in.
  if [ "${X87:-1}" = 1 ]; then set_env WINE_X87_SIDECAR plutonium-bootstrapper-win32.exe; else clear_env WINE_X87_SIDECAR; fi
  if [ -n "${MAXFPS:-}" ]; then set_env DXMT_CONFIG "d3d11.preferredMaxFrameRate=$MAXFPS;"; else clear_env DXMT_CONFIG; fi
  if [ "${HUD:-0}" = 1 ]; then set_env MTL_HUD_ENABLED 1; else clear_env MTL_HUD_ENABLED; fi
  set_env DXVK_ASYNC "${DXVK_ASYNC:-1}"
  if [ "${XAUDIO:-native}" = native ] && [ -f "$WINEPREFIX/xaudio2_7.native.dll" ]; then
    do_put "$WINEPREFIX/xaudio2_7.native.dll" "$syswow64/xaudio2_7.dll"
    do_reg add "$bootstrapper\\DllOverrides" /v xaudio2_7 /d native /f
  elif [ -f "$w32/xaudio2_7.dll" ]; then
    do_put "$w32/xaudio2_7.dll" "$syswow64/xaudio2_7.dll"
    do_reg delete "$bootstrapper\\DllOverrides" /v xaudio2_7 /f
  fi
  if [ "${VERBOSE:-0}" = 1 ]; then export WINEDEBUG=err+module,err+d3d,err+d3d9,err+xaudio2,err+seh
  else export WINEDEBUG=-all; fi
}

# The program to start: Plutonium's updater (it updates, then opens the
# launcher), or the launcher itself with PLUTONIUM_START=launcher.
start_target() {
  local f
  if [ "${PLUTONIUM_START:-updater}" != launcher ]; then
    for f in "$plutonium/plutonium.exe" "$plutonium/launcher/plutonium.exe"; do
      [ -f "$f" ] && { echo "$f"; return 0; }
    done
  fi
  [ -f "$plutonium/bin/plutonium-launcher-win32.exe" ] || [ "$dry" = 1 ] || return 1
  echo "$plutonium/bin/plutonium-launcher-win32.exe"
}

# The Steam files and keys GAME=BO2_SP puts in the prefix. Plutonium's
# programs look for Steam too, and quit when they find this one, so they are
# removed before every Plutonium launch (and steam_staged lists them).
steam_staged() {
  local f
  for f in "$WINEPREFIX/drive_c/Program Files (x86)/Steam" "$WINEPREFIX/drive_c/windows/system32/lsteamclient.dll" \
    "$WINEPREFIX/drive_c/windows/syswow64/lsteamclient.dll"; do
    [ -e "$f" ] && printf '%s\n' "$f"
  done
  return 0
}

steam_clear() {
  local staged=()
  while IFS= read -r f; do staged+=("$f"); done < <(steam_staged)
  [ "${#staged[@]}" -eq 0 ] || do_rm "${staged[@]}"
  if grep -q '^\[Software\\\\Valve' "$WINEPREFIX/user.reg" 2>/dev/null; then do_reg delete 'HKCU\Software\Valve' /f; fi
  if grep -q '^\[Software\\\\\(Wow6432Node\\\\\)\{0,1\}Valve' "$WINEPREFIX/system.reg" 2>/dev/null; then
    do_reg delete 'HKLM\Software\Valve' /f
    do_reg delete 'HKLM\Software\Wow6432Node\Valve' /f
  fi
  return 0
}

# GAME=BO2_SP: BO2's campaign (t6sp.exe), which Plutonium's launcher does not
# offer. It is wrapped in Steam's DRM, so it runs under the engine's steam.exe
# (steam/steam.c) with Valve's steamclient.dll from setup, which the engine
# redirects to lsteamclient (patch 0024, only with WINE_STEAM_BRIDGE=1);
# lsteamclient asks the macOS Steam app, running and signed in to the account
# that owns BO2. Sets bo2sp_game to the game's folder. AIO_STEAM_RUNNING=0|1
# and AIO_STEAM_CLIENT answer for tests.
bo2sp_prepare() {
  local steamdir="$WINEPREFIX/drive_c/Program Files (x86)/Steam" eng=$root/runner/engine game client
  game=$(steam_game_dir bo2) && [ -f "$game/t6sp.exe" ] ||
    stop "BO2's campaign (t6sp.exe) was not found: set BO2_DIR to the game folder (see README)."
  [ -f "$eng/share/steam/steam.exe" ] && [ -f "$eng/lib/wine/x86_64-unix/lsteamclient.so" ] ||
    stop "This engine cannot start the campaign: run ./setup.sh to update it (in $root)."
  [ -f "$WINEPREFIX/steamclient.valve.dll" ] || stop "Run ./setup.sh again (in $root): the campaign needs Valve's steamclient.dll."
  client=${AIO_STEAM_CLIENT:-$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS}
  [ -f "$client/steamclient.dylib" ] || stop "The campaign needs Steam for Mac: install it from store.steampowered.com, sign in, then try again."
  if [ -n "${AIO_STEAM_RUNNING:-}" ]; then [ "$AIO_STEAM_RUNNING" = 1 ]; else pgrep -xq steam_osx; fi ||
    stop "Start Steam and sign in with the account that owns Black Ops II, then try again."
  [ "$dry" = 1 ] || mkdir -p "$steamdir"
  do_put "$WINEPREFIX/steamclient.valve.dll" "$steamdir/steamclient.dll"
  do_put "$eng/share/steam/steam.exe" "$steamdir/steam.exe"
  do_put "$eng/lib/wine/x86_64-windows/lsteamclient.dll" "$WINEPREFIX/drive_c/windows/system32/lsteamclient.dll"
  do_put "$eng/lib/wine/i386-windows/lsteamclient.dll" "$WINEPREFIX/drive_c/windows/syswow64/lsteamclient.dll"
  export SteamAppId=202970 SteamGameId=202970 STEAM_COMPAT_CLIENT_INSTALL_PATH=$client WINE_STEAM_BRIDGE=1
  export WINEDLLOVERRIDES="steamclient=n;steamclient64=n;lsteamclient=b"
  if [ "${X87:-1}" = 1 ]; then set_env WINE_X87_SIDECAR t6sp.exe; fi
  bo2sp_game=$game
}

# x87_check <log>: say when the x87sidecar ended the game or could not hook
# Rosetta, and how to play without it.
x87_check() {
  if grep -q "rosettax87 stub abort" "$1"; then
    dialog "The x87 acceleration ended the game (see the log). To play without it: X87=0 ./launch.sh" OK > /dev/null
  elif grep -q -e "no x87 acceleration" -e "RUNNING WITHOUT X87 ACCELERATION" "$1"; then
    echo "note: the game ran without x87 acceleration (see ./launch.sh log); X87=0 ./launch.sh turns it off" >&2
  fi
  return 0
}

# Keeps the last 3 logs; caps each at 50 MB without stopping Wine. Returns
# when everything in the prefix has ended, so the lock covers the session.
run_logged() {
  local log=$logs/last-run.log pid status waited=0
  [ -f "$log.1" ] && mv -f "$log.1" "$log.2"
  [ -f "$log" ] && mv -f "$log" "$log.1"
  ( set -o pipefail
    "$@" 2>&1 | perl -e '$cap = 50 * 1024 * 1024; while (<STDIN>) { $n += length; if ($n <= $cap) { print } elsif (!$noted++) { print "log capped at 50 MB\n" } }' > "$log" ) &
  pid=$!
  while [ "$waited" -lt 15 ] && kill -0 "$pid" 2>/dev/null; do sleep 1; waited=$((waited + 1)); done
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid"
    status=$?
    if [ "$status" != 0 ]; then
      if [ "$(dialog "Plutonium stopped right away. Last lines of the log:

$(tail -n 20 "$log")" "Open log" "OK")" = "Open log" ]; then open "$log"; fi
      x87_check "$log"
      return 1
    fi
  fi
  wait "$pid"
  "$WINESERVER" -w 2>/dev/null
  x87_check "$log"
}

case "${1:-}" in
kill) use_env; "$WINESERVER" -k 2>/dev/null; exit 0 ;;
log) open "$logs/last-run.log"; exit $? ;;
selftest)
  # The Wine tests (tests/cases.txt), then the legacy ntdll's basic
  # breakpoint check; the selected ntdll comes back afterwards.
  # AIO_TEST_RUN replaces tests/run.sh (tests).
  [ -d "$root/runner/engine" ] || stop "Run ./setup.sh first (in $root)."
  if [ -n "${AIO_TEST_RUN:-}" ]; then read -r -a run <<< "$AIO_TEST_RUN"; else run=("$root/tests/run.sh"); fi
  status=0
  "${run[@]}" --ntdll fixed || status=1
  "${run[@]}" --ntdll legacy "breakpoint-test self" || status=1
  do_select ntdll "${NTDLL:-fixed}"
  exit $status ;;
update|"") ;;
*) echo "usage: ./launch.sh [kill|update|log|selftest]" >&2; exit 2 ;;
esac

[ -d "$root/runner/engine" ] && [ -f "$root/prefix/system.reg" ] || stop "Run ./setup.sh first (in $root)."
problem=$(options_validate 2>&1) || stop "$problem"
check_runner_files
use_env
take_lock
if [ "$dry" != 1 ]; then
  "$WINESERVER" -k 2>/dev/null
  "$WINESERVER" -w 2>/dev/null
fi
export AIO_LOG=$logs/launch.log
apply_switches
if [ "${GAME:-}" = BO2_SP ]; then
  bo2sp_prepare
  [ "$dry" = 1 ] || cd "$bo2sp_game" || stop "Cannot open $bo2sp_game."
  cmd=("$WINE" "C:\\Program Files (x86)\\Steam\\steam.exe" "Z:${bo2sp_game//\//\\}\\t6sp.exe")
  if [ "$dry" = 1 ]; then echo "exec ${cmd[*]}"; exit 0; fi
  run_logged "${cmd[@]}"
  exit $?
fi
steam_clear
bo1_prepare "$plutonium"
bo2_prepare "$plutonium"
if [ "${1:-}" = update ]; then PLUTONIUM_START=updater; fi
target=$(start_target) || stop "Plutonium is not installed in this project: run ./setup.sh."
if [ "$dry" = 1 ]; then echo "exec $WINE $target"; exit 0; fi
run_logged "$WINE" "$target"
