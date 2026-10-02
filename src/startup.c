// Minimal Cortex-M4 startup: vector table + Reset_Handler.
//
// No CMSIS, no Nordic SDK -- same "no unnecessary library machinery"
// discipline as the chess engine itself. This is the entire boot path:
// the CPU reads word 0 as the initial stack pointer and word 1 as the
// reset vector directly out of flash at address 0x00000000, before a
// single instruction of ours has executed.
//
// Only the 16 core Cortex-M exceptions are listed. We never touch NVIC,
// so peripheral IRQ slots (vector 16+) are never read by hardware and
// don't need to exist in this table.
#include <stdint.h>

extern uint32_t _estack;   // from the linker script: top of RAM
extern uint32_t _sidata;   // start of .data's initializer image, in flash
extern uint32_t _sdata, _edata; // .data's RAM range
extern uint32_t _sbss, _ebss;   // .bss's RAM range

void Reset_Handler(void);
static void Default_Handler(void);
int main(void);

// Every fault/exception we don't specifically handle lands here and spins.
// For a bring-up smoke test that's the right failure mode: if something
// goes wrong we want the board to visibly hang, not silently reset.
static void Default_Handler(void) {
    while (1) {}
}

// SysTick is the one exception a checkpoint might actually want to use (for
// timing runs that can last longer than the DWT cycle counter's ~67s
// wraparound window at 64MHz -- see systick.h). Weak so it costs nothing
// for every checkpoint that doesn't touch SysTick (they get this harmless
// do-nothing default and never enable the timer in the first place, so it
// never fires); a checkpoint that does use it just defines a strong
// SysTick_Handler in its own .c file, which the linker prefers over this
// weak one.
void SysTick_Handler(void) __attribute__((weak));
void SysTick_Handler(void) {
}

// Cortex-M vector table. Must be placed at the very start of flash via the
// .isr_vector section (see nrf52833.ld) and marked `used` so the linker
// doesn't discard it as dead code (nothing in C ever references it by name).
__attribute__((section(".isr_vector"), used))
void (* const g_vector_table[16])(void) = {
    (void (*)(void))&_estack,  // 0: initial stack pointer
    Reset_Handler,              // 1: Reset
    Default_Handler,             // 2: NMI
    Default_Handler,             // 3: HardFault
    Default_Handler,             // 4: MemManage
    Default_Handler,             // 5: BusFault
    Default_Handler,             // 6: UsageFault
    0,                           // 7: reserved
    0,                           // 8: reserved
    0,                           // 9: reserved
    0,                           // 10: reserved
    Default_Handler,             // 11: SVCall
    Default_Handler,             // 12: DebugMonitor
    0,                           // 13: reserved
    Default_Handler,             // 14: PendSV
    SysTick_Handler,              // 15: SysTick (weak no-op unless overridden)
};

void Reset_Handler(void) {
    // Copy .data's initial values out of flash into RAM.
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    // Zero .bss.
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    main();

    // main() should never return on an MCU with nothing to return to.
    // If it does, hang visibly rather than executing whatever garbage
    // follows in flash.
    while (1) {}
}
