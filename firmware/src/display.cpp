// Written by Alun Morris and Claude Code
#include "display.h"
#include "config.h"
#include <TJpg_Decoder.h>

TFT_eSPI tft = TFT_eSPI();

// ---------------------------------------------------------------------------
// UTF-8 → ASCII transliteration
// ---------------------------------------------------------------------------

// Decode one UTF-8 codepoint from s[i]; advance i past it.
static uint32_t decodeUtf8(const char *s, int &i, int len) {
    uint8_t c = (uint8_t)s[i++];
    if (c < 0x80) return c;
    uint32_t cp; int extra;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return '?';
    while (extra-- && i < len && ((uint8_t)s[i] & 0xC0) == 0x80)
        cp = (cp << 6) | ((uint8_t)s[i++] & 0x3F);
    return cp;
}

static const char *cpToAscii(uint32_t cp) {
    // Latin-1 supplement (U+00A0–U+00FF)
    switch (cp) {
    case 0x00A0: return " ";   // non-breaking space
    case 0x00A9: return "(c)"; // ©
    case 0x00AE: return "(r)"; // ®
    case 0x00B0: return " deg";// °
    case 0x00B1: return "+-";  // ±
    case 0x00B2: return "2";   // ²
    case 0x00B3: return "3";   // ³
    case 0x00B7: return ".";   // ·
    case 0x00B9: return "1";   // ¹
    case 0x00BC: return "1/4"; // ¼
    case 0x00BD: return "1/2"; // ½
    case 0x00BE: return "3/4"; // ¾
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return "A";
    case 0x00C6: return "AE";
    case 0x00C7: return "C";
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return "E";
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return "I";
    case 0x00D0: return "D";
    case 0x00D1: return "N";
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8: return "O";
    case 0x00D7: return "x";   // ×
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return "U";
    case 0x00DD: return "Y";
    case 0x00DE: return "Th";
    case 0x00DF: return "ss";  // ß
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return "a";
    case 0x00E6: return "ae";
    case 0x00E7: return "c";
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return "e";
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return "i";
    case 0x00F0: return "d";
    case 0x00F1: return "n";
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8: return "o";
    case 0x00F7: return "/";   // ÷
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return "u";
    case 0x00FD: case 0x00FF: return "y";
    case 0x00FE: return "th";
    }
    // Latin Extended-A (U+0100–U+017F): strip diacritics to base letter
    if (cp >= 0x0100 && cp <= 0x017E) {
        static const char *ext_a[] = {
            "A","a","A","a","A","a","C","c","C","c","C","c","C","c", // 00-0D
            "D","d","D","d","E","e","E","e","E","e","E","e","E","e", // 0E-1B
            "G","g","G","g","G","g","G","g","H","h","H","h",         // 1C-27
            "I","i","I","i","I","i","I","i","I","i","IJ","ij",       // 28-33
            "J","j","K","k","k","L","l","L","l","L","l","L","l","L","l", // 34-42
            "N","n","N","n","N","n","n","N","n","O","o","O","o","O","o",  // 43-51
            "OE","oe","R","r","R","r","R","r","S","s","S","s","S","s","S","s", // 52-61
            "T","t","T","t","T","t","U","u","U","u","U","u","U","u","U","u","U","u", // 62-73
            "W","w","Y","y","Y","Z","z","Z","z","Z","z","s"          // 74-7E
        };
        return ext_a[cp - 0x0100];
    }
    // Greek lowercase (U+03B1–U+03C9)
    if (cp >= 0x03B1 && cp <= 0x03C9) {
        static const char *greek_lo[] = {
            "alpha","beta","gamma","delta","epsilon","zeta","eta","theta",
            "iota","kappa","lambda","mu","nu","xi","omicron","pi","rho",
            "sigma","sigma","tau","upsilon","phi","chi","psi","omega"
        };
        return greek_lo[cp - 0x03B1];
    }
    // Greek uppercase (U+0391–U+03A9)
    if (cp >= 0x0391 && cp <= 0x03A9) {
        static const char *greek_up[] = {
            "Alpha","Beta","Gamma","Delta","Epsilon","Zeta","Eta","Theta",
            "Iota","Kappa","Lambda","Mu","Nu","Xi","Omicron","Pi","Rho",
            "?","Sigma","Tau","Upsilon","Phi","Chi","Psi","Omega"
        };
        return greek_up[cp - 0x0391];
    }
    // General punctuation (U+2000–U+206F)
    switch (cp) {
    case 0x2013: return "-";    // en-dash
    case 0x2014: return "-";    // em-dash
    case 0x2018: case 0x2019: return "'";   // curly single quotes
    case 0x201A: return ",";
    case 0x201C: case 0x201D: return "\"";  // curly double quotes
    case 0x2020: return "*";    // dagger
    case 0x2021: return "**";   // double dagger
    case 0x2022: return "*";    // bullet
    case 0x2026: return "...";  // ellipsis
    case 0x2032: return "'";    // prime
    case 0x2033: return "''";   // double prime
    case 0x2122: return "(tm)"; // ™
    case 0x2190: return "<-";   // ←
    case 0x2192: return "->";   // →
    case 0x2194: return "<->"; // ↔
    case 0x2212: return "-";    // minus sign
    case 0x2215: return "/";    // division slash
    case 0x2260: return "!=";   // ≠
    case 0x2264: return "<=";   // ≤
    case 0x2265: return ">=";   // ≥
    case 0x221E: return "inf";  // ∞
    case 0x221A: return "sqrt"; // √
    case 0x00B5: case 0x03BC: return "mu"; // µ / μ
    }
    // Zero-width / formatting chars: silently drop
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF || cp == 0x00AD)
        return "";
    return "?";
}

