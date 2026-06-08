# Image Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add inline JPEG thumbnail display and full-screen tap-to-expand to the Wikipedia offline reader, with images extracted from a ZIM file by the preprocessor and stored in chunk files on the SD card.

**Architecture:** The preprocessor extracts and resizes images from the ZIM into `/wiki/img_NNNN.dat` chunk files indexed by `/wiki/img_index.bin`, and rewrites `<img>` HTML tags to `<img src="/img/ID" w="W" h="H">`. On the ESP32, `html_render.cpp` draws grey placeholders during layout, then `wikiDbLoadImage()` reads each JPEG from SD and `TJpgDec` decodes it directly onto the TFT. A new `STATE_IMAGE` in `ui.cpp` handles full-screen tap-to-expand.

**Tech Stack:** Python/Pillow (preprocessor), TJpgDec (Bodmer, via PlatformIO), TFT_eSPI viewport clipping, ESP32 SD card file I/O.

---

## File Structure

| File | Change |
|------|--------|
| `preprocessor/requirements.txt` | Add `Pillow>=10.0` |
| `preprocessor/build_wiki_db.py` | Add `_g_img_info` global, `_resolve_img_src()`, update `strip_html()`, add `extract_images()`, update `build_database()` and `main()` |
| `firmware/platformio.ini` | Add `bodmer/TJpgDec` |
| `firmware/src/config.h` | Add `IMG_INDEX_PATH`, `IMG_CHUNK_FMT`, `IMG_META_PATH`, `IMG_MAX_JPEG` |
| `firmware/src/display.h` | Add `displayDrawJpeg()` declaration |
| `firmware/src/display.cpp` | Add TJpgDec callback + `displayInit()` setup + `displayDrawJpeg()` |
| `firmware/src/html_render.h` | Add `ImageRegion` struct; add `images` vector to `RenderResult` |
| `firmware/src/html_render.cpp` | Handle `<img>` tag in `processTag()` |
| `firmware/src/wiki_db.h` | Add `wikiDbImagesAvailable()`, `wikiDbLoadImage()` |
| `firmware/src/wiki_db.cpp` | Add image file handles, open in `wikiDbInit()`, implement `wikiDbLoadImage()` |
| `firmware/src/ui.cpp` | Add `g_art_images`, `STATE_IMAGE`, `showImageFull()`, image tap detection, `drawImageThumbnail()`, update `renderArticle()` |

---

## Task 1: Pillow dependency

**Files:**
- Modify: `preprocessor/requirements.txt`

- [ ] **Step 1: Add Pillow to requirements.txt**

Replace the file content with:

```
libzim>=3.0
lz4>=4.0
beautifulsoup4>=4.12
Pillow>=10.0
```

- [ ] **Step 2: Install Pillow in the active Python environment**

```bash
cd /home/alun/esp32/wikipedia_offline/preprocessor
pip install Pillow>=10.0
```

Expected: `Successfully installed Pillow-XX.X.X` (or `already satisfied`).

- [ ] **Step 3: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add preprocessor/requirements.txt
git commit -m "deps: add Pillow for image resizing in preprocessor"
```

---

## Task 2: Update `strip_html()` to rewrite `<img>` tags

**Files:**
- Modify: `preprocessor/build_wiki_db.py`

This changes `strip_html()` to preserve `<img>` tags from the ZIM when an image map is available, rewriting them to the firmware-friendly `<img src="/img/ID" w="W" h="H">` format. When no map is provided (or the image isn't in the map), the tag is removed as before.

- [ ] **Step 1: Add `_g_img_info` module-level global (after the existing `_g_archive` declaration)**

Find this line (around line 73):
```python
# Opened per-worker in _worker_init; not used in the main process.
_g_archive: Archive | None = None
```

Add below it:
```python
# Image info map: {zim_path → (img_id, width, height)}. Set before Pool creation.
_g_img_info: dict[str, tuple[int, int, int]] | None = None
```

- [ ] **Step 2: Add `_resolve_img_src()` helper (after `_is_internal_link()`)**

Add after the `_is_internal_link()` function (around line 100):

```python
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
```

- [ ] **Step 3: Update `strip_html()` to handle `<img>` tags**

The current function signature is:
```python
def strip_html(raw_html: bytes, title_map: dict[str, int]) -> str:
```

Replace it with the full updated version:

```python
def strip_html(raw_html: bytes, title_map: dict[str, int],
               img_info: dict[str, tuple[int, int, int]] | None = None) -> str:
    """
    Strip a Wikipedia HTML article down to minimal, ESP32-friendly HTML.
    Internal links are rewritten to /wiki/<article_id>.
    If img_info is provided, <img> tags with resolvable sources are rewritten
    to <img src="/img/ID" w="W" h="H">; otherwise they are removed.
    """
    soup = BeautifulSoup(raw_html, "lxml")

    # Remove all unwanted tags (img handled separately below)
    for tag in soup.find_all(["script", "style", "table", "figure",
                               "sup", "sub", "noscript", "meta", "link",
                               "header", "footer", "nav", "aside"]):
        tag.decompose()

    for comment in soup.find_all(string=lambda t: isinstance(t, Comment)):
        comment.extract()

    # Handle <img> tags: rewrite to /img/ID format or remove
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
```

Note the change to the `attrs = {}` loop: add `"img"` to the exclusion list alongside `"a"` so the rewritten `<img>` attributes are preserved.

- [ ] **Step 4: Update `_process_entry()` to pass `_g_img_info` to `strip_html()`**

Find (around line 245):
```python
    try:
        html = strip_html(raw_html, _g_title_map)
    except Exception:
        return None
