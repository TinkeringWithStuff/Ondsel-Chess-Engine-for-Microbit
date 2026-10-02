// Minimal register-level driver for UARTE0, used only to print ASCII text
// back to the host over the same USB cable used for flashing. No SDK, no
// EasyDMA abstraction library -- just the registers, confirmed against
// Nordic's own nRF52833 register map and a working bare-metal reference
// (andenore/NordicSnippets).
//
// Pin assignment (P0.06 = TX, P1.08 = RX) is the micro:bit v2's own
// USB-serial bridge wiring, from Lancaster University's codal-microbit-v2
// (model/MicroBitIO.h: MICROBIT_PIN_UART_TX / MICROBIT_PIN_UART_RX) --
// the same pins the official firmware uses, so anything we print here
// shows up on the Mac as a normal /dev/tty.usbmodem* serial port.
#pragma once
#include <stdint.h>

#define UARTE0_BASE 0x40002000UL
#define UREG(off) (*(volatile uint32_t *)(UARTE0_BASE + (off)))

#define UARTE_TASKS_STARTTX UREG(0x008)
#define UARTE_TASKS_STOPTX  UREG(0x00C)
#define UARTE_EVENTS_ENDTX  UREG(0x120)
#define UARTE_EVENTS_TXSTOPPED UREG(0x158)
#define UARTE_ENABLE        UREG(0x500)
#define UARTE_PSEL_TXD      UREG(0x50C)
#define UARTE_PSEL_RXD      UREG(0x514)
#define UARTE_BAUDRATE      UREG(0x524)
#define UARTE_TXD_PTR       UREG(0x544)
#define UARTE_TXD_MAXCNT    UREG(0x548)

// PSEL pin encoding: bits[4:0] = pin number within port, bit[5] = port
// (0 = P0, 1 = P1). P1.08 is therefore (1<<5)|8 = 0x28, NOT 8.
#define PIN_UART_TX 6U          // P0.06
#define PIN_UART_RX ((1U << 5) | 8U) // P1.08

#define GPIO_REG(base, off) (*(volatile uint32_t *)((base) + (off)))
#define P0_BASE 0x50000000UL
#define P0_DIRSET GPIO_REG(P0_BASE, 0x518)

// Both values computed directly from Nordic's own baud-generator formula
// (regval = round((rate << 32) / 16000000) & 0xFFFFF000), not copied from
// memory, so these are exact regardless of which one we end up using.
#define UARTE_BAUDRATE_115200 0x01D7E000UL
#define UARTE_BAUDRATE_9600   0x00275000UL
#define UARTE_ENABLE_ENABLED 8UL

// Diagnostic: dropped/missing (not garbled) characters persisted through
// every target-side fix (RAM buffers, single-transfer sends, proper
// stop/wait handshake, starting the external crystal) at 115200. That
// points outside our own firmware -- at the interface chip's own USB-CDC
// bridging or the host's USB-serial driver, both known to be more baud-
// rate-sensitive than the wire itself. Testing at 9600 to see if slowing
// down clears it up, before chasing further theories.
static inline void uart_init(void) {
    P0_DIRSET = (1U << 6); // P0.06 (TX) as output; RX stays input (default)

    UARTE_PSEL_TXD = PIN_UART_TX;
    UARTE_PSEL_RXD = PIN_UART_RX;
    UARTE_BAUDRATE = UARTE_BAUDRATE_9600;
    UARTE_ENABLE = UARTE_ENABLE_ENABLED;
}

