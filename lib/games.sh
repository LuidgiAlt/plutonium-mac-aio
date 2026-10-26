# Per-game settings, applied to each game's own config files at every
# launch (both games run in the same Plutonium, so each keeps its own).
# shellcheck shell=bash

# set_cvar <file> <name> <value>: an existing "seta <name> ..." line only.
set_cvar() { sed -i '' -E "s/^seta $2 \"[^\"]*\"/seta $2 \"$3\"/" "$1"; }

# Apple GPUs support at most 4x MSAA; a config asking for 8x or more crashes.
clamp_msaa() { sed -i '' -E 's/^seta r_aaSamples "(8|16|32)"/seta r_aaSamples "4"/' "$1"; }

display_mode() {
  if [ "${WINDOWED:-0}" = 1 ]; then set_cvar "$1" r_fullscreen 0; fi
  if [ "${FULLSCREEN:-0}" = 1 ]; then set_cvar "$1" r_fullscreen 1; fi
}

# bo1_seed <cfg> <msaa>: a first config for a BO1 mode that has none. The
# game's first-run detection turns on shadows (BO1 froze with them) and 4x
# MSAA; these are the values of the setup that played well. The game writes
# the rest of the file when it exits.
bo1_seed() {
  [ -f "$1" ] && return 0
  mkdir -p "$(dirname "$1")"
  printf 'seta %s "%s"\n' com_first_time_pc 0 sm_enable 0 r_aaSamples "$2" \
    r_blur_allowed 0 r_distortion 0 com_maxfps 0 > "$1"
}

# bo1_prepare <plutonium_dir>: BO1 (T5). Blur, distortion and the frame cap
# off unless TWEAKS=0 (they cost a lot under Rosetta). A mode with no config
# yet gets a safe first one.
bo1_prepare() {
  local cfg
  if [ -d "$1/storage/t5" ]; then
    bo1_seed "$1/storage/t5/players/config.cfg" 2
    bo1_seed "$1/storage/t5/players/config_mp.cfg" 1
  fi
  for cfg in "$1"/storage/t5/players/*.cfg; do
    [ -f "$cfg" ] || continue
    clamp_msaa "$cfg"
    display_mode "$cfg"
    if [ "${TWEAKS:-1}" != 0 ]; then
      set_cvar "$cfg" r_blur_allowed 0
      set_cvar "$cfg" r_distortion 0
      set_cvar "$cfg" com_maxfps 0
    fi
  done
  return 0
}

# bo2_prepare <plutonium_dir>: BO2 (T6). BO2's MSAA comes from its binary
# hardware profile, which overrides the text config. With none in storage it
# falls back to the one in the game's players folder, which another renderer
# may have written with 8x (Apple GPUs stop at 4x: the game crashes). An
# invalid profile (zeros) in storage makes BO2 detect its own instead, which
# picks no MSAA under DXMT, and replace it.
bo2_prepare() {
  local cfg mode chp
  if [ -d "$1/storage/t6" ]; then
    mkdir -p "$1/storage/t6/players"
    for mode in mp zm; do
      chp=$1/storage/t6/players/hardware_$mode.chp
      [ -f "$chp" ] || head -c 4096 /dev/zero > "$chp"
    done
  fi
  for cfg in "$1"/storage/t6/players/plutonium_mp.cfg "$1"/storage/t6/players/plutonium_zm.cfg; do
    [ -f "$cfg" ] || continue
    clamp_msaa "$cfg"
    display_mode "$cfg"
  done
  return 0
}
