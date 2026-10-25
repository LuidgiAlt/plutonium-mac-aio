# Downloads, checked by SHA-256 and written atomically.
# shellcheck shell=bash

sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }

# fetch <url> <dest> <sha256|->: downloads to <dest>.part and renames it only
# when complete and matching; on any failure nothing is left behind. An
# existing <dest> that matches (or any existing one, for "-") is kept.
fetch() {
  local url=$1 dest=$2 sha=$3
  if [ -f "$dest" ] && { [ "$sha" = - ] || [ "$(sha256_of "$dest")" = "$sha" ]; }; then return 0; fi
  rm -f "$dest.part"
  if ! curl -fsSL --retry 2 --connect-timeout 20 -o "$dest.part" "$url" 2>> "${AIO_LOG:-/dev/null}"; then
    rm -f "$dest.part"
    return 1
  fi
  if [ "$sha" != - ] && [ "$(sha256_of "$dest.part")" != "$sha" ]; then
    echo "$(basename "$dest"): SHA-256 is not $sha" >&2
    rm -f "$dest.part"
    return 1
  fi
  mv -f "$dest.part" "$dest"
}

# manifest_get <name>: prints "url sha" of a "name url sha" line of
# manifest.txt ($AIO_MANIFEST overrides the file).
manifest_get() {
  local manifest=${AIO_MANIFEST:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/manifest.txt}
  awk -v n="$1" '$1 == n { print $2, $3; found = 1; exit } END { exit !found }' "$manifest"
}
