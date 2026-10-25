#!/bin/bash
# Start Plutonium for BO1 and BO2. See README.md.
#   ./launch.sh            start (through Plutonium's updater)
#   ./launch.sh kill       stop everything in this project's prefix
#   ./launch.sh update     run Plutonium's updater only
#   ./launch.sh log        open the last run's log
#   ./launch.sh selftest   check this installation without a game
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

crossover=${CROSSOVER_APP:-/Applications/CrossOver.app}
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
  local rc=$root/runner/CrossOver f
  for f in "lib/wine/x86_64-unix/ntdll.so.${NTDLL:-fixed}" bin/wineserver \
           "lib/wine/i386-windows/$([ "${D3D9:-dxvk}" = dxvk ] && echo d3d9.dll.dxvk || echo wined3d.dll.vk)"; do
    [ -f "$rc/$f" ] || stop "$(basename "$f") is missing from the runner: run ./setup.sh again (in $root)."
  done
}

apply_switches() {
  local w32=$root/runner/CrossOver/lib/wine/i386-windows syswow64=$WINEPREFIX/drive_c/windows/syswow64
  local bootstrapper='HKCU\Software\Wine\AppDefaults\plutonium-bootstrapper-win32.exe'
  do_select ntdll "${NTDLL:-fixed}"
  do_select d3d9 "${D3D9:-dxvk}"
  if [ "${D3D9:-dxvk}" = vulkan ]; then
    do_reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d vulkan /f
    do_reg delete "$bootstrapper\\Direct3D" /v renderer /f
  else
    do_reg delete 'HKCU\Software\Wine\Direct3D' /v renderer /f
    do_reg add "$bootstrapper\\Direct3D" /v renderer /d no3d /f
  fi
  # no3d reports a 128 MB GPU; BO1 reads its video memory through DirectDraw
  # and capped textures to it. 4096 MB is what the GPU renderers report.
  do_reg add "$bootstrapper\\Direct3D" /v VideoMemorySize /d 4096 /f
  # Plutonium starts the games with an environment built from HKCU\Environment.
  set_env WINE_ROSETTA_EXEC_BREAKPOINTS 1
  set_env WINE_ROSETTA_WOW64_BREAKPOINTS 1
  set_env WINE_ROSETTA_CALL_WORKAROUND 1
  set_env CX_GRAPHICS_BACKEND dxmt
  if [ "${LAZY_EXEC:-1}" = 1 ]; then set_env WINE_ROSETTA_LAZY_EXEC 1; else clear_env WINE_ROSETTA_LAZY_EXEC; fi
  if [ "${SOCKFIX:-1}" = 0 ]; then set_env WINE_SOCK_FASTPATH 0; else clear_env WINE_SOCK_FASTPATH; fi
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
      return 1
    fi
  fi
  wait "$pid"
  "$WINESERVER" -w 2>/dev/null
  return 0
}

case "${1:-}" in
kill) use_env; "$WINESERVER" -k 2>/dev/null; exit 0 ;;
log) open "$logs/last-run.log"; exit $? ;;
selftest)
  # The Wine tests (tests/cases.txt) on both renderers, then the legacy
  # ntdll's basic breakpoint check; the selected renderer and ntdll come
  # back afterwards. AIO_TEST_RUN replaces tests/run.sh (tests).
  [ -d "$root/runner/CrossOver" ] || stop "Run ./setup.sh first (in $root)."
  if [ -n "${AIO_TEST_RUN:-}" ]; then read -r -a run <<< "$AIO_TEST_RUN"; else run=("$root/tests/run.sh"); fi
  status=0
  "${run[@]}" --ntdll fixed --d3d9 both || status=1
  "${run[@]}" --ntdll legacy "breakpoint-test self" || status=1
  do_select ntdll "${NTDLL:-fixed}"
  do_select d3d9 "${D3D9:-dxvk}"
  exit $status ;;
update|"") ;;
*) echo "usage: ./launch.sh [kill|update|log|selftest]" >&2; exit 2 ;;
esac

[ -d "$root/runner/CrossOver" ] && [ -f "$root/prefix/system.reg" ] || stop "Run ./setup.sh first (in $root)."
runner_version_ok "$root" "$crossover" || stop "CrossOver was updated: run ./setup.sh again (in $root)."
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
bo1_prepare "$plutonium"
bo2_prepare "$plutonium"
if [ "${1:-}" = update ]; then PLUTONIUM_START=updater; fi
target=$(start_target) || stop "Plutonium is not installed in this project: run ./setup.sh."
if [ "$dry" = 1 ]; then echo "exec $WINE $target"; exit 0; fi
run_logged "$WINE" "$target"
