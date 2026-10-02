// SysTick-based long-duration cycle counter. Register offsets are the
// standard Cortex-M SysTick block, at its fixed core-peripheral address
// (same on every Cortex-M, not chip-specific) -- not something that needs
// Nordic-specific verification the way the nRF52 peripherals do.
//
// Why this exists instead of just using dwt.h's DWT_CYCCNT: DWT_CYCCNT is
// only 32 bits, wrapping roughly every 67s at our 64MHz core clock. That's
// fine for perft (checkpoint 3, depth kept shallow specifically to stay
// under that window) but not for a real search at depth 6+, where a single
// position can legitimately take minutes. SysTick can't dodge the same
// 32-bit-register problem on its own (its COUNTER is only 24 bits, wrapping
// far *more* often, every ~262ms at 64MHz with the max reload) -- but unlike
// DWT_CYCCNT, SysTick can raise an interrupt on every wrap, so we can just
// count the wraps in software and never lose track of how many happened,
// no matter how long the run takes (a 32-bit wrap counter alone would take
// over a year to overflow at this rate).
#pragma once
#include <stdint.h>

#define SYST_CSR (*(volatile uint32_t *)0xE000E010UL)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018UL)

// Max 24-bit reload -- the longest possible period between interrupts
// (~262ms at 64MHz), so we spend as little time as possible in the ISR
// relative to actual work.
#define SYSTICK_RELOAD 0x00FFFFFFUL

#define SYST_CSR_ENABLE    (1UL << 0)
#define SYST_CSR_TICKINT   (1UL << 1)
#define SYST_CSR_CLKSOURCE (1UL << 2) // 1 = processor clock (our 64MHz), not the ~1MHz reference

// The actual wrap counter. Defined (not just declared) here as a plain
// static, since this header is only ever included from one .c file at a
// time in this project; the .c file that includes it must also provide the
// matching SysTick_Handler (see below) -- systick_init() alone does nothing
// useful without it.
static volatile uint32_t systick_overflow_count = 0;

static inline void systick_init(void) {
    SYST_RVR = SYSTICK_RELOAD;
    SYST_CVR = 0; // any write clears the current value and the pending flag
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

// SYST_CVR counts DOWN from SYSTICK_RELOAD to 0, then reloads and fires the
// interrupt on that 0->RELOAD transition (systick_overflow_count++). Given
// two (overflow_count, current_value) snapshots taken before and after a
// run, the elapsed cycle count is (wraps_between)*(period) + (val0 - val1)
// -- the "+ (val0 - val1)" term accounts for the partial period at each
// end, same derivation as any down-counting reload timer.
static inline uint64_t systick_elapsed_cycles(uint32_t overflow0, uint32_t val0,
                                               uint32_t overflow1, uint32_t val1) {
    uint64_t period = (uint64_t)SYSTICK_RELOAD + 1ULL;
    return (uint64_t)(overflow1 - overflow0) * period + (uint64_t)val0 - (uint64_t)val1;
}
