#!/usr/bin/env python3
# Written by Alun Morris and Claude Code
"""
fix_large_articles.py — post-processing patch for articles whose uncompressed HTML
exceeds the ESP32 firmware decompression buffer (DECOMP_BUF_SIZE = 64 KB).

For each oversized article:
  1. Read from its chunk file, decompress, truncate HTML to MAX_HTML bytes.
  2. Recompress (always smaller than original — we reduced content).
  3. Overwrite the same slot in the chunk file.
  4. Update id_index.bin with the new (smaller) compressed length.

The chunk file slot is padded with zeroed bytes after the shorter blob so the
file length stays unchanged.  Offsets of other articles are unaffected.

Usage:
    python3 fix_large_articles.py [output_dir]
"""

import os, struct, sys, lz4.block

OUTPUT_DIR    = sys.argv[1] if len(sys.argv) > 1 else "output_maxi"
MAX_ORIG_SIZE = 64 * 1024   # firmware DECOMP_BUF_SIZE
MAX_HTML      = 60_000      # max HTML bytes after fix

MAGIC = b"WIKI"

id_index_path = os.path.join(OUTPUT_DIR, "id_index.bin")
id_index_size = os.path.getsize(id_index_path) // 12

print(f"Scanning {id_index_size} articles in {OUTPUT_DIR} …")

# Read full id_index into RAM for fast scanning.
with open(id_index_path, "rb") as f:
    id_index_data = bytearray(f.read())

oversized = []
for art_id in range(id_index_size):
    chunk_id, offset, length = struct.unpack_from("<III", id_index_data, art_id * 12)
    if length == 0:
        continue
    # Read just the 4-byte original-size prefix from the chunk file.
    chunk_path = os.path.join(OUTPUT_DIR, f"articles_{chunk_id:04d}.dat")
    with open(chunk_path, "rb") as f:
        f.seek(offset)
        hdr = f.read(4)
    if len(hdr) < 4:
        continue
    orig_size = struct.unpack("<I", hdr)[0]
    if orig_size > MAX_ORIG_SIZE:
        oversized.append((art_id, chunk_id, offset, length, orig_size))

print(f"Found {len(oversized)} oversized articles (orig_size > {MAX_ORIG_SIZE} bytes)")

if not oversized:
    print("Nothing to do.")
    sys.exit(0)

# Open chunk files that we need (read+write mode).
chunk_files: dict[int, object] = {}

def get_chunk_file(chunk_id: int):
    if chunk_id not in chunk_files:
        path = os.path.join(OUTPUT_DIR, f"articles_{chunk_id:04d}.dat")
        chunk_files[chunk_id] = open(path, "r+b")
    return chunk_files[chunk_id]

patched = 0
failed  = 0

for art_id, chunk_id, offset, orig_length, orig_uncompressed in oversized:
    cf = get_chunk_file(chunk_id)

    # Read full compressed blob.
    cf.seek(offset)
    compressed = cf.read(orig_length)
    if len(compressed) != orig_length:
        print(f"  [SKIP] art_id={art_id}: short read ({len(compressed)}/{orig_length})")
        failed += 1
        continue

    # Decompress.
    try:
        raw = lz4.block.decompress(compressed[4:], uncompressed_size=orig_uncompressed)
    except Exception as e:
        print(f"  [SKIP] art_id={art_id}: decompress failed: {e}")
        failed += 1
        continue

    # Parse WIKI blob.
    p = 0
    if raw[p:p+4] != MAGIC:
        print(f"  [SKIP] art_id={art_id}: bad magic")
        failed += 1
        continue
    p += 4

    stored_id = struct.unpack_from("<I", raw, p)[0]; p += 4
    title_len = struct.unpack_from("<H", raw, p)[0]; p += 2
    title = raw[p:p+title_len].decode("utf-8", errors="replace"); p += title_len
    html_len = struct.unpack_from("<I", raw, p)[0]; p += 4
    html_bytes = raw[p:p+html_len]

    if len(html_bytes) <= MAX_HTML:
        # Shouldn't happen (we already filtered on orig_uncompressed), but be safe.
        print(f"  [SKIP] art_id={art_id} '{title}': html={len(html_bytes)} already fits")
        continue

    # Truncate HTML at last '>' before MAX_HTML to avoid cutting mid-tag.
    truncated = html_bytes[:MAX_HTML]
    last_gt = truncated.rfind(b">")
    if last_gt > 0:
        truncated = truncated[:last_gt + 1]

    # Rebuild WIKI blob with truncated HTML.
    title_enc = title.encode("utf-8")
    new_html_bytes = truncated
    new_raw = (
        MAGIC
        + struct.pack("<I", stored_id)
        + struct.pack("<H", len(title_enc))
        + title_enc
        + struct.pack("<I", len(new_html_bytes))
        + new_html_bytes
    )

    # Recompress (with 4-byte original-size prefix).
    new_orig_size = len(new_raw)
    new_compressed_body = lz4.block.compress(new_raw, store_size=False)
    new_blob = struct.pack("<I", new_orig_size) + new_compressed_body

    if len(new_blob) > orig_length:
        print(f"  [FAIL] art_id={art_id} '{title}': new blob ({len(new_blob)}) > original slot ({orig_length})")
        failed += 1
        continue

    # Overwrite in chunk file; pad remainder with zeros.
    pad = orig_length - len(new_blob)
    cf.seek(offset)
    cf.write(new_blob)
    if pad > 0:
        cf.write(b"\x00" * pad)

    # Update id_index.bin in memory.
    struct.pack_into("<III", id_index_data, art_id * 12,
                     chunk_id, offset, len(new_blob))

    patched += 1
    print(f"  Patched art_id={art_id} '{title}': "
          f"{orig_uncompressed}→{new_orig_size} bytes uncompressed, "
          f"{orig_length}→{len(new_blob)} bytes compressed")

# Close chunk files.
for cf in chunk_files.values():
    cf.close()

# Write updated id_index.bin.
if patched > 0:
    with open(id_index_path, "wb") as f:
        f.write(id_index_data)
    print(f"\nDone: {patched} articles patched, {failed} skipped.")
    print(f"id_index.bin updated.")
else:
    print(f"\nDone: nothing patched ({failed} failures).")
