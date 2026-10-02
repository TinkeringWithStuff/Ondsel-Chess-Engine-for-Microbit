/* Linker script for the nRF52833 (micro:bit v2), no SoftDevice.
 *
 * Memory map per the nRF52833 Product Specification:
 *   Flash: 512 KB starting at 0x00000000
 *   RAM:   128 KB starting at 0x20000000
 * We use the whole chip -- no bootloader/SoftDevice reservation, since this
 * is a bare smoke test flashed directly via the DAPLink drag-and-drop
 * interface, which doesn't care what's already in flash.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table must be the very first thing in flash -- the CPU reads
     * the initial stack pointer and Reset_Handler address from here at
     * power-on, before any of our code has run. */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* .data lives in RAM at runtime but its initial values are stored in
     * flash (LOADADDR) -- Reset_Handler copies them across before main(). */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Initial stack pointer: top of RAM, stack grows down from here. */
    _estack = ORIGIN(RAM) + LENGTH(RAM);
}
