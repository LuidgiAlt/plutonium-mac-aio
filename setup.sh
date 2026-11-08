#!/bin/bash
# One-time setup of Plutonium BO1/BO2 for this Mac. Safe to run again:
# finished steps are skipped. See README.md.
#   ./setup.sh             engine, runner, prefix, a fresh Plutonium
#   ./setup.sh uninstall   remove what setup made (asks first)
# Options (environment variables):
#   BUILD=1            build the engine from source (build/build.sh) instead of
#                      downloading the release's
#   AIO_ENGINE=...     an engine built elsewhere (default build/out/wswine.bundle)
#   BO1_DIR=... BO2_DIR=...  game folders, if Steam's libraries do not have them
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/env.sh
. "$root/lib/env.sh"
# shellcheck source=lib/fetch.sh
. "$root/lib/fetch.sh"
# shellcheck source=lib/runner.sh
. "$root/lib/runner.sh"
# shellcheck source=lib/steam.sh
. "$root/lib/steam.sh"

downloads=${AIO_DOWNLOADS:-$root/downloads}
runner=$root/runner/engine
plutonium_rel="drive_c/users/crossover/AppData/Local/Plutonium"

die() { log_error "setup: $*"; exit 1; }
skip() { log_info "skip $1"; }
done_mark() { echo "${2:-done}" > "$1"; }

use_env() {
  aio_env "$root"
  WINE=${AIO_WINE:-$WINE}
  WINESERVER=${AIO_WINESERVER:-$WINESERVER}
}

uninstall() {
  local paths=(runner prefix downloads logs tests/bin tests/prefix) p any=0
  echo "This removes from $root:"
  for p in "${paths[@]}"; do [ -e "$root/$p" ] && { echo "  $p"; any=1; }; done
  [ "$any" = 1 ] || { echo "  (nothing)"; return 0; }
  printf 'Remove them? [y/N] '
  read -r answer || answer=
  case "$answer" in y|Y|yes) ;; *) echo "nothing removed"; return 1 ;; esac
  [ -x "$runner/bin/wineserver" ] && WINEPREFIX="$root/prefix" "$runner/bin/wineserver" -k 2>/dev/null || true
  for p in "${paths[@]}"; do rm -rf "${root:?}/$p"; done
  echo "removed (Steam and your games were not touched)"
}

step_preflight() {
  [ "$(uname -m)" = arm64 ] || die "this is for Apple Silicon Macs"
  arch -x86_64 /usr/bin/true 2>/dev/null || die "Rosetta is not installed: softwareupdate --install-rosetta"
  steam_game_dir bo1 > /dev/null || log_info "warning: BO1 not found in Steam's libraries (set BO1_DIR); its native XAudio2 will be missing"
  steam_game_dir bo2 > /dev/null || log_info "warning: BO2 not found in Steam's libraries (set BO2_DIR)"
}

# engine_download <url> <sha>: the release's engine archive, extracted once
# into downloads/engine (its test programs into tests/bin); prints the
# engine's path.
engine_download() {
  local dir=$downloads/engine
  if [ "$(cat "$dir/.sha" 2>/dev/null)" != "$2" ]; then
    mkdir -p "$downloads"
    log_info "downloading the engine" >&2
    fetch "$1" "$downloads/plutonium-mac-aio-engine.tar.xz" "$2" || die "download of the engine failed: $1"
    rm -rf "$dir" && mkdir -p "$dir"
    tar -xJf "$downloads/plutonium-mac-aio-engine.tar.xz" -C "$dir" || die "the engine archive is damaged"
    if [ -d "$dir/tests/bin" ]; then
      rm -rf "$root/tests/bin" && mkdir -p "$root/tests" && cp -R "$dir/tests/bin" "$root/tests/bin"
    fi
    echo "$2" > "$dir/.sha"
  fi
  echo "$dir/wswine.bundle"
}

