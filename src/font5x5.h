// 5x5 bitmap font, ported verbatim (all 128 entries, extracted
// programmatically from their source rather than retyped by hand) from
// Kitronik's own open-source MakeCode OLED driver
// (KitronikLtd/pxt-kitronik-128x64Display, main.ts) -- same standard this
// project already held for the OLED's init sequence and framebuffer
// format. Indexed by ASCII code (0-127). Each entry packs a 5-column x
// 5-row glyph: bit (5*col + row) set means that pixel is on, col=0..4
// left-to-right, row=0..4 top-to-bottom (their own show() function reads
// it with "charBytes & (1 << 5*xIndex+yIndex)", quoted directly from
// their source). Control codes (0-31, 127) all reuse their placeholder
// diamond glyph in the original -- kept as-is for fidelity even though
// this project never sends control codes to the display.
#pragma once
#include <stdint.h>
#include "oled.h"

static const uint32_t FONT5X5[128] = {
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x0022d422UL,
    0x00000000UL,
    0x000002e0UL,
    0x00018060UL,
    0x00afabeaUL,
    0x00aed6eaUL,
    0x01991133UL,
    0x010556aaUL,
    0x00000060UL,
    0x000045c0UL,
    0x00003a20UL,
    0x00051140UL,
    0x00023880UL,
    0x00002200UL,
    0x00021080UL,
    0x00000100UL,
    0x00111110UL,
    0x0007462eUL,
    0x00087e40UL,
    0x000956b9UL,
    0x0005d629UL,
    0x008fa54cUL,
    0x009ad6b7UL,
    0x008ada88UL,
    0x00119531UL,
    0x00aad6aaUL,
    0x0022b6a2UL,
    0x00000140UL,
    0x00002a00UL,
    0x0008a880UL,
    0x00052940UL,
    0x00022a20UL,
    0x0022d422UL,
    0x00e4d62eUL,
    0x000f14beUL,
    0x000556bfUL,
    0x0008c62eUL,
    0x0007463fUL,
    0x0008d6bfUL,
    0x000094bfUL,
    0x00cac62eUL,
    0x000f909fUL,
    0x000047f1UL,
    0x0017c629UL,
    0x0008a89fUL,
    0x0008421fUL,
    0x01f1105fUL,
    0x01f4105fUL,
    0x0007462eUL,
    0x000114bfUL,
    0x000b6526UL,
    0x010514bfUL,
    0x0004d6b2UL,
    0x0010fc21UL,
    0x0007c20fUL,
    0x00744107UL,
    0x01f4111fUL,
    0x000d909bUL,
    0x00117041UL,
    0x0008ceb9UL,
    0x0008c7e0UL,
    0x01041041UL,
    0x000fc620UL,
    0x00010440UL,
    0x01084210UL,
    0x00000820UL,
    0x010f4a4cUL,
    0x0004529fUL,
    0x00094a4cUL,
    0x000fd288UL,
    0x000956aeUL,
    0x000097c4UL,
    0x0007d6a2UL,
    0x000c109fUL,
    0x000003a0UL,
    0x0006c200UL,
    0x0008289fUL,
    0x000841e0UL,
    0x01e1105eUL,
    0x000e085eUL,
    0x00064a4cUL,
    0x0002295eUL,
    0x000f2944UL,
    0x0001085cUL,
    0x00012a90UL,
    0x010a51e0UL,
    0x010f420eUL,
    0x00644106UL,
    0x01e8221eUL,
    0x00093192UL,
    0x00222292UL,
    0x00095b52UL,
    0x0008fc80UL,
    0x000003e0UL,
    0x000013f1UL,
    0x00841080UL,
    0x0022d422UL,
};

#define FONT_ADVANCE 6 // 5px glyph + 1px gap between characters
#define FONT_LINE_H  7 // 5px glyph + 2px gap between lines

static void draw_char(int x0, int y0, char c) {
    unsigned char uc = (unsigned char)c;
    if (uc > 127) uc = (unsigned char)'?';
    uint32_t bits = FONT5X5[uc];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 5; row++) {
            if (bits & (1UL << (5 * col + row))) {
                oled_set_pixel(x0 + col, y0 + row, 1);
            }
        }
    }
}

static void draw_text(int x0, int y0, const char *s) {
    int x = x0;
    while (*s) {
        draw_char(x, y0, *s);
        x += FONT_ADVANCE;
        s++;
    }
}
