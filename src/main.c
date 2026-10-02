// Barebones smoke test: no SDK, no CMSIS -- direct register pokes to
// prove our own toolchain, linker script, and startup code produce a
// binary that actually boots on real silicon. It blinks the top-left LED
// (row 1, col 1) of the 5x5 matrix.
//
// Register layout (OUT/OUTSET/OUTCLR/DIR/DIRSET/DIRCLR/PIN_CNF offsets) is
// the standard nRF52-series GPIO peripheral, unchanged since the nRF51 --
// confirmed against the nRF52833 Product Specification's peripheral base
// addresses. Pin assignments for the LED matrix come straight from
// Lancaster University's own codal-microbit-v2 target
// (model/MicroBitIO.h): the same header that drives the "official"
// firmware, so we're wired exactly like MicroPython/MakeCode expect.
#include <stdint.h>

#define GPIO_REG(base, offset) (*(volatile uint32_t *)((base) + (offset)))

#define P0_BASE 0x50000000UL
#define P1_BASE 0x50000300UL

#define OUTSET_OFF 0x508UL
#define OUTCLR_OFF 0x50CUL
#define DIRSET_OFF 0x518UL

#define P0_OUTSET GPIO_REG(P0_BASE, OUTSET_OFF)
#define P0_OUTCLR GPIO_REG(P0_BASE, OUTCLR_OFF)
#define P0_DIRSET GPIO_REG(P0_BASE, DIRSET_OFF)

// LED matrix pins, from codal-microbit-v2/model/MicroBitIO.h.
// All five rows and the column we use are on P0; unused columns are left
// untouched (default input, high-impedance, so they can't source/sink
// current and ghost-light anything).
#define PIN_ROW1 21U  // P0.21 -- the row we drive
#define PIN_ROW2 22U  // P0.22
#define PIN_ROW3 15U  // P0.15
#define PIN_ROW4 24U  // P0.24
#define PIN_ROW5 19U  // P0.19
#define PIN_COL1 28U  // P0.28 -- the column we sink, lighting LED(1,1)

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

int main(void) {
    uint32_t rows_mask = (1U << PIN_ROW1) | (1U << PIN_ROW2) | (1U << PIN_ROW3) |
                          (1U << PIN_ROW4) | (1U << PIN_ROW5);
    uint32_t col1_mask = (1U << PIN_COL1);

    // All five rows and column 1 as push-pull outputs.
    P0_DIRSET = rows_mask | col1_mask;

    // Column is the sink: hold it low for the whole test so any row we
    // drive high lights its LED in column 1.
    P0_OUTCLR = col1_mask;

    // Rows 2-5 stay low (off) the whole time; only row 1 toggles.
    P0_OUTCLR = (1U << PIN_ROW2) | (1U << PIN_ROW3) | (1U << PIN_ROW4) | (1U << PIN_ROW5);

    while (1) {
        P0_OUTSET = (1U << PIN_ROW1);   // LED(1,1) on
        delay(600000);
        P0_OUTCLR = (1U << PIN_ROW1);   // LED(1,1) off
        delay(600000);
    }

    return 0;
}
