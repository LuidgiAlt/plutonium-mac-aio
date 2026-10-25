#!/bin/bash
# Build the patched binaries from CodeWeavers' CrossOver 26.3 source.
#   build/build.sh --check            apply every patch to pristine sources, no compile
#   build/build.sh                    --check, then build into build/out/
#   build/build.sh --release <ver>    build, then pack build/out/ for a GitHub Release
#   build/build.sh --pack <ver>       pack the existing build/out/ only
# AIO_DOWNLOADS overrides the download cache (default: downloads/).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=build/common.sh
. "$root/build/common.sh"

out=$root/build/out
jobs=$(sysctl -n hw.ncpu)

# apply_series <kind> <dir> [<patch dir>]: <dir>/<kind> = pristine + every
# patch of patches/<kind> (or <patch dir>) in order.
apply_series() {
  local kind=$1 base=$2 patches=${3:-$root/patches/$1}
  fresh_tree "$kind" "$base"
  for patch in "$patches"/0*.patch; do
    git -C "$base/$kind" apply --whitespace=nowarn "$patch" 2>"$base/$kind.apply.log" ||
      die "patches/$kind/$(basename "$patch") does not apply: $(head -3 "$base/$kind.apply.log")"
  done
  rm -f "$base/$kind.apply.log"
}

# Applies into build/check, never build/src (build/series.sh's work tree).
check() {
  apply_series wine "$root/build/check"
  apply_series dxvk "$root/build/check"
  echo "all patches apply"
}

wine_env() {
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT MACOSX_DEPLOYMENT_TARGET=15.0 CC='clang -arch x86_64' CXX='clang++ -arch x86_64' ac_cv_func_pipe2=no
  export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/bin:$PATH"
}

# build_wine <source dir> <obj dir> <make targets...>
build_wine() {
  local srcdir=$1 obj=$2
  shift 2
  mkdir -p "$obj"
  if [ ! -f "$obj/Makefile" ]; then
    (cd "$obj" && "$srcdir/configure" --host=x86_64-apple-darwin24 --enable-archs=i386,x86_64 \
      --disable-tests --without-x --without-wayland --without-gstreamer --without-ffmpeg \
      --without-freetype --without-gnutls --without-vulkan --without-oss --without-alsa \
      --without-pulse --without-sane --without-usb --without-v4l2 --without-pcap \
      --without-capi --without-opencl --without-cups > configure.log 2>&1) ||
      die "configure failed: see $obj/configure.log"
  fi
  # Objects of everything the patches touch are rebuilt every time: a source
  # restored to a pristine mtime would otherwise look older than its object.
  find "$obj/dlls/ntdll" "$obj/dlls/wined3d" "$obj/libs/vkd3d" "$obj/server" -name '*.o' -delete 2>/dev/null || true
  # The PE objects' assert messages name their source files: map the build
  # folder (and so the builder's home) to /build. ($b is build's.)
  make -C "$obj" -j"$jobs" i386_CFLAGS="-g -O2 -ffile-prefix-map=$b=/build" \
    x86_64_CFLAGS="-g -O2 -ffile-prefix-map=$b=/build" "$@" > "$obj/make.log" 2>&1 ||
    die "make failed: see $obj/make.log"
}

build_dxvk() {
  local srcdir=$1 obj=$2
  mkdir -p "$(dirname "$obj")"
  if [ ! -f "$obj/build.ninja" ]; then
    (cd "$srcdir" && meson setup --cross-file build-win32.txt --buildtype release -Denable_d3d10=false \
      -Denable_d3d11=false -Denable_dxgi=false -Dcpp_args='-include cstdint' "$obj" > "$obj.setup.log" 2>&1) ||
      die "meson setup failed: see $obj.setup.log"
  fi
  ninja -C "$obj" > "$obj.log" 2>&1 || die "ninja failed: see $obj.log"
}

