// Diagnostic for checkpoint 6: X and Y were tracking each other exactly
// (515/515 at rest, 1023/1023 pushed one way, 0/0 pushed the other) and
// neither responded at all to left/right. That could mean either (a) the
// left/right potentiometer on this joystick is physically not producing a
// changing voltage, or (b) our firmware has a channel-switching artifact
// where sampling AIN2 right after AIN1 picks up a "ghost" of AIN1's value
// instead of AIN2's real one.
//
// This program tells the two apart by never interleaving the channels at
// all: hold button C to sample ONLY AIN1 (edge P1) in a tight loop, or hold
// button D to sample ONLY AIN2 (edge P2) in a tight loop -- reported over
// UART, clearly labeled by which physical channel it is (not "x"/"y", to
// avoid re-introducing the same ambiguity). If AIN2-alone still doesn't
// respond to left/right, that points at the hardware; if it does respond
// once read in isolation, that confirms a firmware crosstalk bug in the
// interleaved version.
#include <stdint.h>
#include "clock.h"
#include "dwt.h"
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

int main(void) {
    P0_DIRSET_LED = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR_LED = (1U << PIN_COL1);

    clock_start_hfxo();
    cycle_counter_init();
    uart_init();
    joystick_init();
    saadc_init();

    while (1) {
        int c = joystick_button_c();
        int d = joystick_button_d();

        if (c && !d) {
            // AIN1 only -- LED solid on while sampling this channel.
            P0_OUTSET_LED = (1U << PIN_ROW1);
            int32_t v = saadc_read(SAADC_PSELP_AIN1);
            uart_send_result("ain1=", (uint64_t)(uint32_t)v, 3);
        } else if (d && !c) {
            // AIN2 only -- LED off while sampling this channel, so the two
            // modes are distinguishable at a glance without reading UART.
            P0_OUTCLR_LED = (1U << PIN_ROW1);
            int32_t v = saadc_read(SAADC_PSELP_AIN2);
            uart_send_result("ain2=", (uint64_t)(uint32_t)v, 3);
        } else {
            // Neither (or both) held: idle, LED blinks slowly so you can
            // tell the board is alive but not currently sampling either
            // channel.
            P0_OUTSET_LED = (1U << PIN_ROW1);
            delay_ms(150);
            P0_OUTCLR_LED = (1U << PIN_ROW1);
            delay_ms(150);
        }

        delay_ms(100); // paces the UART output to a readable rate
    }

    return 0;
}
