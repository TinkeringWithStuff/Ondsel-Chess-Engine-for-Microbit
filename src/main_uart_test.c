// Checkpoint 2: prove UARTE0 registers are wired up correctly by printing
// text back over the same USB cable, readable as a normal serial port on
// the Mac. Also keeps the LED blinking as a visual "still alive" signal
// independent of whether serial actually works.
#include <stdint.h>
#include "clock.h"
#include "uart.h" // also brings in GPIO_REG, P0_BASE, P0_DIRSET

#define P0_OUTSET GPIO_REG(P0_BASE, 0x508UL)
#define P0_OUTCLR GPIO_REG(P0_BASE, 0x50CUL)
#define PIN_ROW1 21U
#define PIN_COL1 28U

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

int main(void) {
    P0_DIRSET = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR = (1U << PIN_COL1); // column sink, same as the blink test

    clock_start_hfxo(); // accurate clock before touching UARTE's baud rate
    uart_init();
    uart_puts("microbit v2 uart test: hello from bare-metal C\r\n");

    // ~10x slower than the original blink test: about 1 line/second instead
    // of ~10/second, so nothing floods the terminal's scrollback (a fast,
    // unbroken flood of short lines is exactly what tends to trip up
    // Terminal.app's live link/data-detection scanning).
    uint32_t n = 0;
    while (1) {
        P0_OUTSET = (1U << PIN_ROW1);
        delay(6000000);
        P0_OUTCLR = (1U << PIN_ROW1);
        delay(6000000);

        // Exercises the repeat-and-vote pattern we'll use for the real
        // perft nodes/sec result: 8 checksummed copies of the same value,
        // so the host only needs ONE of them to land clean.
        uart_send_result("tick ", n++, 8);
    }

    return 0;
}