```

Replace with:
```python
    try:
        html = strip_html(raw_html, _g_title_map, _g_img_info)
    except Exception:
        return None
```

- [ ] **Step 5: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add preprocessor/build_wiki_db.py
git commit -m "preprocessor: strip_html rewrites <img> tags to /img/ID format"
```

---

## Task 3: Add `extract_images()` and hook into `main()`

**Files:**
- Modify: `preprocessor/build_wiki_db.py`

- [ ] **Step 1: Add `extract_images()` function (before `build_title_map()`)**

Add this function before the `build_title_map()` function (around line 175):

```python
def extract_images(zim_path: str, archive, output_dir: str,
                   verbose: bool) -> dict[str, tuple[int, int, int]]:
    """
    Extract and resize images from the ZIM file.
    Writes img_NNNN.dat chunk files, img_index.bin, and img_meta.txt.
    Returns img_info: {zim_entry_path → (img_id, width, height)}.
    """
    try:
        from PIL import Image as PilImage
        import io as _io
    except ImportError:
        print("Pillow not available — skipping image extraction (pip install Pillow)")
        return {}

    IMG_CHUNK_SIZE  = 4 * 1024 * 1024   # 4 MB per chunk file
    MAX_W, MAX_H    = 320, 212           # fits 320×240 screen minus NAV_H=28
    JPEG_QUALITY    = 75
    MIN_DIM         = 50                 # skip tiny icons
    INCLUDE_MIME    = {"image/jpeg", "image/png", "image/webp"}

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
        # Skip MediaWiki chrome (UI icons, logos)
        if "_mw_/" in path or path.startswith("_"):
            continue

        try:
            item = entry.get_item()
            mime = item.mimetype
        except Exception:
            continue

        if mime not in INCLUDE_MIME:
            continue

        try:
            raw = item.content.tobytes()
            img = PilImage.open(_io.BytesIO(raw))
            orig_w, orig_h = img.size
        except Exception:
            continue

        # Skip tiny images (icons / decorative elements)
        if orig_w < MIN_DIM or orig_h < MIN_DIM:
            continue

        # Resize to fit within MAX_W × MAX_H, preserving aspect ratio
        img.thumbnail((MAX_W, MAX_H), PilImage.LANCZOS)
        w, h = img.size

        # Convert to RGB JPEG
        if img.mode != "RGB":
            img = img.convert("RGB")
        buf = _io.BytesIO()
        img.save(buf, format="JPEG", quality=JPEG_QUALITY)
        jpeg_bytes = buf.getvalue()

        # Roll to a new chunk file if needed
        if within_chunk_offset + len(jpeg_bytes) > IMG_CHUNK_SIZE:
            chunk_file.close()
            current_chunk_id += 1
            within_chunk_offset = 0
            chunk_file = open(img_chunk_path(current_chunk_id), "wb")

        chunk_file.write(jpeg_bytes)
        index_entries.append((current_chunk_id, within_chunk_offset, len(jpeg_bytes)))
        within_chunk_offset += len(jpeg_bytes)

        # Register by ZIM path and common bare-filename variants
        img_info[path] = (img_id, w, h)
        bare = path.split("/", 1)[-1] if "/" in path else path
        if bare not in img_info:
            img_info[bare] = (img_id, w, h)

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

    print(f"Image extraction complete: {img_id} images in {current_chunk_id + 1} chunk(s)")
    return img_info
```

- [ ] **Step 2: Update `build_database()` to accept and set `_g_img_info`**

Find the `build_database()` function signature:
```python
def build_database(zim_path: str, archive: Archive, title_map: dict[str, int],
                   output_dir: str, limit: int, verbose: bool,
                   num_workers: int = 0, sparse_step: int = SPARSE_STEP) -> int:
    """Pass 2: process articles in parallel and write chunk files + index."""
    global _g_title_map
    _g_title_map = title_map  # inherited by workers via fork
```

Replace with:
```python
def build_database(zim_path: str, archive: Archive, title_map: dict[str, int],
                   output_dir: str, limit: int, verbose: bool,
                   num_workers: int = 0, sparse_step: int = SPARSE_STEP,
                   img_info: dict[str, tuple[int, int, int]] | None = None) -> int:
    """Pass 2: process articles in parallel and write chunk files + index."""
    global _g_title_map, _g_img_info
    _g_title_map = title_map   # inherited by workers via fork
    _g_img_info  = img_info or {}
```

