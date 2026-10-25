#!/usr/bin/env python3
"""Exit 0 if a file is a Windows PE executable with a non-empty certificate
table (Authenticode), else print why and exit 1. Structure only: the chain
is not validated (setup.sh also runs osslsigncode when it is installed).
Run with python3 -I."""
import struct
import sys


def check(path):
    with open(path, "rb") as f:
        data = f.read(4096)
    if data[:2] != b"MZ":
        return "not an MZ executable"
    (pe,) = struct.unpack_from("<I", data, 0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return "no PE header"
    opt = pe + 24
    (magic,) = struct.unpack_from("<H", data, opt)
    dirs = opt + (96 if magic == 0x10B else 112)
    security = dirs + 4 * 8  # data directory 4: certificate table
    if security + 8 > len(data):
        return "truncated optional header"
    _, size = struct.unpack_from("<II", data, security)
    if size == 0:
        return "not signed (empty certificate table)"
    return None


if __name__ == "__main__":
    problem = check(sys.argv[1])
    if problem:
        print(f"{sys.argv[1]}: {problem}", file=sys.stderr)
        sys.exit(1)
