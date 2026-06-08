#!/usr/bin/env python3
# Written by Alun Morris and Claude Code
"""
build_wiki_db.py — Convert a Wikipedia ZIM file into an ESP32-friendly binary database.

Output files:
  index.bin              — fixed-width binary index, sorted by normalised title, binary-searchable
  sparse_index.bin       — every SPARSE_STEP-th title key prefix (12 B each); loaded into ESP32 RAM
                           so wikiDbSearch() needs zero SD seeks to locate the scan start
  articles_NNNN.dat      — LZ4-compressed article blobs split into 32MB chunks
  id_index.bin           — flat array: [chunk_id uint32][offset uint32][length uint32] per article
  index_meta.txt         — plain-text metadata (article_count, record_size, chunk_size, chunk_count)

Usage:
  python3 build_wiki_db.py <input.zim> <output_dir> [--limit N] [--workers N] [--verbose]

Install dependencies:
  pip install libzim lz4 beautifulsoup4 lxml
"""

import argparse
import multiprocessing
import os
import re
import struct
import sys
import tempfile
from datetime import datetime
from urllib.parse import unquote

# --- dependency checks -----------------------------------------------------------

def _check_imports():
    missing = []
    try:
        import libzim  # noqa: F401
    except ImportError:
        missing.append("libzim          (pip install libzim)")
    try:
        import lz4.block  # noqa: F401
    except ImportError:
        missing.append("lz4             (pip install lz4)")
    try:
        import bs4  # noqa: F401
    except ImportError:
        missing.append("beautifulsoup4  (pip install beautifulsoup4)")
    try:
        import lxml  # noqa: F401
    except ImportError:
        missing.append("lxml            (pip install lxml)")
    if missing:
        print("Missing dependencies:")
        for m in missing:
            print(f"  {m}")
        sys.exit(1)

_check_imports()

import lz4.block
from libzim import Archive
from bs4 import BeautifulSoup, Comment

# --- constants ------------------------------------------------------------------
# SPARSE_STEP and SPARSE_KEY_LEN MUST match config.h in the firmware.

INDEX_RECORD_SIZE   = 80
TITLE_KEY_LEN       = 60                    # bytes, null-padded
SPARSE_STEP         = 64                    # sample every Nth record into sparse_index.bin
SPARSE_KEY_LEN      = 12                    # bytes kept per sparse entry (title key prefix)
WORD_KEY_LEN        = 14                    # 13 chars + null per word_index.bin entry
WORD_ENTRY_SIZE     = 20                    # WORD_KEY_LEN(14) + ids_start(4) + ids_count(2)
TITLE_ENTRY_SIZE    = 32                    # 31 chars + null in title_index.bin

# Common short words that don't help discriminate article titles.
_STOP_WORDS = frozenset({
    "the","of","in","and","for","to","a","an","is","are","was","be","by","or",
    "at","on","as","it","its","not","one","two","new","old","all","any","but",
    "can","has","had","his","her","our","us","we","he","she","they","this",
    "that","with","from","into","than","been","have","were","also","which",
    "when","where","who","what","how","may","other","after","both","between",
    "see","use","used","being","about","more","most","some","no","so","do",
})
MAGIC               = b"WKI2"              # 4-byte article magic (v2 = multi-block format)
BLOCK_SIZE          = 60_000               # max bytes of HTML per compressed block (≈ one page)
CHUNK_WARN_SIZE     = 16 * 1024 * 1024     # skip articles larger than 16 MB uncompressed
ARTICLES_CHUNK_SIZE = 32 * 1024 * 1024     # 32MB per chunk file

# --- module-level globals shared with worker processes via fork ------------------

# Set by the main process before Pool creation; inherited copy-on-write by workers.
_g_title_map: dict[str, int] | None = None

# Opened per-worker in _worker_init; not used in the main process.
_g_archive: Archive | None = None

# Image info map: {zim_path → (img_id, width, height)}. Set before Pool creation.
_g_img_info: dict[str, tuple[int, int, int]] | None = None

# --- helpers --------------------------------------------------------------------

def normalise_title(title: str) -> str:
    # Replace Unicode dash variants with ASCII hyphen before lowercasing so
    # that titles like "Proto-Indo-European" (en-dash) index correctly.
    for ch in "‐‑‒–—―−⁃﹘﹣－":
        title = title.replace(ch, "-")
    return " ".join(title.lower().split())