// ---------------------------------------------------------------------------
// Minimal, non-blocking single-byte RX -- everything above this point was
// TX-only (this link only ever had something to say, not to hear), but the
// PC viewer's "End game" button (tools/pc_viewer/ondsel_viewer.py) needs a
// way to tell the device to stop self-play early, which means the device
// needs to listen for once. One byte at a time, via the same EasyDMA
// mechanism TX already uses, since UARTE has no non-DMA byte path on this
// chip.
//
// Deliberately NON-blocking: main_play_test.c's self-play loop already has
// to keep polling the physical buttons every iteration without ever
// stalling, and a receive call that could block waiting for a byte that
// might never arrive would defeat that. uart_rx_arm() points EasyDMA at a
// 1-byte RAM buffer and starts listening; uart_try_read_byte() just checks
// whether EVENTS_ENDRX has fired since the last check, and if so, reports
// the byte and immediately re-arms for the next one. As long as polls
// happen faster than new bytes can arrive -- trivially true at 9600 baud,
// about one byte per millisecond, against a loop that's also doing a full
// chess search between polls -- no byte gets missed.
//
// No checksum/repeat-and-vote scheme here unlike the TX side: this is a
// single, idempotent, low-frequency command ("end the game now"), not a
// stream of distinct events where a silently dropped one would be a real
// loss. The PC side sends it several times back to back for exactly the
// same reason uart.h's own TX comments give for repeats -- if the first
// copy gets dropped on this unreliable link, a later one still lands -- and
// the device side only needs to see ONE 'E' to act, so duplicates are
// harmless rather than needing to be filtered out.
#define UARTE_TASKS_STARTRX UREG(0x000)
#define UARTE_EVENTS_ENDRX  UREG(0x110)
#define UARTE_RXD_PTR       UREG(0x534)
#define UARTE_RXD_MAXCNT    UREG(0x538)

// File-scope (not function-local) so uart_rx_arm() and uart_try_read_byte()
// below share the SAME buffer -- EasyDMA writes here, and the byte has to
// still be sitting here when uart_try_read_byte() goes to read it back.
// "static" at this scope gives each .c file that includes uart.h its own
// private copy (the normal, already-established pattern in this header),
// which is exactly what's needed: whichever single translation unit
// actually calls these functions gets a self-consistent pair sharing one
// buffer, and -ffunction-sections/--gc-sections (already in the Makefile)
// drop the whole thing from any .o that never references it.
static char uart_rx_byte_buf;

static inline void uart_rx_arm(void) {
    UARTE_EVENTS_ENDRX = 0;
    UARTE_RXD_PTR = (uint32_t)(uintptr_t)&uart_rx_byte_buf;
    UARTE_RXD_MAXCNT = 1;
    UARTE_TASKS_STARTRX = 1;
}

// Call once at startup (main()'s uart_init() call site), and again at the
// start of any session that wants to ignore bytes received before it began
// (self_play() does this, so a command left over from a previous game
// can't immediately end a brand new one) -- re-arming discards whatever
// EVENTS_ENDRX state was pending without needing to consume it first.
static inline void uart_rx_init(void) {
    uart_rx_arm();
}

// Non-blocking: returns 1 and fills *out with the received byte if one has
// arrived since the last check (whether via uart_rx_init() or the previous
// uart_try_read_byte() call), or 0 immediately otherwise.
static inline int uart_try_read_byte(char *out) {
    if (UARTE_EVENTS_ENDRX == 0) return 0;
    *out = uart_rx_byte_buf;
    uart_rx_arm();
    return 1;
}

// Blocking send: DMAs `len` bytes out of `buf` and polls for completion.
// `buf` must stay alive/unchanged for the duration (EasyDMA reads it
// directly), which a local array on the stack satisfies fine here since
// we don't return until the transfer's done.
//
// A previous version of this function added a TASKS_STOPTX +
// EVENTS_TXSTOPPED handshake after ENDTX, theorizing that ENDTX alone
// (DMA-done, not physically-done) let a new transfer race the tail end of
// the previous one. That "fix" produced a raw-byte-confirmed regression:
// exactly one dropped character at the START of the very next line every
// time (a hexdump via pyserial -- bypassing screen/terminal emulation
// entirely -- proved these are genuine missing bytes on the wire, not a
// display artifact). The likely explanation: STOPTX itself needs a moment
// to settle after EVENTS_TXSTOPPED fires, and starting a fresh STARTTX
// immediately collided with that settling window, corrupting the new
// transfer's opening bytes instead. Back to the simpler ENDTX-only wait.
//
// With STOPTX removed, a second hexdump still showed corruption, but with a
// cleaner signature: the very first line after boot is always intact, and
// every line after that loses exactly one byte at a drifting position (a
// digit, the inter-word space, the \r, or the \n -- never more than one,
// never the same spot twice). "Always exactly one, position drifts" is the
// signature of a timing-margin problem right at the ENDTX/STARTTX boundary,
// not a data or logic bug: EVENTS_ENDTX fires when EasyDMA has moved the
// last byte out of RAM into the UART's shift register, not when that byte
// has finished actually going out over the wire. Starting the *next*
// transfer's STARTTX immediately after ENDTX can therefore still land
// mid-shift-out of the previous byte. Unlike the STOPTX handshake (which
// tried to synchronize on a hardware event and instead introduced its own
// race), the fix here is a plain fixed settling delay after ENDTX -- cheap
// insurance against exactly this window, with nothing else to race against.
static inline void uart_tx_settle_delay(void) {
    // A handful of microseconds is enough headroom at either baud rate we
    // test (one bit time at 9600 baud is ~104us, so even ~1000 cycles of
    // margin costs far less than a single bit period) -- tuned to be
    // generous rather than minimal, since a slightly-too-long delay only
    // costs throughput, not correctness.
    for (volatile uint32_t i = 0; i < 4000; i++) {
        __asm__ volatile("nop");
    }
}

