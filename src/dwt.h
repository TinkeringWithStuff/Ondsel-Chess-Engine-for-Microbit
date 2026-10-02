// Cortex-M4 cycle counter (DWT->CYCCNT). Part of the core debug hardware,
// not a peripheral -- works with no debugger attached, which is what we
// need since we're only connected via the mass-storage/serial USB path,
// not SWD. Standard on every Cortex-M3/M4/M7.
#pragma once
#include <stdint.h>

#define DEMCR    (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004UL)

#define DEMCR_TRCENA (1UL << 24)
#define DWT_CTRL_CYCCNTENA (1UL << 0)

static inline void cycle_counter_init(void) {
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

static inline uint32_t cycle_counter_read(void) {
    return DWT_CYCCNT;
}
