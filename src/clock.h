// Starts the external 32 MHz crystal (HFXO) as the high-frequency clock
// source. At power-on the chip runs off its internal RC oscillator
// (HFINT) instead, which is loose enough (a few % typically) to cause
// intermittent UART framing errors at 115200 baud -- exactly the sporadic
// dropped/garbled characters seen in testing ("tic 49", a stray bare \r
// overwriting the next line). The radio needs HFXO for BLE regardless, so
// the crystal is definitely populated on the micro:bit v2 board; we just
// never asked the chip to switch to it.
#pragma once
#include <stdint.h>

#define CLOCK_BASE 0x40000000UL
#define CLOCK_REG(off) (*(volatile uint32_t *)(CLOCK_BASE + (off)))

#define CLOCK_TASKS_HFCLKSTART   CLOCK_REG(0x000)
#define CLOCK_EVENTS_HFCLKSTARTED CLOCK_REG(0x100)

static inline void clock_start_hfxo(void) {
    CLOCK_EVENTS_HFCLKSTARTED = 0;
    CLOCK_TASKS_HFCLKSTART = 1;
    while (CLOCK_EVENTS_HFCLKSTARTED == 0) {}
}
