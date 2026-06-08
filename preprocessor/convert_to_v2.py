#!/usr/bin/env python3
# Written by Alun Morris and Claude Code
"""
convert_to_v2.py — Convert an existing WIKI-format output directory to the new
WKI2 multi-block format without re-processing the full ZIM file.

Strategy:
  • For articles with HTML ≤ REREAD_THRESHOLD: re-pack from existing output_maxi
    data (fast — just decompress, reformat, compress per block).
  • For articles with HTML > REREAD_THRESHOLD: re-read from the ZIM file so we
    get the full (untruncated) HTML before splitting into 60 KB blocks.  This
    covers the ~288 articles that fix_large_articles.py truncated to 60 KB.

Usage:
    python3 convert_to_v2.py <zim_file> [input_dir] [output_dir]

Defaults:
    zim_file    — required (e.g. wikipedia_en_simple_all_maxi_2026-05.zim)
    input_dir   — output_maxi
    output_dir  — output_v2

The image files (img_*.dat, img_index.bin, img_meta.txt) and the text index
(index.bin, sparse_index.bin, index_meta.txt) are UNCHANGED — only the article
chunk files and id_index.bin are rebuilt.
"""

import os
import struct
import sys
import shutil
import tempfile

# ---- dependency check -----------------------------------------------------------

def _check():
    missing = []
    for name, pip in [("lz4.block","lz4"), ("libzim","libzim"),
                      ("bs4","beautifulsoup4"), ("lxml","lxml")]:
        try:
            __import__(name)
        except ImportError:
            missing.append(f"  {pip}  (pip install {pip})")
    if missing:
        print("Missing dependencies:\n" + "\n".join(missing))
        sys.exit(1)

_check()

import lz4.block
from libzim import Archive
from bs4 import BeautifulSoup, Comment

# ---- constants (must match build_wiki_db.py + firmware config.h) ---------------

MAGIC_OLD           = b"WIKI"
MAGIC_NEW           = b"WKI2"
BLOCK_SIZE          = 60_000               # max HTML bytes per compressed block
ARTICLES_CHUNK_SIZE = 32 * 1024 * 1024     # 32 MB per chunk file
# Articles whose HTML is ≥ this threshold may have been truncated; re-read from ZIM.
REREAD_THRESHOLD    = 55_000

# ---- argument parsing -----------------------------------------------------------

if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(1)

ZIM_PATH    = sys.argv[1]
INPUT_DIR   = sys.argv[2] if len(sys.argv) > 2 else "output_maxi"
OUTPUT_DIR  = sys.argv[3] if len(sys.argv) > 3 else "output_v2"

if not os.path.isfile(ZIM_PATH):
    print(f"ZIM file not found: {ZIM_PATH}")
    sys.exit(1)
if not os.path.isdir(INPUT_DIR):
    print(f"Input directory not found: {INPUT_DIR}")
    sys.exit(1)

os.makedirs(OUTPUT_DIR, exist_ok=True)

# ---- helpers -------------------------------------------------------------------

def read_u32le(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]

def read_u16le(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]

def split_html_blocks(html_bytes: bytes) -> list[bytes]:
    blocks = []
    start = 0
    total = len(html_bytes)
    while start < total:
        end = start + BLOCK_SIZE
        if end >= total:
            blocks.append(html_bytes[start:])
            break
        cut = html_bytes.rfind(b">", start, end)
        if cut <= start:
            cut = end
        else:
            cut += 1
        blocks.append(html_bytes[start:cut])
        start = cut
    return blocks

def pack_wki2(article_id: int, title: str, html_bytes: bytes) -> bytes:
    title_bytes = title.encode("utf-8")
    blocks      = split_html_blocks(html_bytes)
    compressed  = [lz4.block.compress(b, store_size=True) for b in blocks]
    blob = MAGIC_NEW
    blob += struct.pack("<I", article_id)
    blob += struct.pack("<H", len(title_bytes)) + title_bytes
    blob += struct.pack("<H", len(compressed))
    for c in compressed:
        blob += struct.pack("<I", len(c)) + c
    return blob

# ---- parse old WIKI format article blob ----------------------------------------

