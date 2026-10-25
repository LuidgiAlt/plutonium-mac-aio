"""Write a minimal uncompressed Microsoft CAB holding one file (tests only):
make-cab.py <out.cab> <name in the cabinet> <data file>. Run with python3 -I."""
import struct
import sys

out, name, data_path = sys.argv[1], sys.argv[2].encode() + b"\0", sys.argv[3]
data = open(data_path, "rb").read()
assert len(data) <= 32768, "one data block only"
files_off = 36 + 8
data_off = files_off + 16 + len(name)
total = data_off + 8 + len(data)
header = b"MSCF" + struct.pack("<IIIIIBBHHHHH", 0, total, 0, files_off, 0, 3, 1, 1, 1, 0, 0, 0)
folder = struct.pack("<IHH", data_off, 1, 0)                                  # no compression
cffile = struct.pack("<IIHHHH", len(data), 0, 0, 0x5a21, 0x0000, 0x20) + name
cfdata = struct.pack("<IHH", 0, len(data), len(data)) + data
open(out, "wb").write(header + folder + cffile + cfdata)
