// Checkpoint 3: the actual point of this whole bring-up project. Runs the
// plain-C engine's perft() on real hardware, times it with the DWT cycle
// counter, and reports nodes/cycles/nodes-per-second back over the
// checksummed+repeated UART link proven out in checkpoint 2.
//
// PERFT_DEPTH starts shallow (depth 4 = 197,281 nodes from the start
// position) on purpose: the DWT cycle counter is only 32 bits wide, and at
// a 64MHz core clock it wraps roughly every 67 seconds. We don't yet know
// this engine's real nodes/sec on this chip -- that's the whole question --
// so starting deep enough to risk a multi-wrap (silently wrong) timing
// result would defeat the point. Once this run reports a real nodes/sec
// figure, we can calculate a safe depth ceiling and increase it.
#include <stdint.h>
#include "clock.h"
#include "dwt.h"
#include "uart.h"
#include "engine/board.h"
#include "engine/movegen.h"
#include "engine/attacks.h"

#define P0_OUTSET GPIO_REG(P0_BASE, 0x508UL)
#define P0_OUTCLR GPIO_REG(P0_BASE, 0x50CUL)
#define PIN_ROW1 21U
#define PIN_COL1 28U

// nRF52833 core (Cortex-M4) clock once HFCLK is sourced from the external
// 32MHz crystal via clock_start_hfxo() -- the standard, documented 64MHz
// CPU clock, not something we're guessing at.
#define CPU_HZ 64000000UL

static void delay(volatile uint32_t count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

static const char START_FEN[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

#define PERFT_DEPTH 4

// Wrap-safe timed perft: DWT_CYCCNT is only 32 bits and wraps every ~67s at
// 64MHz, so a single before/after read across a run that takes longer than
// that would silently give a wrong (aliased) answer -- exactly the trap to
// avoid once we start pushing to deeper (much longer-running) depths. This
// duplicates perft()'s own top-level loop (see engine/movegen.c) here, but
// times each ROOT move's subtree individually and accumulates the deltas
// into a 64-bit running total. Each individual delta only needs to stay
// under ~67s to be safe from wraparound -- true for any depth we're likely
// to reach before further optimization -- while the 64-bit accumulator
// sidesteps the 32-bit counter's own limit entirely.
static long long timed_root_perft(Board *b, int depth, uint64_t *total_cycles_out) {
    uint64_t total_cycles = 0;
    if (depth == 0) {
        *total_cycles_out = total_cycles;
        return 1;
    }
    int side = b->side_to_move;
    int opponent = side == WHITE ? BLACK : WHITE;
    generate_pseudo_moves(b, 0);
    MoveList *list = &move_pool[0];
    long long count = 0;
    for (int i = 0; i < list->count; i++) {
        Move m = list->moves[i];
        UndoInfo undo;
        make_move(b, m, &undo);
        int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        if (!is_square_attacked(b, king_sq, opponent)) {
            uint32_t before = cycle_counter_read();
            count += perft(b, depth - 1, 1);
            uint32_t after = cycle_counter_read();
            total_cycles += (uint32_t)(after - before); // correct across one wrap
        }
        unmake_move(b, m, &undo);
    }
    *total_cycles_out = total_cycles;
    return count;
}

int main(void) {
    P0_DIRSET = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR = (1U << PIN_COL1); // column sink, same as the blink test

    clock_start_hfxo();
    uart_init();
    uart_puts("microbit v2 perft test: starting\r\n");

    // LED on solid while perft runs -- a single make/unmake-heavy recursive
    // call with no natural point to blink from, so "solid on" just means
    // "working", and "off" (after) means "done, check UART".
    P0_OUTSET = (1U << PIN_ROW1);

    init_attack_tables();
    Board b;
    board_load_fen(&b, START_FEN);

    cycle_counter_init();
    uint64_t elapsed_cycles = 0;
    long long nodes = timed_root_perft(&b, PERFT_DEPTH, &elapsed_cycles);

    P0_OUTCLR = (1U << PIN_ROW1);

    uint64_t nps = elapsed_cycles ? ((uint64_t)nodes * CPU_HZ) / elapsed_cycles : 0;

    uart_puts("microbit v2 perft test: done\r\n");

    // The result used to go out ONCE here and then the board fell silent
    // forever in the fast-blink loop below -- which meant that unless the
    // capture script happened to already be running and listening at the
    // exact moment perft finished, the whole result burst could come and
    // go before anyone was watching (confirmed: a real capture attempt
    // came back with 0 bytes because a few seconds passed between
    // flashing and starting serial_test.py, which was already longer than
    // this whole computation-plus-print takes). Fixed by re-announcing
    // the result on a loop forever instead of once, so the capture script
    // just needs to be running at SOME point after flashing, not at the
    // one specific instant the board happened to finish.
    while (1) {
        // Fast blink while re-sending -- same visual meaning as before
        // ("done, check UART"), just repeated instead of a one-time flash.
        P0_OUTSET = (1U << PIN_ROW1);
        uart_send_result("depth=", (uint64_t)PERFT_DEPTH, 8);
        uart_send_result("nodes=", (uint64_t)nodes, 8);
        delay(1000000);
        P0_OUTCLR = (1U << PIN_ROW1);
        uart_send_result("cycles=", (uint64_t)elapsed_cycles, 8);
        uart_send_result("nps=", nps, 8);
        delay(1000000);
    }

    return 0;
}
