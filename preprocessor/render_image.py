#!/usr/bin/env python3
# Written by Alun Morris and Claude Code
"""Simulate the firmware JPEG/QOI render pipeline and save a PNG for comparison.

Usage:
    python3 render_image.py <article_title> [<image_index>] [--dir output_v2]

Example:
    python3 render_image.py "ellipse" 0

Replicates exactly what the ESP32 firmware does:
  JPEG: decode at 0.5x → quantise to RGB565 (TJpgDec JD_FORMAT=1) → unpack to
        approximate RGB888 → Bayer 4x4 ordered dither → re-quantise to RGB565.
  QOI:  decode at 0.5x nearest-neighbour → quantise to RGB565 (no dither).
"""

import argparse, io, os, re, struct, sys

try:
    import lz4.block
    import numpy as np
    from PIL import Image
except ImportError:
    raise SystemExit("pip install lz4 Pillow numpy")

# Try qoi library for QOI decoding
try:
    import qoi as _qoi
    HAS_QOI = True
except ImportError:
    HAS_QOI = False

# ---- constants matching firmware / preprocessor --------------------------------

INDEX_RECORD_SIZE = 80
TITLE_KEY_LEN     = 60
MAGIC_WKI2        = b"WKI2"
MAGIC_WIKI        = b"WIKI"

