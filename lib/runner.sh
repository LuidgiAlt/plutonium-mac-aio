# The runner: runner/Sikarugir.app, Sikarugir's wrapper template with the
# engine of build/build.sh in Contents/Frameworks (where it finds the
# template's libraries), and runner/engine, a link to that engine.
# Every file that gets replaced is kept once as <file>.orig.
# Needs lib/env.sh (put) and lib/fetch.sh.
# shellcheck shell=bash

# engine_stamp <engine>: what runner/.version holds: the engine's ntdll and
# its version file (which names the build time).
engine_stamp() {
  echo "engine $(sha256_of "$1/lib/wine/x86_64-unix/ntdll.so.fixed") $(cat "$1/version" 2>/dev/null | sha256_of /dev/stdin)"
}

# runner_create <root> <template archive> <engine>
runner_create() {
  local r=$1/runner tmp
  [ -f "$3/lib/wine/x86_64-unix/ntdll.so.fixed" ] || { echo "no engine at $3" >&2; return 1; }
  mkdir -p "$r"
  # A new runner has none of the installed files: drop the mark that says so.
  rm -rf "$r/engine" "$r/Sikarugir.app" "$r/.thirdparty"
  tmp=$(mktemp -d "$r/.template.XXXXXX") || return 1
  if ! tar -xJf "$2" -C "$tmp" || ! mv "$tmp"/*.app "$r/Sikarugir.app"; then rm -rf "$tmp"; return 1; fi
  rm -rf "$tmp"
  cp -c -R "$3" "$r/Sikarugir.app/Contents/Frameworks/wswine.bundle" 2>/dev/null ||
    cp -R "$3" "$r/Sikarugir.app/Contents/Frameworks/wswine.bundle" || return 1
  ln -s Sikarugir.app/Contents/Frameworks/wswine.bundle "$r/engine" || return 1
  engine_stamp "$3" > "$r/.version"
}

# Pinned DXMT archive; AIO_DXMT_SHA is for tests.
dxmt_sha=${AIO_DXMT_SHA:-8f260e36b5739e68f3bad613381441385c4dc7b85b78ba8de653d5a6a264529d}

# runner_thirdparty <root> <downloads>: installs DXMT v0.80's builtin
# d3d11/dxgi/d3d10core/winemetal (32 and 64-bit) from the archive in
# <downloads>, after checking it.
runner_thirdparty() {
  local root=$1 dl=$2 tmp tree a d want_dxmt
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
  mkdir -p "$tree/lib/wine/x86_64-unix"
  cp "$tmp/v0.80/x86_64-unix/winemetal.so" "$tree/lib/wine/x86_64-unix/" || { rm -rf "$tmp" "$tree"; return 1; }
  runner_install "$root" "$tree"
  local status=$?
  rm -rf "$tmp" "$tree"
  return $status
}

# keep_orig <file>: save the runner's own version of <file> once.
keep_orig() { [ -e "$1.orig" ] || [ ! -e "$1" ] || cp -p "$1" "$1.orig"; }

# runner_install <root> <tree>: copy every file of <tree> (runner-relative
# paths) into the runner; ad-hoc sign Mach-O files. <tree>/tests (the test
# programs of a build or release) is not part of the runner.
runner_install() {
  local runner=$1/runner/engine tree=$2 rel dst
  while IFS= read -r rel; do
    rel=${rel#./}
    dst=$runner/$rel
    mkdir -p "$(dirname "$dst")"
    keep_orig "$dst"
    put "$tree/$rel" "$dst" || return 1
    case "$(file -b "$dst")" in Mach-O*) codesign -fs - "$dst" 2>/dev/null || return 1 ;; esac
  done < <(cd "$tree" && find . -path ./tests -prune -o -type f ! -name .DS_Store -print | sort)
}

# runner_select_ntdll <root> fixed|legacy
runner_select_ntdll() {
  local dir=$1/runner/engine/lib/wine/x86_64-unix
  case "$2" in fixed|legacy) ;; *) echo "ntdll must be fixed or legacy, not '$2'" >&2; return 1 ;; esac
  keep_orig "$dir/ntdll.so"
  put "$dir/ntdll.so.$2" "$dir/ntdll.so"
}