def parse_wiki_blob(raw: bytes):
    """Returns (article_id, title, html_bytes) or None on error."""
    if len(raw) < 4 or raw[:4] != MAGIC_OLD:
        return None
    p = 4
    if p + 4 > len(raw): return None
    article_id = read_u32le(raw, p); p += 4
    if p + 2 > len(raw): return None
    title_len = read_u16le(raw, p); p += 2
    if p + title_len > len(raw): return None
    title = raw[p:p+title_len].decode("utf-8", errors="replace"); p += title_len
    if p + 4 > len(raw): return None
    html_len = read_u32le(raw, p); p += 4
    if p + html_len > len(raw): return None
    html_bytes = raw[p:p+html_len]
    return article_id, title, html_bytes

# ---- read ZIM data for re-processing large articles ----------------------------

def normalise_title(title: str) -> str:
    return " ".join(title.lower().split())

def _is_article_entry(entry) -> bool:
    if entry.is_redirect: return False
    path = entry.path
    if path.startswith("_") or path.startswith("-"): return False
    if "/" in path and not path.startswith("A/") and not path.startswith("a/"): return False
    try:
        return entry.get_item().mimetype.startswith("text/html")
    except Exception:
        return False

def strip_html_from_raw(raw_html: bytes, title_map: dict) -> str:
    soup = BeautifulSoup(raw_html, "lxml")
    for tag in soup.find_all(["script","style","table","sup","sub","noscript",
                               "meta","link","header","footer","nav","aside"]):
        tag.decompose()
    for c in soup.find_all(string=lambda t: isinstance(t, Comment)):
        c.extract()
    for img_tag in list(soup.find_all("img")):
        img_tag.decompose()   # skip images during conversion (already in img chunk files)
    for cap in soup.find_all("figcaption"): cap.decompose()
    for fig in soup.find_all("figure"): fig.unwrap()
    for a_tag in soup.find_all("a"):
        href = a_tag.get("href", "")
        a_tag.attrs = {}
        if href and not href.startswith("#") and not href.startswith("http") \
           and not href.startswith("./") and not href.startswith("../") \
           and "://" not in href:
            raw_title = href.split("#")[0].replace("_", " ")
            norm = normalise_title(raw_title)
            if norm in title_map:
                a_tag["href"] = f"/wiki/{title_map[norm]}"
            else:
                a_tag.unwrap()
    for tag in soup.find_all(True):
        if tag.name not in ("a", "img"):
            tag.attrs = {}
    body = (soup.find("div", id="mw-content-text")
            or soup.find("div", class_="mw-parser-output")
            or soup.find("body") or soup)
    return str(body)

# ---- main ----------------------------------------------------------------------

print(f"Opening ZIM: {ZIM_PATH}")
archive = Archive(ZIM_PATH)

# Build title → article_id map from existing index.bin
print("Loading title→id map from index.bin …")
INDEX_RECORD_SIZE = 80
TITLE_KEY_LEN     = 60
idx_path = os.path.join(INPUT_DIR, "index.bin")
title_map: dict[str, int] = {}
with open(idx_path, "rb") as f:
    while True:
        rec = f.read(INDEX_RECORD_SIZE)
        if len(rec) < INDEX_RECORD_SIZE: break
        key  = rec[:TITLE_KEY_LEN].rstrip(b"\x00").decode("utf-8", errors="replace")
        aid  = struct.unpack_from("<I", rec, TITLE_KEY_LEN)[0]
        title_map[key] = aid
print(f"  {len(title_map)} articles indexed")

# Build ZIM entry path → article_id reverse map for large-article re-read
print("Building ZIM path → article_id map …")
zim_path_map: dict[str, int] = {}   # normalised_title → article_id
for i in range(archive.entry_count):
    try:
        entry = archive._get_entry_by_id(i)
    except Exception:
        continue
    if not _is_article_entry(entry): continue
    path  = entry.path
    title = entry.title or path.split("/", 1)[-1].replace("_", " ")
    norm  = normalise_title(title)
    if norm in title_map:
        zim_path_map[norm] = i   # ZIM entry index
print(f"  {len(zim_path_map)} ZIM entries mapped")

# Read id_index.bin
id_index_path = os.path.join(INPUT_DIR, "id_index.bin")
with open(id_index_path, "rb") as f:
    id_index_data = bytearray(f.read())
article_count = len(id_index_data) // 12

# Prepare new output
new_chunk_id     = 0
new_chunk_offset = 0

def new_chunk_path(cid: int) -> str:
    return os.path.join(OUTPUT_DIR, f"articles_{cid:04d}.dat")

new_chunk_file = open(new_chunk_path(0), "wb")

new_id_index = bytearray(article_count * 12)   # same length as old

