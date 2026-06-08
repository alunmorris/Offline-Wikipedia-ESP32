// Written by Alun Morris and Claude Code
#pragma once
#include <TFT_eSPI.h>

#define SCREEN_W  320
#define SCREEN_H  240

// Fixed UI chrome heights
#define NAV_H     28
#define KB_H      144   // 4 rows × 36px stride (KEY_H=32 square + 4px gap; QWERTY + ASDFG + shift/ZXCVBNM + num/space/done)

// Font metrics
#define FONT2_CHAR_W   7    // approx px per character at TFT built-in font 2

// Nav bar tap geometry
#define NAV_BACK_BTN_W  80  // tap-zone width of the back button

// Colors (RGB565) — light theme
#define COL_BG            TFT_WHITE          // white background
#define COL_TEXT          0x2104             // near-black text  (~32,32,32)
#define COL_LINK          0x000D             // dark navy blue
#define COL_H1            TFT_BLACK          // black headings
#define COL_H2            0x8000             // dark red  (~128,0,0)
#define COL_H3            0x0400             // dark green (~0,128,0)
#define COL_NAV_BG        0x2124             // dark blue-grey nav bar
#define COL_NAV_FG        TFT_WHITE
#define COL_KEY_BG        TFT_BLACK          // black key
#define COL_KEY_FG        TFT_WHITE          // white key text
#define COL_KEY_PRESS     0x4208             // dark grey press
#define COL_KEY_ACTIVE    TFT_WHITE          // active key bg (SFT/NUM on) — inverted: white bg, black text
#define COL_INPUT_BG      0xF7DE             // very light grey input (~240,240,240)
#define COL_INPUT_FG      0x2104             // near-black
#define COL_INPUT_CURSOR  TFT_BLACK
#define COL_RESULT_ODD    0xEF7D             // light grey row  (~232,232,232)
#define COL_RESULT_EVEN   TFT_WHITE
#define COL_RESULT_FG     0x2104             // near-black
#define COL_SCROLLBAR     0xC618             // light grey track
#define COL_SCROLLTHUMB   0xA514             // medium grey thumb
#define COL_BORDER        0x4208             // input box border / divider label text
#define COL_HINT          0x6B6D             // dim/hint text (e.g. "No results")

extern TFT_eSPI tft;

void displayInit();

// Transliterate UTF-8 text to ASCII for display with built-in bitmap fonts.
String utf8ToAscii(const String &s);

// Decode a JPEG from memory and draw at (x, y).
// Clipped to [vp_y, vp_y+vp_h) vertically and [0, clip_w) horizontally. scale: 1/2/4/8.
void displayDrawJpeg(int32_t x, int32_t y, const uint8_t *buf, uint32_t len,
                     uint8_t scale, int16_t vp_y, int16_t vp_h, int16_t clip_w = SCREEN_W);

// Auto-detect JPEG (FF D8) or QOI (qoif) from magic bytes and draw.
// jpeg_scale: JPEG downsample factor (1/2/4/8).
// qoi_scale:  nearest-neighbour QOI downsample factor (1 = full, 2 = half).
void displayDrawImage(int32_t x, int32_t y, const uint8_t *buf, uint32_t len,
                      uint8_t jpeg_scale, int16_t vp_y, int16_t vp_h,
                      int16_t clip_w = SCREEN_W, uint8_t qoi_scale = 1);