static inline void uart_write(const char *buf, uint32_t len) {
    if (len == 0) return;
    UARTE_EVENTS_ENDTX = 0;
    UARTE_TXD_PTR = (uint32_t)(uintptr_t)buf;
    UARTE_TXD_MAXCNT = len;
    UARTE_TASKS_STARTTX = 1;
    while (UARTE_EVENTS_ENDTX == 0) {}
    uart_tx_settle_delay();
}

// Diagnostic evidence forced a change of theory: the settling delay above
// targets the boundary *between* uart_write() calls, but the next hexdump
// (still corrupted) showed the dropped byte landing INSIDE a single line --
// e.g. "tick8\r\n" (missing the space) and "tick 5\n" (missing the \r) --
// and every one of those lines is built in one RAM buffer and sent as ONE
// uart_write() call. A byte vanishing mid-burst, inside one continuous
// EasyDMA transfer, cannot be an inter-transfer race: there is no second
// transfer involved. That rules out our own STARTTX/ENDTX timing as the
// cause entirely.
//
// That points downstream of the nRF52 altogether: the DAPLink interface
// chip has to pull each byte off the UART line and hand it to the host over
// USB, typically once per ~1ms USB frame. A continuous multi-byte burst
// with no gaps can outrun a thin internal buffer on that side and lose
// exactly one byte with no framing/parity symptom -- which matches "always
// exactly one, position random" far better than anything on our side.
//
// This paced version tests that theory directly: send one byte per DMA
// transfer, with a deliberate gap after each, giving the interface chip
// time to drain its buffer before the next byte arrives. Much slower than a
// single burst, but fine for a diagnostic/low-rate print function.
static inline void uart_write_paced(const char *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        UARTE_EVENTS_ENDTX = 0;
        UARTE_TXD_PTR = (uint32_t)(uintptr_t)(buf + i);
        UARTE_TXD_MAXCNT = 1;
        UARTE_TASKS_STARTTX = 1;
        while (UARTE_EVENTS_ENDTX == 0) {}
        // Gap after every byte, not just between lines -- this is the part
        // that's actually being tested here.
        for (volatile uint32_t j = 0; j < 20000; j++) {
            __asm__ volatile("nop");
        }
    }
}

// EasyDMA (which UARTE's TXD.PTR feeds) is an AHB bus master wired only to
// Data RAM -- per Nordic's own nRF52 documentation, "the EasyDMA is not
// able to access the Flash." String literals like "tick " live in flash
// (folded into .text by our linker script), so handing their address
// straight to TXD.PTR fails silently: no error, the transfer just doesn't
// move the bytes. Numbers worked in testing because uart_put_u64 already
// builds its output in a stack (RAM) buffer -- this is the same fix,
// applied to arbitrary strings: copy through a RAM scratch buffer first.
static inline void uart_puts(const char *s) {
    static char ram_buf[128];
    uint32_t len = 0;
    while (s[len] && len < sizeof(ram_buf)) {
        ram_buf[len] = s[len];
        len++;
    }
    uart_write(ram_buf, len);
}

// Minimal unsigned 64-bit -> decimal, no libc.
static inline void uart_put_u64(uint64_t v) {
    char buf[20];
    int i = 20;
    if (v == 0) {
        uart_write("0", 1);
        return;
    }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    uart_write(&buf[i], (uint32_t)(20 - i));
}

