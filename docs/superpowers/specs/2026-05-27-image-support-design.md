# Image Support — Design Spec
**Date:** 2026-05-27  
**Project:** Wikipedia Offline Reader (ESP32 CYD / ESP32-2432S028R)  
**Status:** Approved

---

## Overview

Add inline image display to the Wikipedia offline reader. Images extracted from a ZIM file are stored on SD card as JPEG chunks. The firmware renders thumbnails inline with article text and supports a full-screen tap-to-expand view.

Target ZIM: `wikipedia_en_simple_all_mini` (with-pictures variant). The current `_pictures:no` ZIM must be replaced with a `_pictures:yes` download from https://download.kiwix.org/zim/wikipedia/ before implementation can proceed.

---

## Section 1 — SD Card Storage Layout

### Image chunk files
Images are stored in fixed-size chunk files alongside the article data:

```
/wiki/img_0000.dat   # raw concatenated JPEG bytes
/wiki/img_0001.dat   # next chunk (each up to ~4 MB)
/wiki/img_index.bin  # 12 bytes per entry: chunk_id(u32) + offset(u32) + length(u32)
/wiki/img_meta.txt   # image_count=N, chunk_size=4194304
```

- `img_id` is a 0-based sequential integer assigned during preprocessing.
- `img_index.bin` is sorted by `img_id` so firmware can seek directly: `img_id × 12` bytes.
- Each `.dat` chunk is capped at ~4 MB to keep FAT32 file sizes manageable.
- Total capacity: limited only by SD card size.

### HTML article format (patch to existing pipeline)
The preprocessor rewrites `<img>` tags in stripped HTML to:

```html
<img src="/img/42" w="240" h="180">
```

Where `w` and `h` are the **post-resize** pixel dimensions of the stored JPEG (used by firmware to pick TJpg_Decoder scale factor and compute thumbnail proportions).  
`42` is the `img_id` assigned during the image extraction pass.

---

## Section 2 — Preprocessor Pass

### New image extraction pass in `build_wiki_db.py`

**Input:** ZIM file (must have `_pictures:yes` tag)  
**Output:** `/wiki/img_NNNN.dat` chunk files, `/wiki/img_index.bin`, `/wiki/img_meta.txt`

**Filter — include:**
- `image/jpeg`, `image/png`, `image/webp` MIME types

**Filter — exclude:**
- `image/svg+xml` (not decodable by TJpg_Decoder)
- Paths matching `_mw_/` (MediaWiki chrome: icons, logos, UI elements)
- Images smaller than 50×50 px (icons / decorative)

**Resize pipeline (Pillow):**
- Convert PNG/WebP → JPEG in memory before writing
- Resize so the longest dimension ≤ 320 px, preserving aspect ratio
- Save as JPEG quality 75
- Result is always ≤ 320×240 px; post-resize `w×h` (actual JPEG dimensions) are stored in `<img>` tag

**`strip_html` patch:**
- When an `<img>` tag is encountered during HTML stripping, look up the image by its ZIM path
- If found: replace with `<img src="/img/ID" w="W" h="H">`
- If not found or filtered out: remove the tag entirely

**CLI:**
- `--no-images` flag: skip image extraction entirely (for text-only builds or ZIMs without pictures)
- `--images` flag (default): run image extraction pass before article pass

**Dependencies:** Add `Pillow` to `requirements.txt`.

---

## Section 3 — Firmware Rendering Pipeline

### New library
`TJpg_Decoder` (Bodmer) added to `platformio.ini`:
```ini
lib_deps =
    bodmer/TJpgDec
```

### `html_render.cpp` — inline thumbnails

**Tag parsing:**  
`flushText()` / block parser detects `<img src="/img/ID" w="W" h="H">` and:
1. Computes `thumb_h = min(120, 120 * H / W)` to maintain aspect ratio (cap 120 px)
2. Appends an `ImageRegion { img_id, doc_y, thumb_w=SCREEN_W, thumb_h }` to the render result
3. Draws a grey placeholder rect at `doc_y` and advances `cy` by `thumb_h`

