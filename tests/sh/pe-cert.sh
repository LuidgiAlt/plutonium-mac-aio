#!/bin/bash
# tools/pe-cert.py accepts a PE with a certificate table and rejects the rest.
set -u
test_name=pe-cert  # read by assert.sh
# shellcheck source=tests/sh/assert.sh
. "$(dirname -- "$0")/assert.sh"

make_tmp
command -v i686-w64-mingw32-gcc > /dev/null || { echo "SKIP pe-cert: no mingw"; exit 0; }
echo 'int main(void) { return 0; }' > "$tmp/t.c"
i686-w64-mingw32-gcc -o "$tmp/t.exe" "$tmp/t.c" || fail "cannot build a test exe"
check() { python3 -I "$repo_root/tools/pe-cert.py" "$1" 2>/dev/null; }

if check "$tmp/t.exe"; then fail "an unsigned exe was accepted"; fi
echo "not a program" > "$tmp/text.exe"
if check "$tmp/text.exe"; then fail "a text file was accepted"; fi
# Give the exe a non-empty certificate table entry (data directory 4).
python3 -I - "$tmp/t.exe" "$tmp/signed.exe" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
pe = struct.unpack_from("<I", d, 0x3C)[0]
sec = pe + 24 + 96 + 4 * 8
struct.pack_into("<II", d, sec, len(d), 8)
open(sys.argv[2], "wb").write(d)
PY
check "$tmp/signed.exe" || fail "an exe with a certificate table was rejected"
pass