// Builds "<prefix><n>\r\n" in one RAM buffer and sends it as a single
// transfer. Preferred over separate uart_puts()/uart_put_u64()/uart_puts()
// calls for exactly the reason explained above uart_write(): every extra
// call is another chance to start a new transfer while the last byte of
// the previous one is still on the wire.
static inline void uart_put_line(const char *prefix, uint64_t n) {
    static char line[64];
    uint32_t len = 0;
    while (prefix[len] && len < sizeof(line) - 24) {
        line[len] = prefix[len];
        len++;
    }

    char digits[20];
    int i = 20;
    if (n == 0) {
        digits[--i] = '0';
    } else {
        while (n > 0 && i > 0) {
            digits[--i] = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    while (i < 20 && len < sizeof(line) - 2) {
        line[len++] = digits[i++];
    }

    line[len++] = '\r';
    line[len++] = '\n';
    uart_write_paced(line, len);
}

// Diagnostic evidence (see uart_write_paced above) shows this link can drop
// a single byte even when every byte is sent in isolation with a huge idle
// margin around it -- not a timing bug we can fix from the target side, but
// an inherent limitation of a TX/RX-only link with no hardware flow control
// (confirmed by BBC's own micro:bit interface docs). Rather than keep
// chasing the wire, make the data self-checking: append a one-byte additive
// checksum (as two hex digits) to each line, so the host can tell a good
// line from a corrupted one instead of silently trusting whatever arrives.
// A dropped byte shifts everything after it, so the checksum will fail to
// match in all but a 1-in-256 fluke -- good enough for a diagnostic link.
static inline uint8_t uart_checksum8(const char *buf, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + (uint8_t)buf[i]);
    }
    return sum;
}

// Builds "<prefix><n>*XX\r\n" where XX is the hex checksum of "<prefix><n>",
// and sends it as one plain (non-paced) burst -- pacing bought us nothing
// against this failure mode, so there's no reason to pay its speed cost.
static inline void uart_put_checked_line(const char *prefix, uint64_t n) {
    static char line[80];
    uint32_t len = 0;
    while (prefix[len] && len < sizeof(line) - 8) {
        line[len] = prefix[len];
        len++;
    }

    char digits[20];
    int i = 20;
    if (n == 0) {
        digits[--i] = '0';
    } else {
        while (n > 0 && i > 0) {
            digits[--i] = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    while (i < 20 && len < sizeof(line) - 6) {
        line[len++] = digits[i++];
    }

    uint32_t payload_len = len; // "<prefix><n>", not counting checksum/CRLF
    uint8_t sum = uart_checksum8(line, payload_len);
    static const char hex_digits[] = "0123456789ABCDEF";
    line[len++] = '*';
    line[len++] = hex_digits[(sum >> 4) & 0xFU];
    line[len++] = hex_digits[sum & 0xFU];
    line[len++] = '\r';
    line[len++] = '\n';
    uart_write(line, len);
}

// Repeat-and-vote: the checksum above already tells the host, per line,
// whether that line arrived intact (a corrupted line fails to match with
// only ~1/256 odds of a false pass) -- so getting one trustworthy reading
// through just means sending enough copies that at least one is likely to
// land clean, not building anything cleverer. If a single copy has even a
// 50% chance of surviving, 8 copies fail together well under 1% of the
// time. This is what a one-shot result (e.g. the final perft nodes/sec
// figure) should use instead of a single uart_put_checked_line() call.
static inline void uart_send_result(const char *prefix, uint64_t value, uint32_t repeats) {
    for (uint32_t r = 0; r < repeats; r++) {
        uart_put_checked_line(prefix, value);
    }
}

// Same idea as uart_put_checked_line()/uart_send_result(), but for a plain
// string payload instead of "<prefix><number>" -- e.g. reporting a move as
// readable algebraic text ("move=e2e4") rather than encoding it into a
// number and making the person decode it by hand.
static inline void uart_put_checked_text(const char *text) {
    static char line[96];
    uint32_t len = 0;
    while (text[len] && len < sizeof(line) - 6) {
        line[len] = text[len];
        len++;
    }

    uint32_t payload_len = len;
    uint8_t sum = uart_checksum8(line, payload_len);
    static const char hex_digits[] = "0123456789ABCDEF";
    line[len++] = '*';
    line[len++] = hex_digits[(sum >> 4) & 0xFU];
    line[len++] = hex_digits[sum & 0xFU];
    line[len++] = '\r';
    line[len++] = '\n';
    uart_write(line, len);
}

static inline void uart_send_text_result(const char *text, uint32_t repeats) {
    for (uint32_t r = 0; r < repeats; r++) {
        uart_put_checked_text(text);
    }
}
