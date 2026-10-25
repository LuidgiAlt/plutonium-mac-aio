#!/bin/bash
# lib/steam.sh finds the games in any Steam library (Review Focus 2).
set -u
test_name=steam  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
# shellcheck source=lib/steam.sh
. "$repo_root/lib/steam.sh"

make_tmp
one="$tmp/Steam One"
two="$tmp/External Drive/SteamLibrary"
mkdir -p "$one/steamapps/common" "$two/steamapps/common/Call of Duty Black Ops II"
sed "s|LIBRARY_ONE|$one|; s|LIBRARY_TWO|$two|" "$repo_root/tests/fixtures/libraryfolders.vdf" > "$tmp/libraryfolders.vdf"
export STEAM_VDF=$tmp/libraryfolders.vdf
unset BO1_DIR BO2_DIR

assert_eq "$(steam_game_dir bo2)" "$two/steamapps/common/Call of Duty Black Ops II"
if steam_game_dir bo1 > /dev/null; then fail "BO1 found where it is not installed"; fi

mkdir -p "$tmp/elsewhere/BO2"
assert_eq "$(BO2_DIR="$tmp/elsewhere/BO2" steam_game_dir bo2)" "$tmp/elsewhere/BO2"
if BO1_DIR="$tmp/none" steam_game_dir bo1 > /dev/null; then fail "a missing BO1_DIR accepted"; fi
pass