def pack_index_record(title_key: str, article_id: int, offset: int, length: int) -> bytes:
    key_bytes = title_key.encode("utf-8")[:TITLE_KEY_LEN]
    key_padded = key_bytes.ljust(TITLE_KEY_LEN, b"\x00")
    return struct.pack("<60sIII8s", key_padded, article_id, offset, length, b"\x00" * 8)


def _is_internal_link(href: str) -> bool:
    """True if href is an internal article link (bare title, not CSS/JS/anchor/external)."""
    return (
        href
        and not href.startswith("#")
        and not href.startswith("http")
        and not href.startswith("./_")
        and not href.startswith("./")
        and not href.startswith("../")
        and "://" not in href
    )


def _resolve_img_src(src: str, img_info: dict[str, tuple[int, int, int]]) -> tuple[int, int, int] | None:
    """Resolve an <img src> value to (img_id, w, h) using the img_info map, or None if not found."""
    if not src:
        return None
    # Direct lookup
    if src in img_info:
        return img_info[src]
    # Strip leading ../ or ./ prefixes
    s = src
    while s.startswith("../") or s.startswith("./"):
        s = s.split("/", 1)[1]
    if s in img_info:
        return img_info[s]
    # Try bare filename with common ZIM image namespace prefixes
    basename = s.split("/")[-1]
    for prefix in ("I/", "A/I/"):
        key = prefix + basename
        if key in img_info:
            return img_info[key]
    # Try the bare filename itself
    if basename in img_info:
        return img_info[basename]
    return None


def strip_html(raw_html: bytes, title_map: dict[str, int],
               img_info: dict[str, tuple[int, int, int]] | None = None) -> str:
    """
    Strip a Wikipedia HTML article down to minimal, ESP32-friendly HTML.
    Internal links are rewritten to /wiki/<article_id>.
    If img_info is provided, <img> tags with resolvable sources are rewritten
    to <img src="/img/ID" w="W" h="H">; otherwise they are removed.
    """
    soup = BeautifulSoup(raw_html, "lxml")

    # Remove the ZIM-injected licence footer (saves ~40 MB across the database).
    for tag in soup.find_all("div", class_="zim-footer"):
        tag.decompose()

    # Remove tags that contain no useful content (NOT figure — it wraps <img>)
    for tag in soup.find_all(["script", "style", "table",
                               "sup", "sub", "noscript", "meta", "link",
                               "header", "footer", "nav", "aside", "math"]):
        tag.decompose()

    for comment in soup.find_all(string=lambda t: isinstance(t, Comment)):
        comment.extract()

    # Handle <img> tags: rewrite to /img/ID format or remove.
    # Must run BEFORE figure unwrap so imgs inside <figure> are processed first.
    for img_tag in list(soup.find_all("img")):
        info = None
        if img_info:
            info = _resolve_img_src(img_tag.get("src", ""), img_info)
        if info is not None:
            img_id, iw, ih = info
            new_tag = soup.new_tag("img", src=f"/img/{img_id}", w=str(iw), h=str(ih))
            img_tag.replace_with(new_tag)
        else:
            img_tag.decompose()

    # Unwrap <figure> containers: keep the rewritten <img> child in place,
    # discard the <figure> wrapper and any <figcaption> text.
    for cap in soup.find_all("figcaption"):
        cap.decompose()
    for fig in soup.find_all("figure"):
        fig.unwrap()

    for a_tag in soup.find_all("a"):
        href = a_tag.get("href", "")
        a_tag.attrs = {}
        if _is_internal_link(href):
            raw_title = href.split("#")[0].replace("_", " ")
            target_norm = normalise_title(raw_title)
            if target_norm in title_map:
                a_tag["href"] = f"/wiki/{title_map[target_norm]}"
            else:
                a_tag.unwrap()

    for tag in soup.find_all(True):
        if tag.name not in ("a", "img"):
            tag.attrs = {}

    body = (soup.find("div", id="mw-content-text")
            or soup.find("div", class_="mw-parser-output")
            or soup.find("body")
            or soup)
    return str(body)


def _split_html_blocks(html_bytes: bytes) -> list[bytes]:
    """Split html_bytes into chunks of at most BLOCK_SIZE, breaking at '>' boundaries."""
    blocks: list[bytes] = []
    start = 0
    total = len(html_bytes)
    while start < total:
        end = start + BLOCK_SIZE
        if end >= total:
            blocks.append(html_bytes[start:])
            break
        # Prefer to cut just after the last '>' so we never split a tag.
        cut = html_bytes.rfind(b">", start, end)
        if cut <= start:
            cut = end  # fallback: hard cut
        else:
            cut += 1   # include the '>'
        blocks.append(html_bytes[start:cut])
        start = cut
    return blocks


