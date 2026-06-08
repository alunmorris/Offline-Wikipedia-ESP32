// Written by Alun Morris and Claude Code
#include "touch.h"
#include "display.h"
#include <Preferences.h>

// XPT2046 on its own dedicated GPIO lines — bit-banged SPI.
// The CYD (ESP32-2432S028R) physically wires XPT2046 to pins 25/39/32/33/36,
// which are DIFFERENT from the ILI9341 HSPI pins (13/12/14/15) and the SD VSPI
// pins (18/19/23/5). ESP32 only has two user SPI peripherals, so bit-bang is the
// only clean solution that doesn't corrupt the TFT or SD buses.
#define TOUCH_SCLK    25
#define TOUCH_MISO    39   // input-only ADC pin — no pullup needed (XPT2046 drives it)
#define TOUCH_MOSI    32
#define TOUCH_CS_PIN  33
#define TOUCH_IRQ_PIN 36   // input-only ADC pin — XPT2046 drives PENIRQ active-LOW

// XPT2046 channel commands: start=1, 12-bit, differential, power-down.
// On CYD (ESP32-2432S028R) landscape the electrode wiring is transposed:
//   0x90 (A=001, "Y electrode") tracks the display's LEFT-RIGHT axis → screen X
//   0xD0 (A=101, "X electrode") tracks the display's TOP-BOTTOM axis → screen Y
#define XPT_CMD_X  0x90   // reads display X (left→right)
#define XPT_CMD_Y  0xD0   // reads display Y (top→bottom, may be inverted — cal handles it)

// Calibration: raw ADC → screen pixel mapping
static int g_xmin = 200, g_xmax = 3900;
static int g_ymin = 200, g_ymax = 3900;

// ---- NVS persistence ---------------------------------------------------------

static void loadCal() {
    Preferences p;
    p.begin("wikiTch2", true);
    if (p.getBool("valid", false)) {
        g_xmin = p.getInt("xmin", g_xmin);
        g_xmax = p.getInt("xmax", g_xmax);
        g_ymin = p.getInt("ymin", g_ymin);
        g_ymax = p.getInt("ymax", g_ymax);
    }
    p.end();
}

static void saveCal() {
    Preferences p;
    p.begin("wikiTch2", false);
    p.putInt("xmin", g_xmin); p.putInt("xmax", g_xmax);
    p.putInt("ymin", g_ymin); p.putInt("ymax", g_ymax);
    p.putBool("valid", true);
    p.end();
}

// ---- Bit-bang SPI for XPT2046 ------------------------------------------------
// SPI Mode 0: CPOL=0 CPHA=0 — data driven on falling edge, sampled on rising edge.

static void tp_write_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(TOUCH_MOSI, (b >> i) & 1);
        digitalWrite(TOUCH_SCLK, HIGH);
        digitalWrite(TOUCH_SCLK, LOW);
    }
}

// Send command, return 12-bit ADC result (16 clocks, result in bits [14:3]).
static uint16_t tp_read_channel(uint8_t cmd) {
    digitalWrite(TOUCH_CS_PIN, LOW);
    tp_write_byte(cmd);
    uint16_t val = 0;
    for (int i = 15; i >= 0; i--) {
        digitalWrite(TOUCH_SCLK, HIGH);
        val |= (uint16_t)(digitalRead(TOUCH_MISO)) << i;
        digitalWrite(TOUCH_SCLK, LOW);
    }
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return (val >> 3) & 0x0FFF;
}

static bool tp_touched() {
    return digitalRead(TOUCH_IRQ_PIN) == LOW;
}

// Average 4 samples for noise rejection.
static void tp_get_raw(int16_t &rx, int16_t &ry) {
    long sx = 0, sy = 0;
    for (int i = 0; i < 4; i++) {
        sx += tp_read_channel(XPT_CMD_X);
        sy += tp_read_channel(XPT_CMD_Y);
    }
    rx = (int16_t)(sx / 4);
    ry = (int16_t)(sy / 4);
}

// ---- Calibration -------------------------------------------------------------