**Lazy decode on draw:**  
After the render pass, each `ImageRegion` whose thumbnail rect intersects the current viewport is decoded and drawn via a new `wiki_db.cpp` helper:

```cpp
// wiki_db.h — new addition
// Reads img_id's JPEG into caller-supplied buf (max buf_size bytes).
// Returns actual byte count, or 0 on error.
size_t wikiDbLoadImage(uint32_t img_id, uint8_t* buf, size_t buf_size);
```

- `wikiDbLoadImage` opens `img_index.bin`, seeks to `img_id × 12`, reads `chunk_id + offset + length`, then reads `length` bytes from `/wiki/img_NNNN.dat`
- Caller passes buffer to `TJpgDec.drawJpg(x, y, buf, len)` with scale factor (1/1, 1/2, 1/4, or 1/8) chosen so output height ≥ `thumb_h`
- Images scroll with the article — the placeholder rect is in doc coordinate space, drawn only when visible

**`RenderResult` struct** (in `html_render.h`):
```cpp
struct ImageRegion {
    uint32_t img_id;
    int32_t  doc_y;     // position in document coordinates
    int16_t  thumb_w;
    int16_t  thumb_h;
};

struct RenderResult {
    int32_t doc_height;
    std::vector<ImageRegion> images;
};
```

`renderArticle()` is updated to return `RenderResult` instead of `int32_t` (current return is doc height). `ui.cpp` stores the returned `RenderResult` in a file-scope `static RenderResult g_render` so the `images` list persists across scroll redraws for tap-detection and lazy decode.

### `ui.cpp` — full-screen image view

**New state:** `STATE_IMAGE`

**Tap detection:**  
In `STATE_ARTICLE`, when a `TAP` event lands within a rendered `ImageRegion` (screen Y within `screenY(img.doc_y)` to `screenY(img.doc_y) + img.thumb_h`):
- Store `g_image_id = img.img_id`
- Store `g_article_scroll_save = g_art_scroll` (to restore on back)
- Transition to `STATE_IMAGE`

**Full-screen render:**  
- Fill screen with `COL_BG`
- Fetch JPEG as above, decode at scale 1/1 or 1/2 to fit 320×240, centred
- Draw nav bar: `drawNavBar("", true)` (back arrow only, no title)

**Exit:**  
Any tap → pop back to `STATE_ARTICLE`, restore `g_art_scroll`, re-render article.

---

## Data Flow Summary

```
ZIM file
  │
  ▼ build_wiki_db.py (image pass)
img_index.bin + img_NNNN.dat + img_meta.txt
  │
  ▼ SD card /wiki/
  │
  ▼ firmware boot (wiki_db.cpp)
  │  loads img_meta (image_count)
  │
  ▼ article render (html_render.cpp)
  │  grey placeholder rects + ImageRegion list
  │
  ▼ lazy decode (wiki_db.cpp helper)
     TJpgDec → tft.drawBitmap at doc_y position
  │
  ▼ tap on thumbnail (ui.cpp)
     STATE_IMAGE: full-screen JPEG + back tap
```

---

## Constraints & Limits

| Item | Value |
|------|-------|
| Max thumbnail height | 120 px |
| Max full-screen decode | 320×240 |
| JPEG decode buffer | heap-allocated per image; freed immediately after draw |
| Chunk file max size | ~4 MB |
| TJpg_Decoder scale factors | 1/1, 1/2, 1/4, 1/8 |
| Min image size (preprocessor filter) | 50×50 px |

---

## Out of Scope

- PNG/WebP display on device (all images converted to JPEG in preprocessor)
- Image caching in RAM between scrolls (re-decoded each time from SD)
- Multiple images side-by-side
- Animated GIF support
- Image captions (discarded during HTML strip)
