# Environment and small helpers shared by setup.sh, launch.sh and the tests.
# shellcheck shell=bash

# aio_env <root>: the runner (runner/CrossOver) and prefix (prefix/, unless
# WINEPREFIX is already set) of the project at <root>. Sets $WINE.
aio_env() {
  local runner=$1/runner/CrossOver
  export CX_ROOT="$runner"
  export WINEPREFIX="${WINEPREFIX:-$1/prefix}"
  export WINESERVER="$runner/bin/wineserver"
  export CX_HOME="$HOME/Library/Application Support/CrossOver"
  export WINEDLLPATH="$runner/lib/wine/x86_64-windows:$runner/lib/wine/x86_64-unix"
  export PATH="$runner/bin:/usr/bin:/bin:/usr/sbin:/sbin"
  export WINEMSYNC=1
  export WINEDEBUG="${WINEDEBUG:--all}"
  WINE="$runner/lib/wine/x86_64-unix/wine"
}

# put <src> <dst>: install a file unless it is already identical, by rename,
# so nothing ever maps a half-written file. The temp name is unique, so two
# runs at once cannot collide.
put() {
  cmp -s "$1" "$2" && return 0
  local t
  t=$(mktemp "$2.XXXXXX") || return 1
  cp "$1" "$t" && chmod "$(stat -f %Lp "$1")" "$t" && mv -f "$t" "$2" || { rm -f "$t"; return 1; }
}

# reg <args...>: Wine's reg in the prefix. Its messages go to $AIO_LOG; the
# exit status is returned, not hidden.
reg() { "$WINE" reg "$@" >> "${AIO_LOG:-/dev/null}" 2>&1; }

log_info() { echo "$*"; [ -n "${AIO_LOG:-}" ] && echo "$(date '+%H:%M:%S') $*" >> "$AIO_LOG"; return 0; }
log_error() { echo "$*" >&2; [ -n "${AIO_LOG:-}" ] && echo "$(date '+%H:%M:%S') error: $*" >> "$AIO_LOG"; return 0; }

# dialog <message> [button...]: a macOS dialog; prints the button pressed.
# The last button is the default. With AIO_NO_DIALOG=1 (tests, Terminal use
# without a GUI) it prints the message to stderr and answers the first button.
dialog() {
  local msg=$1 b list=""
  shift
  [ $# -gt 0 ] || set -- OK
  if [ "${AIO_NO_DIALOG:-0}" = 1 ]; then
    echo "$msg" >&2
    echo "$1"
    return 0
  fi
  for b in "$@"; do list="$list${list:+, }\"${b//\"/\\\"}\""; done
  osascript -e "display dialog \"${msg//\"/\\\"}\" with title \"Plutonium\" buttons {$list} default button ${#}" \
    -e 'button returned of result' 2>/dev/null || echo ""
}