static void drawCrosshair(int x, int y) {
    tft.drawLine(x - 12, y,      x + 12, y,      TFT_WHITE);
    tft.drawLine(x,      y - 12, x,      y + 12, TFT_WHITE);
    tft.drawCircle(x, y, 5, TFT_WHITE);
}

static void runCalibration() {
    const int T1X = 20,  T1Y = 20;
    const int T2X = 299, T2Y = 219;

    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Touch calibration", SCREEN_W / 2, 60, 2);

    while (tp_touched()) delay(5);
    delay(200);

    // Point 1: top-left
    drawCrosshair(T1X, T1Y);
    tft.drawString("Tap the crosshair", SCREEN_W / 2, SCREEN_H / 2, 2);
    while (!tp_touched()) delay(5);
    long sumX = 0, sumY = 0; int n = 0;
    while (tp_touched()) {
        int16_t rx, ry; tp_get_raw(rx, ry);
        sumX += rx; sumY += ry; n++; delay(10);
    }
    if (n < 1) n = 1;
    int rx1 = sumX / n, ry1 = sumY / n;
    Serial.printf("[touch cal] P1 raw: %d, %d\n", rx1, ry1);

    // Point 2: bottom-right
    tft.fillScreen(COL_BG);
    drawCrosshair(T2X, T2Y);
    tft.drawString("Tap the crosshair", SCREEN_W / 2, SCREEN_H / 2, 2);
    delay(300);
    while (!tp_touched()) delay(5);
    sumX = 0; sumY = 0; n = 0;
    while (tp_touched()) {
        int16_t rx, ry; tp_get_raw(rx, ry);
        sumX += rx; sumY += ry; n++; delay(10);
    }
    if (n < 1) n = 1;
    int rx2 = sumX / n, ry2 = sumY / n;
    Serial.printf("[touch cal] P2 raw: %d, %d\n", rx2, ry2);

    // Extrapolate to full 320×240 range
    long dRx = rx2 - rx1, dTx = T2X - T1X;
    long dRy = ry2 - ry1, dTy = T2Y - T1Y;
    g_xmin = (int)(rx1 - dRx * T1X / dTx);
    g_xmax = (int)(rx2 + dRx * (319 - T2X) / dTx);
    g_ymin = (int)(ry1 - dRy * T1Y / dTy);
    g_ymax = (int)(ry2 + dRy * (239 - T2Y) / dTy);
    Serial.printf("[touch cal] xmin=%d xmax=%d ymin=%d ymax=%d\n",
                  g_xmin, g_xmax, g_ymin, g_ymax);
    saveCal();

    tft.fillScreen(COL_BG);
    tft.setTextColor(TFT_GREEN, COL_BG);
    tft.drawString("Calibration saved!", SCREEN_W / 2, SCREEN_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    delay(1200);
}

// ---- Public init -------------------------------------------------------------

void touchInit() {
    loadCal();

    pinMode(TOUCH_SCLK,    OUTPUT); digitalWrite(TOUCH_SCLK,    LOW);
    pinMode(TOUCH_MOSI,    OUTPUT); digitalWrite(TOUCH_MOSI,    LOW);
    pinMode(TOUCH_MISO,    INPUT);
    pinMode(TOUCH_CS_PIN,  OUTPUT); digitalWrite(TOUCH_CS_PIN,  HIGH);
    pinMode(TOUCH_IRQ_PIN, INPUT);

    delay(10);
    Serial.println("[touch] bit-bang init done");

    Preferences p;
    p.begin("wikiTch2", true);
    bool valid = p.getBool("valid", false);
    p.end();
    if (!valid) runCalibration();
}

// ---- Coordinate mapping ------------------------------------------------------

static void mapTouch(int16_t rx, int16_t ry, int16_t &sx, int16_t &sy) {
    sx = (int16_t)map(rx, g_xmin, g_xmax, 0, 319);
    sy = (int16_t)map(ry, g_ymin, g_ymax, 0, 239);
    sx = constrain(sx, 0, 319);
    sy = constrain(sy, 0, 239);
}

// ---- Gesture detection -------------------------------------------------------

static bool     g_pressed      = false;
static int16_t  g_start_x, g_start_y;
static int16_t  g_last_x,  g_last_y;
static uint32_t g_press_ms     = 0;
static int32_t  g_total_dy;
static uint32_t g_hold_next_ms = 0;   // time of next HOLD repeat; 0 = not started
static bool     g_hold_fired   = false;
#define TOUCH_DEBOUNCE_MS  60
#define HOLD_INITIAL_MS   500   // delay before first repeat
#define HOLD_REPEAT_MS    100   // interval between repeats

TouchEvent touchProcess() {
    TouchEvent evt = { TouchEvent::NONE, 0, 0, 0 };

    bool pressed = tp_touched();

    if (pressed && !g_pressed) {
        if (g_press_ms != 0 && millis() - g_press_ms < TOUCH_DEBOUNCE_MS) return evt;

        int16_t rx, ry, sx, sy;
        tp_get_raw(rx, ry);
        mapTouch(rx, ry, sx, sy);

        g_start_x = g_last_x = sx;
        g_start_y = g_last_y = sy;
        g_press_ms     = millis();
        g_hold_next_ms = 0;
        g_hold_fired   = false;
        g_total_dy     = 0;
        g_pressed      = true;
        Serial.printf("[touch] press (raw %d,%d) -> screen (%d,%d)\n", rx, ry, sx, sy);

    } else if (pressed && g_pressed) {
        int16_t rx, ry, sx, sy;
        tp_get_raw(rx, ry);
        mapTouch(rx, ry, sx, sy);
        g_total_dy += sy - g_last_y;
        g_last_x = sx;
        g_last_y = sy;

        // Emit HOLD repeat events while finger is stationary
        uint32_t now = millis();
        if (abs(g_total_dy) < 15) {   // not a swipe
            if (g_hold_next_ms == 0 && now - g_press_ms >= HOLD_INITIAL_MS) {
                g_hold_next_ms = now + HOLD_REPEAT_MS;
                g_hold_fired   = true;
                evt.type = TouchEvent::HOLD;
                evt.x    = g_start_x;
                evt.y    = g_start_y;
            } else if (g_hold_next_ms != 0 && now >= g_hold_next_ms) {
                g_hold_next_ms = now + HOLD_REPEAT_MS;
                evt.type = TouchEvent::HOLD;
                evt.x    = g_start_x;
                evt.y    = g_start_y;
            }
        }

    } else if (!pressed && g_pressed) {
        g_pressed = false;
        uint32_t dur = millis() - g_press_ms;
        int16_t dx   = g_last_x - g_start_x;
        int16_t dy   = g_last_y - g_start_y;
        int16_t dist = (int16_t)sqrtf((float)(dx*dx + dy*dy));

        Serial.printf("[touch] release start=(%d,%d) end=(%d,%d) dist=%d dur=%ums tdy=%d\n",
                      g_start_x, g_start_y, g_last_x, g_last_y, dist, dur, (int)g_total_dy);

        // Suppress TAP on release if a HOLD was already fired
        if (g_hold_fired) {
            Serial.printf("[touch] hold-release, suppressing TAP\n");
        } else if (abs(g_total_dy) >= 25) {
            evt.type = (g_total_dy < 0) ? TouchEvent::SWIPE_UP : TouchEvent::SWIPE_DOWN;
            evt.dy   = (int16_t)g_total_dy;
            Serial.printf("[touch] SWIPE dy=%d\n", evt.dy);
        } else if (dist < 40 && dur < 500) {
            evt.type = TouchEvent::TAP;
            evt.x    = g_start_x;
            evt.y    = g_start_y;
            Serial.printf("[touch] TAP (%d,%d)\n", evt.x, evt.y);
        } else {
            Serial.printf("[touch] no event (dist=%d dur=%ums tdy=%d)\n", dist, dur, (int)g_total_dy);
        }
    }
    return evt;
}
