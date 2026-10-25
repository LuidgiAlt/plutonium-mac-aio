#!/bin/bash
# lib/games.sh edits each game's configs with its own values only.
set -u
test_name=games  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"
# shellcheck source=lib/games.sh
. "$repo_root/lib/games.sh"

make_tmp
p=$tmp/plutonium
cp -R "$repo_root/tests/fixtures/plutonium" "$p"
t5=$p/storage/t5/players/config_mp.cfg t6=$p/storage/t6/players/plutonium_mp.cfg
t6_before=$(cat "$t6")
t5sp=$p/storage/t5/players/config.cfg

unset TWEAKS WINDOWED FULLSCREEN
bo1_prepare "$p"
assert_contains "$(cat "$t5")" 'seta r_aaSamples "4"'
assert_contains "$(cat "$t5")" 'seta r_blur_allowed "0"'
assert_contains "$(cat "$t5")" 'seta r_distortion "0"'
assert_contains "$(cat "$t5")" 'seta com_maxfps "0"'
assert_eq "$(cat "$t6")" "$t6_before" "bo1_prepare touched BO2's config"
# A missing BO1 config is written with safe values (the game's first-run
# detection turns on shadows and 4x MSAA); an existing one is only edited.
assert_file "$t5sp"
assert_contains "$(cat "$t5sp")" 'seta com_first_time_pc "0"'
assert_contains "$(cat "$t5sp")" 'seta sm_enable "0"'
assert_contains "$(cat "$t5sp")" 'seta r_aaSamples "2"'
assert_contains "$(cat "$t5sp")" 'seta r_blur_allowed "0"'
case "$(cat "$t5")" in *com_first_time_pc*) fail "bo1_prepare replaced an existing config" ;; esac
echo 'seta sm_enable "1"' >> "$t5sp"
bo1_prepare "$p"
assert_contains "$(cat "$t5sp")" 'seta sm_enable "1"'

t5_before=$(cat "$t5")
bo2_prepare "$p"
assert_contains "$(cat "$t6")" 'seta r_aaSamples "4"'
assert_contains "$(cat "$t6")" 'seta com_maxfps "85"'
assert_eq "$(cat "$t5")" "$t5_before" "bo2_prepare touched BO1's config"

cp -R "$repo_root/tests/fixtures/plutonium/." "$p/"
TWEAKS=0 bo1_prepare "$p"
assert_contains "$(cat "$t5")" 'seta r_blur_allowed "1"'
assert_contains "$(cat "$t5")" 'seta r_aaSamples "4"'

WINDOWED=1 bo2_prepare "$p"
assert_contains "$(cat "$t6")" 'seta r_fullscreen "0"'
FULLSCREEN=1 bo1_prepare "$p"
assert_contains "$(cat "$t5")" 'seta r_fullscreen "1"'

bo1_prepare "$tmp/no-such-plutonium" || fail "missing configs must not be an error"
[ ! -e "$tmp/no-such-plutonium" ] || fail "bo1_prepare created a Plutonium folder"
pass
