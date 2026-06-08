#!/usr/bin/env python3
# Written by Alun Morris and Claude Code
"""Compute the firmware JPEG render checksum for a given image ID.
Run from the preprocessor directory:
    python3 check_csum.py 95762 1
"""
import struct, sys, debug_server as ds

img_id = int(sys.argv[1]) if len(sys.argv) > 1 else 95762
scale  = int(sys.argv[2]) if len(sys.argv) > 2 else 1

class DB:
    def get_image(self, i):
        with open("output_v2/img_index.bin", "rb") as f:
            f.seek(i * 12)
            d = f.read(12)
        cid, off, length = struct.unpack("<III", d)
        with open(f"output_v2/img_{cid:04d}.dat", "rb") as f:
            f.seek(off)
            return f.read(length)

ds._db = DB()
raw = ds._db.get_image(img_id)
cs = 0
for b in raw:
    cs = (cs * 31 + b) & 0xFFFFFFFF
print(f"[wiki_db] id={img_id} len={len(raw)} csum=0x{cs:08X}")
ds.fw_emulate_image(raw, scale=scale)
