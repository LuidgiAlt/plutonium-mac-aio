# The runner: an APFS clone of CrossOver's runtime with the patched files.
# Every CrossOver file that gets replaced is kept once as <file>.orig.
# Needs lib/env.sh (put) and, for runner_thirdparty, lib/fetch.sh.
# shellcheck shell=bash

crossover_version() {
  /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$1/Contents/Info.plist" 2>/dev/null
}

# runner_create <root> <crossover_app>
runner_create() {
  local runner=$1/runner/CrossOver version
  version=$(crossover_version "$2") || { echo "no CrossOver at $2" >&2; return 1; }
  mkdir -p "$1/runner"
  # A new copy has none of the installed files: drop the marks that say so.
  rm -rf "$runner" "$1/runner/.binaries" "$1/runner/.thirdparty"
  cp -c -R "$2/Contents/SharedSupport/CrossOver" "$runner" 2>/dev/null ||
    cp -R "$2/Contents/SharedSupport/CrossOver" "$runner" || return 1
  echo "$version" > "$1/runner/.version"
}

# runner_version_ok <root> <crossover_app>: the runner was made from the
# CrossOver version installed now.
runner_version_ok() {
  [ -f "$1/runner/.version" ] && [ "$(cat "$1/runner/.version")" = "$(crossover_version "$2")" ]
}

# Pinned DXMT archive; AIO_DXMT_SHA is for tests.
# MoltenVK-macos.tar is pinned in manifest.txt and checked when downloaded.
dxmt_sha=${AIO_DXMT_SHA:-8f260e36b5739e68f3bad613381441385c4dc7b85b78ba8de653d5a6a264529d}

# runner_thirdparty <root> <downloads>: installs DXMT v0.80's builtin
# d3d11/dxgi/d3d10core/winemetal (32 and 64-bit) and the x86_64 slice of
# MoltenVK 1.4.2 (as lib64/libMoltenVK.dylib.142) from the archives in
# <downloads>, after checking them.
runner_thirdparty() {
  local root=$1 dl=$2 tmp tree a d member want_dxmt
  want_dxmt=${AIO_DXMT_SHA:-$dxmt_sha}
  [ "$(sha256_of "$dl/dxmt-v0.80-builtin.tar.gz")" = "$want_dxmt" ] ||
    { echo "dxmt-v0.80-builtin.tar.gz: SHA-256 is not $want_dxmt" >&2; return 1; }
  tmp=$(mktemp -d) tree=$(mktemp -d)
  tar -xzf "$dl/dxmt-v0.80-builtin.tar.gz" -C "$tmp" || { rm -rf "$tmp" "$tree"; return 1; }
  for a in i386-windows x86_64-windows; do
    mkdir -p "$tree/lib/wine/$a"
    for d in d3d11 dxgi d3d10core winemetal; do
      cp "$tmp/v0.80/$a/$d.dll" "$tree/lib/wine/$a/$d.dll" || { rm -rf "$tmp" "$tree"; return 1; }
    done
  done
  mkdir -p "$tree/lib/wine/x86_64-unix" "$tree/lib64"
  cp "$tmp/v0.80/x86_64-unix/winemetal.so" "$tree/lib/wine/x86_64-unix/" || { rm -rf "$tmp" "$tree"; return 1; }
  member=$(tar -tf "$dl/MoltenVK-macos.tar" | grep -m1 'dynamic/dylib/macOS/libMoltenVK.dylib$')
  if [ -z "$member" ] || ! tar -xf "$dl/MoltenVK-macos.tar" -C "$tmp" "$member" ||
     ! lipo -thin x86_64 "$tmp/$member" -output "$tree/lib64/libMoltenVK.dylib.142" 2>/dev/null; then
    echo "MoltenVK-macos.tar: no macOS libMoltenVK.dylib with an x86_64 slice" >&2
    rm -rf "$tmp" "$tree"
    return 1
  fi
  runner_install "$root" "$tree"
  local status=$?
  rm -rf "$tmp" "$tree"
  return $status
}

# keep_orig <file>: save CrossOver's version of <file> once.
keep_orig() { [ -e "$1.orig" ] || [ ! -e "$1" ] || cp -p "$1" "$1.orig"; }

# runner_install <root> <tree>: copy every file of <tree> (runner-relative
# paths) into the runner; ad-hoc sign Mach-O files. <tree>/tests (the test
# programs of a build or release) is not part of the runner.
runner_install() {
  local runner=$1/runner/CrossOver tree=$2 rel dst
  while IFS= read -r rel; do
    rel=${rel#./}
    dst=$runner/$rel
    mkdir -p "$(dirname "$dst")"
    keep_orig "$dst"
    put "$tree/$rel" "$dst" || return 1
    case "$(file -b "$dst")" in Mach-O*) codesign -fs - "$dst" 2>/dev/null || return 1 ;; esac
  done < <(cd "$tree" && find . -path ./tests -prune -o -type f ! -name .DS_Store -print | sort)
}

# runner_select_d3d9 <root> dxvk|vulkan
#   dxvk:   d3d9.dll = DXVK, wined3d.dll = CrossOver's, MoltenVK 1.4.2
#   vulkan: d3d9.dll = CrossOver's (wined3d), wined3d.dll = patched wined3d,
#           CrossOver's MoltenVK (wined3d's Vulkan renderer loses draws after
#           clears on 1.4.2; DXVK needs 1.4.2's features)
runner_select_d3d9() {
  local arch dir mvk=$1/runner/CrossOver/lib64/libMoltenVK.dylib
  case "$2" in dxvk|vulkan) ;; *) echo "d3d9 renderer must be dxvk or vulkan, not '$2'" >&2; return 1 ;; esac
  dir=$1/runner/CrossOver/lib/wine/i386-windows
  keep_orig "$dir/d3d9.dll"
  for arch in i386-windows x86_64-windows; do
    keep_orig "$1/runner/CrossOver/lib/wine/$arch/wined3d.dll"
  done
  keep_orig "$mvk"
  if [ "$2" = dxvk ]; then
    [ -e "$mvk.142" ] && { put "$mvk.142" "$mvk" || return 1; }
    put "$dir/d3d9.dll.dxvk" "$dir/d3d9.dll" || return 1
    for arch in i386-windows x86_64-windows; do
      dir=$1/runner/CrossOver/lib/wine/$arch
      [ -e "$dir/wined3d.dll.orig" ] && { put "$dir/wined3d.dll.orig" "$dir/wined3d.dll" || return 1; }
    done
  else
    put "$mvk.orig" "$mvk" || return 1
    put "$dir/d3d9.dll.orig" "$dir/d3d9.dll" || return 1
    for arch in i386-windows x86_64-windows; do
      dir=$1/runner/CrossOver/lib/wine/$arch
      [ -e "$dir/wined3d.dll.vk" ] && { put "$dir/wined3d.dll.vk" "$dir/wined3d.dll" || return 1; }
    done
  fi
  return 0
}

# runner_select_ntdll <root> fixed|legacy
runner_select_ntdll() {
  local dir=$1/runner/CrossOver/lib/wine/x86_64-unix
  case "$2" in fixed|legacy) ;; *) echo "ntdll must be fixed or legacy, not '$2'" >&2; return 1 ;; esac
  keep_orig "$dir/ntdll.so"
  put "$dir/ntdll.so.$2" "$dir/ntdll.so"
}
