// Written by Alun Morris and Claude Code
#pragma once

// CYD (ESP32-2432S028) SD card uses VSPI bus — separate from the TFT HSPI bus.
// Standard CYD SD wiring: MOSI=23, MISO=19, SCK=18, CS=5
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18

// CYD TFT backlight (PWM or digital)
#define TFT_BACKLIGHT_PIN 21

// XPT2046 touch pins are defined in touch.cpp (separate HSPI bus)

// WiFi AP settings
#define WIFI_SSID "Wikipedia"
#define WIFI_PASS ""   // open network

// SD paths
#define INDEX_PATH          "/wiki/index.bin"
#define SPARSE_INDEX_PATH   "/wiki/sparse_index.bin"

// LittleFS path for the cached sparse index (written from SD on first use).
// Avoids SD reads for the sparse index on subsequent boots.
#define FLASH_SPARSE_PATH   "/sparse_index.bin"
#define ARTICLES_CHUNK_FMT  "/wiki/articles_%04u.dat"
#define META_PATH           "/wiki/index_meta.txt"
#define ID_INDEX_PATH       "/wiki/id_index.bin"

#define INDEX_RECORD_SIZE 80
#define TITLE_KEY_LEN     60

// Sparse index parameters.
// SPARSE_KEY_LEN is fixed (always 12 bytes per entry).
// SPARSE_STEP is the *default* fallback; the actual value is read from index_meta.txt
// (sparse_step=N) at runtime so large datasets can use a bigger step without a
// firmware rebuild.  MUST match build_wiki_db.py when regenerating from scratch.
#define SPARSE_STEP_DEFAULT  64
#define SPARSE_KEY_LEN       12

// Word index (word_index.bin + title_index.bin) for secondary "contains" search.
#define WORD_INDEX_PATH   "/wiki/word_index.bin"
#define TITLE_INDEX_PATH  "/wiki/title_index.bin"
#define WORD_KEY_LEN       14   // 13 chars + null per word entry
#define WORD_ENTRY_SIZE    20   // WORD_KEY_LEN(14) + ids_start(4) + ids_count(2)
#define TITLE_ENTRY_SIZE   32   // 31 chars + null per title entry in title_index.bin

// Image scratch buffer (bytes). Used for JPEG and QOI image loading.
// QOI diagrams (≤256 colours, 320×212) are typically well under 64 KB.
#define IMG_BUF_SIZE (64 * 1024)

// Article pagination: max HTML bytes rendered per page.
// Splitting keeps per-page heap usage predictable on the 320×240 device.
#define ART_PAGE_BYTES  60000u

// Magic article IDs used for Prev/Next page navigation links injected into page HTML.
// Must not collide with real article IDs (max real ID ≈ 285 551 for the maxi dump).
#define ARTICLE_ID_NEXT_PAGE (0xFFFFFFFEu)
#define ARTICLE_ID_PREV_PAGE (0xFFFFFFFDu)

// Image database paths
#define IMG_INDEX_PATH  "/wiki/img_index.bin"
#define IMG_CHUNK_FMT   "/wiki/img_%04u.dat"
#define IMG_META_PATH   "/wiki/img_meta.txt"