- [ ] **Step 3: Add `--no-images` argument and image pass call in `main()`**

Find in `main()`:
```python
    title_map = build_title_map(archive, args.limit, args.verbose)
    written = build_database(args.zim_file, archive, title_map,
                             args.output_dir, args.limit, args.verbose,
                             args.workers, args.sparse_step)
```

Replace with:
```python
    parser.add_argument("--no-images", action="store_true",
                        help="Skip image extraction (use for _pictures:no ZIM files)")
```

Wait — `parser` is already fully defined and `parse_args()` already called at this point. Instead find the argument parser block (around line 401) and add the `--no-images` argument there, then find the `main()` body and update the call.

Find in the argument parser setup (after the `--verbose` argument):
```python
    parser.add_argument("--verbose", action="store_true", help="Extra progress output")
    args = parser.parse_args()
```

Replace with:
```python
    parser.add_argument("--verbose", action="store_true", help="Extra progress output")
    parser.add_argument("--no-images", action="store_true",
                        help="Skip image extraction pass (use for _pictures:no ZIM files)")
    args = parser.parse_args()
```

Then find:
```python
    title_map = build_title_map(archive, args.limit, args.verbose)
    written = build_database(args.zim_file, archive, title_map,
                             args.output_dir, args.limit, args.verbose,
                             args.workers, args.sparse_step)
```

Replace with:
```python
    title_map = build_title_map(archive, args.limit, args.verbose)

    img_info: dict[str, tuple[int, int, int]] = {}
    if not args.no_images:
        img_info = extract_images(args.zim_file, archive, args.output_dir, args.verbose)

    written = build_database(args.zim_file, archive, title_map,
                             args.output_dir, args.limit, args.verbose,
                             args.workers, args.sparse_step, img_info)
```

- [ ] **Step 4: Test preprocessor compiles and runs without error on an existing ZIM**

Run against the existing (no-pictures) ZIM with `--no-images` to verify no regressions:

```bash
cd /home/alun/esp32/wikipedia_offline/preprocessor
python3 build_wiki_db.py wikipedia_en_simple_all_mini_2026-05.zim \
    ../output --limit 100 --no-images --verbose
```

Expected: Same output as before. No crash. Then test the image pass (which should find 0 images):

```bash
python3 build_wiki_db.py wikipedia_en_simple_all_mini_2026-05.zim \
    ../output --limit 100 --verbose
```

Expected: `Image extraction: 0 images found (ZIM may have _pictures:no)` then normal article processing.

- [ ] **Step 5: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add preprocessor/build_wiki_db.py
git commit -m "preprocessor: add extract_images() pass and --no-images flag"
```

---

## Task 4: Firmware — library + config constants

**Files:**
- Modify: `firmware/platformio.ini`
- Modify: `firmware/src/config.h`

- [ ] **Step 1: Add TJpgDec to `platformio.ini`**

Find:
```ini
lib_deps =
    bodmer/TFT_eSPI @ 2.5.43
```

Replace with:
```ini
lib_deps =
    bodmer/TFT_eSPI @ 2.5.43
    bodmer/TJpgDec @ ^3.0.0
```

- [ ] **Step 2: Add image constants to `config.h`**

Add at the end of `config.h`:
```c
// Image database paths
#define IMG_INDEX_PATH  "/wiki/img_index.bin"
#define IMG_CHUNK_FMT   "/wiki/img_%04u.dat"
#define IMG_META_PATH   "/wiki/img_meta.txt"

// Maximum JPEG size per image (bytes). Images are ≤320×212 at q=75; 64KB is ample.
#define IMG_MAX_JPEG    (64 * 1024)
```

- [ ] **Step 3: Verify firmware still builds**

```bash
cd /home/alun/esp32/wikipedia_offline/firmware
pio run
```

Expected: `SUCCESS`. TJpgDec will be downloaded and compiled. May take a minute on first run.

- [ ] **Step 4: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add firmware/platformio.ini firmware/src/config.h
git commit -m "firmware: add TJpgDec library and image DB config constants"
```

---

## Task 5: `ImageRegion` struct + `<img>` tag parsing in `html_render`

**Files:**
- Modify: `firmware/src/html_render.h`
- Modify: `firmware/src/html_render.cpp`

- [ ] **Step 1: Add `ImageRegion` to `html_render.h`**

Find in `html_render.h`:
```cpp
struct RenderResult {
    std::vector<LinkRegion> links;   // tappable link rects (screen space)
    int32_t content_height;          // total pixel height of all rendered content
};
```

Replace with:
```cpp
struct ImageRegion {
    uint32_t img_id;
    int32_t  doc_y;    // content-space Y of the top of this image placeholder
    int16_t  img_w;    // post-resize JPEG pixel width  (stored in <img w="…">)
    int16_t  img_h;    // post-resize JPEG pixel height (stored in <img h="…">)
    int16_t  thumb_h;  // display height in pixels (≤ 120)
};

struct RenderResult {
    std::vector<LinkRegion>  links;    // tappable link rects (screen space)
    std::vector<ImageRegion> images;   // image placeholder regions (doc space)
    int32_t content_height;            // total pixel height of all rendered content
};
```