# Track which old chunk files we have open
old_chunk_cache: dict[int, object] = {}
def get_old_chunk(cid: int):
    if cid not in old_chunk_cache:
        p = os.path.join(INPUT_DIR, f"articles_{cid:04d}.dat")
        old_chunk_cache[cid] = open(p, "rb")
    return old_chunk_cache[cid]

reread_count  = 0
convert_count = 0
skip_count    = 0

print("Converting articles …")
for art_id in range(article_count):
    old_chunk_id, old_offset, old_length = struct.unpack_from("<III", id_index_data, art_id * 12)
    if old_length == 0:
        struct.pack_into("<III", new_id_index, art_id * 12, 0, 0, 0)
        skip_count += 1
        continue

    # Read old compressed blob
    cf = get_old_chunk(old_chunk_id)
    cf.seek(old_offset)
    compressed_blob = cf.read(old_length)
    if len(compressed_blob) != old_length:
        struct.pack_into("<III", new_id_index, art_id * 12, 0, 0, 0)
        skip_count += 1
        continue

    # Decompress old blob
    try:
        orig_size = read_u32le(compressed_blob, 0)
        raw = lz4.block.decompress(compressed_blob[4:], uncompressed_size=orig_size)
    except Exception as e:
        print(f"  [SKIP] id={art_id}: decompress failed: {e}")
        struct.pack_into("<III", new_id_index, art_id * 12, 0, 0, 0)
        skip_count += 1
        continue

    parsed = parse_wiki_blob(raw)
    if parsed is None:
        print(f"  [SKIP] id={art_id}: parse failed")
        struct.pack_into("<III", new_id_index, art_id * 12, 0, 0, 0)
        skip_count += 1
        continue

    stored_id, title, html_bytes = parsed

    # If HTML is large (possibly truncated), re-read from ZIM for full content
    if len(html_bytes) >= REREAD_THRESHOLD:
        norm = normalise_title(title)
        zim_idx = zim_path_map.get(norm)
        if zim_idx is not None:
            try:
                entry = archive._get_entry_by_id(zim_idx)
                raw_html = entry.get_item().content.tobytes()
                full_html = strip_html_from_raw(raw_html, title_map)
                html_bytes = full_html.encode("utf-8")
                reread_count += 1
            except Exception as e:
                pass  # keep existing (possibly truncated) html_bytes

    # Pack into WKI2 format
    wki2_blob = pack_wki2(stored_id, title, html_bytes)

    # Roll to new chunk if needed
    if new_chunk_offset + len(wki2_blob) > ARTICLES_CHUNK_SIZE:
        new_chunk_file.close()
        new_chunk_id     += 1
        new_chunk_offset  = 0
        new_chunk_file = open(new_chunk_path(new_chunk_id), "wb")

    new_chunk_file.write(wki2_blob)
    struct.pack_into("<III", new_id_index, art_id * 12,
                     new_chunk_id, new_chunk_offset, len(wki2_blob))
    new_chunk_offset += len(wki2_blob)
    convert_count    += 1

    if convert_count % 10000 == 0:
        print(f"  {convert_count} articles converted (re-read from ZIM: {reread_count}) …")

new_chunk_file.close()
for f in old_chunk_cache.values():
    f.close()

# Write new id_index.bin
new_id_index_path = os.path.join(OUTPUT_DIR, "id_index.bin")
with open(new_id_index_path, "wb") as f:
    f.write(new_id_index)

print(f"\nDone: {convert_count} converted, {reread_count} re-read from ZIM, {skip_count} skipped")
print(f"Chunk files: {new_chunk_id + 1}")

# Copy unchanged files from input to output
COPY_FILES = ["index.bin", "sparse_index.bin", "index_meta.txt",
              "img_index.bin", "img_meta.txt"]
for fname in COPY_FILES:
    src = os.path.join(INPUT_DIR, fname)
    dst = os.path.join(OUTPUT_DIR, fname)
    if os.path.isfile(src):
        shutil.copy2(src, dst)
        print(f"Copied {fname}")

# Copy img chunk files
img_cid = 0
while True:
    fname = f"img_{img_cid:04d}.dat"
    src = os.path.join(INPUT_DIR, fname)
    if not os.path.isfile(src): break
    shutil.copy2(src, os.path.join(OUTPUT_DIR, fname))
    img_cid += 1
if img_cid:
    print(f"Copied {img_cid} image chunk file(s)")

print(f"\nOutput written to: {OUTPUT_DIR}")
print("Next steps:")
print("  1. Copy output_v2/* to SD card /wiki/ directory")
print("  2. Flash new firmware (already done if you just built it)")
print("  3. Reboot device")