String utf8ToAscii(const String &s) {
    String out;
    out.reserve(s.length());
    int i = 0, len = s.length();
    while (i < len) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { out += (char)c; i++; }
        else {
            uint32_t cp = decodeUtf8(s.c_str(), i, len);
            out += cpToAscii(cp);
        }
    }
    return out;
}

// Per-MCU block output buffer — max 16×16 at scale=1, 8×8 at scale=2.
static uint16_t s_jpeg_block[16 * 16];

// Checksum accumulated across all callback blocks for one image.
static uint32_t s_jpeg_csum;
static uint32_t s_jpeg_pixels;

static bool jpegDitherCallback(int16_t bx, int16_t by,
                                uint16_t w, uint16_t h, uint16_t *data) {
    if (by >= (int16_t)tft.height()) return 0;
    const uint8_t *src = (const uint8_t *)data;

    for (uint16_t row = 0; row < h; row++) {
        if (by + (int16_t)row >= (int16_t)SCREEN_H) break;
        for (uint16_t col = 0; col < w; col++) {
            uint32_t i = ((uint32_t)row * w + col) * 3;
            uint16_t px = ((uint16_t)(src[i  ]>>3)<<11)
                        | ((uint16_t)(src[i+1]>>2)<< 5)
                        |  (uint16_t)(src[i+2]>>3);
            s_jpeg_block[row * w + col] = (px >> 8) | (px << 8);
            // Checksum over the pre-swap RGB565 value (what we actually computed).
            s_jpeg_csum = s_jpeg_csum * 31 + px;
            s_jpeg_pixels++;
        }
    }
    tft.pushImage(bx, by, w, h, s_jpeg_block);
    return 1;
}

void displayInit() {
    tft.init();
    tft.setRotation(1);   // landscape, USB connector on right
    tft.fillScreen(COL_BG);

    pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(TFT_BACKLIGHT_PIN, HIGH);

    TJpgDec.setCallback(jpegDitherCallback);
}

void displayDrawJpeg(int32_t x, int32_t y, const uint8_t *buf, uint32_t len,
                     uint8_t scale, int16_t vp_y, int16_t vp_h, int16_t clip_w) {
    s_jpeg_csum   = 0;
    s_jpeg_pixels = 0;
    tft.setViewport(0, vp_y, clip_w, vp_h, false);
    TJpgDec.setJpgScale(scale);
    TJpgDec.drawJpg(x, y, buf, len);
    tft.resetViewport();
    Serial.printf("[jpeg] csum=0x%08X pixels=%u\n", s_jpeg_csum, s_jpeg_pixels);
}

// ---------------------------------------------------------------------------
// QOI decoder — outputs directly to TFT one scanline at a time.
// scale=1 → 1:1, scale=2 → 2×2 box-filter average (matches TJpgDec behaviour).
// Decoder state: 64×4 byte hash table + 4 bytes prev pixel = 260 bytes total.
// Spec: https://qoiformat.org
// ---------------------------------------------------------------------------