- [ ] **Step 2: Add `<img>` tag handling to `processTag()` in `html_render.cpp`**

In `html_render.cpp`, find the `processTag()` method. Locate the final `else if (name == "div")` block:

```cpp
        else if (name == "div") {
            // Divs are block containers — just ensure we're on a new line
            flushText();
            if (!closing && cx > leftEdge()) blockBreak();
        }
        // All other tags (span, b, i, em, strong) are ignored — their content flows normally
```

Add a new `else if` block immediately before the `div` handler:

```cpp
        else if (name == "img" && !closing) {
            flushText();
            // Parse: src="/img/ID" w="W" h="H"
            int src_pos = attrs.indexOf("src=\"/img/");
            if (src_pos >= 0) {
                int idstart = src_pos + 10;   // skip 'src="/img/'
                int idend   = attrs.indexOf('"', idstart);
                if (idend > idstart) {
                    uint32_t img_id = (uint32_t)attrs.substring(idstart, idend).toInt();

                    // Parse w and h attributes
                    int16_t iw = 0, ih = 0;
                    int wpos = attrs.indexOf("w=\"");
                    if (wpos >= 0) {
                        int wend = attrs.indexOf('"', wpos + 3);
                        if (wend > wpos + 3)
                            iw = (int16_t)attrs.substring(wpos + 3, wend).toInt();
                    }
                    int hpos = attrs.indexOf("h=\"");
                    if (hpos >= 0) {
                        int hend = attrs.indexOf('"', hpos + 3);
                        if (hend > hpos + 3)
                            ih = (int16_t)attrs.substring(hpos + 3, hend).toInt();
                    }

                    // Thumbnail display height: max 120px, preserve aspect ratio
                    int16_t thumb_h = 120;
                    if (iw > 0 && ih > 0)
                        thumb_h = (int16_t)min(120, (int)(120 * ih / iw));
                    if (thumb_h < 12) thumb_h = 12;   // minimum visible height

                    // Draw grey placeholder rect (only if visible in viewport)
                    if (visible(cy)) {
                        int16_t sy = screenY(cy);
                        if (sy >= clip_y && sy < clip_y + clip_h) {
                            tft.fillRect(x0, sy, col_width, thumb_h, 0x2104);
                        }
                    }

                    // Always record the region so tap detection and lazy decode work
                    // at all scroll positions, not just when first visible.
                    result.images.push_back({img_id, cy, iw, ih, thumb_h});

                    // Advance cursor past image
                    cy += thumb_h + 4;    // 4px gap below image
                    if (cy > result.content_height) result.content_height = cy;
                }
            }
            cx = leftEdge();
        }
        else if (name == "div") {
```

- [ ] **Step 3: Verify build**

```bash
cd /home/alun/esp32/wikipedia_offline/firmware
pio run
```

Expected: `SUCCESS`. Fix any compile errors before continuing.

- [ ] **Step 4: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add firmware/src/html_render.h firmware/src/html_render.cpp
git commit -m "firmware: ImageRegion struct and <img> tag parsing in html_render"
```

---

## Task 6: `wikiDbLoadImage()` in `wiki_db.cpp/h`

**Files:**
- Modify: `firmware/src/wiki_db.h`
- Modify: `firmware/src/wiki_db.cpp`

- [ ] **Step 1: Add declarations to `wiki_db.h`**

Find at the end of `wiki_db.h`:
```cpp
std::vector<SearchResult> wikiDbSearch(const String &prefix, int maxResults = 10);
```

Add after it:
```cpp
/*
 * Returns true if image data files were found and opened at init.
 * When false, wikiDbLoadImage() always returns 0.
 */
bool wikiDbImagesAvailable();

/*
 * Read the JPEG bytes for img_id into caller-supplied buf (max buf_size bytes).
 * Returns the number of bytes written, or 0 on any error (not found, buf too small, etc.).
 * The returned bytes are a complete, valid JPEG suitable for passing to TJpgDec.drawJpg().
 */
size_t wikiDbLoadImage(uint32_t img_id, uint8_t *buf, size_t buf_size);
```

- [ ] **Step 2: Add image state variables to `wiki_db.cpp`**

Find in `wiki_db.cpp`:
```cpp
// File handles kept open across calls to avoid repeated open/close overhead.
static File     g_index_file;
static File     g_id_index_file;
// Chunk file cache: keep the most-recently-used chunk open to avoid open/close on every request.
static File     g_chunk_file;
static uint32_t g_open_chunk_id = UINT32_MAX;
```

Add after it:
```cpp
// Image database (optional — absent when ZIM was built with --no-images)
static bool     g_images_available   = false;
static uint32_t g_image_count        = 0;
static File     g_img_index_file;
static File     g_img_chunk_file;
static uint32_t g_open_img_chunk_id  = UINT32_MAX;
```

- [ ] **Step 3: Add image init to `wikiDbInit()` in `wiki_db.cpp`**

Find in `wikiDbInit()` the block that opens chunk 0 (ends with `g_open_chunk_id = 0;`):

```cpp
        g_open_chunk_id = 0;
    }

    // Load sparse index: flash cache first (zero SD access), SD fallback + re-cache.