# The engine: AIO_ENGINE or a local build (build/out), else the release's
# (pinned in manifest.txt), or one built now with BUILD=1. A different
# engine replaces the runner's, keeping the prefix.
step_runner() {
  local engine=${AIO_ENGINE:-$root/build/out/wswine.bundle} url sha
  if [ "${BUILD:-0}" = 1 ]; then
    log_info "engine: building Wine from source (about an hour the first time; build/build.sh)"
    "$root/build/build.sh" || die "build failed (see the log it names)"
    engine=$root/build/out/wswine.bundle
  elif [ ! -f "$engine/lib/wine/x86_64-unix/ntdll.so.fixed" ]; then
    if read -r url sha < <(manifest_get plutonium-mac-aio-engine.tar.xz); then
      engine=$(engine_download "$url" "$sha")
    elif [ -d "$runner" ]; then
      skip runner; return
    else
      die "manifest.txt names no prebuilt engine: build one with BUILD=1 ./setup.sh"
    fi
  fi
  [ -f "$engine/lib/wine/x86_64-unix/ntdll.so.fixed" ] || die "no engine at $engine"
  if [ -d "$runner" ] && [ "$(cat "$root/runner/.version" 2>/dev/null)" = "$(engine_stamp "$engine")" ]; then
    skip runner; return
  fi
  read -r url sha < <(manifest_get Sikarugir-Template.tar.xz) || die "manifest.txt has no Sikarugir-Template.tar.xz"
  mkdir -p "$downloads"
  log_info "downloading Sikarugir's wrapper template"
  fetch "$url" "$downloads/Sikarugir-Template.tar.xz" "$sha" || die "download of Sikarugir's template failed: $url"
  log_info "runner: Sikarugir's wrapper template with the engine"
  runner_create "$root" "$downloads/Sikarugir-Template.tar.xz" "$engine" || die "could not make the runner"
}

# The mark holds the archive's checksum, so a new DXMT in manifest.txt is
# installed by the next setup run.
step_thirdparty() {
  local mark=$root/runner/.thirdparty name=dxmt-v0.80-builtin.tar.gz url sha
  read -r url sha < <(manifest_get "$name") || die "manifest.txt has no $name"
  if [ -f "$mark" ] && [ "$(cat "$mark")" = "$sha" ]; then skip thirdparty; return; fi
  mkdir -p "$downloads"
  log_info "downloading $name"
  fetch "$url" "$downloads/$name" "$sha" || die "download of $name failed: $url"
  runner_thirdparty "$root" "$downloads" || die "could not install DXMT"
  runner_select_ntdll "$root" fixed || die "could not select the patched ntdll"
  done_mark "$mark" "$sha"
}

step_prefix() {
  local mark=$root/prefix/.aio-registry
  if [ -f "$root/prefix/system.reg" ] && [ -f "$mark" ]; then skip prefix; return; fi
  use_env
  log_info "prefix: creating $WINEPREFIX"
  [ -f "$WINEPREFIX/system.reg" ] || "$WINE" wineboot -i >> "$AIO_LOG" 2>&1 || die "wineboot failed (see logs/setup.log)"
  # Plutonium starts the games with an environment built from HKCU\Environment.
  reg add 'HKCU\Environment' /v WINE_ROSETTA_EXEC_BREAKPOINTS /d 1 /f
  reg add 'HKCU\Environment' /v WINE_ROSETTA_WOW64_BREAKPOINTS /d 1 /f
  reg add 'HKCU\Environment' /v WINE_ROSETTA_CALL_WORKAROUND /d 1 /f
  reg add 'HKCU\Environment' /v WINE_ROSETTA_LAZY_EXEC /d 1 /f
  # Sony pads through SDL as XInput gamepads (the games read XInput only).
  reg add 'HKLM\System\CurrentControlSet\Services\WineBus\Devices\054c' /v Hidraw /t REG_DWORD /d 0 /f
  # The launcher's window fails on wined3d's Vulkan renderer with MoltenVK 1.4.2.
  reg add 'HKCU\Software\Wine\AppDefaults\plutonium-launcher-win32.exe\Direct3D' /v renderer /d gl /f
  # Both games run in the bootstrapper: BO2 needs no 3D from wined3d (DXMT),
  # BO1's d3d9 is DXVK's.
  reg add 'HKCU\Software\Wine\AppDefaults\plutonium-bootstrapper-win32.exe\Direct3D' /v renderer /d no3d /f
  "$WINESERVER" -w 2>/dev/null || true
  done_mark "$mark"
}

# Microsoft's XAudio2 2.7 from BO1's redistributables decodes BO1's xWMA
# sounds (Wine's has no WMA decoder); shared with BO2. Its own step, so
# that a later run adds it once BO1 can be found.
step_xaudio() {
  local bo1 cab tmp
  if [ -f "$root/prefix/xaudio2_7.native.dll" ]; then skip xaudio; return; fi
  if ! bo1=$(steam_game_dir bo1); then log_info "xaudio: BO1 not found; run setup again once it is (or set BO1_DIR)"; return; fi
  cab="$bo1/Redist/DirectX/Jun2010_XAudio_x86.cab"
  [ -f "$cab" ] || { log_info "xaudio: no $cab"; return; }
  use_env
  tmp=$(mktemp -d)
  if bsdtar -xf "$cab" -C "$tmp" XAudio2_7.dll 2>/dev/null; then
    mkdir -p "$WINEPREFIX/drive_c/windows/syswow64"
    cp "$tmp/XAudio2_7.dll" "$WINEPREFIX/drive_c/windows/syswow64/xaudio2_7.dll"
    reg add 'HKCU\Software\Wine\AppDefaults\plutonium-bootstrapper-win32.exe\DllOverrides' /v xaudio2_7 /d native /f
    cp "$tmp/XAudio2_7.dll" "$WINEPREFIX/xaudio2_7.native.dll"
    log_info "xaudio: Microsoft's XAudio2 2.7 installed from BO1"
  else
    log_info "xaudio: could not extract XAudio2_7.dll from $cab"
  fi
  rm -rf "$tmp"
}

