# Helpers for tests/sh/*.sh. Source this file; each test script exits non-zero
# on the first failed assertion.
# shellcheck shell=bash

# shellcheck disable=SC2034  # used by the test scripts that source this file
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

fail() { echo "FAIL ${test_name:-?}: $*" >&2; exit 1; }
pass() { echo "PASS ${test_name:-?}"; }

assert_eq() { [ "$1" = "$2" ] || fail "expected '$2', got '$1'${3:+ ($3)}"; }
assert_contains() { case "$1" in *"$2"*) ;; *) fail "output does not contain '$2'${3:+ ($3)}" ;; esac; }
assert_file() { [ -e "$1" ] || fail "missing $1"; }
assert_no_file() { [ ! -e "$1" ] || fail "unexpected $1"; }

# Sets $tmp to a new directory whose name contains a space (Review Focus 1),
# removed on exit. Call it directly, not in $(...).
make_tmp() {
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/aio test.XXXXXX")
  # shellcheck disable=SC2064
  trap "rm -rf '$tmp'" EXIT
}