def pack_article(article_id: int, title: str, html: str) -> bytes:
    """Pack an article into the WKI2 multi-block format.

    Layout (stored directly in chunk file — NOT wrapped in an outer LZ4 blob):
        WKI2[4]           magic
        article_id[4]     uint32 LE
        title_len[2]      uint16 LE
        title[N]          UTF-8 bytes
        num_blocks[2]     uint16 LE
        repeated:
          compressed_len[4]  uint32 LE  (= len(lz4.block.compress(block, store_size=True)))
          lz4_data[N]        bytes      (first 4B = original block size; rest = LZ4 block)
    """
    title_bytes = title.encode("utf-8")
    html_bytes  = html.encode("utf-8")
    blocks      = _split_html_blocks(html_bytes)
    compressed  = [lz4.block.compress(b, store_size=True) for b in blocks]

    blob = MAGIC
    blob += struct.pack("<I", article_id)
    blob += struct.pack("<H", len(title_bytes)) + title_bytes
    blob += struct.pack("<H", len(compressed))
    for c in compressed:
        blob += struct.pack("<I", len(c)) + c
    return blob


def compress_article(raw: bytes) -> bytes:
    """WKI2 blobs are self-contained (per-block LZ4); no outer compression needed."""
    return raw


# --- main passes ----------------------------------------------------------------

def _is_article_entry(entry) -> bool:
    """True if this ZIM entry is a real HTML article (not a redirect, resource, or metadata)."""
    if entry.is_redirect:
        return False
    path = entry.path
    if path.startswith("_") or path.startswith("-"):
        return False
    if "/" in path and not path.startswith("A/") and not path.startswith("a/"):
        return False
    try:
        item = entry.get_item()
        return item.mimetype.startswith("text/html")
    except Exception:
        return False