step_plutonium() {
  local dir=$root/prefix/$plutonium_rel url sha
  if [ "${AIO_SKIP_PLUTONIUM:-0}" = 1 ]; then skip plutonium; return; fi
  [ -L "$dir" ] && die "$dir is a link; this project uses its own Plutonium, remove the link"
  if [ -f "$dir/bin/plutonium-launcher-win32.exe" ]; then skip plutonium; return; fi
  read -r url sha < <(manifest_get plutonium.exe) || die "manifest.txt has no plutonium.exe"
  mkdir -p "$downloads"
  log_info "downloading Plutonium's installer from $url"
  fetch "$url" "$downloads/plutonium.exe" "$sha" || die "download of plutonium.exe failed"
  python3 -I "$root/tools/pe-cert.py" "$downloads/plutonium.exe" || die "plutonium.exe is not a signed Windows program"
  if command -v osslsigncode > /dev/null; then
    osslsigncode verify -in "$downloads/plutonium.exe" > /dev/null 2>&1 || die "plutonium.exe: signature check failed"
  fi
  use_env
  log_info "Plutonium's installer is starting: log in with your Plutonium account, then close it"
  "$WINE" "$downloads/plutonium.exe" >> "$AIO_LOG" 2>&1 || true
  "$WINESERVER" -w 2>/dev/null || true
}

# Valve's Windows steamclient.dll for GAME=BO2_SP (see launch.sh): the
# campaign's DRM loads it and the engine redirects it to the macOS Steam app.
# It comes from a package of Valve's Linux Steam client (manifest.txt); this
# installs no Steam. Kept outside drive_c: launch.sh puts it in place for the
# campaign only (Plutonium looks for Steam too). Only when BO2 is there; a
# later run adds it otherwise.
steamclient_sha=e9e961c914a418b6a3c597822bdc920bdea62c842ff42de23dc306331da563f8
step_steam() {
  local dst=$root/prefix/steamclient.valve.dll bo2 url sha tmp
  if [ -f "$dst" ] && [ "$(sha256_of "$dst")" = "$steamclient_sha" ]; then skip steam; return; fi
  if ! bo2=$(steam_game_dir bo2) || [ ! -f "$bo2/t6sp.exe" ]; then
    log_info "steam: BO2's campaign not found; run setup again once it is (or set BO2_DIR)"
    return
  fi
  read -r url sha < <(manifest_get steam-bins-misc.zip) || die "manifest.txt has no steam-bins-misc.zip"
  mkdir -p "$downloads"
  log_info "downloading Valve's steamclient.dll for BO2's campaign (GAME=BO2_SP)"
  fetch "$url" "$downloads/steam-bins-misc.zip" "$sha" || die "download of steam-bins-misc.zip failed: $url"
  tmp=$(mktemp "$dst.XXXXXX")
  unzip -p "$downloads/steam-bins-misc.zip" legacycompat/steamclient.dll > "$tmp" &&
    [ "$(sha256_of "$tmp")" = "$steamclient_sha" ] && chmod 644 "$tmp" && mv -f "$tmp" "$dst" ||
    { rm -f "$tmp"; die "steamclient.dll in steam-bins-misc.zip is not the expected one"; }
}

step_folders() {
  local game dir
  echo "In the Plutonium launcher, choose these game folders:"
  for game in bo1 bo2; do
    if dir=$(steam_game_dir "$game"); then echo "  $game: Z:${dir//\//\\}"
    else echo "  $game: not found (set $(echo "$game" | tr a-z A-Z)_DIR and run setup again)"; fi
  done
}

case "${1:-}" in
uninstall) uninstall; exit $? ;;
"") ;;
*) echo "usage: ./setup.sh [uninstall]" >&2; exit 2 ;;
esac

mkdir -p "$root/logs"
export AIO_LOG=$root/logs/setup.log
step_preflight
step_runner
step_thirdparty
step_prefix
step_xaudio
step_plutonium
step_steam
step_folders
log_info "setup done: start with ./launch.sh"
