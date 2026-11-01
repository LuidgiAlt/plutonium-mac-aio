#!/bin/bash
# Build the Wine engine from CodeWeavers' published CrossOver 26.3 source with
# this project's patches: build/out/wswine.bundle, a Sikarugir engine that
# runs from the Frameworks folder of Sikarugir's wrapper template.
#   build/build.sh --check            apply every patch to pristine sources, no compile
#   build/build.sh                    --check, then build into build/out/
#   build/build.sh --pack <version>   pack build/out (engine and test programs)
#                                     for a GitHub Release
# AIO_DOWNLOADS overrides the download cache (default: downloads/).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=build/common.sh
. "$root/build/common.sh"
# shellcheck source=lib/fetch.sh
. "$root/lib/fetch.sh"

out=$root/build/out
engine=$out/wswine.bundle
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

# wine_make <obj dir> <make arguments...>: objects of everything the patches
# touch are rebuilt every time (a source restored to a pristine mtime would
# otherwise look older than its object). The PE objects' assert messages
# name their source files: map the build folder (and so the builder's home)
# to /build ($b is build's). 32-bit Windows code uses SSE maths: mingw's
# default x87 code is slow under Rosetta (DirectSound's mixer could not keep
# up with BO1's video sound); make does not see a flag change, so the 32-bit
# objects are rebuilt when it changes.
wine_make() {
  local obj=$1 i386_flags="-g -O2 -msse2 -mfpmath=sse -ffile-prefix-map=$b=/build"
  shift
  find "$obj/dlls/ntdll" "$obj/dlls/wined3d" "$obj/libs/vkd3d" "$obj/server" -name '*.o' -delete 2>/dev/null || true
  if [ "$(cat "$obj/.i386_cflags" 2>/dev/null)" != "$i386_flags" ]; then
    find "$obj" -path '*i386-windows*' \( -name '*.o' -o -name '*.a' -o -name '*.dll' -o -name '*.exe' -o -name '*.drv' -o -name '*.sys' \) -delete
    echo "$i386_flags" > "$obj/.i386_cflags"
  fi
  make -C "$obj" -j"$jobs" i386_CFLAGS="$i386_flags" x86_64_CFLAGS="-g -O2 -ffile-prefix-map=$b=/build" "$@" \
    > "$obj/make.log" 2>&1 || die "make failed: see $obj/make.log"
}

# build_legacy_ntdll <source dir> <obj dir>: ntdll.so only, so no library.
build_legacy_ntdll() {
  local srcdir=$1 obj=$2
  mkdir -p "$obj"
  if [ ! -f "$obj/Makefile" ]; then
    (cd "$obj" && "$srcdir/configure" --host=x86_64-apple-darwin24 --enable-archs=i386,x86_64 \
      --disable-tests --without-x --without-wayland --without-gstreamer --without-ffmpeg \
      --without-freetype --without-gnutls --without-vulkan --without-oss --without-alsa \
      --without-pulse --without-sane --without-usb --without-v4l2 --without-pcap \
      --without-capi --without-opencl --without-cups > configure.log 2>&1) ||
      die "configure failed: see $obj/configure.log"
  fi
  wine_make "$obj" dlls/ntdll/ntdll.so
}