def extract_images(zim_path: str, archive, output_dir: str,
                   verbose: bool, jpeg_quality: int = 90,
                   thumb_size: tuple[int, int] = (320, 212)) -> dict[str, tuple[int, int, int]]:
    """
    Extract and resize images from the ZIM file.
    Writes img_NNNN.dat chunk files, img_index.bin, and img_meta.txt.
    Returns img_info: {zim_entry_path → (img_id, width, height)}.
    """
    try:
        from PIL import Image as PilImage
        import io as _io
        import numpy as _np
        import qoi as _qoi
    except ImportError:
        print("Pillow/qoi not available — skipping image extraction (pip install Pillow qoi)")
        return {}

    IMG_CHUNK_SIZE  = 4 * 1024 * 1024   # 4 MB per chunk file
    MAX_W, MAX_H    = thumb_size
    MIN_DIM         = 50                 # skip tiny icons (not applied to SVG math)
    INCLUDE_MIME    = {"image/jpeg", "image/png", "image/webp"}
    # SVG equations use a prefix match (MIME may carry charset/profile params)
    SVG_MIME_PREFIX = "image/svg+xml"

    img_info: dict[str, tuple[int, int, int]] = {}
    index_entries: list[tuple[int, int, int]] = []   # (chunk_id, offset, length)

    img_id               = 0
    current_chunk_id     = 0
    within_chunk_offset  = 0

    def img_chunk_path(cid: int) -> str:
        return os.path.join(output_dir, f"img_{cid:04d}.dat")

    chunk_file = open(img_chunk_path(0), "wb")

    total = archive.entry_count
    for i in range(total):
        try:
            entry = archive._get_entry_by_id(i)
        except Exception:
            continue

        if entry.is_redirect:
            continue

        path = entry.path
        # Skip MediaWiki chrome (UI icons, logos).
        # Note: article images live under _assets_/HASH/filename so we must NOT
        # filter on startswith("_") — only the _mw_/ namespace is chrome.
        if "_mw_/" in path:
            continue

        try:
            item = entry.get_item()
            mime = item.mimetype
        except Exception:
            continue

        is_svg = mime.startswith(SVG_MIME_PREFIX)
        if not is_svg and mime not in INCLUDE_MIME:
            continue

        try:
            raw = item.content.tobytes()
            if is_svg:
                # Rasterise SVG equation at screen width.
                # Keep natural dark ink — composite onto white to match light theme.
                import cairosvg as _cairosvg
                png_data = _cairosvg.svg2png(bytestring=raw, output_width=MAX_W)
                img = PilImage.open(_io.BytesIO(png_data))
                if img.mode != "RGBA":
                    img = img.convert("RGBA")
            else:
                img = PilImage.open(_io.BytesIO(raw))
            orig_w, orig_h = img.size
        except Exception:
            continue

        # Skip tiny icons — but don't apply to SVG (equations can be short)
        if not is_svg and (orig_w < MIN_DIM or orig_h < MIN_DIM):
            continue

        # Source format is the primary signal (ZIM re-encodes everything as WebP
        # internally, so mime is always image/webp — use the path extension instead):
        #   .png/.PNG → lossless QOI (Wikipedia uses PNG for diagrams/illustrations;
        #               anti-aliased edges create >256 colours so colour-count alone
        #               wrongly classifies them as photos → DCT ringing artefacts).
        #   .jpg/.jpeg → JPEG q=90 (already lossy).
        #   SVG        → QOI (rasterised diagram).
        # Colour-count fallback for anything else.
        src_ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
        is_diagram = (is_svg
                      or src_ext == "png"
                      or img.convert("RGB").getcolors(maxcolors=256) is not None)

        # Resize to fit MAX_W × MAX_H for both raster and SVG.
        # SVG equations can be arbitrarily tall; thumbnail caps the height
        # so that firmware thumbnails (ih/2) never crop the bottom.
        img.thumbnail((MAX_W, MAX_H), PilImage.LANCZOS)
        w, h = img.size
        if is_diagram:
            if img.mode in ("RGBA", "LA", "P"):
                img_rgba = img.convert("RGBA")
                bg = PilImage.new("RGB", img_rgba.size, (255, 255, 255))
                bg.paste(img_rgba, mask=img_rgba.split()[3])
                rgb = bg
            else:
                rgb = img.convert("RGB")
            img_bytes = _qoi.encode(_np.array(rgb))
        else:
            if img.mode in ("RGBA", "LA", "P"):
                img_rgba = img.convert("RGBA")
                bg = PilImage.new("RGB", img_rgba.size, (255, 255, 255))
                bg.paste(img_rgba, mask=img_rgba.split()[3])
                rgb = bg
            else:
                rgb = img.convert("RGB")
            buf = _io.BytesIO()
            rgb.save(buf, format="JPEG", quality=jpeg_quality)
            img_bytes = buf.getvalue()

        # Roll to a new chunk file if needed
        if within_chunk_offset + len(img_bytes) > IMG_CHUNK_SIZE:
            chunk_file.close()
            current_chunk_id += 1
            within_chunk_offset = 0
            chunk_file = open(img_chunk_path(current_chunk_id), "wb")

        chunk_file.write(img_bytes)
        index_entries.append((current_chunk_id, within_chunk_offset, len(img_bytes)))
        within_chunk_offset += len(img_bytes)

        # Register by ZIM path and common bare-filename variants.
        # Path example: "_assets_/HASH/filename.jpg"
        # HTML src example: "./_assets_/HASH/filename.jpg" → _resolve_img_src strips "./"
        img_info[path] = (img_id, w, h)
        # Also register by bare filename for fallback resolution
        basename = path.split("/")[-1] if "/" in path else path
        if basename not in img_info:
            img_info[basename] = (img_id, w, h)

        img_id += 1
        if verbose and img_id % 1000 == 0:
            print(f"  Images: {img_id} extracted...")

    chunk_file.close()

    if img_id == 0:
        # No images found — remove empty chunk file and skip writing index
        try:
            os.unlink(img_chunk_path(0))
        except OSError:
            pass
        print("Image extraction: 0 images found (ZIM may have _pictures:no)")
        return {}

    # Write img_index.bin (12 bytes per entry: chunk_id u32 + offset u32 + length u32)
    img_index_path = os.path.join(output_dir, "img_index.bin")
    with open(img_index_path, "wb") as f:
        for cid, off, length in index_entries:
            f.write(struct.pack("<III", cid, off, length))

    # Write img_meta.txt
    img_meta_path = os.path.join(output_dir, "img_meta.txt")
    with open(img_meta_path, "w") as f:
        f.write(f"image_count={img_id}\n")
        f.write(f"chunk_count={current_chunk_id + 1}\n")
        f.write(f"chunk_size={IMG_CHUNK_SIZE}\n")

    # Remove stale chunk files from a previous run that had more chunks.
    stale = current_chunk_id + 1
    while os.path.exists(img_chunk_path(stale)):
        os.unlink(img_chunk_path(stale))
        stale += 1

    print(f"Image extraction complete: {img_id} images in {current_chunk_id + 1} chunk(s)")
    return img_info


