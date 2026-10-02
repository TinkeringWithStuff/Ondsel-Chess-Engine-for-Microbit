// Minimal driver for the Kitronik :VIEW 128x64 OLED (SSD1306-compatible
// controller, I2C address 0x3C). The init command sequence and framebuffer
// layout below are copied verbatim from Kitronik's own open-source
// MakeCode driver (KitronikLtd/pxt-kitronik-128x64Display, main.ts) --
// this is proven-working hardware initialization for this exact physical
// display, not a generic SSD1306 sequence assembled from a datasheet, so
// it's deliberately NOT "cleaned up" or reordered.
#pragma once
#include <stdint.h>
#include "i2c.h"

#define OLED_I2C_ADDR 0x3CU // = 60 decimal, matches Kitronik's DISPLAY_ADDR_1

#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_PAGES  (OLED_HEIGHT / 8)

// Byte 0 is the fixed 0x40 "data stream follows" control byte the SSD1306
// protocol expects; bytes 1..1024 are the actual pixel data, one bit per
// pixel, LSB = top row of each 8-pixel-tall "page" (matches Kitronik's own
// screenBuf[ind] = ... | (1 << shift_page) with shift_page = y % 8).
static uint8_t oled_framebuf[1 + OLED_WIDTH * OLED_PAGES];

static inline void oled_cmd1(uint8_t c) {
    uint8_t buf[2] = {0x00, c};
    i2c_write(OLED_I2C_ADDR, buf, 2);
}

static inline void oled_cmd2(uint8_t c, uint8_t a) {
    uint8_t buf[3] = {0x00, c, a};
    i2c_write(OLED_I2C_ADDR, buf, 3);
}

static inline void oled_cmd3(uint8_t c, uint8_t a, uint8_t b) {
    uint8_t buf[4] = {0x00, c, a, b};
    i2c_write(OLED_I2C_ADDR, buf, 4);
}

static inline void oled_init(void) {
    i2c_init();

    oled_cmd1(0xAE);            // Display OFF (safe to reconfigure while off)
    oled_cmd1(0xA4);             // Display all resume (show RAM contents, not "all on")
    oled_cmd2(0xD5, 0xF0);        // Clock divider / oscillator frequency
    oled_cmd2(0xA8, 0x3F);        // Multiplex ratio: 64 rows (0x3F = 63, i.e. 64 MUX)
    oled_cmd2(0xD3, 0x00);        // Display offset: none
    oled_cmd1(0x00);             // Display start line (Kitronik sends this as a
                                   // plain single command byte, not OR'd with 0x40
                                   // as some SSD1306 examples do -- kept exactly
                                   // as their proven-working driver has it)
    oled_cmd2(0x8D, 0x14);        // Charge pump: enable
    oled_cmd2(0x20, 0x00);        // Memory addressing mode: horizontal
    oled_cmd3(0x21, 0, 127);      // Column address range: 0-127
    oled_cmd3(0x22, 0, 63);       // Page address range: 0-7 (0-63 in the API, /8 internally)
    oled_cmd1(0xA1);              // Segment remap (mirrors horizontally)
    oled_cmd1(0xC8);              // COM scan direction (mirrors vertically)
    oled_cmd2(0xDA, 0x12);        // COM pins hardware configuration
    oled_cmd2(0x81, 0xCF);        // Contrast
    oled_cmd2(0xD9, 0xF1);        // Pre-charge period
    oled_cmd2(0xDB, 0x40);        // VCOMH deselect level
    oled_cmd1(0xA6);              // Normal display (not inverted)
    oled_cmd2(0xD6, 0x00);        // Zoom: off
    oled_cmd1(0xAF);              // Display ON

    oled_framebuf[0] = 0x40; // fixed data-stream control byte, set once
}

static inline void oled_clear(void) {
    for (uint32_t i = 1; i < sizeof(oled_framebuf); i++) {
        oled_framebuf[i] = 0x00;
    }
}

static inline void oled_fill(void) {
    for (uint32_t i = 1; i < sizeof(oled_framebuf); i++) {
        oled_framebuf[i] = 0xFF;
    }
}

// Pushes the whole framebuffer over I2C in one transaction. The column/page
// address range set at init (0-127 / 0-7) plus horizontal addressing mode
// means the controller auto-wraps through the entire display as bytes
// arrive, so a single 1025-byte write redraws the full screen with no
// separate "set cursor position" step needed for this simple whole-screen
// case.
static inline void oled_flush(void) {
    i2c_write(OLED_I2C_ADDR, oled_framebuf, sizeof(oled_framebuf));
}

static inline void oled_set_pixel(int x, int y, int on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    int page = y >> 3;
    int shift = y & 7;
    uint32_t idx = 1u + (uint32_t)x + (uint32_t)page * OLED_WIDTH;
    if (on) {
        oled_framebuf[idx] |= (uint8_t)(1u << shift);
    } else {
        oled_framebuf[idx] &= (uint8_t)~(1u << shift);
    }
}
