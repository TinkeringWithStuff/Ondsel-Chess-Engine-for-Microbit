// Checkpoint 4: bring up I2C + the Kitronik :VIEW 128x64 OLED. Deliberately
// the simplest possible test -- full-screen fill, full-screen clear, then a
// diagonal line -- before attempting any text rendering. Fill/clear alone
// prove the I2C link and init sequence work at all; the diagonal line
// additionally proves oled_set_pixel()'s x/y-to-buffer-bit mapping is
// oriented the way we think it is (a bug there could still pass a plain
// fill/clear test, since every pixel looks the same either way).
//
// The LED keeps blinking throughout, independent of whatever the OLED is
// doing -- if I2C hangs (e.g. no ACK from a miswired display), the CPU
// would otherwise sit frozen in i2c_write()'s poll loop with no visible
// sign of life at all.
#include <stdint.h>
#include "clock.h"
#include "dwt.h"
#include "oled.h"

#define P0_DIRSET GPIO_REG(P0_BASE, 0x518UL)
#define P0_OUTSET GPIO_REG(P0_BASE, 0x508UL)
#define P0_OUTCLR GPIO_REG(P0_BASE, 0x50CUL)
#define PIN_ROW1 21U
#define PIN_COL1 28U

// nRF52833 core clock once HFCLK is sourced from the external crystal.
#define CPU_HZ 64000000UL

// A plain NOP-loop delay needs its iteration count calibrated against how
// many cycles the compiled loop body actually takes -- guessing that (as
// the first version of this file did, assuming roughly enough cycles for
// 3000000 iterations to take ~3s) is exactly the kind of unverified
// assumption this project has otherwise avoided by computing things from
// known clock rates. We already have the DWT cycle counter working from
// the perft checkpoint, so use it directly instead of guessing again: wait
// for an exact number of CPU cycles, computed from a real millisecond
// figure and the known 64MHz clock, with no iteration-overhead guesswork.
static void delay_ms(uint32_t ms) {
    uint32_t start = cycle_counter_read();
    uint32_t target_cycles = (uint32_t)(((uint64_t)ms * CPU_HZ) / 1000);
    while ((uint32_t)(cycle_counter_read() - start) < target_cycles) {
    }
}

int main(void) {
    P0_DIRSET = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR = (1U << PIN_COL1);

    clock_start_hfxo();
    cycle_counter_init();
    oled_init();

    while (1) {
        P0_OUTSET = (1U << PIN_ROW1);
        oled_fill();
        oled_flush();
        delay_ms(3000);

        P0_OUTCLR = (1U << PIN_ROW1);
        oled_clear();
        oled_flush();
        delay_ms(3000);

        P0_OUTSET = (1U << PIN_ROW1);
        oled_clear();
        for (int i = 0; i < OLED_WIDTH && i < OLED_HEIGHT * 2; i++) {
            oled_set_pixel(i, i / 2, 1);
        }
        oled_flush();
        delay_ms(3000);
    }

    return 0;
}
