#!/bin/bash
# Stand-in for wine and wineserver in the shell tests: logs each call to
# $AIO_STUB_LOG; "wineboot -i" creates the prefix's system.reg; running an
# .exe exits with $AIO_STUB_EXIT (default 0).
echo "$(basename "$0") $*" >> "${AIO_STUB_LOG:-/dev/null}"
if [ "${1:-}" = wineboot ]; then
  mkdir -p "$WINEPREFIX/drive_c/windows/syswow64"
  echo "WINE REGISTRY Version 2" > "$WINEPREFIX/system.reg"
fi
case "${1:-}" in *.exe) exit "${AIO_STUB_EXIT:-0}" ;; esac
exit 0
