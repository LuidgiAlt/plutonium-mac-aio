# Shared by build/build.sh and build/series.sh. Expects $root (repository root).
# shellcheck shell=bash

downloads=${AIO_DOWNLOADS:-$root/downloads}
src=$root/build/src

tarball=crossover-sources-26.3.0.tar.gz
tarball_url=https://media.codeweavers.com/pub/crossover/source/$tarball
tarball_sha=ac99c8ca4b3848f3e81784135f023df266b61c2345726ea55a50b3e030dd6872

die() { echo "$(basename "$0"): $*" >&2; exit 1; }

# Pristine sources/wine and sources/dxvk, extracted once into the download
# cache; prints its path. Callers clone trees from there (APFS, instant).
pristine() {
  local p=$downloads/pristine-26.3.0 t=$downloads/$tarball
  if [ ! -f "$p/.done" ]; then
    mkdir -p "$downloads"
    if [ ! -f "$t" ]; then
      curl -fL --retry 2 -o "$t.part" "$tarball_url" || { rm -f "$t.part"; die "download of $tarball failed"; }
      mv -f "$t.part" "$t"
    fi
    [ "$(shasum -a 256 "$t" | cut -d' ' -f1)" = "$tarball_sha" ] || die "$t: SHA-256 is not $tarball_sha"
    rm -rf "$p" "$p.tmp"
    mkdir -p "$p.tmp"
    tar -xzf "$t" -C "$p.tmp" sources/wine sources/dxvk
    mv "$p.tmp/sources/wine" "$p.tmp/wine"
    mv "$p.tmp/sources/dxvk" "$p.tmp/dxvk"
    rmdir "$p.tmp/sources"
    touch "$p.tmp/.done"
    mv "$p.tmp" "$p"
  fi
  echo "$p"
}

# Fresh build/src/<kind> cloned from the pristine tree, as its own git
# repository (so git apply resolves paths inside it, not in this repository).
# fresh_tree <kind> [<dir>]: <dir> defaults to $src (build/src). Copies keep
# the pristine mtimes (-p), so a rebuild only recompiles what the patches touch.
fresh_tree() {
  local kind=$1 base=${2:-$src} p
  p=$(pristine)
  rm -rf "${base:?}/$kind"
  mkdir -p "$base"
  cp -c -R -p "$p/$kind" "$base/$kind" 2>/dev/null || cp -R -p "$p/$kind" "$base/$kind"
  git -C "$base/$kind" init -q
}

# build_path <dir>: <dir> itself, or, when its path has a space (Wine's
# configure and makedep cannot handle one), a stable space-free link to it
# in /tmp, named after the path.
build_path() {
  case "$1" in *" "*) ;; *) echo "$1"; return 0 ;; esac
  local link
  link=/tmp/plutonium-mac-aio-$(printf '%s' "$1" | shasum | cut -c1-12)
  if [ ! -L "$link" ] || [ "$(readlink "$link")" != "$1" ]; then
    rm -f "$link"
    ln -s "$1" "$link" || return 1
  fi
  echo "$link"
}

# Header of a patch file: every line up to and including the first "---".
patch_header() { awk '/^---$/ {print; exit} {print}' "$1"; }
