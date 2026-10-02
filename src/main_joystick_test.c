// Checkpoint 5: bring up the joystick:bit's four digital buttons (C/D/E/F).
// The analog rocker is a separate, later checkpoint (needs the SAADC
// peripheral, untouched so far). Reuses the now-working OLED from
// checkpoint 4 as the test's actual output: one filled square per button in
// each screen quadrant (C=top-left, D=top-right, E=bottom-left,
// F=bottom-right), filled while held and cleared while released -- a
// continuous, immediate visual readout rather than something that needs a
// serial capture to check.
#include <stdint.h>
#include "clock.h"
#include "dwt.h"
#include "oled.h"
#include "joystick.h"

#define P0_DIRSET_LED GPIO_REG(P0_BASE, 0x518UL)
#define P0_OUTSET_LED GPIO_REG(P0_BASE, 0x508UL)
#define P0_OUTCLR_LED GPIO_REG(P0_BASE, 0x50CUL)
#define PIN_ROW1 21U
#define PIN_COL1 28U

#define CPU_HZ 64000000UL

static void delay_ms(uint32_t ms) {
    uint32_t start = cycle_counter_read();
    uint32_t target_cycles = (uint32_t)(((uint64_t)ms * CPU_HZ) / 1000);
    while ((uint32_t)(cycle_counter_read() - start) < target_cycles) {
    }
}

static void fill_rect(int x0, int y0, int w, int h, int on) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            oled_set_pixel(x, y, on);
        }
    }
}

int main(void) {
    P0_DIRSET_LED = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR_LED = (1U << PIN_COL1);

    clock_start_hfxo();
    cycle_counter_init();
    oled_init();
    joystick_init();

    while (1) {
        int c = joystick_button_c();
        int d = joystick_button_d();
        int e = joystick_button_e();
        int f = joystick_button_f();

        // LED on if ANY button is held -- a signal independent of the OLED,
        // same reasoning as every earlier checkpoint's "LED means alive".
        if (c || d || e || f) {
            P0_OUTSET_LED = (1U << PIN_ROW1);
        } else {
            P0_OUTCLR_LED = (1U << PIN_ROW1);
        }

        oled_clear();
        fill_rect(4, 4, 50, 24, c);          // C: top-left
        fill_rect(74, 4, 50, 24, d);          // D: top-right
        fill_rect(4, 36, 50, 24, e);          // E: bottom-left
        fill_rect(74, 36, 50, 24, f);         // F: bottom-right
        oled_flush();

        delay_ms(30); // just paces the I2C refresh rate, not for correctness
    }

    return 0;
}