def build_title_map(archive: Archive, limit: int, verbose: bool) -> dict[str, int]:
    """Pass 1: assign sequential article IDs and build normalised title → ID map."""
    title_map: dict[str, int] = {}
    article_id = 0

    total = archive.entry_count
    if verbose:
        print(f"ZIM has {total} entries (including redirects and non-article pages)")

    for i in range(total):
        if limit and article_id >= limit:
            break
        try:
            entry = archive._get_entry_by_id(i)
        except Exception:
            continue

        if not _is_article_entry(entry):
            continue

        path = entry.path
        title = entry.title or unquote(path.split("/", 1)[-1]).replace("_", " ")
        norm = normalise_title(title)
        if norm and norm not in title_map:
            title_map[norm] = article_id
            article_id += 1

        if verbose and article_id % 10000 == 0:
            print(f"  Pass 1: {article_id} articles indexed...")

    print(f"Pass 1 complete: {len(title_map)} unique articles found")
    return title_map


# --- worker functions (run in child processes) -----------------------------------

def _worker_init(zim_path: str) -> None:
    """Called once per worker process to open a private Archive handle."""
    global _g_archive
    _g_archive = Archive(zim_path)


def _process_entry(entry_id: int):
    """
    Process one ZIM entry. Returns (article_id, norm_title, compressed_bytes) or None.
    _g_title_map is inherited from the parent via fork (read-only, copy-on-write).
    """
    try:
        entry = _g_archive._get_entry_by_id(entry_id)
    except Exception:
        return None

    if not _is_article_entry(entry):
        return None

    path = entry.path
    title = entry.title or path.split("/", 1)[-1].replace("_", " ")
    norm = normalise_title(title)
    if not norm or norm not in _g_title_map:
        return None

    article_id = _g_title_map[norm]

    try:
        item = entry.get_item()
        raw_html = item.content.tobytes()
    except Exception:
        return None

    try:
        html = strip_html(raw_html, _g_title_map, _g_img_info)
    except Exception:
        return None

    raw = pack_article(article_id, title, html)
    if len(raw) > CHUNK_WARN_SIZE:
        return None

    return (article_id, norm, compress_article(raw))


# --- ZIM display name -----------------------------------------------------------

_LANG_MAP = {
    'en': 'English', 'fr': 'French', 'de': 'German', 'es': 'Spanish',
    'it': 'Italian', 'pt': 'Portuguese', 'nl': 'Dutch', 'pl': 'Polish',
    'ru': 'Russian', 'ja': 'Japanese', 'zh': 'Chinese', 'ar': 'Arabic',
}

# Variants that prefix the language name (e.g. "simple" → "Simple English")
_LANG_VARIANTS = {'simple', 'nopic'}

def zim_display_name(zim_path: str) -> str:
    """Derive a human-readable name from the ZIM filename.

    wikipedia_en_simple_all_maxi_2026-05.zim → 'Simple English: All Maxi May 2026'
    wikipedia_en_all_mini_2026-03.zim        → 'English: All Mini March 2026'
    """
    stem = re.sub(r'\.zim$', '', os.path.basename(zim_path), flags=re.IGNORECASE)
    stem = re.sub(r'^wikipedia_', '', stem)
    parts = stem.split('_')

    # First part is always the language code; optionally followed by a variant.
    lang = _LANG_MAP.get(parts[0], parts[0].title()) if parts else 'Wikipedia'
    rest = parts[1:]

    if rest and rest[0].lower() in _LANG_VARIANTS:
        lang = f"{rest[0].title()} {lang}"
        rest = rest[1:]

    # Format remaining parts; convert YYYY-MM date to "Month YYYY".
    detail = []
    for p in rest:
        m = re.match(r'^(\d{4})-(\d{2})$', p)
        if m:
            try:
                dt = datetime(int(m.group(1)), int(m.group(2)), 1)
                detail.append(dt.strftime('%B %Y'))
            except ValueError:
                detail.append(p)
        else:
            detail.append(p.title())

    return f"{lang}: {' '.join(detail)}" if detail else lang


# --- Pass 2 ----------------------------------------------------------------------