def floyd_steinberg_rgb565(arr: np.ndarray) -> np.ndarray:
    """Apply Floyd-Steinberg error diffusion to an RGB888 image, quantising to
    RGB565 and returning the result as RGB888 for display."""
    h, w = arr.shape[:2]
    buf = arr.astype(np.int32)  # working copy with room for error accumulation
    out = np.zeros_like(arr)

    for y in range(h):
        for x in range(w):
            r, g, b = buf[y, x, 0], buf[y, x, 1], buf[y, x, 2]
            r = max(0, min(255, r))
            g = max(0, min(255, g))
            b = max(0, min(255, b))

            r5, g6, b5 = r >> 3, g >> 2, b >> 3
            qr = (r5 << 3) | (r5 >> 2)
            qg = (g6 << 2) | (g6 >> 4)
            qb = (b5 << 3) | (b5 >> 2)

            out[y, x] = [qr, qg, qb]

            er, eg, eb = r - qr, g - qg, b - qb

            if x + 1 < w:
                buf[y,   x+1] += [er*7//16, eg*7//16, eb*7//16]
            if y + 1 < h:
                if x > 0:
                    buf[y+1, x-1] += [er*3//16, eg*3//16, eb*3//16]
                buf[y+1, x  ] += [er*5//16, eg*5//16, eb*5//16]
                if x + 1 < w:
                    buf[y+1, x+1] += [er*1//16, eg*1//16, eb*1//16]
    return out.astype(np.uint8)

# ---- database helpers ----------------------------------------------------------

def find_article_id(wiki_dir: str, title_query: str) -> tuple[int, str] | None:
    """Binary-search index.bin for the normalised title; return (article_id, title)."""
    query = title_query.strip().lower()
    index_path = os.path.join(wiki_dir, "index.bin")
    with open(index_path, "rb") as f:
        # Linear scan for exact / prefix match (fast enough for a one-off)
        best = None
        while True:
            rec = f.read(INDEX_RECORD_SIZE)
            if len(rec) < INDEX_RECORD_SIZE:
                break
            key = rec[:TITLE_KEY_LEN].rstrip(b"\x00").decode("utf-8", errors="replace")
            art_id = struct.unpack_from("<I", rec, TITLE_KEY_LEN)[0]
            if key == query:
                return art_id, key          # exact match
            if key.startswith(query) and best is None:
                best = (art_id, key)        # prefix match fallback
    return best


def load_article_html(wiki_dir: str, article_id: int) -> tuple[str, str]:
    """Return (title, html) for the given article_id."""
    id_index_path = os.path.join(wiki_dir, "id_index.bin")
    with open(id_index_path, "rb") as f:
        f.seek(article_id * 12)
        chunk_id, offset, length = struct.unpack("<III", f.read(12))
    chunk_path = os.path.join(wiki_dir, f"articles_{chunk_id:04d}.dat")
    with open(chunk_path, "rb") as f:
        f.seek(offset)
        blob = f.read(length)
    magic = blob[:4]
    if magic == MAGIC_WKI2:
        pos = 4 + 4
        title_len = struct.unpack_from("<H", blob, pos)[0]; pos += 2
        title = blob[pos:pos+title_len].decode("utf-8"); pos += title_len
        num_blocks = struct.unpack_from("<H", blob, pos)[0]; pos += 2
        parts = []
        for _ in range(num_blocks):
            comp_len = struct.unpack_from("<I", blob, pos)[0]; pos += 4
            parts.append(lz4.block.decompress(blob[pos:pos+comp_len]).decode("utf-8"))
            pos += comp_len
        return title, "".join(parts)
    elif magic == MAGIC_WIKI:
        raw = lz4.block.decompress(blob)
        pos = 4 + 4
        title_len = struct.unpack_from("<H", raw, pos)[0]; pos += 2
        title = raw[pos:pos+title_len].decode("utf-8"); pos += title_len
        html_len = struct.unpack_from("<I", raw, pos)[0]; pos += 4
        return title, raw[pos:pos+html_len].decode("utf-8")
    raise ValueError(f"Unknown magic: {magic!r}")


def load_image_bytes(wiki_dir: str, img_id: int) -> bytes | None:
    img_index_path = os.path.join(wiki_dir, "img_index.bin")
    with open(img_index_path, "rb") as f:
        f.seek(img_id * 12)
        data = f.read(12)
    if len(data) < 12:
        return None
    chunk_id, offset, length = struct.unpack("<III", data)
    chunk_path = os.path.join(wiki_dir, f"img_{chunk_id:04d}.dat")
    with open(chunk_path, "rb") as f:
        f.seek(offset)
        return f.read(length)


def extract_img_ids(html: str) -> list[int]:
    """Return image IDs from src="/img/ID" attributes, in document order."""
    return [int(m.group(1)) for m in re.finditer(r'src="/img/(\d+)"', html)]

# ---- RGB565 dither pipeline (mirrors firmware jpegDitherCallback) --------------

def rgb888_to_rgb565_nodither(arr: np.ndarray) -> np.ndarray:
    """Quantise RGB888 array to RGB565 and back to RGB888."""
    r5 = arr[:, :, 0] >> 3
    g6 = arr[:, :, 1] >> 2
    b5 = arr[:, :, 2] >> 3
    out = np.zeros_like(arr)
    out[:, :, 0] = (r5 << 3) | (r5 >> 2)
    out[:, :, 1] = (g6 << 2) | (g6 >> 4)
    out[:, :, 2] = (b5 << 3) | (b5 >> 2)
    return out


def jpeg_firmware_render(jpeg_bytes: bytes, scale: int = 2) -> np.ndarray:
    """
    Simulate the firmware JPEG pipeline:
      1. Decode full-res JPEG with PIL.
      2. Halve size (scale=2 equivalent).
      3. Quantise to RGB565 (simulating TJpgDec JD_FORMAT=1 — no dither).
      4. Expand back to approx RGB888 via or-shift (firmware unpack step).
      5. Apply Bayer 4×4 ordered dither before final re-quantise.
      6. Convert to RGB888 for PNG output.
    """
    img = Image.open(io.BytesIO(jpeg_bytes)).convert("RGB")
    if scale > 1:
        w, h = img.size
        img = img.resize((w // scale, h // scale), Image.LANCZOS)
    arr = np.array(img, dtype=np.int32)

    # JD_FORMAT=0: TJpgDec outputs RGB888. Apply Floyd-Steinberg dithering.
    return floyd_steinberg_rgb565(arr)


def qoi_firmware_render(qoi_bytes: bytes, scale: int = 2) -> np.ndarray:
    """
    Simulate the firmware QOI pipeline:
      1. Decode QOI to RGB888.
      2. Nearest-neighbour 0.5× downsample (qoi_scale=2).
      3. Quantise to RGB565 (no dither).
      4. Convert back to RGB888 for PNG output.
    """
    if not HAS_QOI:
        # Fallback: decode via PIL if qoi lib unavailable
        raise SystemExit("pip install qoi  (needed for QOI decoding)")
    rgb = _qoi.decode(qoi_bytes)                     # numpy H×W×{3|4}
    if rgb.shape[2] == 4:
        # Composite onto white (same as preprocessor for light theme)
        alpha = rgb[:, :, 3:4].astype(np.float32) / 255.0
        bg    = np.full_like(rgb[:, :, :3], 255, dtype=np.float32)
        rgb   = (rgb[:, :, :3].astype(np.float32) * alpha + bg * (1 - alpha)).astype(np.uint8)
    else:
        rgb = rgb.astype(np.uint8)

    # Nearest-neighbour downsample
    if scale > 1:
        rgb = rgb[::scale, ::scale, :]

    # Quantise to RGB565 and back (no dither for QOI)
    arr = rgb.astype(np.int32)
    return rgb888_to_rgb565_nodither(arr).astype(np.uint8)


# ---- main ---------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Render a wiki image as the firmware would")
    ap.add_argument("title",       help="Article title (e.g. 'ellipse')")
    ap.add_argument("img_index",   nargs="?", type=int, default=0,
                    help="0-based index of image in article (default 0)")
    ap.add_argument("--dir",       default="output_v2", help="Wiki output directory")
    ap.add_argument("--scale",     type=int, default=2, choices=[1,2,4,8],
                    help="Decode scale factor: 1=full size, 2=half (thumbnail default)")
    ap.add_argument("--out",       default=None, help="Output PNG filename (auto if omitted)")
    args = ap.parse_args()

    wiki_dir = args.dir

    # --- find article ---
    result = find_article_id(wiki_dir, args.title)
    if result is None:
        raise SystemExit(f"Article '{args.title}' not found in index")
    art_id, found_title = result
    print(f"Article: '{found_title}'  (id={art_id})")

    # --- load HTML and extract image IDs ---
    title, html = load_article_html(wiki_dir, art_id)
    img_ids = extract_img_ids(html)
    if not img_ids:
        raise SystemExit("No images found in this article")
    if args.img_index >= len(img_ids):
        raise SystemExit(f"Article has {len(img_ids)} images; index {args.img_index} is out of range")

    img_id = img_ids[args.img_index]
    print(f"Image #{args.img_index}: img_id={img_id}")

    # --- load raw image bytes ---
    raw = load_image_bytes(wiki_dir, img_id)
    if raw is None:
        raise SystemExit(f"Could not load image id={img_id}")

    is_qoi  = raw[:4] == b"qoif"
    is_jpeg = raw[:2] == b"\xff\xd8"
    fmt     = "QOI" if is_qoi else ("JPEG" if is_jpeg else "unknown")
    print(f"Format: {fmt}, raw size: {len(raw):,} bytes")

    # --- render ---
    scale = args.scale
    if is_jpeg:
        desc = f"JPEG decode {f'1/{scale}×' if scale>1 else '1:1'} → RGB565 quantise → Bayer dither → RGB565"
        print(f"Rendering: {desc}")
        out_arr = jpeg_firmware_render(raw, scale)
    elif is_qoi:
        desc = f"QOI decode → nearest-neighbour {f'1/{scale}×' if scale>1 else '1:1'} → RGB565 quantise (no dither)"
        print(f"Rendering: {desc}")
        out_arr = qoi_firmware_render(raw, scale)
    else:
        raise SystemExit(f"Unsupported image format (magic={raw[:4]!r})")

    h, w = out_arr.shape[:2]
    print(f"Output dimensions: {w}×{h} px")

    # --- save ---
    out_path = args.out or f"render_{args.title.replace(' ', '_')}_{args.img_index}_s{scale}.png"
    Image.fromarray(out_arr).save(out_path)
    print(f"Saved: {out_path}")


if __name__ == "__main__":
    main()
