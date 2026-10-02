// Minimal register-level driver for the SAADC (successive-approximation
// ADC), used to read the joystick:bit's analog rocker. Every offset and
// enum value here comes from Nordic's own generated register definitions
// (nrf52832-pac), the same standard already held for UARTE/TWIM/GPIO in
// this project -- SAADC has a LOT of configuration fields, more than
// enough room to silently get one bit-shift wrong from memory.
#pragma once
#include <stdint.h>

#define SAADC_BASE 0x40007000UL
#define SREG(off) (*(volatile uint32_t *)(SAADC_BASE + (off)))

#define SAADC_TASKS_START   SREG(0x000)
#define SAADC_TASKS_SAMPLE  SREG(0x004)
#define SAADC_TASKS_STOP    SREG(0x008)
#define SAADC_EVENTS_STARTED SREG(0x100)
#define SAADC_EVENTS_END    SREG(0x104)
#define SAADC_EVENTS_STOPPED SREG(0x114)
#define SAADC_ENABLE        SREG(0x500)
#define SAADC_CH0_PSELP     SREG(0x510)
#define SAADC_CH0_PSELN     SREG(0x514)
#define SAADC_CH0_CONFIG    SREG(0x518)
#define SAADC_RESOLUTION    SREG(0x5F0)
#define SAADC_RESULT_PTR    SREG(0x62C)
#define SAADC_RESULT_MAXCNT SREG(0x630)

#define SAADC_ENABLE_ENABLED 1UL
#define SAADC_RESOLUTION_10BIT 1UL

// PSELP encoding: NC=0, AnalogInput0=1, AnalogInput1=2, ... AnalogInput7=8.
// The joystick:bit's rocker reads on the micro:bit's own AnalogPin.P1/P2,
// which are fixed-in-silicon AIN1 (P0.03) and AIN2 (P0.04) -- confirmed
// against Nordic's own AIN-to-GPIO table, not assumed from the pin numbers
// looking similar.
#define SAADC_PSELP_NC   0UL
#define SAADC_PSELP_AIN1 2UL // X rocker: edge P1 = nRF52 P0.03
#define SAADC_PSELP_AIN2 3UL // Y rocker: edge P2 = nRF52 P0.04

// CH[0].CONFIG bit layout: RESP[1:0]=0 (bypass), RESN[5:4]=0 (bypass),
// GAIN[10:8]=2 (1/4), REFSEL[12]=1 (VDD/4), TACQ[18:16]=2 (10us),
// MODE[20]=0 (single-ended), BURST[24]=0 (disabled). GAIN=1/4 with
// REFSEL=VDD/4 gives a full-scale input range of exactly 0..VDD (full
// scale = reference / gain = (VDD/4) / (1/4) = VDD) -- the standard
// combination for reading a plain 0..VDD analog signal like a
// potentiometer/rocker output, rather than the tiny 0..0.6V range the
// internal reference alone would give.
#define SAADC_CONFIG_SE_FULL_VDD ((2UL << 8) | (1UL << 12) | (2UL << 16))

static int16_t saadc_result_buf;

static inline void saadc_init(void) {
    SAADC_ENABLE = SAADC_ENABLE_ENABLED;
    SAADC_RESOLUTION = SAADC_RESOLUTION_10BIT;
    SAADC_RESULT_PTR = (uint32_t)(uintptr_t)&saadc_result_buf;
    SAADC_RESULT_MAXCNT = 1;
}

// One full START/SAMPLE/STOP handshake, assuming CH0 is already configured
// for whichever channel we want. Broken out so saadc_read() can run it
// twice per call (see below).
static inline int32_t saadc_take_sample(void) {
    SAADC_EVENTS_STARTED = 0;
    SAADC_TASKS_START = 1;
    while (SAADC_EVENTS_STARTED == 0) {
    }

    SAADC_EVENTS_END = 0;
    SAADC_TASKS_SAMPLE = 1;
    while (SAADC_EVENTS_END == 0) {
    }

    SAADC_EVENTS_STOPPED = 0;
    SAADC_TASKS_STOP = 1;
    while (SAADC_EVENTS_STOPPED == 0) {
    }

    return (int32_t)saadc_result_buf;
}

// Blocking single-shot read of one analog input (SAADC_PSELP_AIN1/2/...).
// Returns roughly 0..1023 for a 0..VDD single-ended input at 10-bit
// resolution. Each call reconfigures the one channel we use (CH[0]) rather
// than running multiple channels in scan mode -- simpler, and plenty fast
// enough for reading a hand-moved joystick a few dozen times a second.
//
// Confirmed on real hardware: alternating this between two different pins
// every call (X then Y then X then Y...) made the second reading come back
// as an exact copy of the first, every time -- not a blend, an exact
// duplicate -- which is the signature of a known nRF52 SAADC behavior where
// a freshly-switched channel's first conversion can still reflect the
// previous channel rather than the new one. The fix is the standard one:
// after changing PSELP, take and discard one throwaway sample before
// trusting the result. Doubles the time per call, but at joystick-polling
// rates (tens of Hz) that's still free.
static inline int32_t saadc_read(uint32_t pselp) {
    SAADC_CH0_PSELP = pselp;
    SAADC_CH0_PSELN = SAADC_PSELP_NC;
    SAADC_CH0_CONFIG = SAADC_CONFIG_SE_FULL_VDD;

    saadc_take_sample(); // throwaway: may still reflect the previous channel
    return saadc_take_sample();
}
