#!/bin/bash
# Run the Wine-side test programs (tests/cases.txt) in the project's runner,
# in a throwaway prefix (tests/prefix).
#   tests/run.sh [--ntdll fixed|legacy|both] [--d3d9 dxvk|vulkan|gl|both] [case...]
# (gl: the patched wined3d on OpenGL; "both" means dxvk and vulkan)
# A case argument selects every case whose name starts with it. Defaults:
# --ntdll fixed --d3d9 both. Leaves the runner on the last renderer/ntdll
# used; launch.sh selects its own on every start.
# AIO_ROOT: project with runner/ (default: this repository).
# AIO_TEST_BIN: test programs (default: build/out/tests/bin, then tests/bin).
set -u
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/.." && pwd)
root=${AIO_ROOT:-$repo}
# shellcheck source=lib/env.sh
. "$repo/lib/env.sh"
# shellcheck source=lib/runner.sh
. "$repo/lib/runner.sh"

want_ntdll=fixed want_d3d9=both
filters=()
while [ $# -gt 0 ]; do
  case "$1" in
  --ntdll) want_ntdll=$2; shift 2 ;;
  --d3d9) want_d3d9=$2; shift 2 ;;
  *) filters+=("$1"); shift ;;
  esac
done

bin=${AIO_TEST_BIN:-}
[ -n "$bin" ] || { [ -d "$repo/build/out/tests/bin" ] && bin=$repo/build/out/tests/bin; } || true
[ -n "$bin" ] || bin=$here/bin
[ -d "$root/runner/CrossOver" ] || { echo "no runner in $root (run setup.sh)" >&2; exit 2; }
[ -d "$bin" ] || { echo "no test programs in $bin (build/build.sh)" >&2; exit 2; }

export WINEPREFIX=$here/prefix
aio_env "$root"
logdir=$here/logs
mkdir -p "$logdir"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
  "$WINE" wineboot -i > "$logdir/wineboot.log" 2>&1
  "$WINESERVER" -w
fi

modes() { # modes <case value> <wanted> <both...>
  local c=$1 w=$2 all
  case "$c" in -) echo "-"; return ;; both) all="$3 $4" ;; *) all=$c ;; esac
  for m in $all; do
    [ "$w" = both ] || [ "$w" = "$m" ] && echo "$m"
  done
}

selected() {
  [ ${#filters[@]} = 0 ] && return 0
  local f
  for f in "${filters[@]}"; do case "$1" in "$f"*) return 0 ;; esac; done
  return 1
}

pass=0 fail=0 skipped=0 failed_names=()
while IFS='|' read -r name env ntdll d3d9 logcheck; do
  case "$name" in ''|'#'*) continue ;; esac
  selected "$name" || continue
  for n in $(modes "$ntdll" "$want_ntdll" fixed legacy); do
    for d in $(modes "$d3d9" "$want_d3d9" dxvk vulkan); do
      [ "$n" = - ] || runner_select_ntdll "$root" "$n" || exit 2
      if [ "$d" != - ]; then
        case "$d" in
        dxvk) runner_select_d3d9 "$root" dxvk && { reg delete 'HKCU\Software\Wine\Direct3D' /v renderer /f || true; } ;;
        vulkan) runner_select_d3d9 "$root" vulkan && reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d vulkan /f ;;
        gl) runner_select_d3d9 "$root" vulkan && reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d gl /f ;;
        esac || exit 2
      fi
      label="$name [ntdll $n, d3d9 $d]"
      log=$logdir/$(echo "$name-$n-$d" | tr ' /' '__').log
      prog=${name%% *}
      args=${name#"$prog"}
      # shellcheck disable=SC2086  # env and args are word lists by design
      (
        export DXVK_ASYNC="${DXVK_ASYNC:-0}" WINEDEBUG=err+all DXVK_LOG_PATH=none DXVK_STATE_CACHE=0
        [ "$env" = - ] || { IFS=,; for kv in $env; do export "${kv?}"; done; IFS=$' \t\n'; }
        cd "$bin" && exec perl -e 'alarm shift; exec @ARGV' 120 "$WINE" "$prog.exe" $args
      ) > "$log.out" 2> "$log"
      status=$?
      # A device that cannot even be created is the GPU being busy (another
      # app holding it), not a result: run the case once more.
      if [ "$status" != 0 ] && [ "$status" != 77 ] && grep -qE 'CreateDevice failed|draw failed \(2\)' "$log.out"; then
        echo "note: $label could not create a device; running it again" >&2
        (
          export DXVK_ASYNC="${DXVK_ASYNC:-0}" WINEDEBUG=err+all DXVK_LOG_PATH=none DXVK_STATE_CACHE=0
          [ "$env" = - ] || { IFS=,; for kv in $env; do export "${kv?}"; done; IFS=$' \t\n'; }
          # shellcheck disable=SC2086
          cd "$bin" && exec perl -e 'alarm shift; exec @ARGV' 120 "$WINE" "$prog.exe" $args
        ) > "$log.out" 2> "$log"
        status=$?
      fi
      line=$(grep -E '^(PASS|FAIL|SKIP) ' "$log.out" | tail -1)
      if [ "$status" = 0 ] && [ "$logcheck" != - ]; then
        re=${logcheck#count:}; re=${re%==*}; want=${logcheck##*==}
        got=$(grep -cE "$re" "$log")
        [ "$got" = "$want" ] || { status=1; line="${line:-FAIL $prog:} (log: '$re' $got times, expected $want)"; }
      fi
      case "$status" in
      0) pass=$((pass + 1)); echo "PASS $label" ;;
      77) skipped=$((skipped + 1)); echo "SKIP $label: ${line#SKIP * }" ;;
      *) fail=$((fail + 1)); failed_names+=("$label"); echo "FAIL $label: ${line:-exit $status, no result line} (log: $log)" ;;
      esac
    done
  done
done < "$here/cases.txt"
"$WINESERVER" -k 2>/dev/null || true
echo "wine tests: $pass passed, $fail failed, $skipped skipped"
[ "$fail" = 0 ]