```

Insert between these two blocks:

```cpp
        g_open_chunk_id = 0;
    }

    // Try to open the optional image database.
    // img_meta.txt is absent for _pictures:no ZIM builds — handle gracefully.
    {
        File img_meta = SD.open(IMG_META_PATH, FILE_READ);
        if (img_meta) {
            while (img_meta.available()) {
                String line = img_meta.readStringUntil('\n');
                line.trim();
                if (line.startsWith("image_count=")) {
                    uint32_t v = (uint32_t)line.substring(12).toInt();
                    if (v > 0) g_image_count = v;
                }
            }
            img_meta.close();
        }
        if (g_image_count > 0) {
            g_img_index_file = SD.open(IMG_INDEX_PATH, FILE_READ);
            if (g_img_index_file) {
                char img_c0[32];
                snprintf(img_c0, sizeof(img_c0), IMG_CHUNK_FMT, 0u);
                g_img_chunk_file = SD.open(img_c0, FILE_READ);
                if (g_img_chunk_file) {
                    g_open_img_chunk_id = 0;
                    g_images_available = true;
                    Serial.printf("[wiki_db] Images: %u available\n", g_image_count);
                } else {
                    Serial.println("[wiki_db] img_0000.dat missing — images disabled");
                    g_img_index_file.close();
                }
            } else {
                Serial.println("[wiki_db] img_index.bin missing — images disabled");
            }
        }
    }

    // Load sparse index: flash cache first (zero SD access), SD fallback + re-cache.
```

- [ ] **Step 4: Implement `wikiDbImagesAvailable()` and `wikiDbLoadImage()` at the end of `wiki_db.cpp`**

Add after the closing brace of `wikiDbSearch()`:

```cpp
// ---------------------------------------------------------------------------
// Image loading
// ---------------------------------------------------------------------------

bool wikiDbImagesAvailable() {
    return g_images_available;
}

size_t wikiDbLoadImage(uint32_t img_id, uint8_t *buf, size_t buf_size) {
    if (!g_images_available) return 0;
    if (img_id >= g_image_count) {
        Serial.printf("[wiki_db] wikiDbLoadImage: id %u >= count %u\n", img_id, g_image_count);
        return 0;
    }
    if (!g_img_index_file.seek(img_id * 12)) {
        Serial.printf("[wiki_db] img_index seek failed for id=%u\n", img_id);
        return 0;
    }
    uint32_t chunk_id = readU32LE(g_img_index_file);
    uint32_t offset   = readU32LE(g_img_index_file);
    uint32_t length   = readU32LE(g_img_index_file);

    if (length == 0 || length > buf_size) {
        Serial.printf("[wiki_db] img id=%u length=%u buf_size=%u — skip\n",
                      img_id, length, (unsigned)buf_size);
        return 0;
    }

    if (chunk_id != g_open_img_chunk_id) {
        g_img_chunk_file.close();
        char path[32];
        snprintf(path, sizeof(path), IMG_CHUNK_FMT, chunk_id);
        g_img_chunk_file = SD.open(path, FILE_READ);
        if (!g_img_chunk_file) {
            Serial.printf("[wiki_db] Cannot open img chunk %s\n", path);
            g_open_img_chunk_id = UINT32_MAX;
            return 0;
        }
        g_open_img_chunk_id = chunk_id;
    }

    if (!g_img_chunk_file.seek(offset)) {
        Serial.printf("[wiki_db] img chunk seek failed id=%u offset=%u\n", img_id, offset);
        return 0;
    }
    if (!readExact(g_img_chunk_file, buf, length)) {
        Serial.printf("[wiki_db] img read failed id=%u length=%u\n", img_id, length);
        return 0;
    }
    return length;
}
```

- [ ] **Step 5: Verify build**

```bash
cd /home/alun/esp32/wikipedia_offline/firmware
pio run
```

Expected: `SUCCESS`.

- [ ] **Step 6: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add firmware/src/wiki_db.h firmware/src/wiki_db.cpp
git commit -m "firmware: wikiDbLoadImage() and image DB init in wikiDbInit()"
```

---

## Task 7: TJpgDec setup + thumbnail rendering

**Files:**
- Modify: `firmware/src/display.h`
- Modify: `firmware/src/display.cpp`
- Modify: `firmware/src/ui.cpp`

- [ ] **Step 1: Add `displayDrawJpeg()` declaration to `display.h`**

Find at the end of `display.h`:
```cpp
void displayInit();
```

Replace with:
```cpp
void displayInit();

// Decode a JPEG from memory and draw it at (x, y) on screen.
// Output is clipped to the vertical window [vp_y, vp_y + vp_h) to avoid
// drawing into the nav bar or outside the content area.
// scale: 1 = full size, 2 = half, 4 = quarter, 8 = eighth.
void displayDrawJpeg(int32_t x, int32_t y, const uint8_t *buf, uint32_t len,
                     uint8_t scale, int16_t vp_y, int16_t vp_h);
```

