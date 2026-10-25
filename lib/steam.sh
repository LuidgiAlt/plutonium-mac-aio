# Where Steam installed the games.
# shellcheck shell=bash

# steam_game_dir bo1|bo2: prints the game folder. BO1_DIR / BO2_DIR win;
# otherwise every library in Steam's libraryfolders.vdf ($STEAM_VDF
# overrides it) is searched, then Steam's default folder.
steam_game_dir() {
  local override folder vdf lib
  case "$1" in
  bo1) override=${BO1_DIR:-} folder="Call of Duty Black Ops" ;;
  bo2) override=${BO2_DIR:-} folder="Call of Duty Black Ops II" ;;
  *) return 2 ;;
  esac
  if [ -n "$override" ]; then
    [ -d "$override" ] || return 1
    echo "$override"
    return 0
  fi
  vdf=${STEAM_VDF:-$HOME/Library/Application Support/Steam/steamapps/libraryfolders.vdf}
  while IFS= read -r lib; do
    if [ -d "$lib/steamapps/common/$folder" ]; then
      echo "$lib/steamapps/common/$folder"
      return 0
    fi
  done < <(
    if [ -f "$vdf" ]; then sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf"
    else echo "$HOME/Library/Application Support/Steam"; fi
  )
  return 1
}
