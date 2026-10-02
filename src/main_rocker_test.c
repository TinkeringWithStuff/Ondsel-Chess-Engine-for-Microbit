// Checkpoint 6: bring up the joystick:bit's analog rocker (X/Y) via the
// SAADC peripheral. Buttons (checkpoint 5) are already confirmed working;
// this reuses the same OLED-as-readout approach: a small filled square that
// moves around the screen, mapped directly from the two raw ADC readings,
// so the rocker's range/centering/orientation can all be checked just by
// looking at the display and moving the stick -- no serial capture needed.
//
// Also calls joystick_init() (checkpoint 5's driver) even though this test
// only cares about the analog rocker: ELECFREAKS' own driver runs the same
// unexplained init step (driving P0 low, forcing the motor pin high) before
// *any* use of the board, buttons or rocker alike. The first version of this
// test skipped it on the assumption it only mattered for the buttons, which
// was exactly the kind of guess this project has otherwise avoided -- and
// the actual hardware test (rocker not affecting the reading at all, stuck
// dead in one corner) matches what you'd expect if that init step is what
// actually powers up the board's analog circuitry.
// Also reports the raw ADC readings over UART (same checksummed
// repeat-and-vote pattern as the perft checkpoint) so we can see the actual
// numbers ground-truth, rather than only inferring them from where the
// cursor lands on the OLED.
#include <stdint.h>
#include "clock.h"
#include "dwt.h"
#include "oled.h"
#include "joystick.h"
#include "saadc.h"
#include "uart.h"

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

// Cursor square side length, in pixels.
#define CURSOR_SIZE 8

int main(void) {
    P0_DIRSET_LED = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR_LED = (1U << PIN_COL1);

    clock_start_hfxo();
    cycle_counter_init();
    uart_init();
    oled_init();
    joystick_init();
    saadc_init();

    int led_on = 0;
    uint32_t loop_count = 0;

    while (1) {
        // Raw 10-bit readings, roughly 0..1023 across the rocker's full
        // travel (exact endpoints/center depend on the hardware -- that is
        // exactly what this test is for finding out).
        int32_t raw_x = saadc_read(SAADC_PSELP_AIN1);
        int32_t raw_y = saadc_read(SAADC_PSELP_AIN2);

        // Map 0..1023 onto the usable screen range for the cursor's
        // top-left corner (0 .. size-CURSOR_SIZE), clamped defensively in
        // case the real range overshoots slightly past 0/1023.
        int32_t cx = (raw_x * (OLED_WIDTH - CURSOR_SIZE)) / 1023;
        int32_t cy = (raw_y * (OLED_HEIGHT - CURSOR_SIZE)) / 1023;
        if (cx < 0) cx = 0;
        if (cx > OLED_WIDTH - CURSOR_SIZE) cx = OLED_WIDTH - CURSOR_SIZE;
        if (cy < 0) cy = 0;
        if (cy > OLED_HEIGHT - CURSOR_SIZE) cy = OLED_HEIGHT - CURSOR_SIZE;

        oled_clear();
        fill_rect((int)cx, (int)cy, CURSOR_SIZE, CURSOR_SIZE, 1);
        oled_flush();

        // Every ~300ms (30ms loop * 10), report the raw readings over UART.
        // Cast through uint32_t rather than sending the int32_t directly --
        // if a reading ever comes back negative (it shouldn't for a
        // single-ended read), that cast makes it show up as a huge number
        // instead of silently looking like a small valid one.
        loop_count++;
        if (loop_count % 10 == 0) {
            uart_send_result("x=", (uint64_t)(uint32_t)raw_x, 3);
            uart_send_result("y=", (uint64_t)(uint32_t)raw_y, 3);
        }

        // LED toggles every loop -- an independent heartbeat, same purpose
        // as every earlier checkpoint's "LED means alive" signal, just
        // blinking rather than level-driven since there's no simple
        // pressed/released state here to reflect.
        led_on = !led_on;
        if (led_on) {
            P0_OUTSET_LED = (1U << PIN_ROW1);
        } else {
            P0_OUTCLR_LED = (1U << PIN_ROW1);
        }

        delay_ms(30); // paces the I2C refresh rate, not for correctness
    }

    return 0;
}
