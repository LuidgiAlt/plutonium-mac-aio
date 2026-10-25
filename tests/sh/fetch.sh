#!/bin/bash
# lib/fetch.sh: downloads are checked and atomic (Review Focus 3: an
# interrupted or wrong download leaves nothing a re-run would trust).
set -u
test_name=fetch  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
# shellcheck source=lib/fetch.sh
. "$repo_root/lib/fetch.sh"

make_tmp
echo "payload" > "$tmp/source.bin"
sha=$(shasum -a 256 "$tmp/source.bin" | cut -d' ' -f1)
url="file://${tmp// /%20}/source.bin"  # the temp path has a space

fetch "$url" "$tmp/ok.bin" "$sha" || fail "fetch with the right SHA failed"
assert_eq "$(cat "$tmp/ok.bin")" "payload"
assert_no_file "$tmp/ok.bin.part"

if fetch "$url" "$tmp/bad.bin" 0000000000000000000000000000000000000000000000000000000000000000; then fail "wrong SHA accepted"; fi
assert_no_file "$tmp/bad.bin"
assert_no_file "$tmp/bad.bin.part"

if fetch "file://${tmp// /%20}/missing.bin" "$tmp/gone.bin" -; then fail "unreachable URL accepted"; fi
assert_no_file "$tmp/gone.bin"
assert_no_file "$tmp/gone.bin.part"

# An existing file with the right SHA is kept (not downloaded again).
echo "changed" > "$tmp/source.bin"
fetch "$url" "$tmp/ok.bin" "$sha" || fail "existing good file refetched"
assert_eq "$(cat "$tmp/ok.bin")" "payload"

# manifest_get reads "name url sha" lines.
printf '# comment\nthing https://example.invalid/thing.tgz abc123\n' > "$tmp/manifest.txt"
AIO_MANIFEST=$tmp/manifest.txt
assert_eq "$(manifest_get thing)" "https://example.invalid/thing.tgz abc123"
if manifest_get nothing > /dev/null; then fail "missing manifest entry found"; fi
pass