def build_database(zim_path: str, archive: Archive, title_map: dict[str, int],
                   output_dir: str, limit: int, verbose: bool,
                   num_workers: int = 0, sparse_step: int = SPARSE_STEP,
                   img_info: dict[str, tuple[int, int, int]] | None = None) -> int:
    """Pass 2: process articles in parallel and write chunk files + index."""
    global _g_title_map, _g_img_info
    _g_title_map = title_map   # inherited by workers via fork
    _g_img_info  = img_info or {}

    if num_workers <= 0:
        num_workers = min(4, multiprocessing.cpu_count())

    print(f"Pass 2: using {num_workers} workers")

    # Bitmap to detect duplicate article IDs without a large Python set.
    max_article_id = len(title_map)
    seen_bitmap = bytearray((max_article_id + 7) // 8)

    def is_seen(aid: int) -> bool:
        return bool(seen_bitmap[aid >> 3] & (1 << (aid & 7)))

    def mark_seen(aid: int) -> None:
        seen_bitmap[aid >> 3] |= 1 << (aid & 7)

    written = 0
    skipped = 0

    current_chunk_id = 0
    within_chunk_offset = 0

    def chunk_path(cid: int) -> str:
        return os.path.join(output_dir, f"articles_{cid:04d}.dat")

    chunk_file = open(chunk_path(0), "wb")

    total = archive.entry_count

    # Stream index records to a temp file to avoid holding millions of tuples in RAM.
    # Format per record: 60s title_key + I article_id + I chunk_id + I offset + I length = 76 bytes
    TEMP_RECORD_FMT = "<60sIIII"
    TEMP_RECORD_SIZE = struct.calcsize(TEMP_RECORD_FMT)
    tmp_index_fd, tmp_index_path = tempfile.mkstemp(dir=output_dir, prefix=".idx_tmp_")
    tmp_index_file = os.fdopen(tmp_index_fd, "wb")

    try:
        with multiprocessing.Pool(
            processes=num_workers,
            initializer=_worker_init,
            initargs=(zim_path,),
        ) as pool:
            for result in pool.imap_unordered(_process_entry, range(total), chunksize=64):
                if result is None:
                    continue

                article_id, norm, compressed = result

                if is_seen(article_id):
                    skipped += 1
                    continue
                mark_seen(article_id)

                if within_chunk_offset + len(compressed) > ARTICLES_CHUNK_SIZE:
                    chunk_file.close()
                    current_chunk_id += 1
                    within_chunk_offset = 0
                    chunk_file = open(chunk_path(current_chunk_id), "wb")

                chunk_file.write(compressed)

                # Write record to temp file immediately — no in-memory list.
                key_bytes = norm.encode("utf-8")[:60].ljust(60, b"\x00")
                tmp_index_file.write(struct.pack(TEMP_RECORD_FMT,
                    key_bytes, article_id, current_chunk_id, within_chunk_offset, len(compressed)))

                within_chunk_offset += len(compressed)
                written += 1

                if written % 5000 == 0:
                    print(f"  Pass 2: {written} articles written...")

                if limit and written >= limit:
                    pool.terminate()
                    break
    finally:
        chunk_file.close()
        tmp_index_file.close()

    print(f"  Chunks written: {current_chunk_id + 1}")
    print(f"Pass 2 complete: {written} written, {skipped} skipped")

    # Read temp records, sort by title key, write index.bin + id_index.bin.
    print("Building index files...")
    records = []
    with open(tmp_index_path, "rb") as f:
        while True:
            chunk = f.read(TEMP_RECORD_SIZE)
            if len(chunk) < TEMP_RECORD_SIZE:
                break
            key_bytes, art_id, cid, off, length = struct.unpack(TEMP_RECORD_FMT, chunk)
            records.append((key_bytes.rstrip(b"\x00").decode("utf-8", errors="replace"),
                            art_id, cid, off, length))
    os.unlink(tmp_index_path)

    records.sort(key=lambda r: r[0])

    # Write index.bin and sparse_index.bin in one pass over sorted records.
    # sparse_index.bin: first SPARSE_KEY_LEN bytes of every sparse_step-th title key.
    # The ESP32 loads this entirely into RAM so wikiDbSearch() needs zero SD seeks
    # to locate the scan start.  sparse_step is recorded in index_meta.txt so the
    # firmware knows the stride without re-reading the full index.
    index_path  = os.path.join(output_dir, "index.bin")
    sparse_path = os.path.join(output_dir, "sparse_index.bin")
    with open(index_path, "wb") as idx, open(sparse_path, "wb") as sp:
        for i, (title_key, article_id, _cid, art_offset, length) in enumerate(records):
            idx.write(pack_index_record(title_key, article_id, art_offset, length))
            if i % sparse_step == 0:
                key_bytes = title_key.encode("utf-8")[:SPARSE_KEY_LEN].ljust(SPARSE_KEY_LEN, b"\x00")
                sp.write(key_bytes)

    # id_index.bin — flat array: [chunk_id uint32][offset uint32][length uint32] = 12 bytes per slot.
    max_id = max(r[1] for r in records) if records else 0
    id_array: list[tuple[int, int, int]] = [(0, 0, 0)] * (max_id + 1)
    for _title_key, art_id, cid, art_offset, length in records:
        id_array[art_id] = (cid, art_offset, length)
    id_index_path = os.path.join(output_dir, "id_index.bin")
    with open(id_index_path, "wb") as iid:
        for cid, art_offset, length in id_array:
            iid.write(struct.pack("<III", cid, art_offset, length))

    # Write metadata.
    meta_path = os.path.join(output_dir, "index_meta.txt")
    with open(meta_path, "w") as m:
        m.write(f"article_count={written}\n")
        m.write(f"record_size={INDEX_RECORD_SIZE}\n")
        m.write(f"chunk_size={ARTICLES_CHUNK_SIZE}\n")
        m.write(f"chunk_count={current_chunk_id + 1}\n")
        m.write(f"sparse_step={sparse_step}\n")
        m.write(f"sparse_key_len={SPARSE_KEY_LEN}\n")
        m.write(f"db_name={zim_display_name(zim_path)}\n")

    return written


# --- word index -----------------------------------------------------------------

def build_word_index(output_dir: str) -> None:
    """Build word_index.bin and title_index.bin from the existing index.bin.

    word_index.bin  — binary-searchable sorted word table mapping each title
                      word to a list of article IDs whose title contains it.
    title_index.bin — flat array of TITLE_ENTRY_SIZE-byte titles indexed by
                      article_id, for O(1) title lookup by the firmware.

    No ZIM file needed; reads only index.bin from output_dir.
    """
    index_path       = os.path.join(output_dir, "index.bin")
    word_index_path  = os.path.join(output_dir, "word_index.bin")
    title_index_path = os.path.join(output_dir, "title_index.bin")

    if not os.path.isfile(index_path):
        print(f"Error: {index_path} not found — run full build first.")
        return

    print("Reading index.bin to build word / title indexes...")
    word_to_ids: dict[str, set[int]] = {}
    titles: dict[int, str] = {}
    max_article_id = 0

    with open(index_path, "rb") as f:
        while True:
            rec = f.read(INDEX_RECORD_SIZE)
            if len(rec) < INDEX_RECORD_SIZE:
                break
            title = rec[:TITLE_KEY_LEN].rstrip(b"\x00").decode("utf-8", errors="replace")
            article_id = struct.unpack_from("<I", rec, TITLE_KEY_LEN)[0]
            titles[article_id] = title
            if article_id > max_article_id:
                max_article_id = article_id
            for raw in title.split():
                # Keep only alnum and ASCII hyphen; strip edge hyphens
                word = "".join(ch for ch in raw if ch.isalnum() or ch == "-").strip("-")
                if len(word) < 3 or word in _STOP_WORDS:
                    continue
                word_to_ids.setdefault(word[:WORD_KEY_LEN - 1], set()).add(article_id)

    print(f"  {len(titles)} articles, {len(word_to_ids)} unique words")

    # Build sorted word table + flat IDs array
    sorted_words = sorted(word_to_ids)
    ids_flat: list[int] = []
    entries: list[tuple[bytes, int, int]] = []
    for word in sorted_words:
        ids = sorted(word_to_ids[word])
        ids_start  = len(ids_flat)
        ids_count  = min(len(ids), 500)   # cap per-word list to keep file manageable
        key = word.encode()[:WORD_KEY_LEN - 1].ljust(WORD_KEY_LEN, b"\x00")
        entries.append((key, ids_start, ids_count))
        ids_flat.extend(ids[:ids_count])

    W = len(entries)
    ids_section_offset = 8 + W * WORD_ENTRY_SIZE

    word_kb  = (ids_section_offset + len(ids_flat) * 4) // 1024
    title_kb = (max_article_id + 1) * TITLE_ENTRY_SIZE // 1024
    print(f"  Writing word_index.bin  ({word_kb} KB, {W} words, {len(ids_flat)} ID entries)")
    with open(word_index_path, "wb") as f:
        f.write(struct.pack("<II", W, ids_section_offset))
        for key, ids_start, ids_count in entries:
            f.write(key)
            f.write(struct.pack("<IH", ids_start, ids_count))
        for aid in ids_flat:
            f.write(struct.pack("<I", aid))

    print(f"  Writing title_index.bin ({title_kb} KB, {max_article_id + 1} entries)")
    with open(title_index_path, "wb") as f:
        for article_id in range(max_article_id + 1):
            title = titles.get(article_id, "")
            f.write(title.encode("utf-8")[:TITLE_ENTRY_SIZE - 1].ljust(TITLE_ENTRY_SIZE, b"\x00"))

    print("Word index build complete.")


# --- entry point ----------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Convert Wikipedia ZIM to ESP32 binary database")
    parser.add_argument("zim_file", nargs="?", help="Path to input .zim file (not needed for --word-index-only)")
    parser.add_argument("output_dir", help="Directory for output files")
    parser.add_argument("--limit", type=int, default=0, metavar="N",
                        help="Process only first N articles (0 = all)")
    parser.add_argument("--workers", type=int, default=0, metavar="N",
                        help="Number of worker processes (0 = auto, uses all CPU cores)")
    parser.add_argument("--sparse-step", type=int, default=SPARSE_STEP, metavar="N",
                        help=f"Sample every Nth index record into sparse_index.bin (default {SPARSE_STEP}). "
                             f"Increase for very large ZIM files to keep the sparse index within ESP32 RAM.")
    parser.add_argument("--verbose", action="store_true", help="Extra progress output")
    parser.add_argument("--jpeg-quality", type=int, default=90, metavar="Q",
                        help="JPEG quality for photo thumbnails, 1–95 (default: 90)")
    parser.add_argument("--thumb-size", default="320x212", metavar="WxH",
                        help="Max thumbnail dimensions in pixels (default: 320x212)")
    parser.add_argument("--no-images", action="store_true",
                        help="Skip image extraction pass (use for _pictures:no ZIM files)")
    parser.add_argument("--images-only", action="store_true",
                        help="Only redo image extraction; skip article build (reuses existing article files)")
    parser.add_argument("--word-index-only", action="store_true",
                        help="Build word_index.bin and title_index.bin from existing index.bin (no ZIM needed)")
    args = parser.parse_args()

    try:
        tw, th = (int(x) for x in args.thumb_size.lower().split("x"))
        thumb_size = (tw, th)
    except Exception:
        print(f"Error: --thumb-size must be WxH (e.g. 240x159), got: {args.thumb_size}")
        sys.exit(1)

    if args.word_index_only:
        build_word_index(args.output_dir)
        return

    if not args.zim_file or not os.path.isfile(args.zim_file):
        print(f"Error: ZIM file not found: {args.zim_file}")
        sys.exit(1)

    os.makedirs(args.output_dir, exist_ok=True)

    print(f"Opening ZIM: {args.zim_file}")
    archive = Archive(args.zim_file)

    if args.images_only:
        if not args.no_images:
            extract_images(args.zim_file, archive, args.output_dir, args.verbose,
                           args.jpeg_quality, thumb_size)
        print("Done (images only).")
        return

    title_map = build_title_map(archive, args.limit, args.verbose)

    img_info: dict[str, tuple[int, int, int]] = {}
    if not args.no_images:
        img_info = extract_images(args.zim_file, archive, args.output_dir, args.verbose,
                                  args.jpeg_quality, thumb_size)

    written = build_database(args.zim_file, archive, title_map,
                             args.output_dir, args.limit, args.verbose,
                             args.workers, args.sparse_step, img_info)

    index_path  = os.path.join(args.output_dir, "index.bin")
    sparse_path = os.path.join(args.output_dir, "sparse_index.bin")
    index_mb  = os.path.getsize(index_path)  / 1024 / 1024
    sparse_kb = os.path.getsize(sparse_path) / 1024

    chunk_id = 0
    total_articles_bytes = 0
    while True:
        p = os.path.join(args.output_dir, f"articles_{chunk_id:04d}.dat")
        if not os.path.isfile(p):
            break
        total_articles_bytes += os.path.getsize(p)
        chunk_id += 1

    total_mb = total_articles_bytes / 1024 / 1024
    avg_kb = (total_articles_bytes / written / 1024) if written else 0

    print(f"\nDone. {written} articles written.")
    print(f"  articles chunks : {chunk_id} × ≤32MB  ({total_mb:.1f} MB total)")
    print(f"  index.bin       : {index_mb:.1f} MB")
    print(f"  sparse_index.bin: {sparse_kb:.1f} KB  ({written // args.sparse_step + 1} entries, step={args.sparse_step})")
    print(f"  Avg compressed article size: {avg_kb:.1f} KB")


if __name__ == "__main__":
    main()