# The engine links against the x86_64 libraries of Sikarugir's wrapper
# template (Contents/Frameworks, the folder a wrapper keeps its engine in):
# headers for gnutls and FreeType come from the CrossOver source archive,
# SDL2's from Homebrew. Prints build/engine-deps.
engine_deps() {
  local d=$root/build/engine-deps name=Sikarugir-Template.tar.xz url sha inc lib
  if [ ! -f "$d/.done" ]; then
    read -r url sha < <(manifest_get "$name") || die "manifest.txt has no $name"
    mkdir -p "$downloads"
    fetch "$url" "$downloads/$name" "$sha" || die "download of $name failed: $url"
    rm -rf "$d"
    mkdir -p "$d/tmp" "$d/lib" "$d/include"
    tar -xJf "$downloads/$name" -C "$d/tmp" || die "$name is damaged"
    cp -a "$d"/tmp/*.app/Contents/Frameworks/lib*.dylib "$d/lib/"
    # Some ids are bare names ("libfreetype.dylib"), which dyld reads as a
    # path from the current folder: the build tools linking FreeType could
    # not start. @rpath, like their dependencies.
    for lib in "$d"/lib/*.dylib; do
      [ -L "$lib" ] && continue
      case "$(otool -D "$lib" | sed -n 2p)" in /*|@*) continue ;; esac
      install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null && codesign -fs - "$lib" 2>/dev/null
    done
    pristine > /dev/null
    tar -xzf "$downloads/$tarball" -C "$d/tmp" sources/gnutls/gnutls/lib/includes/gnutls sources/freetype/include
    inc=$d/tmp/sources/gnutls/gnutls/lib/includes/gnutls
    mkdir -p "$d/include/gnutls"
    cp "$inc"/*.h "$d/include/gnutls/"
    # gnutls.h is generated by gnutls' configure; the template has 3.8.
    sed -e 's/@VERSION@/3.8.3/; s/@MAJOR_VERSION@/3/; s/@MINOR_VERSION@/8/; s/@PATCH_VERSION@/3/' \
      -e 's/@NUMBER_VERSION@/0x030803/' \
      -e 's/@DEFINE_IOVEC_T@/#include <sys\/uio.h>\ntypedef struct iovec giovec_t;/' "$inc/gnutls.h.in" > "$d/include/gnutls/gnutls.h"
    cp -R "$d/tmp/sources/freetype/include" "$d/include/freetype"
    rm -rf "$d/tmp"
    touch "$d/.done"
  fi
  echo "$d"
}

# build_engine <source dir> <obj dir> <deps>: the whole Wine with FreeType,
# gnutls, Vulkan (MoltenVK) and SDL2, installed into build/out/wswine.bundle.
# The libraries are loaded through @rpath, which points at the wrapper's
# Frameworks folder from bin/ and from lib/wine/*-unix/, and at lib/ first
# (the engine's own MoltenVK), so no DYLD_* variable is needed.
build_engine() {
  local srcdir=$1 obj=$2 deps=$3 sdl=()
  if [ -d /opt/homebrew/include/SDL2 ]; then
    sdl=(SDL2_CFLAGS="-I/opt/homebrew/include/SDL2 -D_THREAD_SAFE" SDL2_LIBS="-L$deps/lib -lSDL2"
         ac_cv_lib_soname_SDL2=@rpath/libSDL2-2.0.0.dylib)
  else
    sdl=(--without-sdl)
    echo "no SDL2 headers (brew install sdl2): building without game controllers"
  fi
  if [ ! -f "$obj/.engine" ]; then
    rm -rf "$obj"
    mkdir -p "$obj"
    (cd "$obj" && "$srcdir/configure" --host=x86_64-apple-darwin24 --enable-archs=i386,x86_64 --prefix=/ \
      --disable-tests --without-x --without-wayland --without-gstreamer --without-ffmpeg \
      --without-oss --without-alsa --without-pulse --without-sane --without-usb --without-v4l2 \
      --without-pcap --without-capi --without-opencl --without-cups \
      LDFLAGS="-L$deps/lib -Wl,-rpath,@loader_path/../../ -Wl,-rpath,@loader_path/../../../../ -Wl,-rpath,$deps/lib" \
      FREETYPE_CFLAGS="-I$deps/include/freetype" FREETYPE_LIBS="-L$deps/lib -lfreetype" \
      GNUTLS_CFLAGS="-I$deps/include" GNUTLS_LIBS="-L$deps/lib -lgnutls" \
      ac_cv_lib_soname_freetype=@rpath/libfreetype.6.dylib ac_cv_lib_soname_gnutls=@rpath/libgnutls.30.dylib \
      ac_cv_lib_soname_vulkan= ac_cv_lib_soname_MoltenVK=@rpath/libMoltenVK.dylib "${sdl[@]}" \
      > configure.log 2>&1) || die "configure failed: see $obj/configure.log"
    touch "$obj/.engine"
  fi
  wine_make "$obj"
  rm -rf "$engine"
  make -C "$obj" install-lib DESTDIR="$engine" > "$obj/install.log" 2>&1 || die "make install failed: see $obj/install.log"
  cp "$engine/lib/wine/x86_64-unix/ntdll.so" "$engine/lib/wine/x86_64-unix/ntdll.so.fixed"
  # The template's libMoltenVK.dylib is a Vulkan loader; DXVK needs
  # MoltenVK 1.4.2, found first in lib/ (@loader_path/../../).
  local url sha member tmp
  read -r url sha < <(manifest_get MoltenVK-macos.tar) || die "manifest.txt has no MoltenVK-macos.tar"
  fetch "$url" "$downloads/MoltenVK-macos.tar" "$sha" || die "download of MoltenVK-macos.tar failed: $url"
  member=$(tar -tf "$downloads/MoltenVK-macos.tar" | grep -m1 'dynamic/dylib/macOS/libMoltenVK.dylib$')
  tmp=$(mktemp -d)
  tar -xf "$downloads/MoltenVK-macos.tar" -C "$tmp" "$member" &&
    lipo -thin x86_64 "$tmp/$member" -output "$engine/lib/libMoltenVK.dylib" || { rm -rf "$tmp"; die "no x86_64 MoltenVK"; }
  rm -rf "$tmp"
  # The build time makes each build's stamp (lib/runner.sh) new for setup.sh.
  echo "WineCX 26.3.0 plutonium-mac-aio $(date -u +%Y%m%dT%H%M%SZ)" > "$engine/version"
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

# finish_engine <deps>: no debug information (it names the builder's
# folders), the deps rpath (for the build tools only) dropped, every Mach-O
# file signed; refuses an engine that still names the builder's home.
finish_engine() {
  local deps=$1 f
  while IFS= read -r f; do
    case "$f" in
    */i386-windows/*) i686-w64-mingw32-strip --strip-debug "$f" 2>/dev/null || true ;;
    */x86_64-windows/*) x86_64-w64-mingw32-strip --strip-debug "$f" 2>/dev/null || true ;;
    *)
      file -b "$f" | grep -q '^Mach-O' || continue
      strip -S "$f" 2>/dev/null || true
      install_name_tool -delete_rpath "$deps/lib" "$f" 2>/dev/null || true
      codesign -fs - "$f" 2>/dev/null || die "cannot sign $f"
      ;;
    esac
  done < <(find "$engine" -type f)
  if grep -r -l -a -F -- "$HOME/" "$engine" > /dev/null; then
    die "the engine names $HOME: $(grep -r -l -a -F -- "$HOME/" "$engine" | head -3)"
  fi
}

# AIO_BUILD_PARTS: any of "wine legacy dxvk tests" (default: all). A partial
# build only replaces its own files in the engine.
build() {
  local tree obj parts=${AIO_BUILD_PARTS:-wine legacy dxvk tests} part b deps
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
  deps=$(engine_deps)

  if has wine; then
    echo "building wine (about an hour the first time; log: $obj/wine/make.log)"
    build_engine "$tree/wine" "$obj/wine" "$deps"
  fi
  [ -d "$engine" ] || die "no engine yet: run a build with the wine part first"
  if has legacy; then
    echo "building ntdll with the original 0002 (log: $obj/wine-legacy/make.log)"
    local legacy=$root/build/legacy-patches
    rm -rf "$legacy"; mkdir -p "$legacy"
    cp "$root"/patches/wine/0*.patch "$legacy/"
    cp "$root"/build/legacy/0002-*.patch "$legacy/"
    apply_series wine "$b/legacy-tree" "$legacy"
    build_legacy_ntdll "$b/legacy-tree/wine" "$obj/wine-legacy"
    cp "$obj/wine-legacy/dlls/ntdll/ntdll.so" "$engine/lib/wine/x86_64-unix/ntdll.so.legacy"
  fi
  if has dxvk; then
    echo "building dxvk (log: $obj/dxvk.log)"
    build_dxvk "$tree/dxvk" "$obj/dxvk"
    # Direct3D 9 is DXVK's, marked as a Wine builtin.
    i686-w64-mingw32-strip -o "$engine/lib/wine/i386-windows/d3d9.dll" "$obj/dxvk/src/d3d9/d3d9.dll"
    "$obj/wine/tools/winebuild/winebuild" --builtin "$engine/lib/wine/i386-windows/d3d9.dll" ||
      die "winebuild failed (the wine part builds it)"
  fi
  finish_engine "$deps"
  if has tests; then
    make -C "$(build_path "$root")/tests" OUT="$b/out/tests/bin" > "$root/build/tests.log" 2>&1 ||
      die "test programs failed to build: see build/tests.log"
  fi
  echo "engine: build/out/wswine.bundle ($(cat "$engine/version"), $(du -sh "$engine" | cut -f1))"
}

# pack <version>: build/plutonium-mac-aio-engine-<version>.tar.xz from
# build/out, and the manifest.txt line for it.
pack() {
  local version=$1 archive sha
  [ -n "$version" ] || die "--pack needs a version, e.g. 0.2.0"
  [ -f "$engine/lib/wine/x86_64-unix/ntdll.so.fixed" ] && [ -d "$out/tests/bin" ] ||
    die "nothing to pack: build/out has no engine or test programs (run build/build.sh)"
  # The archive is public: no file may carry the builder's home folder.
  if grep -r -l -a -F -- "$HOME/" "$engine" "$out/tests/bin" > /dev/null; then
    die "these files contain $HOME (rebuild with build/build.sh): $(grep -r -l -a -F -- "$HOME/" "$engine" "$out/tests/bin" | head -3)"
  fi
  archive=$root/build/plutonium-mac-aio-engine-$version.tar.xz
  echo "packing $archive (a few minutes)"
  COPYFILE_DISABLE=1 XZ_OPT=-T0 tar -cJf "$archive.part" --uid 0 --gid 0 --uname root --gname wheel \
    -C "$out" --exclude .DS_Store wswine.bundle tests/bin && mv -f "$archive.part" "$archive" ||
    die "could not write $archive"
  sha=$(shasum -a 256 "$archive" | cut -d' ' -f1)
  echo "release archive: $archive ($(du -h "$archive" | cut -f1))"
  echo "upload it to the GitHub Release v$version, then put this line in manifest.txt:"
  echo "plutonium-mac-aio-engine.tar.xz https://github.com/LuidgiAlt/plutonium-mac-aio/releases/download/v$version/plutonium-mac-aio-engine-$version.tar.xz $sha"
}

case "${1:-}" in
--check) check ;;
--pack) pack "${2:-}" ;;
"") if [ "${AIO_BUILD_TREE:-}" = src ]; then build; else check && build; fi ;;
*) die "usage: build/build.sh [--check | --pack <version>]" ;;
esac