- [ ] **Step 2: Add TJpgDec callback, update `displayInit()`, add `displayDrawJpeg()` to `display.cpp`**

Replace the entire `display.cpp` with:

```cpp
#include "display.h"
#include "config.h"
#include <TJpg_Decoder.h>

TFT_eSPI tft = TFT_eSPI();

// TJpgDec pixel-output callback — called for each decoded MCU block.
static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (y >= (int16_t)tft.height()) return 0;
    tft.pushImage(x, y, w, h, bitmap);
    return 1;
}

void displayInit() {
    tft.init();
    tft.setRotation(1);   // landscape, USB connector on right
    tft.fillScreen(COL_BG);

    pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(TFT_BACKLIGHT_PIN, HIGH);

    TJpgDec.setSwapBytes(true);   // ESP32 is little-endian; TFT_eSPI expects byte-swapped RGB565
    TJpgDec.setCallback(tft_output);
}

void displayDrawJpeg(int32_t x, int32_t y, const uint8_t *buf, uint32_t len,
                     uint8_t scale, int16_t vp_y, int16_t vp_h) {
    // Set a viewport so nothing is drawn outside [vp_y, vp_y + vp_h).
    // TFT_eSPI's pushImage() respects the viewport, so the TJpgDec callback is
    // automatically clipped without any per-pixel logic in this function.
    tft.setViewport(0, vp_y, SCREEN_W, vp_h);
    TJpgDec.setJpgScale(scale);
    TJpgDec.drawJpg(x, y, buf, len);
    tft.resetViewport();
}
```

- [ ] **Step 3: Add `#include "config.h"` to `ui.cpp`**

`IMG_MAX_JPEG` is defined in `config.h` and used in the new thumbnail code. Add the include.

Find at the top of `ui.cpp`:
```cpp
#include "ui.h"
#include "display.h"
```

Replace with:
```cpp
#include "ui.h"
#include "config.h"
#include "display.h"
```

- [ ] **Step 4: Add `g_art_images` state and `drawImageThumbnail()` helper to `ui.cpp`**

Find in `ui.cpp`:
```cpp
static int32_t                   g_art_scroll = 0;
static int32_t                   g_art_total_h = 0;
static std::vector<LinkRegion>   g_art_links;
```

Replace with:
```cpp
static int32_t                   g_art_scroll = 0;
static int32_t                   g_art_total_h = 0;
static std::vector<LinkRegion>   g_art_links;
static std::vector<ImageRegion>  g_art_images;
```

Then add the `drawImageThumbnail()` helper just before `renderArticle()`:

```cpp
// Decode and draw a single image thumbnail at its current scroll position.
// Clips to the article content area (ART_CONTENT_Y … ART_CONTENT_Y + ART_CONTENT_H).
// Only called for images that are at least partially visible.
static void drawImageThumbnail(const ImageRegion &img) {
    if (!wikiDbImagesAvailable()) return;

    // Screen Y of the top of this image's placeholder rect
    int16_t sy = (int16_t)(img.doc_y - g_art_scroll + ART_CONTENT_Y);

    // Skip if entirely off screen
    if (sy + img.thumb_h <= ART_CONTENT_Y) return;
    if (sy >= ART_CONTENT_Y + ART_CONTENT_H) return;

    // Allocate a temporary buffer for the JPEG bytes
    uint8_t *buf = (uint8_t *)malloc(IMG_MAX_JPEG);
    if (!buf) return;

    size_t len = wikiDbLoadImage(img.img_id, buf, IMG_MAX_JPEG);
    if (len == 0) { free(buf); return; }

    // Choose the largest TJpgDec scale such that decoded_h >= thumb_h.
    // Valid scales are 1, 2, 4, 8 (output = JPEG / scale pixels).
    uint8_t scale = 1;
    if (img.img_h > 0 && img.thumb_h > 0) {
        int max_s = img.img_h / img.thumb_h;
        if      (max_s >= 8) scale = 8;
        else if (max_s >= 4) scale = 4;
        else if (max_s >= 2) scale = 2;
        else                  scale = 1;
    }

    // Centre the decoded image horizontally within the article column
    int16_t dec_w = (img.img_w > 0) ? img.img_w / scale : ART_COL_W;
    int16_t draw_x = (ART_COL_W > dec_w) ? (ART_COL_W - dec_w) / 2 : 0;

    // Vertical clip window: intersection of thumbnail rect with content area.
    // This prevents the decoded image from overflowing into the nav bar (top)
    // or the content below the thumbnail (bottom, via thumb_h cap).
    int16_t vp_top = max((int16_t)ART_CONTENT_Y, sy);
    int16_t vp_bot = min((int16_t)(ART_CONTENT_Y + ART_CONTENT_H),
                         (int16_t)(sy + img.thumb_h));
    if (vp_bot > vp_top) {
        displayDrawJpeg(draw_x, sy, buf, (uint32_t)len, scale, vp_top, vp_bot - vp_top);
    }

    free(buf);
}
```

