// Written by Alun Morris and Claude Code
#include "keyboard.h"
#include "display.h"
#include <string.h>

// ---- Layout constants --------------------------------------------------------
// 4 rows × 36px stride (KEY_H=32 square + 4px inter-row gap) = 144px total (KB_H).

#define KEY_W   32
#define KEY_H   32   // square
#define KEY_R    3   // corner radius
#define ROW_H   36   // row stride: KEY_H + 4px gap

// Row character sets — letter mode
static const char ROW1[]      = "QWERTYUIOP";   // row 0 — 10 keys
static const char ROW2[]      = "ASDFGHJKL";    // row 1 —  9 keys, x offset = 16
static const char ROW3[]      = "ZXCVBNM";      // row 2 —  7 keys

// Row 0: digits / shifted specials (NUM mode)
static const char ROW_NUM[]   = "1234567890";
static const char ROW_NUM_S[] = "!@#$%^&*()";

// Rows 1-2: symbols (NUM mode) — iOS-style secondary symbol set
static const char ROW_SYM2[]  = "-/:;()$&@";    // row 1, 9 symbols
static const char ROW_SYM3[]  = "!'\"`,.?";    // row 2, 7 symbols: ,.? on B N M

// Row 1 X-offset (centres 9 × 32 = 288 within 320)
#define R2_X0   16

// Row 2: SHIFT | ZXCVBNM | DEL
#define SHIFT_W  48
#define SHIFT_X   0
#define R3_X0   SHIFT_W
#define DEL3_X  (SHIFT_W + 7 * KEY_W)  // 48 + 224 = 272
#define DEL3_W  (SCREEN_W - DEL3_X)    // 48

// Row 3: NUM | SPACE | CLR | GO
#define NUM_W    32
#define SP_X     NUM_W
#define SP_W     144
#define CLR_X    (NUM_W + SP_W)         // 176
#define CLR_W    48
#define DONE_X   (CLR_X + CLR_W)        // 224
#define DONE_W   (SCREEN_W - DONE_X)   // 96

// ---- State -------------------------------------------------------------------

static bool g_shift = false;
static bool g_num   = false;

bool keyboardGetShift() { return g_shift; }
bool keyboardGetNum()   { return g_num; }

// ---- Drawing helpers ---------------------------------------------------------

