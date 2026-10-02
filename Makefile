# Barebones micro:bit v2 (nRF52833) build. No SDK, no CMSIS, no RTOS --
# just our own startup code and linker script targeting the chip directly.
#
# Targets:
#   make blink        -- checkpoint 1: LED smoke test (already verified working)
#   make uart_test    -- checkpoint 2: prints over USB-serial + blinks (verified working)
#   make perft        -- checkpoint 3: runs the plain-C engine's perft(), times it
#                         with the DWT cycle counter, reports nodes/sec over UART
#   make oled_test    -- checkpoint 4: brings up I2C + the Kitronik :VIEW 128x64
#                         OLED (fill/clear/diagonal-line test, no text yet)
#   make joystick_test -- checkpoint 5: joystick:bit's 4 digital buttons, shown
#                         live on the OLED (analog rocker is a later checkpoint)
#   make rocker_test  -- checkpoint 6: joystick:bit's analog rocker (SAADC),
#                         shown live on the OLED as a moving cursor square
#   make rocker_debug -- diagnostic: hold button C to sample ONLY AIN1, or
#                         button D to sample ONLY AIN2, never interleaved --
#                         isolates whether X/Y tracking each other is a
#                         firmware channel-switch artifact or a hardware fault
#   make search_test  -- checkpoint 7: the REAL engine (negamax/alpha-beta/
#                         eval/SEE, not just perft) at depth 6 on the 4
#                         "Ondsel test positions", reporting each position's
#                         move/score/nodes/time/nps as one plain line over
#                         UART as soon as that position finishes
#   make play_test    -- checkpoint 8: a full playable game -- top menu,
#                         Play vs Engine (cursor-driven move entry), Self-play,
#                         and Testing (perft + the 4 Ondsel test positions),
#                         all navigated with C/D/E/F=left/up/right/down,
#                         A=select, B=cancel. Board + menus + results now all
#                         render directly on the OLED (font5x5.h, a 5x5 font
#                         ported from Kitronik's own OLED driver), not just
#                         over UART -- the serial link proved too unreliable
#                         on real hardware to read menus off of comfortably.
#   make (default)    -- builds play_test, the current checkpoint

TOOLCHAIN = arm-none-eabi
CC        = $(TOOLCHAIN)-gcc
OBJCOPY   = $(TOOLCHAIN)-objcopy
SIZE      = $(TOOLCHAIN)-size

BUILD_DIR = build
LDSCRIPT  = src/nrf52833.ld

# -mfloat-abi=soft: the nRF52833 does have an FPU, but nothing here has any
# reason to touch it, and soft-float sidesteps any ABI mismatch between our
# hand-rolled startup code and libgcc's floating point helpers.
CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
          -Wall -Wextra -O2 -ffreestanding -fno-builtin -fno-common \
          -ffunction-sections -fdata-sections \
          -Isrc -std=c11

LDFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
          -T $(LDSCRIPT) -nostdlib -nostartfiles \
          -Wl,--gc-sections

# -lgcc: NOT libc -- this is the compiler's own freestanding runtime support
# library (64-bit shifts/divides, __ctzdi2/__aeabi_uldivmod helpers the
# compiler emits for 64-bit ops on a 32-bit target, etc.). It has no OS
# dependency and is the normal thing to link on bare-metal Cortex-M;
# -nostdlib only drops the C library itself. This MUST come after the
# object files on the command line -- a static library only satisfies
# undefined symbols that the linker has already seen by the time it reaches
# the library, so -lgcc before the .c files (as LDFLAGS would put it) links
# clean for anything that doesn't need it and fails with "undefined
# reference to __ctzdi2" etc. for anything that does.
LDLIBS = -lgcc

.PHONY: all clean blink uart_test perft oled_test joystick_test rocker_test rocker_debug search_test play_test

all: play_test

blink: $(BUILD_DIR)/blink.hex

uart_test: $(BUILD_DIR)/uart_test.hex

perft: $(BUILD_DIR)/perft.hex

oled_test: $(BUILD_DIR)/oled_test.hex

joystick_test: $(BUILD_DIR)/joystick_test.hex

rocker_test: $(BUILD_DIR)/rocker_test.hex

rocker_debug: $(BUILD_DIR)/rocker_debug.hex

search_test: $(BUILD_DIR)/search_test.hex

play_test: $(BUILD_DIR)/play_test.hex

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/blink.elf: src/startup.c src/main.c $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/blink.map src/startup.c src/main.c $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/uart_test.elf: src/startup.c src/main_uart_test.c src/uart.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/uart_test.map src/startup.c src/main_uart_test.c $(LDLIBS) -o $@
	$(SIZE) $@

PERFT_SRCS = src/startup.c src/main_perft.c src/libc_shim.c \
             src/engine/board.c src/engine/movegen.c src/engine/attacks.c \
             src/engine/zobrist.c

$(BUILD_DIR)/perft.elf: $(PERFT_SRCS) src/uart.h src/dwt.h src/clock.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/perft.map $(PERFT_SRCS) $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/oled_test.elf: src/startup.c src/main_oled_test.c src/i2c.h src/oled.h src/clock.h src/dwt.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/oled_test.map src/startup.c src/main_oled_test.c $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/joystick_test.elf: src/startup.c src/main_joystick_test.c src/i2c.h src/oled.h src/joystick.h src/clock.h src/dwt.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/joystick_test.map src/startup.c src/main_joystick_test.c $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/rocker_test.elf: src/startup.c src/main_rocker_test.c src/i2c.h src/oled.h src/joystick.h src/saadc.h src/uart.h src/clock.h src/dwt.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/rocker_test.map src/startup.c src/main_rocker_test.c $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/rocker_debug.elf: src/startup.c src/main_rocker_debug.c src/joystick.h src/saadc.h src/uart.h src/clock.h src/dwt.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/rocker_debug.map src/startup.c src/main_rocker_debug.c $(LDLIBS) -o $@
	$(SIZE) $@

SEARCH_TEST_SRCS = src/startup.c src/main_search_test.c src/libc_shim.c \
                   src/engine/board.c src/engine/movegen.c src/engine/attacks.c \
                   src/engine/eval.c src/engine/see.c src/engine/search.c \
                   src/engine/zobrist.c

$(BUILD_DIR)/search_test.elf: $(SEARCH_TEST_SRCS) src/uart.h src/systick.h src/clock.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/search_test.map $(SEARCH_TEST_SRCS) $(LDLIBS) -o $@
	$(SIZE) $@

PLAY_TEST_SRCS = src/startup.c src/main_play_test.c src/libc_shim.c \
                 src/engine/board.c src/engine/movegen.c src/engine/attacks.c \
                 src/engine/eval.c src/engine/see.c src/engine/search.c \
                 src/engine/zobrist.c

$(BUILD_DIR)/play_test.elf: $(PLAY_TEST_SRCS) src/uart.h src/systick.h src/clock.h \
                             src/i2c.h src/oled.h src/joystick.h src/buttons.h \
                             src/input.h src/board_render.h src/font5x5.h $(LDSCRIPT) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(BUILD_DIR)/play_test.map $(PLAY_TEST_SRCS) $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O ihex $< $@
	@echo ""
	@echo "Built $@ -- copy it onto the MICROBIT/NO NAME USB drive to flash."

clean:
	rm -rf $(BUILD_DIR)