static void displayDrawQoi(int32_t x, int32_t y, const uint8_t *d, uint32_t len,
                            int16_t vp_y, int16_t vp_h, int16_t clip_w, uint8_t scale) {
    if (len < 14) return;
    uint32_t w = ((uint32_t)d[4]<<24)|((uint32_t)d[5]<<16)|((uint32_t)d[6]<<8)|d[7];
    uint32_t h = ((uint32_t)d[8]<<24)|((uint32_t)d[9]<<16)|((uint32_t)d[10]<<8)|d[11];
    if (w == 0 || h == 0 || w > SCREEN_W) return;
    if (scale < 1) scale = 1;

    // Running hash table: each entry is packed RGBA (r<<24|g<<16|b<<8|a)
    uint32_t tbl[64] = {};
    uint8_t  pr = 0, pg = 0, pb = 0, pa = 255;
    uint8_t  run = 0;
    uint32_t pos = 14;
    const uint32_t end = len - 8;   // stop before 8-byte end marker

    // Output scanline + accumulator for box-filter (uint16 safe: max 255*scale^2 ≤ 65535 for scale≤16)
    static uint16_t line[SCREEN_W];
    static uint16_t acc[SCREEN_W * 3];  // R,G,B channel sums per output column

    // Output dimensions after downsampling
    int32_t out_w = (int32_t)(w / scale);
    if (x + out_w > clip_w) out_w = clip_w - x;
    if (out_w <= 0) return;

    uint32_t out_row = 0;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t row_in_block = row % scale;
        bool is_block_start   = (row_in_block == 0);
        bool is_block_end     = (row_in_block == (uint32_t)(scale - 1)) || (row == h - 1);

        for (uint32_t col = 0; col < w; col++) {
            uint8_t r, g, b;

            if (run) {
                run--;
                r = pr; g = pg; b = pb;
            } else {
                if (pos >= end) { r = pr; g = pg; b = pb; }
                else {
                    uint8_t b1 = d[pos++];
                    if (b1 == 0xFE) {                       // QOI_OP_RGB
                        r=d[pos++]; g=d[pos++]; b=d[pos++]; pa=255;
                    } else if (b1 == 0xFF) {                // QOI_OP_RGBA
                        r=d[pos++]; g=d[pos++]; b=d[pos++]; pa=d[pos++];
                    } else if ((b1>>6) == 0) {              // QOI_OP_INDEX
                        uint32_t e = tbl[b1&0x3F];
                        r=(e>>24)&0xFF; g=(e>>16)&0xFF; b=(e>>8)&0xFF; pa=e&0xFF;
                    } else if ((b1>>6) == 1) {              // QOI_OP_DIFF
                        r = pr + ((b1>>4)&3) - 2;
                        g = pg + ((b1>>2)&3) - 2;
                        b = pb + ( b1    &3) - 2;
                    } else if ((b1>>6) == 2) {              // QOI_OP_LUMA
                        uint8_t b2 = d[pos++];
                        int8_t dg = (int8_t)((b1&0x3F) - 32);
                        r = pr + dg + ((b2>>4)&0xF) - 8;
                        g = pg + dg;
                        b = pb + dg + (b2&0xF) - 8;
                    } else {                                // QOI_OP_RUN
                        run = (b1&0x3F);
                        r = pr; g = pg; b = pb;
                    }
                    tbl[(r*3+g*5+b*7+pa*11)&63] = ((uint32_t)r<<24)|((uint32_t)g<<16)|((uint32_t)b<<8)|pa;
                    pr = r; pg = g; pb = b;
                }
            }

            // Snap near-white anti-aliasing artifacts to pure white.
            // Diagrams always have a white background; pixels this bright are edge bleed.
            if (r >= 220 && g >= 220 && b >= 220) { r = 255; g = 255; b = 255; }

            if (scale == 1) {
                if ((int32_t)col < out_w) {
                    uint16_t px = ((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5)|(b>>3);
                    line[col] = (px>>8)|(px<<8);
                }
            } else {
                // Accumulate into box-filter buffer
                uint32_t oc = col / scale;
                if ((int32_t)oc < out_w) {
                    uint32_t ci = oc * 3;
                    if (is_block_start && col % scale == 0) {
                        acc[ci] = r; acc[ci+1] = g; acc[ci+2] = b;
                    } else {
                        acc[ci] += r; acc[ci+1] += g; acc[ci+2] += b;
                    }
                }
            }
        }

        if (scale == 1 || is_block_end) {
            if (scale > 1) {
                // Average the accumulated block (vcount rows × scale cols per output pixel)
                uint32_t count = (row_in_block + 1) * (uint32_t)scale;
                for (int32_t oc = 0; oc < out_w; oc++) {
                    uint32_t ci = (uint32_t)oc * 3;
                    uint8_t rv = (uint8_t)(acc[ci  ] / count);
                    uint8_t gv = (uint8_t)(acc[ci+1] / count);
                    uint8_t bv = (uint8_t)(acc[ci+2] / count);
                    uint16_t px = ((uint16_t)(rv>>3)<<11)|((uint16_t)(gv>>2)<<5)|(bv>>3);
                    line[oc] = (px>>8)|(px<<8);
                }
            }
            int32_t sy = y + (int32_t)out_row;
            if (sy >= vp_y && sy < vp_y + vp_h)
                tft.pushImage(x, sy, out_w, 1, line);
            out_row++;
        }
    }
}

void displayDrawImage(int32_t x, int32_t y, const uint8_t *buf, uint32_t len,
                      uint8_t jpeg_scale, int16_t vp_y, int16_t vp_h,
                      int16_t clip_w, uint8_t qoi_scale) {
    if (len >= 4 && buf[0]=='q' && buf[1]=='o' && buf[2]=='i' && buf[3]=='f') {
        Serial.printf("[img] format=QOI len=%u scale=%u\n", len, qoi_scale);
        displayDrawQoi(x, y, buf, len, vp_y, vp_h, clip_w, qoi_scale);
    } else {
        Serial.printf("[img] format=JPEG len=%u scale=%u\n", len, jpeg_scale);
        displayDrawJpeg(x, y, buf, len, jpeg_scale, vp_y, vp_h, clip_w);
    }
}
