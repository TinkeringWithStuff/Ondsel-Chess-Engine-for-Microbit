// Minimal driver for the ELECFREAKS Joystick:bit V2's four digital buttons
// (the analog rocker is a separate checkpoint -- it needs the SAADC
// peripheral, which this doesn't touch). Pin assignments and the active-low
// pull-up wiring come straight from ELECFREAKS' own open-source MakeCode
// driver (elecfreaks/pxt-joystickbit, joystickbit.ts) -- same standard this
// project has held every other piece of third-party hardware to.
#pragma once
#include <stdint.h>

#define GPIO_REG(base, off) (*(volatile uint32_t *)((base) + (off)))
#define P0_BASE 0x50000000UL
#define P1_BASE 0x50000300UL

#define P0_IN     GPIO_REG(P0_BASE, 0x510UL)
#define P0_DIRSET GPIO_REG(P0_BASE, 0x518UL)
#define P0_OUTSET GPIO_REG(P0_BASE, 0x508UL)
#define P0_OUTCLR GPIO_REG(P0_BASE, 0x50CUL)
#define P1_DIRSET GPIO_REG(P1_BASE, 0x518UL)
#define P1_OUTSET GPIO_REG(P1_BASE, 0x508UL)

#define GPIO_PIN_CNF(base, pin) GPIO_REG(base, 0x700UL + 4UL * (pin))
// PULL field is bits[3:2] of PIN_CNF; PULLUP = 3 (same enum this project
// already confirmed for the I2C pins' PULL field).
#define PIN_CNF_PULLUP (3UL << 2)

// Edge P12/P13/P14/P15 -> nRF52 P0.12/P0.17/P0.01/P0.13, confirmed against
// codal-microbit-v2's own edge-connector pin table (the same source already
// trusted for the LED/UART/I2C pins). E and F are swapped here relative to
// that table's raw P14/P15 order -- confirmed empirically on real hardware
// (Rune: pressing physical E moved the cursor down, physical F moved it
// right, i.e. exactly reversed from joystick_button_e()/_f() below), so
// either this board's silkscreen or ELECFREAKS' own pin table doesn't match
// the edge-connector table 1:1 for E/F. Swapping which physical pin each
// function reads (not touching input.h's logical left/up/right/down
// mapping) fixes it without redefining what "E" and "F" mean anywhere else.
#define PIN_BTN_C 12U // edge P12
#define PIN_BTN_D 17U // edge P13
#define PIN_BTN_E 13U // edge P15 (swapped, see above)
#define PIN_BTN_F 1U  // edge P14 (swapped, see above)

// From the joystick:bit's own init sequence: P0 (edge, = nRF52 P0.02) held
// low and P16 (nRF52 P1.02, the vibration motor) held high (off -- the
// motor is active-low, confirmed by their Vibration_Motor() driving P16
// LOW to turn it ON). Neither is explained in their driver's own comments,
// but both are cheap to replicate exactly rather than guess they're
// unnecessary.
#define PIN_UNKNOWN_P0 2U  // edge P0
#define PIN_MOTOR_P16  2U  // nRF52 P1.02 (edge P16)

static inline void joystick_init(void) {
    // Buttons: input with internal pull-up, active low (pressed reads 0).
    GPIO_PIN_CNF(P0_BASE, PIN_BTN_C) = PIN_CNF_PULLUP;
    GPIO_PIN_CNF(P0_BASE, PIN_BTN_D) = PIN_CNF_PULLUP;
    GPIO_PIN_CNF(P0_BASE, PIN_BTN_E) = PIN_CNF_PULLUP;
    GPIO_PIN_CNF(P0_BASE, PIN_BTN_F) = PIN_CNF_PULLUP;

    P0_DIRSET = (1U << PIN_UNKNOWN_P0);
    P0_OUTCLR = (1U << PIN_UNKNOWN_P0);

    P1_DIRSET = (1U << PIN_MOTOR_P16);
    P1_OUTSET = (1U << PIN_MOTOR_P16); // motor off (active low)
}

static inline int joystick_button_c(void) { return ((P0_IN >> PIN_BTN_C) & 1U) == 0U; }
static inline int joystick_button_d(void) { return ((P0_IN >> PIN_BTN_D) & 1U) == 0U; }
static inline int joystick_button_e(void) { return ((P0_IN >> PIN_BTN_E) & 1U) == 0U; }
static inline int joystick_button_f(void) { return ((P0_IN >> PIN_BTN_F) & 1U) == 0U; }