- [ ] **Step 5: Update `renderArticle()` to decode visible thumbnails**

Find `renderArticle()`:
```cpp
static void renderArticle() {
    tft.fillRect(0, ART_CONTENT_Y, ART_COL_W, ART_CONTENT_H, COL_BG);
    RenderResult rr = htmlRender(g_art_html,
                                  0, ART_CONTENT_Y, ART_CONTENT_H,
                                  g_art_scroll, ART_COL_W);
    g_art_links   = rr.links;
    g_art_total_h = rr.content_height;
    drawScrollBar(g_art_scroll, g_art_total_h);
    // Defence-in-depth: re-assert nav bar in case htmlRender drew above clip_y.
    drawNavBar(g_art_title, true);
}
```

Replace with:
```cpp
static void renderArticle() {
    tft.fillRect(0, ART_CONTENT_Y, ART_COL_W, ART_CONTENT_H, COL_BG);
    RenderResult rr = htmlRender(g_art_html,
                                  0, ART_CONTENT_Y, ART_CONTENT_H,
                                  g_art_scroll, ART_COL_W);
    g_art_links   = rr.links;
    g_art_images  = rr.images;
    g_art_total_h = rr.content_height;

    // Decode and draw visible image thumbnails (lazy: from SD each scroll redraw).
    for (const auto &img : g_art_images) {
        drawImageThumbnail(img);
    }

    drawScrollBar(g_art_scroll, g_art_total_h);
    // Defence-in-depth: re-assert nav bar in case htmlRender drew above clip_y.
    drawNavBar(g_art_title, true);
}
```

- [ ] **Step 6: Verify build**

```bash
cd /home/alun/esp32/wikipedia_offline/firmware
pio run
```

Expected: `SUCCESS`.

- [ ] **Step 7: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add firmware/src/display.h firmware/src/display.cpp firmware/src/ui.cpp
git commit -m "firmware: TJpgDec setup and thumbnail rendering in renderArticle()"
```

---

## Task 8: `STATE_IMAGE` full-screen view + tap detection

**Files:**
- Modify: `firmware/src/ui.cpp`

- [ ] **Step 1: Add `STATE_IMAGE` to the state enum and image state variables**

Find:
```cpp
enum UIState { STATE_SEARCH, STATE_ARTICLE };
static UIState g_state = STATE_SEARCH;
```

Replace with:
```cpp
enum UIState { STATE_SEARCH, STATE_ARTICLE, STATE_IMAGE };
static UIState g_state = STATE_SEARCH;
```

Find (after the `g_history` declaration):
```cpp
#define ART_CONTENT_Y  NAV_H
```

Add before that `#define`:
```cpp
// Full-screen image state
static uint32_t  g_full_img_id   = UINT32_MAX;
static int16_t   g_full_img_w    = 0;
static int16_t   g_full_img_h    = 0;
static int32_t   g_art_scroll_save = 0;   // scroll position to restore when exiting STATE_IMAGE
```

- [ ] **Step 2: Add `showImageFull()` function (after `drawScrollBar()`, before `renderArticle()`)**

```cpp
static void showImageFull(uint32_t img_id, int16_t img_w, int16_t img_h) {
    g_full_img_id = img_id;
    g_full_img_w  = img_w;
    g_full_img_h  = img_h;
    g_state       = STATE_IMAGE;

    tft.fillScreen(COL_BG);
    drawNavBar("", true);   // back arrow only, no title

    if (!wikiDbImagesAvailable()) return;

    uint8_t *buf = (uint8_t *)malloc(IMG_MAX_JPEG);
    if (!buf) return;

    size_t len = wikiDbLoadImage(img_id, buf, IMG_MAX_JPEG);
    if (len == 0) { free(buf); return; }

    // Full-screen: image is ≤ 320 × 212 from preprocessor; use scale 1 for best quality.
    // Centre within the content area (below nav bar).
    int16_t avail_w = SCREEN_W;
    int16_t avail_h = SCREEN_H - NAV_H;
    int16_t draw_x  = (avail_w > img_w) ? (avail_w - img_w) / 2 : 0;
    int16_t draw_y  = NAV_H + ((avail_h > img_h) ? (avail_h - img_h) / 2 : 0);

    displayDrawJpeg(draw_x, draw_y, buf, (uint32_t)len, 1, NAV_H, avail_h);
    free(buf);
}
```

- [ ] **Step 3: Update `handleArticleTap()` to detect taps on image thumbnails**

Find `handleArticleTap()`:
```cpp
static void handleArticleTap(int16_t tx, int16_t ty) {
    if (ty < NAV_H) {
        if (tx < 80) {
            // Back button
            ...
        }
        return;
    }

    // Check link taps
    for (const auto &link : g_art_links) {
```

Add image tap detection between the nav check and the link check:

