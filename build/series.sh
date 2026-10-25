#!/bin/bash
# Edit a patch series as git commits.
#   build/series.sh import <wine|dxvk>   build/src/<kind>: tag "base" = pristine,
#                                        then one commit per patches/<kind>/*.patch
#   build/series.sh export <wine|dxvk>   rewrite each patch file whose commit
#                                        changed since import (header kept)
# Workflow for a fix to patch N: edit build/src/<kind>, then
#   git commit --fixup=<commit of N>
#   GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash base
#   build/series.sh export <kind>
# and edit the header of patches/<kind>/<N>-*.patch by hand.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=build/common.sh
. "$root/build/common.sh"

kind=${2:-}
case "$kind" in wine|dxvk) ;; *) die "usage: build/series.sh import|export wine|dxvk" ;; esac
tree=$src/$kind
g() { git -C "$tree" -c user.name=series -c user.email=series@localhost "$@"; }
diff_sha() { g diff --no-color --no-ext-diff "$1~1" "$1" | shasum -a 256 | cut -d' ' -f1; }

import() {
  fresh_tree "$kind"
  g add -A
  g commit -q --no-verify -m base
  g tag base
  for patch in "$root/patches/$kind"/0*.patch; do
    g apply --whitespace=nowarn "$patch" || die "patches/$kind/$(basename "$patch") does not apply"
    g add -A
    g commit -q --no-verify -m "$(basename "$patch")"
    g commit -q --no-verify --amend -m "$(basename "$patch")" -m "Import-Diff: $(diff_sha HEAD)"
  done
  echo "build/src/$kind: $(g rev-list --count base..HEAD) patches imported"
}

export_series() {
  [ -d "$tree/.git" ] || die "run build/series.sh import $kind first"
  local c name file trailer sha header
  for c in $(g rev-list --reverse base..HEAD); do
    name=$(g log -1 --format=%s "$c")
    file=$root/patches/$kind/$name
    [ -f "$file" ] || die "commit $c names $name, which is not in patches/$kind"
    trailer=$(g log -1 --format=%B "$c" | sed -n 's/^Import-Diff: //p')
    sha=$(diff_sha "$c")
    [ "$sha" = "$trailer" ] && continue
    header=$(patch_header "$file")
    { printf '%s\n' "$header"; g diff --no-color --no-ext-diff "$c~1" "$c"; } > "$file.new"
    mv -f "$file.new" "$file"
    echo "exported $name"
  done
}

case "${1:-}" in
import) import ;;
export) export_series ;;
*) die "usage: build/series.sh import|export wine|dxvk" ;;
esac
