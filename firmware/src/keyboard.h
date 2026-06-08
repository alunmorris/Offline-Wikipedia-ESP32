// Written by Alun Morris and Claude Code
#pragma once
#include <Arduino.h>
#include "display.h"

// Keyboard occupies the bottom KB_H pixels of the screen.
#define KB_TOP_Y   (SCREEN_H - KB_H)

// Draw the full 4-row keyboard starting at y = y0.
void keyboardDraw(int16_t y0 = KB_TOP_Y);

// Check whether (tap_x, tap_y) hit a key.  Returns:
//   'a'-'z' / 'A'-'Z'   — letter (uppercase when shift is active, then shift auto-clears)
//   '0'-'9'              — digit  (NUM mode top row)
//   '!','@','#','$','%','^','&','*','(',')' — NUM + SHIFT top row
//   ' '                  — space
//   '\b'                 — backspace / delete
//   '\r'                 — done / confirm
//   '\x01'               — shift key toggled (caller should redraw keyboard)
//   '\x02'               — NUM key toggled  (caller should redraw keyboard)
//   '\x03'               — CLR key (caller should clear query and redraw)
//   0                    — no hit
char keyboardHitTest(int16_t tap_x, int16_t tap_y, int16_t y0 = KB_TOP_Y);

// Flash only the key at (tx,ty) — call BEFORE keyboardHitTest so the label
// reflects the pre-tap shift state.  Highlights the key briefly then restores it.
void keyboardFlashKey(int16_t tx, int16_t ty, int16_t y0 = KB_TOP_Y);

// Query current modifier state.
bool keyboardGetShift();
bool keyboardGetNum();