```cpp
static void handleArticleTap(int16_t tx, int16_t ty) {
    if (ty < NAV_H) {
        if (tx < 80) {
            // Back button
            if (!g_history.empty()) {
                uint32_t prev = g_history.back();
                g_history.pop_back();
                uint32_t was = g_art_id;
                goToArticle(prev, false);
                g_art_id = was;
                (void)was;
            } else {
                showSearchScreen();
            }
        }
        return;
    }

    // Check image thumbnail taps (before link taps — images are large regions)
    for (const auto &img : g_art_images) {
        int16_t sy = (int16_t)(img.doc_y - g_art_scroll + ART_CONTENT_Y);
        if (ty >= sy && ty < sy + img.thumb_h) {
            g_art_scroll_save = g_art_scroll;
            showImageFull(img.img_id, img.img_w, img.img_h);
            return;
        }
    }

    // Check link taps
    for (const auto &link : g_art_links) {
        if (tx >= link.x && tx < link.x + link.w &&
            ty >= link.y && ty < link.y + link.h) {
            goToArticle(link.article_id, true);
            return;
        }
    }
}
```

- [ ] **Step 4: Add `STATE_IMAGE` to `uiLoop()`**

Find in `uiLoop()`:
```cpp
    switch (g_state) {
    case STATE_SEARCH:
        if (evt.type == TouchEvent::TAP)
            handleSearchTap(evt.x, evt.y);
        else if (evt.type == TouchEvent::SWIPE_UP || evt.type == TouchEvent::SWIPE_DOWN)
            handleSearchSwipe(evt.dy);
        break;
    case STATE_ARTICLE:
        if (evt.type == TouchEvent::TAP)
            handleArticleTap(evt.x, evt.y);
        else if (evt.type == TouchEvent::SWIPE_UP || evt.type == TouchEvent::SWIPE_DOWN)
            handleArticleSwipe(evt.dy);
        break;
    }
```

Replace with:
```cpp
    switch (g_state) {
    case STATE_SEARCH:
        if (evt.type == TouchEvent::TAP)
            handleSearchTap(evt.x, evt.y);
        else if (evt.type == TouchEvent::SWIPE_UP || evt.type == TouchEvent::SWIPE_DOWN)
            handleSearchSwipe(evt.dy);
        break;
    case STATE_ARTICLE:
        if (evt.type == TouchEvent::TAP)
            handleArticleTap(evt.x, evt.y);
        else if (evt.type == TouchEvent::SWIPE_UP || evt.type == TouchEvent::SWIPE_DOWN)
            handleArticleSwipe(evt.dy);
        break;
    case STATE_IMAGE:
        // Any tap (including the back arrow in the nav bar) returns to the article.
        if (evt.type == TouchEvent::TAP) {
            g_state      = STATE_ARTICLE;
            g_art_scroll = g_art_scroll_save;
            drawNavBar(g_art_title, true);
            renderArticle();
        }
        break;
    }
```

- [ ] **Step 5: Full build**

```bash
cd /home/alun/esp32/wikipedia_offline/firmware
pio run
```

Expected: `SUCCESS`. Fix any compile errors before continuing.

- [ ] **Step 6: Upload and smoke-test on device**

```bash
cd /home/alun/esp32/wikipedia_offline/firmware
pio run --target upload --upload-port /dev/ttyUSB0 && pio device monitor --port /dev/ttyUSB0 --baud 115200
```

Expected serial output:
```
=== Wikipedia Offline Reader ===
[wiki_db] Ready: NNNNN articles
[wiki_db] Images: 0 available   ← or absent if img_meta.txt missing (no-pictures ZIM)
[main] Ready
```

The firmware should work exactly as before (articles, search, links). No images appear yet because the ZIM has `_pictures:no`. This verifies all code paths that handle `g_images_available == false` without crashing.

- [ ] **Step 7: Commit**

```bash
cd /home/alun/esp32/wikipedia_offline
git add firmware/src/ui.cpp
git commit -m "firmware: STATE_IMAGE full-screen view and image tap detection in ui.cpp"
```

---

## Next Steps (after downloading a `_pictures:yes` ZIM)

Once you have a ZIM with pictures (e.g. `wikipedia_en_simple_all_maxi_YYYY-MM.zim` from https://download.kiwix.org/zim/wikipedia/):

1. **Run the preprocessor:**
   ```bash
   cd /home/alun/esp32/wikipedia_offline/preprocessor
   python3 build_wiki_db.py <new_zim_with_pictures.zim> ../output-pictures --workers 4
   ```
   Expect image extraction log lines and `img_meta.txt` in the output dir.

2. **Copy all `/wiki/` files to SD card:**
   Copy `index.bin`, `sparse_index.bin`, `id_index.bin`, `index_meta.txt`, `articles_NNNN.dat`,
   `img_index.bin`, `img_NNNN.dat`, `img_meta.txt` to the `/wiki/` folder on the SD card.

3. **Invalidate LittleFS sparse cache** (first boot after new ZIM will re-cache automatically —
   the article_count mismatch is detected and the old cache is overwritten).

4. **Upload and test** — articles with images should show grey placeholders that fill with
   decoded JPEGs; tapping a thumbnail should show the full-screen view.
