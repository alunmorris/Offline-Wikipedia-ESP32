#!/usr/bin/env python3
# Written by Alun Morris and Claude Code
"""
debug_server.py — Browser-based debug viewer for Wikipedia offline reader output.

Reads articles and images directly from the preprocessor output binary files,
so you can verify the content without flashing to the device.

Usage:
    python3 debug_server.py                   # defaults: output_maxi/ on port 8765
    python3 debug_server.py --dir /path/to/output --port 9000

Then open: http://localhost:8765
"""

import argparse, os, struct, json, re, bisect, io
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

try:
    import lz4.block
except ImportError:
    raise SystemExit("pip install lz4")

try:
    import numpy as np
    from PIL import Image as _PIL
    _HAS_NP = True
except ImportError:
    _HAS_NP = False

import subprocess, struct as _struct
_HELPER = os.path.join(os.path.dirname(__file__), "tjpgdec_helper")

def _tjpgdec_decode(jpeg_bytes, scale=2):
    """Decode JPEG via the exact TJpgDec binary (firmware-identical IDCT+YCbCr+average).
    Returns H×W×3 int32 array, or None if helper unavailable."""
    if not os.path.exists(_HELPER):
        return None
    pass  # all scales now supported
    try:
        proc = subprocess.run([_HELPER, str(scale)], input=jpeg_bytes, capture_output=True, timeout=10)
        if proc.returncode != 0 or len(proc.stdout) < 8:
            return None
        w, h = _struct.unpack_from("<II", proc.stdout, 0)
        npix = w * h
        data_end = 8 + npix * 3
        arr = np.frombuffer(proc.stdout[8:data_end], dtype=np.uint8).reshape(h, w, 3).astype(np.int32)
        csum = npix_c = 0
        if len(proc.stdout) >= data_end + 8:
            csum, npix_c = _struct.unpack_from("<II", proc.stdout, data_end)
        return arr, csum
    except Exception:
        return None

# ---------------------------------------------------------------------------
# Exact firmware image pipeline emulation
# ---------------------------------------------------------------------------

def _fw_fs_dither(arr):
    """Exact firmware Floyd-Steinberg: 7/16 right, 3/16 bottom-left, 6/16 bottom.
    Uses C arithmetic-right-shift >>4 (floor). arr: H×W×3 int32.
    Returns H×W×3 uint8 RGB888 output (representing quantised RGB565 values)."""
    h, w = arr.shape[:2]
    # Per-column bottom errors (xi = col+1, mirrors firmware fs_down[(SCREEN_W+2)*3])
    fd_r = [0] * (w + 2)
    fd_g = [0] * (w + 2)
    fd_b = [0] * (w + 2)
    out = np.zeros((h, w, 3), dtype=np.uint8)

    for row in range(h):
        # Each new scan line starts with carry=0, matching firmware (memset + no
        # cross-row carry since we process the full row width in one pass).
        cr = cg = cb = 0
        for col in range(w):
            xi = col + 1
            sr = int(arr[row, col, 0]); sg = int(arr[row, col, 1]); sb = int(arr[row, col, 2])
            r = sr + cr + fd_r[xi]
            g = sg + cg + fd_g[xi]
            b = sb + cb + fd_b[xi]
            r = 0 if r < 0 else (255 if r > 255 else r)
            g = 0 if g < 0 else (255 if g > 255 else g)
            b = 0 if b < 0 else (255 if b > 255 else b)
            if sr >= 248 and r < 248: r = 248
            if sg >= 252 and g < 252: g = 252
            if sb >= 248 and b < 248: b = 248
            fd_r[xi] = 0; fd_g[xi] = 0; fd_b[xi] = 0

            r5 = r >> 3; g6 = g >> 2; b5 = b >> 3
            # Or-shift expansion matches what firmware uses for error calculation.
            # Display output uses simple truncation (r5<<3) — RGB565 max white is
            # (248,252,248), not (255,255,255).
            qr_err = (r5 << 3) | (r5 >> 2)
            qg_err = (g6 << 2) | (g6 >> 4)
            qb_err = (b5 << 3) | (b5 >> 2)
            out[row, col] = [r5 << 3, g6 << 2, b5 << 3]

            er = r - qr_err; eg = g - qg_err; eb = b - qb_err
            if r5 >= 30 and er < 0: er = 0
            if g6 >= 62 and eg < 0: eg = 0
            if b5 >= 30 and eb < 0: eb = 0

            cr = (er * 7) >> 4   # 7/16 right carry
            cg = (eg * 7) >> 4
            cb = (eb * 7) >> 4

            fd_r[xi-1] += (er * 3) >> 4   # 3/16 bottom-left
            fd_g[xi-1] += (eg * 3) >> 4
            fd_b[xi-1] += (eb * 3) >> 4

            fd_r[xi]   += (er * 6) >> 4   # 6/16 bottom (= 5/16 + redistributed 1/16)
            fd_g[xi]   += (eg * 6) >> 4
            fd_b[xi]   += (eb * 6) >> 4
            # 1/16 bottom-right omitted: accumulates across MCU block rows,
            # creating ~16x amplification at block column boundaries.

    return out