# AIO_BUILD_PARTS: any of "wine legacy dxvk tests" (default: all). A partial
# build only replaces its own files in build/out.
build() {
  local tree obj parts=${AIO_BUILD_PARTS:-wine legacy dxvk tests} part b
  has() { case " $parts " in *" $1 "*) return 0 ;; esac; return 1; }
  wine_env
  # Every path the compilers and makefiles see goes through b (no spaces).
  b=$(build_path "$root/build") || die "cannot make a space-free link to $root/build"
  if [ "${AIO_BUILD_TREE:-}" = src ]; then
    tree=$b/src obj=$b/obj-dev
    for part in wine dxvk; do
      if has "$part" && [ ! -d "$src/$part/.git" ]; then die "AIO_BUILD_TREE=src needs build/series.sh import $part"; fi
    done
  else
    tree=$b/tree obj=$b/obj
    if has wine; then apply_series wine "$tree"; fi
    if has dxvk; then apply_series dxvk "$tree"; fi
  fi
  [ "$parts" = "wine legacy dxvk tests" ] && rm -rf "$out"
  mkdir -p "$out/bin" "$out/lib/wine/x86_64-unix" "$out/lib/wine/i386-windows" "$out/lib/wine/x86_64-windows"

  if has wine; then
    echo "building wine (log: $obj/wine/make.log)"
    build_wine "$tree/wine" "$obj/wine" dlls/ntdll/ntdll.so server/wineserver \
      dlls/wined3d/i386-windows/wined3d.dll dlls/wined3d/x86_64-windows/wined3d.dll tools/winebuild/winebuild
    # strip -S: the debug map names every object file by its full path.
    strip -S -o "$out/bin/wineserver" "$obj/wine/server/wineserver"
    strip -S -o "$out/lib/wine/x86_64-unix/ntdll.so.fixed" "$obj/wine/dlls/ntdll/ntdll.so"
    i686-w64-mingw32-strip --strip-debug -o "$out/lib/wine/i386-windows/wined3d.dll.vk" "$obj/wine/dlls/wined3d/i386-windows/wined3d.dll"
    x86_64-w64-mingw32-strip --strip-debug -o "$out/lib/wine/x86_64-windows/wined3d.dll.vk" "$obj/wine/dlls/wined3d/x86_64-windows/wined3d.dll"
  fi
  if has legacy; then
    echo "building ntdll with the original 0002 (log: $obj/wine-legacy/make.log)"
    local legacy=$root/build/legacy-patches
    rm -rf "$legacy"; mkdir -p "$legacy"
    cp "$root"/patches/wine/0*.patch "$legacy/"
    cp "$root"/build/legacy/0002-*.patch "$legacy/"
    apply_series wine "$b/legacy-tree" "$legacy"
    build_wine "$b/legacy-tree/wine" "$obj/wine-legacy" dlls/ntdll/ntdll.so
    strip -S -o "$out/lib/wine/x86_64-unix/ntdll.so.legacy" "$obj/wine-legacy/dlls/ntdll/ntdll.so"
  fi
  if has dxvk; then
    echo "building dxvk (log: $obj/dxvk.log)"
    build_dxvk "$tree/dxvk" "$obj/dxvk"
    local winebuild=$b/obj/wine/tools/winebuild/winebuild
    [ -x "$winebuild" ] || winebuild=$obj/wine/tools/winebuild/winebuild
    [ -x "$winebuild" ] || die "winebuild not built yet: run a build with the wine part first"
    i686-w64-mingw32-strip -o "$out/lib/wine/i386-windows/d3d9.dll.dxvk" "$obj/dxvk/src/d3d9/d3d9.dll"
    "$winebuild" --builtin "$out/lib/wine/i386-windows/d3d9.dll.dxvk"
  fi
  if has tests; then
    make -C "$(build_path "$root")/tests" OUT="$b/out/tests/bin" > "$root/build/tests.log" 2>&1 ||
      die "test programs failed to build: see build/tests.log"
  fi
  echo "build/out:"
  (cd "$out" && find . -type f | sort)
}

# pack <version>: build/plutonium-mac-aio-bin-<version>.tar.gz from build/out,
# and the manifest.txt line for it.
pack() {
  local version=$1 archive sha leaks
  [ -n "$version" ] || die "--pack and --release need a version, e.g. 0.1.0"
  [ -d "$out/bin" ] && [ -d "$out/lib" ] || die "nothing to pack: build/out is missing (run build/build.sh)"
  # The archive is public: no file may carry the builder's home folder.
  leaks=$(cd "$out" && { grep -r -l -a -F -- "$HOME/" . || true; } | sed 's#^\./##')
  [ -z "$leaks" ] || die "these files contain $HOME (rebuild with build/build.sh): $leaks"
  archive=$root/build/plutonium-mac-aio-bin-$version.tar.gz
  COPYFILE_DISABLE=1 tar -czf "$archive.part" --uid 0 --gid 0 --uname root --gname wheel \
    -C "$out" --exclude .DS_Store . && mv -f "$archive.part" "$archive" ||
    die "could not write $archive"
  sha=$(shasum -a 256 "$archive" | cut -d' ' -f1)
  echo "release archive: $archive"
  echo "upload it to the GitHub Release v$version, then put this line in manifest.txt:"
  echo "plutonium-mac-aio-bin.tar.gz https://github.com/LuidgiAlt/plutonium-mac-aio/releases/download/v$version/plutonium-mac-aio-bin-$version.tar.gz $sha"
}

case "${1:-}" in
--check) check ;;
--pack) pack "${2:-}" ;;
--release) [ -n "${2:-}" ] || die "--release needs a version, e.g. 0.1.0"; check && build && pack "$2" ;;
"") if [ "${AIO_BUILD_TREE:-}" = src ]; then build; else check && build; fi ;;
*) die "usage: build/build.sh --check | (no argument) | --release <version> | --pack <version>" ;;
esac
