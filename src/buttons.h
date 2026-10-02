// Onboard Button A / Button B driver. Pin numbers confirmed against two
// independent sources -- the micro:bit v2's own published schematic
// (tech.microbit.org/hardware/schematic: "P0.14 | BTN_A", "P0.23 | BTN_B")
// and Lancaster University's codal-microbit-v2 (model/MicroBitIO.h:
// MICROBIT_PIN_BUTTON_A/B) -- same standard this project has held every
// other piece of hardware to, rather than guessing from memory. Both pins
// also double as edge-connector pins P5/P11, but we read them via the
// board's own onboard footprint, same wiring either way.
#pragma once
#include <stdint.h>
#include "joystick.h" // GPIO_REG/P0_BASE/GPIO_PIN_CNF/PIN_CNF_PULLUP/P0_IN already defined here

#define PIN_BTN_A 14U // P0.14
#define PIN_BTN_B 23U // P0.23

static inline void buttons_ab_init(void) {
    GPIO_PIN_CNF(P0_BASE, PIN_BTN_A) = PIN_CNF_PULLUP;
    GPIO_PIN_CNF(P0_BASE, PIN_BTN_B) = PIN_CNF_PULLUP;
}

// Active low, pull-up, same convention as every other button in this
// project (pressed reads 0).
static inline int button_a(void) { return ((P0_IN >> PIN_BTN_A) & 1U) == 0U; }
static inline int button_b(void) { return ((P0_IN >> PIN_BTN_B) & 1U) == 0U; }
