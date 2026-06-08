# Wikipedia Offline Reader for ESP32 CYD

An offline Wikipedia reader for the ESP32-2432S028 ("Cheap Yellow Display") — a 320×240 ILI9341 touchscreen module. Articles, images, and a full search index are stored on a microSD card and read on-device with no internet connection.
<img width="2048" height="1528" alt="image" src="https://github.com/user-attachments/assets/0412ca0f-a21c-41e1-a242-1b9dbddb307e" />

Written by Alun Morris and Claude Code.
<img width="2048" height="1497" alt="image" src="https://github.com/user-attachments/assets/b713e92a-6ba9-4e7e-b10c-57586139830f" />

---

## Hardware

| Component | Detail |
|-----------|--------|
| Board | ESP32-2432S028 (CYD) |
| Display | 320×240 ILI9341 (HSPI bus) |
| Touch | XPT2046 resistive (shared HSPI) |
| SD card | VSPI bus — CS=5, MOSI=23, MISO=19, SCK=18 |
| Backlight | GPIO 21 |

A microSD card of **at least 8 GB** is required (Simple English Wikipedia uses ~7 GB on card).

---

## Choosing a ZIM File

The database is built from a [Kiwix](https://www.kiwix.org/) ZIM file. Download one from ([Wikimedia dumps](https://dumps.wikimedia.org/kiwix/zim/wikipedia/)).

**Recommended: Simple English Wikipedia — `wikipedia_en_simple_all_maxi_YYYY-MM.zim`**

- ~3.3 GB download, ~285 000 articles
- Shorter articles and simpler language — well-suited to a small screen
- Includes images (`_maxi` variant)
- processed size is ~10GB

Other ZIM files will work but larger editions (eg full English, ~90 GB) may exceed the SD card size the CYD is known to handle (32GB OK, 64GB may work).
> Choose the `_maxi` variant (includes images). The `_mini` variant omits images and has only the top 50-100k articles.

**There is a small demo Wiki about knots in the preprocessor folder**

---

## Preprocessor

The preprocessor converts a ZIM file into the binary database format read by the firmware.

### Why a preprocessor is needed

The ESP32 cannot read a ZIM file directly. Several hard constraints make a purpose-built binary format necessary:

- **ZIM uses zstd cluster compression.** Decompressing a zstd cluster requires holding the entire cluster in RAM. ZIM clusters are typically 1–4 MB, which exceeds the ESP32's ~300 KB of usable heap. The preprocessor re-compresses each article individually with LZ4, which decompresses in a few KB of working memory.

- **ZIM's index structure is too complex for embedded use.** ZIM uses a URL-sorted B-tree-style index with variable-length entries. The binary index produced here is fixed-width (80 bytes/record), sorted by normalised title, and paired with a tiny sparse index (one entry per 64 articles) that fits entirely in RAM (~7 KB). Together they allow title lookup with zero SD seeks to find the scan start.

- **Images must be in formats the ESP32 can decode.** The firmware decodes JPEG via the hardware-accelerated TJpgDec library and QOI via a lightweight software decoder. ZIM stores images as WebP internally (with the original format preserved in the file path), which the ESP32 cannot guarantee to decode in available RAM. The preprocessor converts everything to JPEG (photos) or QOI (diagrams/SVGs).

- **ZIM HTML needs cleaning.** Wikipedia ZIM files inject boilerplate footers, navigation chrome, and complex class structures into every article. The preprocessor strips these with BeautifulSoup so the firmware's minimal HTML renderer only has to handle the subset of tags that actually appear in article bodies.

The processed folder is bigger than the ZIM because:

- ZIM uses WebP images, which is extremely efficient. The preprocessor decodes WebP then re-encodes as JPEG (photos) or QOI (diagrams). QOI is lossless — it faithfully preserves every pixel, which is great for quality but much larger than WebP. JPEG at quality 90 is also larger than WebP at equivalent visual quality. WebP is simply a better codec.

- ZIM uses cross-article zstd compression. Articles are packed into large clusters (1–4 MB) and compressed together — repeated phrases and boilerplate across articles compress away. The preprocessor re-compresses each article individually with LZ4, which loses that cross-article redundancy. LZ4 is chosen for decompression speed on the ESP32, not compression ratio.

### Requirements

- Python 3.9+
- Dependencies listed in `preprocessor/requirements.txt`:

```
pip install libzim lz4 beautifulsoup4 lxml Pillow cairosvg qoi
```

`cairosvg` also requires the system `cairo` library:
- Ubuntu/Debian: `sudo apt install libcairo2`
- macOS: `brew install cairo`

### Running the build

```bash
cd preprocessor
python3 build_wiki_db.py <input.zim> <output_dir>
```

Example:

```bash
python3 build_wiki_db.py --thumb-size 240x159 wikipedia_en_simple_all_maxi_2026-05.zim output_en_simple_all_maxi
```

This runs two passes:
1. **Pass 1** — indexes all article titles, builds `index.bin`, `sparse_index.bin`, `id_index.bin`
2. **Pass 2** — decompresses, cleans (strips ZIM boilerplate), and re-compresses articles in parallel, producing `articles_NNNN.dat` chunks
3. **Images** — renders and encodes thumbnails into `img_NNNN.dat` chunks
4. **Word index** — builds `word_index.bin` + `title_index.bin` for full-text "contains" search

Build time is roughly 30–60 minutes on a modern PC (uses up to 4 CPU cores by default).

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--limit N` | 0 (all) | Process only the first N articles (useful for testing) |
| `--workers N` | auto | Number of parallel compression workers (threads) |
| `--thumb-size WxH` | 320x212 | Max thumbnail dimensions in pixels |
| `--jpeg-quality Q` | 90 | JPEG quality for photo thumbnails, 1–95. Does not affect size much. |
| `--no-images` | off | Skip image processing |
| `--images-only` | off | Rebuild image database only (articles already built) |
| `--word-index-only` | off | Rebuild word/title index only |
| `--verbose` | off | Extra progress output |

### Output files

All files go into `<output_dir>/` and must be copied to a `wiki/` folder on the SD card root.

| File | Description |
|------|-------------|
| `index.bin` | Fixed-width title index (binary searchable, sorted) |
| `sparse_index.bin` | Every 64th title key — loaded into ESP32 RAM for fast search |
| `id_index.bin` | Maps article ID → chunk + offset + length |
| `index_meta.txt` | Metadata: article count, chunk size, database name |
| `articles_NNNN.dat` | LZ4-compressed article HTML, split into 32 MB chunks |
| `img_index.bin` | Maps image ID → chunk + offset + length |
| `img_NNNN.dat` | Encoded image thumbnails (JPEG or QOI), split into 4 MB chunks |
| `word_index.bin` | Word → article ID list for "contains" search |
| `title_index.bin` | Article title index for "contains" search |

---

## SD Card Setup

1. Format the card as FAT32.
2. Create a `wiki/` folder at the root.
3. Copy all files from `<output_dir>/` into `/wiki/`.

The card should look like:

```
/wiki/
  index.bin
  sparse_index.bin
  id_index.bin
  index_meta.txt
  articles_0000.dat
  articles_0001.dat
  ...
  img_index.bin
  img_0000.dat
  ...
  word_index.bin
  title_index.bin
```

---

## Firmware Build & Upload

The firmware is a PlatformIO project.

```bash
cd firmware
pio run -t upload
```

Monitor serial output at 115200 baud:

```bash
pio device monitor
```

On first boot the sparse index is cached to LittleFS so subsequent boots are faster.

---

## Usage

- **Search**: tap the search box to show the keyboard. Type a query and press **GO**.
  - Prefix-matching results appear first; a "contains:" divider separates full-text matches.
- **Navigate**: tap a blue underlined link to follow it. Tap **< BACK** to return.
- **Scroll**: swipe up/down, or use the arrow buttons in the nav bar.
- **Images**: tap an image thumbnail to view it full-screen.

---

## Project Structure

```
firmware/        PlatformIO ESP32 firmware
  src/
    main.cpp     Boot, splash screen
    ui.cpp       Search, article, and image views
    wiki_db.cpp  SD database access (search, load, images)
    html_render.cpp  HTML → TFT renderer
    display.cpp  TFT helpers, UTF-8 transliteration
    keyboard.cpp On-screen QWERTY keyboard
    touch.cpp    XPT2046 touch driver
    config.h     Hardware pins, file paths, constants

preprocessor/    PC-side database builder
  build_wiki_db.py   Main build script (ZIM → binary DB)
  debug_server.py    Local HTTP server for browser-based preview
  convert_to_v2.py   One-off migration from v1 format
  output_en_knots_maxi/ small example wiki. Has smaller images to reduce size
```
