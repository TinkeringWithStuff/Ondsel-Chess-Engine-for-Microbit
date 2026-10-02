// Checkpoint 7: run the REAL engine (not just perft's raw move generation)
// on real hardware -- negamax + alpha-beta + eval + SEE-ordered captures +
// killers/history, the same search.c already proven correct on the host
// (test_search.c: "ALL SEARCH TESTS PASSED"). This runs it on the four
// "Ondsel test positions" used earlier to sanity-check the v7 engine's move
// choices, reporting each position's chosen move, score, node count, time,
// and nodes/sec.
//
// Depth 6 was requested once depth 4's real hardware nps (~6,300-11,400,
// well below perft's 42,600 -- eval()/see() run on every node, not just
// leaves) showed it was affordable. At depth 6, host node counts run up to
// ~1.1M for the heaviest of these four positions, which at this chip's real
// search speed can take well over a minute -- past the 32-bit DWT cycle
// counter's ~67s wraparound window used for checkpoint 3's perft timing.
// This uses systick.h's wrap-counted SysTick timer instead, which stays
// correct no matter how long a single search takes.
//
// Output is a single plain line per position, printed once as soon as that
// position finishes (not held until all four are done, and not repeated
// forever like checkpoints 3/6) -- readable directly off MakeCode's own
// serial monitor, which doesn't drop bytes the way this project's earlier
// raw-terminal captures did, so the checksum+repeat machinery isn't needed
// here.
#include <stdint.h>
#include "clock.h"
#include "uart.h"
#include "systick.h"
#include "engine/board.h"
#include "engine/movegen.h"
#include "engine/attacks.h"
#include "engine/eval.h"
#include "engine/search.h"

#define P0_OUTSET GPIO_REG(P0_BASE, 0x508UL)
#define P0_OUTCLR GPIO_REG(P0_BASE, 0x50CUL)
#define PIN_ROW1 21U
#define PIN_COL1 28U

#define CPU_HZ 64000000UL
#define SEARCH_DEPTH 6

// The SysTick wrap counter (systick.h) only does anything once this ISR is
// actually wired up -- startup.c's vector table points SysTick's slot at a
// weak SysTick_Handler that does nothing; this strong definition overrides
// it for this checkpoint only, per-symbol, at link time.
void SysTick_Handler(void) {
    systick_overflow_count++;
}

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

// The four positions from the earlier v7 consistency testing (generated
// via v7-test/board-to-fen.js from the same recorded game), each one a
// spot where the "obvious" move and the engine's move were worth comparing.
#define NUM_POSITIONS 4
static const char *TEST_FENS[NUM_POSITIONS] = {
    "r2qkb1r/ppp1ppp1/2n4p/3p4/3P1nP1/2N1PN2/PPP2PP1/R2QKB1R b KQkq - 0 1",
    "r2qkb1r/1pp1ppp1/p1n1n2p/1B1p4/3P2P1/2N1PN2/PPP2PP1/R2QK2R w KQkq - 0 1",
    "1r2kb1r/2p1ppp1/p1p1n2p/3p4/3PPqP1/2N2N2/PPP2PP1/1R1Q1RK1 w k - 0 1",
    "1r2kb1r/2p1ppp1/p1p1n2p/3p4/3PP1q1/2NQ1N2/PPP2PP1/1R3RK1 w k - 0 1",
};
static const char *TEST_LABELS[NUM_POSITIONS] = {
    "8...Ne6?",
    "10.Bxc6+ or Ba4?",
    "14.Qd3 or exd5?",
    "15.exd5 or ...?",
};

static void move_to_str(Move m, char *out) {
    int from = move_from(m), to = move_to(m);
    out[0] = (char)('a' + (from % 8));
    out[1] = (char)('1' + (from / 8));
    out[2] = (char)('a' + (to % 8));
    out[3] = (char)('1' + (to / 8));
    int len = 4;
    if (move_is_promotion(m)) {
        static const char promo_char[] = "??nbrq"; // indexed by piece type (KNIGHT=2..QUEEN=5)
        out[len++] = promo_char[move_promotion_piece_type(m)];
    }
    out[len] = '\0';
}

// Small plain-text line builder -- no checksum/repeat here (see file header
// comment for why), just append pieces into one buffer and send it as a
// single transfer so the whole line lands atomically.
static void append_str(char *buf, uint32_t *len, const char *s) {
    while (*s) {
        buf[(*len)++] = *s++;
    }
}

static void append_udec(char *buf, uint32_t *len, uint64_t v) {
    char tmp[20];
    int i = 20;
    if (v == 0) {
        tmp[--i] = '0';
    } else {
        while (v > 0 && i > 0) {
            tmp[--i] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (i < 20) {
        buf[(*len)++] = tmp[i++];
    }
}

static void append_sdec(char *buf, uint32_t *len, int v) {
    if (v < 0) {
        buf[(*len)++] = '-';
        append_udec(buf, len, (uint64_t)(-v));
    } else {
        append_udec(buf, len, (uint64_t)v);
    }
}

int main(void) {
    P0_DIRSET = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR = (1U << PIN_COL1);

    clock_start_hfxo();
    uart_init();
    systick_init();
    uart_puts("microbit v2 search test (depth 6): starting\r\n");

    init_attack_tables();

    for (int i = 0; i < NUM_POSITIONS; i++) {
        Board b;
        board_load_fen(&b, TEST_FENS[i]);
        search_init();
        debug_node_count = 0;

        P0_OUTSET = (1U << PIN_ROW1); // solid on while this position searches
        uint32_t o0 = systick_overflow_count;
        uint32_t c0 = SYST_CVR;
        Move m = find_best_move(&b, SEARCH_DEPTH);
        uint32_t c1 = SYST_CVR;
        uint32_t o1 = systick_overflow_count;
        P0_OUTCLR = (1U << PIN_ROW1);

        char move_str[8];
        move_to_str(m, move_str);
        int score = last_best_score;
        long long nodes = debug_node_count;
        uint64_t elapsed_cycles = systick_elapsed_cycles(o0, c0, o1, c1);
        uint64_t time_ms = elapsed_cycles / (CPU_HZ / 1000UL);
        uint64_t nps = elapsed_cycles ? ((uint64_t)nodes * CPU_HZ) / elapsed_cycles : 0;

        char line[160];
        uint32_t len = 0;
        append_str(line, &len, "pos ");
        append_udec(line, &len, (uint64_t)i);
        append_str(line, &len, " (");
        append_str(line, &len, TEST_LABELS[i]);
        append_str(line, &len, "): move=");
        append_str(line, &len, move_str);
        append_str(line, &len, " score=");
        append_sdec(line, &len, score);
        append_str(line, &len, " nodes=");
        append_udec(line, &len, (uint64_t)nodes);
        append_str(line, &len, " time_ms=");
        append_udec(line, &len, time_ms);
        append_str(line, &len, " nps=");
        append_udec(line, &len, nps);
        append_str(line, &len, "\r\n");
        uart_write(line, len);

        // A brief flash between positions, distinct from the solid-on of
        // an active search, so "still running" vs "just finished one" are
        // visually distinguishable even without watching the serial output.
        delay(300000);
    }

    uart_puts("microbit v2 search test: all positions done\r\n");

    // Idle forever, slow blink -- no more UART output. Nothing left to
    // repeat: unlike checkpoints 3/6, every line here already went out
    // exactly once, in plain readable text, as soon as it was ready.
    while (1) {
        P0_OUTSET = (1U << PIN_ROW1);
        delay(3000000);
        P0_OUTCLR = (1U << PIN_ROW1);
        delay(3000000);
    }

    return 0;
}
