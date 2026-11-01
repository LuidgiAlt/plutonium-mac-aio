#!/bin/bash
# lib/options.sh accepts the documented values and names the first bad one.
set -u
test_name=options  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
# shellcheck source=lib/options.sh
. "$repo_root/lib/options.sh"

ok() { (unset NTDLL XAUDIO MAXFPS LAZY_EXEC SOCKFIX HUD WINDOWED FULLSCREEN TWEAKS VERBOSE; export "${@?}"; options_validate) || fail "rejected: $*"; }
bad() {
  local out
  if out=$(unset NTDLL XAUDIO MAXFPS; export "${1?}"; options_validate 2>&1); then fail "accepted: $1"; fi
  assert_contains "$out" "bad option: $1"
}
ok NTDLL=fixed; ok NTDLL=legacy; ok XAUDIO=native; ok XAUDIO=builtin
ok MAXFPS=60; ok LAZY_EXEC=0; ok SOCKFIX=1; ok HUD=1; ok WINDOWED=1; ok FULLSCREEN=0; ok TWEAKS=0; ok VERBOSE=1
(unset NTDLL XAUDIO MAXFPS LAZY_EXEC SOCKFIX HUD WINDOWED FULLSCREEN TWEAKS VERBOSE; options_validate) || fail "defaults rejected"
bad MAXFPS=abc; bad MAXFPS=0; bad HUD=2; bad NTDLL=old; bad XAUDIO=wine
pass
