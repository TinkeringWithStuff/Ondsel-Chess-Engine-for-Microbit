# micro:bit v2 hardware bring-up: barebones smoke test

Goal: prove a toolchain + linker script + startup code we wrote ourselves
can produce a binary that actually boots on the real nRF52833, before the
chess engine goes anywhere near it. Success = the top-left LED blinks.

No Nordic SDK, no CMSIS, no mbed -- just `src/startup.c` (vector table +
Reset_Handler), `src/nrf52833.ld` (linker script), and `src/main.c`
(direct GPIO register pokes to blink one LED). Same "no unnecessary
library machinery" discipline as the C++ engine itself.

## 1. Install the toolchain (one-time)

```bash
brew install arm-none-eabi-gcc
```

That's a plain homebrew-core formula -- no tap needed. Confirm it landed:

```bash
arm-none-eabi-gcc --version
```

## 2. Build

```bash
cd microbit-hw
make
```

This produces `build/firmware.hex`. `make` also prints the binary's
size -- worth a glance: `.text` should be well under 512K (it'll be well
under 1K for something this small), and `.bss`/`.data` well under 128K.

## 3. Flash

Plug the micro:bit v2 into your Mac via USB. It mounts as a drive named
`MICROBIT`. Just copy the hex file onto it:

```bash
cp build/firmware.hex /Volumes/MICROBIT/
```

The `MICROBIT` drive's activity LED (on the back, near the USB port)
will flicker for a couple seconds while DAPLink programs the flash, then
the board resets and runs the new firmware automatically.

## 4. What you should see

The single LED in the top-left corner of the 5x5 matrix should blink
on and off roughly twice a second, forever.

If it doesn't light up at all, most likely causes in order of probability:
- Copy didn't finish before the board was unplugged/reset -- wait for the
  drive's activity light to settle, then retry.
- `make` silently picked up a different/older `arm-none-eabi-gcc` --
  check `which arm-none-eabi-gcc` points at the Homebrew one.
- A wrong pin assumption on my part -- everything in `main.c` traces back
  to `model/MicroBitIO.h` in Lancaster University's own `codal-microbit-v2`
  repo (the code that ships on real micro:bits), so this would be
  surprising, but let me know what happened and I'll re-check it.

## What this does and doesn't prove

Proves: our own build pipeline (cross-compiler, linker script, startup
code, vector table) produces code the chip will actually execute from
power-on -- the whole boot path, which nothing in the sandbox testing
could touch.

Doesn't prove: timing, clock configuration, anything about running the
chess engine itself. That's next, once this blinks.
