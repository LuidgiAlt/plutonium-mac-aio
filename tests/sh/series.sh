#!/bin/bash
# build/series.sh turns a patch series into commits and back: a round trip
# without edits changes nothing, and a fixup to one patch rewrites only that
# patch file.
set -u
test_name=series  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
export AIO_DOWNLOADS="${AIO_DOWNLOADS:-$repo_root/downloads}"

make_tmp
mkdir "$tmp/repo"
(cd "$repo_root" && git ls-files -z | xargs -0 tar -cf - 2>/dev/null) | tar -xf - -C "$tmp/repo"
r=$tmp/repo
git -C "$r" init -q && git -C "$r" add -A && git -C "$r" -c user.name=t -c user.email=t@t commit -qm base

# (a) Round trip without edits.
"$r/build/series.sh" import wine > "$tmp/import.log" 2>&1 || fail "import failed: $(tail -3 "$tmp/import.log")"
"$r/build/series.sh" export wine > "$tmp/export.log" 2>&1 || fail "export failed: $(tail -3 "$tmp/export.log")"
assert_eq "$(git -C "$r" status --porcelain patches/)" "" "round trip changed patch files"

# (b) A fixup to 0015 rewrites only 0015, and the series still applies.
s=$r/build/src/wine
c=$(git -C "$s" log --format='%H %s' base..HEAD | awk '$2 ~ /^0015-/ {print $1}')
[ -n "$c" ] || fail "no commit for 0015"
printf '/* series test */\n' >> "$s/dlls/ntdll/unix/msync.c"
git -C "$s" -c user.name=t -c user.email=t@t commit -qam "fixup! $(git -C "$s" log -1 --format=%s "$c")"
GIT_SEQUENCE_EDITOR=: git -C "$s" -c user.name=t -c user.email=t@t rebase -q -i --autosquash base > /dev/null 2>&1 || fail "autosquash failed"
"$r/build/series.sh" export wine > "$tmp/export2.log" 2>&1 || fail "export failed: $(tail -3 "$tmp/export2.log")"
changed=$(git -C "$r" status --porcelain patches/ | awk '{print $2}')
assert_eq "$changed" "patches/wine/0015-ntdll-msync-pulse-event-yield-only-with-waiters.patch"
header_before=$(git -C "$r" show HEAD:"$changed" | awk '/^---$/{print; exit} {print}')
header_after=$(awk '/^---$/{print; exit} {print}' "$r/$changed")
assert_eq "$header_after" "$header_before" "header changed"
"$r/build/build.sh" --check > /dev/null 2>&1 || fail "--check fails after export"
grep -q 'series test' "$r/$changed" || fail "the fixup is not in the exported patch"
pass