static void drawKey(int16_t x, int16_t y, int16_t w, const char *label,
                    uint16_t bg = COL_KEY_BG, uint16_t fg = COL_KEY_FG) {
    // 2px grey border: fill full key area with border colour, then fill
    // interior (inset 2px per side) with the key background colour.
    tft.fillRoundRect(x + 1, y + 1, w - 2, KEY_H - 2, KEY_R,     0x8410);
    tft.fillRoundRect(x + 3, y + 3, w - 6, KEY_H - 6, KEY_R - 1, bg);
    tft.setTextColor(fg, bg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, x + w / 2, y + KEY_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
}

void keyboardDraw(int16_t y0) {
    tft.fillRect(0, y0, SCREEN_W, KB_H, COL_BG);

    // Row 0 — QWERTY or digits (NUM mode)
    for (int i = 0; i < 10; i++) {
        char lbl[2];
        if (g_num)
            lbl[0] = g_shift ? ROW_NUM_S[i] : ROW_NUM[i];
        else
            lbl[0] = g_shift ? ROW1[i] : (char)(ROW1[i] + 32);
        lbl[1] = 0;
        drawKey(i * KEY_W, y0, KEY_W, lbl);
    }

    // Row 1 — ASDFGHJKL or symbols (NUM mode)
    for (int i = 0; i < 9; i++) {
        char lbl[2];
        lbl[0] = g_num ? ROW_SYM2[i]
                       : (g_shift ? ROW2[i] : (char)(ROW2[i] + 32));
        lbl[1] = 0;
        drawKey(R2_X0 + i * KEY_W, y0 + ROW_H, KEY_W, lbl);
    }

    // Row 2 — SHIFT | ZXCVBNM | DEL  (or symbols in NUM mode)
    uint16_t shiftBg = g_shift ? COL_KEY_ACTIVE : COL_KEY_BG;
    uint16_t shiftFg = g_shift ? TFT_BLACK      : COL_KEY_FG;
    drawKey(SHIFT_X, y0 + 2 * ROW_H, SHIFT_W, "SFT", shiftBg, shiftFg);
    for (int i = 0; i < 7; i++) {
        char lbl[2];
        lbl[0] = g_num ? ROW_SYM3[i]
                       : (g_shift ? ROW3[i] : (char)(ROW3[i] + 32));
        lbl[1] = 0;
        drawKey(R3_X0 + i * KEY_W, y0 + 2 * ROW_H, KEY_W, lbl);
    }
    drawKey(DEL3_X, y0 + 2 * ROW_H, DEL3_W, "DEL");

    // Row 3 — NUM | SPACE | CLR | GO
    uint16_t numBg = g_num ? COL_KEY_ACTIVE : COL_KEY_BG;
    uint16_t numFg = g_num ? TFT_BLACK      : COL_KEY_FG;
    drawKey(0,      y0 + 3 * ROW_H, NUM_W,  "NUM", numBg, numFg);
    drawKey(SP_X,   y0 + 3 * ROW_H, SP_W,   "SPACE");
    drawKey(CLR_X,  y0 + 3 * ROW_H, CLR_W,  "CLR");
    drawKey(DONE_X, y0 + 3 * ROW_H, DONE_W, "GO");
}

// ---- Key geometry helper -----------------------------------------------------

// Returns position/size/label of the key at (tx,ty).  Uses current g_shift/g_num
// state so the label matches what the user sees — call before any state change.
static bool keyGeometry(int16_t tx, int16_t ty, int16_t y0,
                        int16_t &kx, int16_t &ky, int16_t &kw, char lbl[8]) {
    if (ty < y0 || ty >= y0 + KB_H) return false;
    int row = (ty - y0) / ROW_H;
    ky = y0 + row * ROW_H;
    switch (row) {
    case 0: {
        int col = tx / KEY_W;
        if (col < 0 || col >= 10) return false;
        kx = col * KEY_W; kw = KEY_W;
        lbl[0] = g_num ? (g_shift ? ROW_NUM_S[col] : ROW_NUM[col])
                       : (g_shift ? ROW1[col]       : (char)(ROW1[col] + 32));
        lbl[1] = 0; return true;
    }
    case 1: {
        int col = (tx - R2_X0) / KEY_W;
        if (col < 0 || col >= 9) return false;
        kx = R2_X0 + col * KEY_W; kw = KEY_W;
        lbl[0] = g_num ? ROW_SYM2[col]
                       : (g_shift ? ROW2[col] : (char)(ROW2[col] + 32));
        lbl[1] = 0; return true;
    }
    case 2: {
        if (tx < SHIFT_W) { kx = SHIFT_X; kw = SHIFT_W; strcpy(lbl, "SFT"); return true; }
        if (tx >= DEL3_X) { kx = DEL3_X;  kw = DEL3_W;  strcpy(lbl, "DEL"); return true; }
        int col = (tx - R3_X0) / KEY_W;
        if (col < 0 || col >= 7) return false;
        kx = R3_X0 + col * KEY_W; kw = KEY_W;
        lbl[0] = g_num ? ROW_SYM3[col]
                       : (g_shift ? ROW3[col] : (char)(ROW3[col] + 32));
        lbl[1] = 0; return true;
    }
    case 3: {
        if (tx < NUM_W)  { kx = 0;      kw = NUM_W;  strcpy(lbl, "NUM");   return true; }
        if (tx < CLR_X)  { kx = SP_X;   kw = SP_W;   strcpy(lbl, "SPACE"); return true; }
        if (tx < DONE_X) { kx = CLR_X;  kw = CLR_W;  strcpy(lbl, "CLR");   return true; }
        kx = DONE_X; kw = DONE_W; strcpy(lbl, "GO"); return true;
    }
    }
    return false;
}

void keyboardFlashKey(int16_t tx, int16_t ty, int16_t y0) {
    int16_t kx, ky, kw; char lbl[8];
    if (!keyGeometry(tx, ty, y0, kx, ky, kw, lbl)) return;
    drawKey(kx, ky, kw, lbl, COL_KEY_ACTIVE, TFT_BLACK);
    delay(40);
    drawKey(kx, ky, kw, lbl, COL_KEY_BG, COL_KEY_FG);
}

// ---- Hit testing -------------------------------------------------------------

char keyboardHitTest(int16_t tx, int16_t ty, int16_t y0) {
    if (ty < y0 || ty >= y0 + KB_H) return 0;

    int row = (ty - y0) / ROW_H;   // 0..3

    switch (row) {

    case 0: {   // QWERTY or digits (NUM mode)
        int col = tx / KEY_W;
        if (col >= 0 && col < 10) {
            if (g_num)
                return g_shift ? ROW_NUM_S[col] : ROW_NUM[col];
            return g_shift ? ROW1[col] : (char)(ROW1[col] + 32);
        }
        break;
    }

    case 1: {   // ASDFGHJKL or symbols
        int col = (tx - R2_X0) / KEY_W;
        if (col >= 0 && col < 9) {
            if (g_num) return ROW_SYM2[col];
            char c = g_shift ? ROW2[col] : (char)(ROW2[col] + 32);
            g_shift = false;
            return c;
        }
        break;
    }

    case 2: {   // SHIFT | ZXCVBNM | DEL  (or symbols)
        if (tx < SHIFT_W) {
            g_shift = !g_shift;
            return '\x01';        // caller redraws keyboard
        }
        if (tx >= DEL3_X) return '\b';
        int col = (tx - R3_X0) / KEY_W;
        if (col >= 0 && col < 7) {
            if (g_num) return ROW_SYM3[col];
            char c = g_shift ? ROW3[col] : (char)(ROW3[col] + 32);
            g_shift = false;
            return c;
        }
        break;
    }

    case 3: {   // NUM | SPACE | CLR | GO
        if (tx < NUM_W) {
            g_num = !g_num;
            return '\x02';        // caller redraws keyboard
        }
        if (tx < CLR_X) return ' ';
        if (tx < DONE_X) return '\x03';  // CLR
        return '\r';
    }
    }
    return 0;
}
