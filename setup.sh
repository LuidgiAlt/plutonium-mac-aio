#!/bin/bash
# One-time setup of Plutonium BO1/BO2 for this Mac. Safe to run again:
# finished steps are skipped. See README.md.
#   ./setup.sh             runner, binaries, prefix, a fresh Plutonium
#   ./setup.sh uninstall   remove what setup made (asks first)
# Options (environment variables):
#   CROSSOVER_APP=...  CrossOver.app (default /Applications/CrossOver.app)
#   BUILD=1            build the patched binaries from source instead of downloading them
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

crossover=${CROSSOVER_APP:-/Applications/CrossOver.app}
want_version=26.3.0.39832
downloads=${AIO_DOWNLOADS:-$root/downloads}
runner=$root/runner/CrossOver
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
  echo "removed (CrossOver.app, Steam and your games were not touched)"
}

step_preflight() {
  local version
  [ "$(uname -m)" = arm64 ] || die "this is for Apple Silicon Macs"
  arch -x86_64 /usr/bin/true 2>/dev/null || die "Rosetta is not installed: softwareupdate --install-rosetta"
  version=$(crossover_version "$crossover") || die "no CrossOver at $crossover (set CROSSOVER_APP)"
  [ "$version" = "$want_version" ] || die "needs CrossOver $want_version, found $version at $crossover"
  steam_game_dir bo1 > /dev/null || log_info "warning: BO1 not found in Steam's libraries (set BO1_DIR); its native XAudio2 will be missing"
  steam_game_dir bo2 > /dev/null || log_info "warning: BO2 not found in Steam's libraries (set BO2_DIR)"
}

step_runner() {
  if [ -d "$runner" ]; then
    if runner_version_ok "$root" "$crossover"; then skip runner; return; fi
    printf 'CrossOver changed: rebuild runner? [y/N] '
    read -r answer || answer=
    case "$answer" in y|Y|yes) ;; *) die "runner kept; run setup again to rebuild it" ;; esac
  fi
  log_info "runner: copying CrossOver's runtime (APFS clone)"
  runner_create "$root" "$crossover" || die "could not copy $crossover"
}

step_binaries() {
  local mark=$root/runner/.binaries tree source url sha
  if [ "${BUILD:-0}" = 1 ]; then
    source=build
  else
    read -r url sha < <(manifest_get plutonium-mac-aio-bin.tar.gz) || die "manifest.txt has no release archive"
    source=$sha
  fi
  if [ -f "$mark" ] && [ "$(cat "$mark")" = "$source" ]; then skip binaries; return; fi
  if [ "$source" = build ]; then
    log_info "binaries: building from source (build/build.sh, logs in build/obj)"
    "$root/build/build.sh" || die "build failed"
    tree=$root/build/out
  else
    mkdir -p "$downloads"
    log_info "binaries: downloading the release archive"
    fetch "$url" "$downloads/plutonium-mac-aio-bin.tar.gz" "$sha" || die "download of the release archive failed: $url"
    tree=$downloads/bin
    rm -rf "$tree" && mkdir -p "$tree"
    tar -xzf "$downloads/plutonium-mac-aio-bin.tar.gz" -C "$tree" || die "release archive is damaged"
  fi
  runner_install "$root" "$tree" || die "could not install the binaries"
  if [ -d "$tree/tests/bin" ]; then
    rm -rf "$root/tests/bin" && mkdir -p "$root/tests" && cp -R "$tree/tests/bin" "$root/tests/bin"
  fi
  done_mark "$mark" "$source"
}

step_thirdparty() {
  local mark=$root/runner/.thirdparty name url sha
  if [ -f "$mark" ]; then skip thirdparty; return; fi
  mkdir -p "$downloads"
  for name in dxmt-v0.80-builtin.tar.gz MoltenVK-macos.tar; do
    read -r url sha < <(manifest_get "$name") || die "manifest.txt has no $name"
    log_info "downloading $name"
    fetch "$url" "$downloads/$name" "$sha" || die "download of $name failed: $url"
  done
  runner_thirdparty "$root" "$downloads" || die "could not install DXMT and MoltenVK"
  runner_select_d3d9 "$root" dxvk && runner_select_ntdll "$root" fixed || die "could not select DXVK and the patched ntdll"
  done_mark "$mark"
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
  reg add 'HKCU\Environment' /v CX_GRAPHICS_BACKEND /d dxmt /f
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
# sounds (CrossOver's has no WMA decoder); shared with BO2. Its own step, so
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
step_binaries
step_thirdparty
step_prefix
step_xaudio
step_plutonium
step_folders
log_info "setup done: start with ./launch.sh"