def fw_emulate_image(raw_bytes, scale=2):
    """Simulate the firmware render pipeline. Returns PNG bytes, or None on error."""
    if not _HAS_NP:
        return None
    try:
        if raw_bytes[:4] == b'qoif':
            # QOI: firmware uses simple RGB565 quantise (no FS dither)
            try:
                import qoi
                arr = qoi.decode(raw_bytes)
            except ImportError:
                return None
            if arr.shape[2] == 4:
                alpha = arr[:, :, 3:4].astype(float) / 255.0
                arr = (arr[:, :, :3] * alpha + 255 * (1 - alpha)).astype(np.uint8)
            else:
                arr = arr[:, :, :3].astype(np.uint8)
            if scale > 1:
                # 2×2 (or scale×scale) box-filter average — matches firmware displayDrawQoi
                h2, w2 = arr.shape[:2]
                nh = (h2 // scale) * scale
                nw = (w2 // scale) * scale
                a = arr[:nh, :nw].astype(np.uint32)
                arr = (a.reshape(nh//scale, scale, nw//scale, scale, 3).sum(axis=(1,3)) // (scale*scale)).astype(np.uint8)
            r5 = (arr[:,:,0] >> 3).astype(np.uint8)
            g6 = (arr[:,:,1] >> 2).astype(np.uint8)
            b5 = (arr[:,:,2] >> 3).astype(np.uint8)
            out = np.stack([r5 << 3, g6 << 2, b5 << 3], axis=2).astype(np.uint8)
        elif raw_bytes[:2] == b'\xff\xd8':
            # JPEG: TJpgDec decode then simple RGB565 quantise (no dither),
            # matching current firmware jpegDitherCallback.
            result = _tjpgdec_decode(raw_bytes, scale)
            if result is None:
                arr, csum = None, 0
            else:
                arr, csum = result
            if arr is None:
                img = _PIL.open(io.BytesIO(raw_bytes)).convert("RGB")
                a = np.array(img, dtype=np.int32)
                if scale > 1:
                    h2, w2 = a.shape[:2]
                    nh = (h2 // scale) * scale; nw = (w2 // scale) * scale
                    shift = {2: 2, 4: 4, 8: 6}.get(scale, 2)
                    a = a[:nh, :nw].reshape(nh//scale, scale, nw//scale, scale, 3)
                    arr = (a.sum(axis=(1, 3)) >> shift).astype(np.uint8)
                else:
                    arr = a.astype(np.uint8)
            r5 = (arr[:,:,0] >> 3).astype(np.uint16)
            g6 = (arr[:,:,1] >> 2).astype(np.uint16)
            b5 = (arr[:,:,2] >> 3).astype(np.uint16)
            print(f"[jpeg] csum=0x{csum:08X} pixels={r5.size}")
            out = np.stack([r5 << 3, g6 << 2, b5 << 3], axis=2).astype(np.uint8)
        else:
            return None

        buf = io.BytesIO()
        _PIL.fromarray(out.astype(np.uint8)).save(buf, format="BMP")
        return buf.getvalue()
    except Exception:
        return None

MAGIC_WIKI        = b"WIKI"
MAGIC_WKI2        = b"WKI2"
INDEX_RECORD_SIZE = 80
TITLE_KEY_LEN     = 60


# ---------------------------------------------------------------------------
# Data layer
# ---------------------------------------------------------------------------

class WikiDb:
    def __init__(self, wiki_dir: str):
        self.dir = wiki_dir

        # Read index_meta.txt
        meta = {}
        meta_path = os.path.join(wiki_dir, "index_meta.txt")
        if os.path.exists(meta_path):
            for line in open(meta_path):
                k, _, v = line.strip().partition("=")
                meta[k] = v
        self.article_count = int(meta.get("article_count", 0))

        # Read img_meta.txt (optional — absent if built without images)
        img_meta_path = os.path.join(wiki_dir, "img_meta.txt")
        self.has_images = os.path.exists(img_meta_path)
        self.img_count = 0
        if self.has_images:
            for line in open(img_meta_path):
                k, _, v = line.strip().partition("=")
                if k == "image_count":
                    self.img_count = int(v)

        # Load all titles into RAM for fast in-memory search.
        # index.bin is 285K × 80 B = ~23 MB — fine for a desktop debug tool.
        print(f"Loading index ({self.article_count:,} articles)...", flush=True)
        self._titles: list[tuple[str, int]] = []   # (title_key, article_id)
        index_path = os.path.join(wiki_dir, "index.bin")
        with open(index_path, "rb") as f:
            while True:
                rec = f.read(INDEX_RECORD_SIZE)
                if len(rec) < INDEX_RECORD_SIZE:
                    break
                key = rec[:TITLE_KEY_LEN].rstrip(b"\x00").decode("utf-8", errors="replace")
                art_id = struct.unpack_from("<I", rec, TITLE_KEY_LEN)[0]
                self._titles.append((key, art_id))
        # Already sorted (index.bin is written sorted), but ensure it.
        self._titles.sort(key=lambda t: t[0].lower())
        self._keys_lower = [t[0].lower() for t in self._titles]
        print(f"Index loaded. Images: {self.img_count:,}", flush=True)

    # --- Search ---------------------------------------------------------------

    def search(self, query: str, limit: int = 40) -> list[dict]:
        """Return up to `limit` articles whose title contains `query` (case-insensitive).
        Prefix matches are listed first, then substring matches."""
        q = query.lower().strip()
        if not q:
            return []
        prefix_hits, substr_hits = [], []
        for key, art_id in self._titles:
            kl = key.lower()
            if kl.startswith(q):
                prefix_hits.append({"id": art_id, "title": key})
            elif q in kl:
                substr_hits.append({"id": art_id, "title": key})
            if len(prefix_hits) >= limit:
                break
        results = prefix_hits + substr_hits
        return results[:limit]

    # --- Article loading ------------------------------------------------------

    def get_article(self, article_id: int) -> tuple[str | None, str | None]:
        """Return (title, html) for the given article_id, or (None, None).
        Handles both WKI2 (multi-block) and legacy WIKI (single-block) formats."""
        id_index_path = os.path.join(self.dir, "id_index.bin")
        with open(id_index_path, "rb") as f:
            f.seek(article_id * 12)
            data = f.read(12)
        if len(data) < 12:
            return None, None
        chunk_id, offset, length = struct.unpack("<III", data)
        if length == 0:
            return None, None

        chunk_path = os.path.join(self.dir, f"articles_{chunk_id:04d}.dat")
        with open(chunk_path, "rb") as f:
            f.seek(offset)
            blob = f.read(length)

        magic = blob[:4]

        if magic == MAGIC_WKI2:
            # WKI2: self-contained multi-block format (no outer LZ4 wrapper).
            # Concatenate all blocks to show the full article.
            pos = 4
            pos += 4                                                      # article_id
            title_len = struct.unpack_from("<H", blob, pos)[0]; pos += 2
            title = blob[pos:pos+title_len].decode("utf-8");              pos += title_len
            num_blocks = struct.unpack_from("<H", blob, pos)[0];          pos += 2
            parts = []
            for _ in range(num_blocks):
                comp_len = struct.unpack_from("<I", blob, pos)[0]; pos += 4
                lz4_data = blob[pos:pos+comp_len];                 pos += comp_len
                parts.append(lz4.block.decompress(lz4_data).decode("utf-8"))
            return title, "".join(parts)

        elif magic == MAGIC_WIKI:
            # Legacy WIKI: outer LZ4-compressed blob.
            raw = lz4.block.decompress(blob)
            pos = 4
            pos += 4                                                      # article_id
            title_len = struct.unpack_from("<H", raw, pos)[0]; pos += 2
            title = raw[pos:pos+title_len].decode("utf-8");               pos += title_len
            html_len = struct.unpack_from("<I", raw, pos)[0];             pos += 4
            html = raw[pos:pos+html_len].decode("utf-8")
            return title, html

        return None, None

    # --- Image loading --------------------------------------------------------

    def get_image(self, img_id: int) -> bytes | None:
        """Return raw image bytes (JPEG or PNG) for img_id, or None."""
        if not self.has_images:
            return None
        img_index_path = os.path.join(self.dir, "img_index.bin")
        with open(img_index_path, "rb") as f:
            f.seek(img_id * 12)
            data = f.read(12)
        if len(data) < 12:
            return None
        chunk_id, offset, length = struct.unpack("<III", data)
        chunk_path = os.path.join(self.dir, f"img_{chunk_id:04d}.dat")
        with open(chunk_path, "rb") as f:
            f.seek(offset)
            return f.read(length)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

_db: WikiDb = None   # set in main


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass   # suppress per-request noise

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, html: str, status=200):
        body = html.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path   = parsed.path
        qs     = parse_qs(parsed.query)

        if path == "/" or path == "/index.html":
            self.send_html(MAIN_PAGE)

        elif path == "/api/search":
            q = qs.get("q", [""])[0]
            results = _db.search(q) if q else []
            self.send_json(results)

        elif path.startswith("/api/article/"):
            try:
                art_id = int(path.split("/")[-1])
            except ValueError:
                self.send_json({"error": "bad id"}, 400); return
            title, html = _db.get_article(art_id)
            if title is None:
                self.send_json({"error": "not found"}, 404); return
            # Rewrite /wiki/N links to navigate within the debug UI
            html = re.sub(r'href="/wiki/(\d+)"', r'href="#" onclick="loadArticle(\1);return false;"', html)
            # Leave src="/img/N" as-is — JS applyFirmwareRender rewrites to /img_fw/N?scale=1
            self.send_json({"id": art_id, "title": title, "html": html})

        elif path.startswith("/img_fw/"):
            # Firmware-emulated render (exact C FS algorithm, server-side)
            try:
                img_id = int(path.split("/")[-1])
            except ValueError:
                self.send_response(400); self.end_headers(); return
            scale = int(qs.get("scale", ["2"])[0])
            raw = _db.get_image(img_id)
            if raw is None:
                self.send_response(404); self.end_headers(); return
            png = fw_emulate_image(raw, scale)
            if png is None:
                self.send_response(500); self.end_headers(); return
            self.send_response(200)
            self.send_header("Content-Type", "image/bmp")
            self.send_header("Content-Length", len(png))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(png)

        elif path.startswith("/img/"):
            try:
                img_id = int(path.split("/")[-1])
            except ValueError:
                self.send_response(400); self.end_headers(); return
            data = _db.get_image(img_id)
            if data is None:
                self.send_response(404); self.end_headers(); return
            # QOI images are not browser-renderable — transcode to PNG on the fly.
            if data[:4] == b'qoif':
                try:
                    import qoi, io
                    from PIL import Image
                    arr = qoi.decode(data)
                    buf = io.BytesIO()
                    Image.fromarray(arr).save(buf, format="PNG")
                    data = buf.getvalue()
                except Exception:
                    self.send_response(500); self.end_headers(); return
            mime = "image/png" if data[:4] == b'\x89PNG' else "image/jpeg"
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", len(data))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        elif path == "/api/info":
            self.send_json({
                "article_count": _db.article_count,
                "img_count":     _db.img_count,
                "has_images":    _db.has_images,
            })

        else:
            self.send_response(404); self.end_headers()


# ---------------------------------------------------------------------------
# Embedded single-page UI
# ---------------------------------------------------------------------------

MAIN_PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Wiki Debug</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, sans-serif; display: flex; height: 100vh; overflow: hidden; background: #1a1a2e; color: #e0e0e0; }

  /* Sidebar */
  #sidebar { width: 300px; min-width: 200px; display: flex; flex-direction: column; border-right: 1px solid #333; background: #16213e; }
  #search-box { padding: 12px; border-bottom: 1px solid #333; }
  #search { width: 100%; padding: 8px 12px; background: #0f3460; border: 1px solid #444; border-radius: 6px; color: #fff; font-size: 14px; outline: none; }
  #search:focus { border-color: #e94560; }
  #stats { padding: 6px 12px; font-size: 11px; color: #666; border-bottom: 1px solid #222; }
  #results { flex: 1; overflow-y: auto; }
  .result-item { padding: 8px 12px; cursor: pointer; font-size: 13px; border-bottom: 1px solid #222; line-height: 1.4; }
  .result-item:hover { background: #0f3460; }
  .result-item.active { background: #e94560; color: #fff; }
  .no-results { padding: 16px; color: #666; font-size: 13px; text-align: center; }

  /* Article pane */
  #article-pane { flex: 1; display: flex; flex-direction: column; overflow: hidden; }
  #article-header { padding: 12px 20px; background: #16213e; border-bottom: 1px solid #333; display: flex; align-items: center; gap: 12px; }
  #article-id-badge { font-size: 11px; color: #888; background: #0f3460; padding: 3px 8px; border-radius: 10px; white-space: nowrap; }
  #article-title { font-size: 18px; font-weight: 600; flex: 1; }
  #article-body { flex: 1; overflow-y: auto; padding: 20px 28px; line-height: 1.7; font-size: 15px; }

  /* Article HTML styles */
  #article-body h1 { font-size: 1.5em; margin: 1em 0 0.4em; border-bottom: 1px solid #333; padding-bottom: 4px; }
  #article-body h2 { font-size: 1.2em; margin: 1em 0 0.3em; color: #a0c4ff; }
  #article-body h3, #article-body h4 { font-size: 1em; margin: 0.8em 0 0.2em; color: #bbb; }
  #article-body p  { margin-bottom: 0.7em; }
  #article-body a  { color: #e94560; text-decoration: none; }
  #article-body a:hover { text-decoration: underline; }
  #article-body ul, #article-body ol { margin: 0.4em 0 0.7em 1.5em; }
  #article-body li { margin-bottom: 0.3em; }
  #article-body img { max-width: 280px; max-height: 200px; display: block; margin: 12px auto; border-radius: 4px; background: #0f3460; }
  #article-body figure { margin: 12px 0; text-align: center; }
  #article-body .esp32-preview { display: inline-block; margin: 8px auto; position: relative; }
  #loading { display: none; padding: 40px; text-align: center; color: #666; }
  #placeholder { padding: 60px 40px; text-align: center; color: #444; }
  #placeholder h2 { font-size: 1.1em; margin-bottom: 8px; }

  /* Debug overlay on images */
  #article-body img { position: relative; cursor: zoom-in; }

  /* ESP32 screen preview */
  #preview-toggle { padding: 6px 12px; background: #0f3460; border: 1px solid #444; border-radius: 6px; color: #aaa; font-size: 12px; cursor: pointer; white-space: nowrap; }
  #preview-toggle.active { background: #e94560; border-color: #e94560; color: #fff; }
  #rgb565-toggle { padding: 6px 12px; background: #0f3460; border: 1px solid #444; border-radius: 6px; color: #aaa; font-size: 12px; cursor: pointer; white-space: nowrap; }
  #rgb565-toggle.active { background: #1a7a3a; border-color: #1a7a3a; color: #fff; }
  #article-body img.rgb565-processed { image-rendering: pixelated; }
  #esp32-frame { display: none; width: 320px; min-width: 320px; border-left: 1px solid #333; background: #000; flex-direction: column; }
  #esp32-frame.visible { display: flex; }
  #esp32-screen { width: 320px; height: 240px; background: #111; overflow: hidden; position: relative; font-size: 10px; line-height: 1.4; color: #ddd; }
  #esp32-label { padding: 4px 8px; font-size: 10px; color: #555; text-align: center; border-top: 1px solid #222; }

  scrollbar-width: thin;
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: #111; }
  ::-webkit-scrollbar-thumb { background: #444; border-radius: 3px; }
</style>
</head>
<body>

<div id="sidebar">
  <div id="search-box">
    <input id="search" type="text" placeholder="Search articles..." autocomplete="off" autofocus>
  </div>
  <div id="stats">Loading…</div>
  <div id="results"><div class="no-results">Type to search</div></div>
</div>

<div id="article-pane">
  <div id="article-header">
    <span id="article-title" style="color:#555">No article selected</span>
    <span id="article-id-badge" style="display:none"></span>
    <button id="rgb565-toggle"  onclick="toggleRgb565()">RGB565</button>
    <button id="preview-toggle" onclick="togglePreview()">📺 ESP32 view</button>
  </div>
  <div id="loading">Loading…</div>
  <div id="article-body">
    <div id="placeholder">
      <h2>ESP32 Wikipedia Debug Viewer</h2>
      <p>Search for an article in the sidebar to inspect its stored HTML and images.</p>
    </div>
  </div>
</div>

<div id="esp32-frame">
  <div id="esp32-screen"></div>
  <div id="esp32-label">320 × 240 ESP32 CYD preview</div>
</div>

<script>
const search   = document.getElementById('search');
const results  = document.getElementById('results');
const artTitle = document.getElementById('article-title');
const artBadge = document.getElementById('article-id-badge');
const artBody  = document.getElementById('article-body');
const loading  = document.getElementById('loading');
const stats    = document.getElementById('stats');
const esp32Fr  = document.getElementById('esp32-frame');
const esp32Sc  = document.getElementById('esp32-screen');
const prevBtn  = document.getElementById('preview-toggle');

let debounce = null;
let activeItem = null;
let showPreview = false;
let showRgb565  = true;   // firmware emulation on by default

// ---------------------------------------------------------------------------
// Firmware RGB565 emulation — server-side (exact C algorithm match)
// Images are re-fetched from /img_fw/N, rendered by Python on the server.
// ---------------------------------------------------------------------------

function applyFirmwareRender(container) {
  container.querySelectorAll('img').forEach(img => {
    const src = img.getAttribute('src') || '';
    const m = src.match(/\/img\/(\d+)/);
    if (!m) return;
    if (!img.dataset.origSrc) img.dataset.origSrc = img.src;
    if (!img.dataset.origStyle) img.dataset.origStyle = img.getAttribute('style') || '';
    img.src = '/img_fw/' + m[1] + '?scale=1';
    // Force 1:1 pixel mapping — no CSS scaling, pixelated rendering so
    // individual firmware pixels are visible (matching TFT 1:1 output).
    img.style.cssText = 'image-rendering: pixelated; image-rendering: crisp-edges; width: auto; height: auto; max-width: none;';
    img.classList.add('rgb565-processed');
  });
}

function restoreImage(imgEl) {
  if (imgEl.dataset.origSrc) {
    imgEl.src = imgEl.dataset.origSrc;
    imgEl.setAttribute('style', imgEl.dataset.origStyle || '');
    delete imgEl.dataset.origSrc;
    delete imgEl.dataset.origStyle;
    imgEl.classList.remove('rgb565-processed');
  }
}

function toggleRgb565() {
  showRgb565 = !showRgb565;
  document.getElementById('rgb565-toggle').classList.toggle('active', showRgb565);
  if (showRgb565) {
    applyFirmwareRender(artBody);
  } else {
    artBody.querySelectorAll('img.rgb565-processed').forEach(restoreImage);
  }
}

// Reflect default-on state of firmware emulation toggle
document.getElementById('rgb565-toggle').classList.add('active');

// Load info on startup
fetch('/api/info').then(r => r.json()).then(info => {
  stats.textContent = `${info.article_count.toLocaleString()} articles · ${info.img_count.toLocaleString()} images`;
});

// Search
search.addEventListener('input', () => {
  clearTimeout(debounce);
  debounce = setTimeout(doSearch, 250);
});

search.addEventListener('keydown', e => {
  if (e.key === 'ArrowDown') { focusResult(0); e.preventDefault(); }
});

async function doSearch() {
  const q = search.value.trim();
  if (!q) { results.innerHTML = '<div class="no-results">Type to search</div>'; return; }
  const data = await fetch('/api/search?q=' + encodeURIComponent(q)).then(r => r.json());
  if (!data.length) {
    results.innerHTML = '<div class="no-results">No results</div>';
    return;
  }
  results.innerHTML = data.map(r =>
    `<div class="result-item" tabindex="0" data-id="${r.id}" onclick="loadArticle(${r.id}, this)"
          onkeydown="if(event.key==='Enter') loadArticle(${r.id}, this)">
       ${escHtml(r.title)}
     </div>`
  ).join('');
}

function focusResult(idx) {
  const items = results.querySelectorAll('.result-item');
  if (items[idx]) items[idx].focus();
}

results.addEventListener('keydown', e => {
  const items = [...results.querySelectorAll('.result-item')];
  const cur = document.activeElement;
  const idx = items.indexOf(cur);
  if (e.key === 'ArrowDown' && idx < items.length - 1) { items[idx+1].focus(); e.preventDefault(); }
  if (e.key === 'ArrowUp')  {
    if (idx > 0) { items[idx-1].focus(); e.preventDefault(); }
    else         { search.focus(); e.preventDefault(); }
  }
});

async function loadArticle(id, el) {
  if (activeItem) activeItem.classList.remove('active');
  if (el) { activeItem = el; el.classList.add('active'); }

  artBody.style.display = 'none';
  loading.style.display = 'block';

  const data = await fetch('/api/article/' + id).then(r => r.json());
  loading.style.display = 'none';
  artBody.style.display = '';

  artTitle.textContent = data.title || '(untitled)';
  artTitle.style.color = '';
  artBadge.style.display = '';
  artBadge.textContent = 'id=' + data.id;

  // Inject article HTML
  artBody.innerHTML = data.html || '<em>(empty)</em>';
  annotateImages(artBody);
  if (showRgb565) applyFirmwareRender(artBody);

  if (showPreview) renderEsp32(data.title, artBody.innerHTML);
}

function annotateImages(container) {
  container.querySelectorAll('img').forEach(img => {
    const src = img.getAttribute('src') || '';
    const m = src.match(/\\/img\\/(\\d+)/);
    if (m) {
      img.title = 'img_id=' + m[1];
      img.addEventListener('error', () => {
        img.style.background = '#3a1a1a';
        img.style.border = '1px dashed #e94560';
        img.alt = '⚠ img ' + m[1] + ' failed';
      });
    }
  });
}

function togglePreview() {
  showPreview = !showPreview;
  prevBtn.classList.toggle('active', showPreview);
  esp32Fr.classList.toggle('visible', showPreview);
  if (showPreview) renderEsp32(artTitle.textContent, artBody.innerHTML);
}

function renderEsp32(title, html) {
  // Rough 320×240 preview — same font size constraints as the real device
  esp32Sc.innerHTML = `
    <div style="background:#0f3460;color:#fff;padding:2px 4px;font-size:9px;font-weight:bold;white-space:nowrap;overflow:hidden">${escHtml(title)}</div>
    <div style="padding:2px 4px;overflow:hidden;height:226px;font-size:9px;line-height:1.3">${html}</div>
  `;
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    global _db
    parser = argparse.ArgumentParser(description="Debug server for Wikipedia offline reader output")
    parser.add_argument("--dir",  default=os.path.join(os.path.dirname(__file__), "output_v2"),
                        help="Path to preprocessor output directory (default: output_v2/)")
    parser.add_argument("--port", type=int, default=8765, help="HTTP port (default: 8765)")
    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        raise SystemExit(f"Output directory not found: {args.dir}")
    if not os.path.exists(os.path.join(args.dir, "index.bin")):
        raise SystemExit(f"index.bin not found in {args.dir} — run the preprocessor first")

    _db = WikiDb(args.dir)
    server = HTTPServer(("0.0.0.0", args.port), Handler)
    print(f"Open: http://localhost:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")

if __name__ == "__main__":
    main()
