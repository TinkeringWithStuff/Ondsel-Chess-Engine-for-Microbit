// Minimal register-level driver for TWIM0 (nRF52's EasyDMA I2C master), used
// to talk to the Kitronik :VIEW 128x64 OLED over the micro:bit's external
// I2C bus. Every offset here was pulled from Nordic's own generated
// register definitions (nrf52832-pac's svd2rust output, which is the same
// SVD-derived source real Nordic tooling uses) rather than memorized, the
// same standard this project has held UARTE/GPIO to.
#pragma once
#include <stdint.h>

#define TWIM0_BASE 0x40003000UL
#define TREG(off) (*(volatile uint32_t *)(TWIM0_BASE + (off)))

#define TWIM_TASKS_STARTTX  TREG(0x008)
#define TWIM_TASKS_STOP     TREG(0x014)
#define TWIM_EVENTS_STOPPED TREG(0x104)
#define TWIM_EVENTS_ERROR   TREG(0x124)
#define TWIM_EVENTS_LASTTX  TREG(0x160)
#define TWIM_SHORTS         TREG(0x200)
#define TWIM_ERRORSRC       TREG(0x4C4)
#define TWIM_ENABLE         TREG(0x500)
#define TWIM_PSEL_SCL       TREG(0x508)
#define TWIM_PSEL_SDA       TREG(0x50C)
#define TWIM_FREQUENCY      TREG(0x524)
#define TWIM_TXD_PTR        TREG(0x544)
#define TWIM_TXD_MAXCNT     TREG(0x548)
#define TWIM_ADDRESS        TREG(0x588)

// TWIM's own ENABLE code is 6, NOT the 1/8 pattern other peripherals use --
// confirmed against Nordic's own enum (DISABLED=0, ENABLED=6), easy to get
// wrong by assuming a "usual" value.
#define TWIM_ENABLE_ENABLED 6UL
#define TWIM_FREQUENCY_K100 0x01980000UL
// LASTTX_STOP: automatically issue a STOP after the last byte of a TXD
// transfer, so a plain "poll EVENTS_STOPPED" after STARTTX is a complete
// single I2C write transaction with no separate TASKS_STOP call needed --
// same "let the shortcut do it" simplification as UARTE's ENDTX-only wait.
#define TWIM_SHORTS_LASTTX_STOP (1UL << 9)

// External I2C bus on the micro:bit edge connector (P19=SCL, P20=SDA) --
// what accessory boards like the :VIEW OLED plug into, as opposed to the
// separate INTERNAL I2C bus (P0.08/P0.16) wired to the onboard
// accelerometer/compass. Confirmed against codal-microbit-v2's own pin
// table, the same source already trusted for the LED matrix/UART pins.
#define PIN_I2C_SCL 26U           // P0.26 (edge P19)
#define PIN_I2C_SDA ((1U << 5) | 0U) // P1.00 (edge P20)

#define GPIO_REG(base, off) (*(volatile uint32_t *)((base) + (off)))
#define P0_BASE 0x50000000UL
#define P1_BASE 0x50000300UL
#define GPIO_PIN_CNF(base, pin) GPIO_REG(base, 0x700UL + 4UL * (pin))

// I2C lines are open-drain, wired-AND busses: a driver that could pull the
// line HIGH as well as low would fight another device (or the bus's own
// pull-up) trying to hold it low. DRIVE=S0D1 (6: "standard 0, disconnect
// 1") means "drive low normally, but let go entirely instead of driving
// high" -- the correct nRF52 GPIO config for I2C, confirmed against
// Nordic's own PIN_CNF DRIVE enum and a known nRF5 SDK issue about exactly
// this (plain S0S1 push-pull breaks multi-device I2C busses). PULL=pullup
// (3) is belt-and-suspenders alongside whatever pull-ups the OLED board
// itself provides.
#define PIN_CNF_I2C_VALUE ((3UL << 2) | (6UL << 8))

static inline void i2c_init(void) {
    GPIO_PIN_CNF(P0_BASE, 26) = PIN_CNF_I2C_VALUE; // SCL
    GPIO_PIN_CNF(P1_BASE, 0)  = PIN_CNF_I2C_VALUE; // SDA

    TWIM_PSEL_SCL = PIN_I2C_SCL;
    TWIM_PSEL_SDA = PIN_I2C_SDA;
    TWIM_FREQUENCY = TWIM_FREQUENCY_K100; // 100kbps: slow and safe to start
    TWIM_SHORTS = TWIM_SHORTS_LASTTX_STOP;
    TWIM_ENABLE = TWIM_ENABLE_ENABLED;
}

// Blocking I2C write of `len` bytes from `buf` to the 7-bit address
// `addr7`. `buf` must be in RAM (EasyDMA can't read flash -- the exact same
// lesson learned the hard way with UARTE's TXD.PTR earlier in this
// project). Returns true on success, false if the peripheral reported an
// error (e.g. no device ACKed that address).
static inline int i2c_write(uint8_t addr7, const uint8_t *buf, uint32_t len) {
    if (len == 0) {
        return 1;
    }
    TWIM_ADDRESS = addr7;
    TWIM_EVENTS_STOPPED = 0;
    TWIM_EVENTS_ERROR = 0;
    TWIM_TXD_PTR = (uint32_t)(uintptr_t)buf;
    TWIM_TXD_MAXCNT = len;
    TWIM_TASKS_STARTTX = 1;
    while (TWIM_EVENTS_STOPPED == 0 && TWIM_EVENTS_ERROR == 0) {
    }
    if (TWIM_EVENTS_ERROR) {
        // Clear the error and make sure the peripheral actually stops
        // (an error doesn't imply STOPPED fires on its own) before the
        // next transaction reuses it.
        TWIM_ERRORSRC = TWIM_ERRORSRC; // write-1-to-clear-style register
        TWIM_TASKS_STOP = 1;
        while (TWIM_EVENTS_STOPPED == 0) {
        }
        return 0;
    }
    return 1;
}
