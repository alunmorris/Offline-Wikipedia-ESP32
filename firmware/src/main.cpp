// Written by Alun Morris and Claude Code
#include <Arduino.h>
#include "config.h"
#include "wiki_db.h"
#include "display.h"
#include "ui.h"

// Splash screen Y positions
#define SPLASH_TITLE_Y   80    // "Wikipedia" title
#define SPLASH_BODY_Y   130    // "Loading…" and single-line version text
#define SPLASH_VER_Y1   112    // three-line version: line 1
#define SPLASH_VER_Y2   130    // three-line version: line 2
#define SPLASH_VER_Y3   148    // three-line version: line 3
#define SPLASH_2L_Y1    120    // two-line fallback: line 1
#define SPLASH_2L_Y2    140    // two-line fallback: line 2

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Wikipedia Offline Reader ===");

    // Grab the decompression buffer FIRST, before display/SD/LittleFS fragment the heap.
    wikiDbPreInit();

    // Display must be up before wikiDbInit so we can show the boot screen.
    displayInit();
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_H1, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Wikipedia", SCREEN_W / 2, SPLASH_TITLE_Y, 4);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("Loading...", SCREEN_W / 2, SPLASH_BODY_Y, 2);
    tft.setTextDatum(TL_DATUM);

    if (wikiDbInit()) {
        Serial.printf("[main] Database ready: %u articles\n", wikiDbArticleCount());
        // Replace "Loading..." with the database name (split at ": " for two lines).
        tft.fillRect(0, 110, SCREEN_W, 50, COL_BG);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.setTextDatum(MC_DATUM);
        const String &dbName = wikiDbName();
        if (dbName.length() > 0) {
            int sep = dbName.indexOf(": ");
            if (sep >= 0) {
                String line1 = dbName.substring(0, sep);       // e.g. "Simple English"
                String rest  = dbName.substring(sep + 2);      // e.g. "All Maxi May 2026"
                // Split rest before the date (last two words = "Month YYYY")
                int lastSp   = rest.lastIndexOf(' ');
                int dateSep  = (lastSp > 0) ? rest.lastIndexOf(' ', lastSp - 1) : -1;
                if (dateSep >= 0) {
                    tft.drawString(line1,                      SCREEN_W / 2, SPLASH_VER_Y1, 2);
                    tft.drawString(rest.substring(0, dateSep), SCREEN_W / 2, SPLASH_VER_Y2, 2);
                    tft.drawString(rest.substring(dateSep + 1),SCREEN_W / 2, SPLASH_VER_Y3, 2);
                } else {
                    tft.drawString(line1, SCREEN_W / 2, SPLASH_2L_Y1, 2);
                    tft.drawString(rest,  SCREEN_W / 2, SPLASH_2L_Y2, 2);
                }
            } else {
                tft.drawString(dbName, SCREEN_W / 2, SPLASH_BODY_Y, 2);
            }
        }
        tft.setTextDatum(TL_DATUM);
        delay(3000);
    } else {
        Serial.println("[main] WARNING: SD / database init failed");
        tft.setTextColor(TFT_RED, COL_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("SD card error!", SCREEN_W / 2, SCREEN_H / 2, 2);
        tft.setTextDatum(TL_DATUM);
        delay(3000);
    }

    uiInit();   // display already up; sets up touch + draws search screen
    Serial.println("[main] Ready");
}

void loop() {
    uiLoop();
    delay(5);
}
