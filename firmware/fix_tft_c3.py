Import("env")
import os

# arduino-esp32 v3 / IDF 5 defines REG_SPI_BASE(i) returning 0 when i != 2, but
# TFT_eSPI passes SPI2_HOST (=1) → _spi_user ends up at address 0x10 → Store Access Fault.
# TFT_eSPI's own #ifndef guard never fires because IDF 5 headers already defined the macro.
# Patch: replace #ifndef with #undef so the correct (ignore-i) definition always wins.

header = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "TFT_eSPI", "Processors", "TFT_eSPI_ESP32_C3.h"
)

if not os.path.isfile(header):
    print("[fix_tft_c3] Header not found:", header)
else:
    with open(header) as f:
        txt = f.read()

    old = (
        "  // Fix ESP32C3 IDF bug for missing definition (VSPI/FSPI only tested at the moment)\n"
        "  #ifndef REG_SPI_BASE\n"
        "    #define REG_SPI_BASE(i) DR_REG_SPI2_BASE\n"
        "  #endif"
    )
    new = (
        "  // Patched: IDF 5 defines REG_SPI_BASE with (i)==2 check that returns 0 for\n"
        "  // SPI2_HOST=1. Force the correct definition by undefining first.\n"
        "  #undef REG_SPI_BASE\n"
        "  #define REG_SPI_BASE(i) DR_REG_SPI2_BASE"
    )

    if new in txt:
        print("[fix_tft_c3] Already patched")
    elif old in txt:
        with open(header, "w") as f:
            f.write(txt.replace(old, new))
        print("[fix_tft_c3] Patched TFT_eSPI_ESP32_C3.h for arduino-esp32 v3 / IDF 5")
    else:
        print("[fix_tft_c3] WARNING: Expected text not found — patch skipped (library may have changed)")
        print("[fix_tft_c3] Header:", header)
