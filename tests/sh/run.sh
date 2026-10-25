#!/bin/bash
# Run every shell test in tests/sh (or the ones named) and summarise.
#   tests/sh/run.sh [name...]     e.g. tests/sh/run.sh runner games
set -u
here=$(cd -- "$(dirname -- "$0")" && pwd)
if [ $# -gt 0 ]; then
  tests=()
  for n in "$@"; do tests+=("$here/${n%.sh}.sh"); done
else
  tests=()
  for t in "$here"/*.sh; do
    case "$(basename "$t")" in run.sh|assert.sh) ;; *) tests+=("$t") ;; esac
  done
fi
failed=0
for t in "${tests[@]}"; do
  if bash "$t"; then :; else failed=$((failed + 1)); echo "FAIL $(basename "$t")" >&2; fi
done
echo "shell tests: $((${#tests[@]} - failed))/${#tests[@]} passed"
[ "$failed" = 0 ]
