// Checkpoint 8: a genuinely playable game on real hardware. C/D/E/F move a
// cursor around the board (left/up/right/down), A picks up a piece and
// then confirms its destination, B cancels a selection. The rocker is
// deliberately unused here -- digital buttons only, per Rune's request.
//
// All readable text (menus, moves, scores, game-over messages, test
// results) goes out over UART as full plain lines, same as checkpoint 7.
// It ALSO now renders on the OLED itself (board_render.h's draw_menu_screen/
// draw_text_screen, built on font5x5.h -- a 5x5 bitmap font ported from
// Kitronik's own MakeCode OLED driver), so the menus and game are readable
// without depending on the serial link at all, which turned out to be
// unreliable enough on real hardware to be worth routing around rather than
// patching with a heavier checksum scheme.
//
// The board screen itself is kept deliberately simple: plain letters for
// pieces (uppercase White, lowercase Black) and dots for dark squares, no
// side-to-move indicator or on-board status text. Holding A+B together
// during your own move opens a separate info screen (show_info_screen())
// with the last move played, nodes searched, depth reached, time taken, and
// the engine's eval -- UART still carries all of this in full detail
// regardless.
//
// Engine strength (depth or time budget) is chosen per play/self-play
// session by choose_engine_strength() -- see the "Engine strength" section
// below. The Testing menu's perft and test-position runs use SysTick
// (systick.h) for correct timing past 67s, same reasoning as checkpoint 7,
// since those intentionally go deeper.
#include <stdint.h>
#include "clock.h"
#include "uart.h"
#include "systick.h"
#include "input.h"
#include "oled.h"
#include "board_render.h"
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

void SysTick_Handler(void) {
    systick_overflow_count++;
}

// ---- small plain-text UART helpers (no checksum -- see file header) ----

static void append_str(char *buf, uint32_t *len, const char *s) {
    while (*s) buf[(*len)++] = *s++;
}

static void append_udec(char *buf, uint32_t *len, uint64_t v) {
    char tmp[20];
    int i = 20;
    if (v == 0) {
        tmp[--i] = '0';
    } else {
        while (v > 0 && i > 0) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    }
    while (i < 20) buf[(*len)++] = tmp[i++];
}

static void append_sdec(char *buf, uint32_t *len, int v) {
    if (v < 0) { buf[(*len)++] = '-'; append_udec(buf, len, (uint64_t)(-v)); }
    else append_udec(buf, len, (uint64_t)v);
}

static void move_to_str(Move m, char *out) {
    int from = move_from(m), to = move_to(m);
    out[0] = (char)('a' + (from % 8));
    out[1] = (char)('1' + (from / 8));
    out[2] = (char)('a' + (to % 8));
    out[3] = (char)('1' + (to / 8));
    int len = 4;
    if (move_is_promotion(m)) {
        static const char promo_char[] = "??nbrq";
        out[len++] = promo_char[move_promotion_piece_type(m)];
    }
    out[len] = '\0';
}

// ---- menu helper: prints a titled list with '>' on the selected item ----

static void print_menu(const char *title, const char *const *items, int count, int selected) {
    char line[96];
    uint32_t len = 0;
    append_str(line, &len, "=== ");
    append_str(line, &len, title);
    append_str(line, &len, " ===\r\n");
    uart_write(line, len);

    for (int i = 0; i < count; i++) {
        len = 0;
        append_str(line, &len, (i == selected) ? "> " : "  ");
        append_str(line, &len, items[i]);
        append_str(line, &len, "\r\n");
        uart_write(line, len);
    }
    uart_puts("(D=up F=down A=select B=back)\r\n");

    draw_menu_screen(title, items, count, selected);
}

// Returns the chosen index, or -1 if the person cancelled (B).
static int run_menu(const char *title, const char *const *items, int count) {
    int selected = 0;
    print_menu(title, items, count, selected);
    while (1) {
        InputEvent ev = input_wait_press();
        if (ev == INPUT_UP) {
            selected = (selected - 1 + count) % count;
            print_menu(title, items, count, selected);
        } else if (ev == INPUT_DOWN) {
            selected = (selected + 1) % count;
            print_menu(title, items, count, selected);
        } else if (ev == INPUT_SELECT) {
            return selected;
        } else if (ev == INPUT_CANCEL) {
            return -1;
        }
    }
}

// Generic "pick a number" prompt: D increments, F decrements, A confirms,
// B cancels (returning default_v). Shared by pick_depth() and pick_seconds()
// below -- same UI, just a different label/range.
static int pick_number(const char *label, int min_v, int max_v, int default_v) {
    int v = default_v;
    char line[48];
    uint32_t len;
    for (;;) {
        len = 0;
        append_str(line, &len, label);
        append_str(line, &len, ": ");
        append_udec(line, &len, (uint64_t)v);
        append_str(line, &len, "  (D=+ F=- A=confirm B=cancel)\r\n");
        uart_write(line, len);

        len = 0;
        append_str(line, &len, label);
        append_str(line, &len, ": ");
        append_udec(line, &len, (uint64_t)v);
        line[len] = '\0';
        static const char *screen_lines[3];
        static char title[24];
        uint32_t tlen = 0;
        append_str(title, &tlen, "Pick "); append_str(title, &tlen, label); title[tlen] = '\0';
        screen_lines[0] = title;
        screen_lines[1] = line;
        screen_lines[2] = "D=+ F=- A=ok";
        draw_text_screen(screen_lines, 3);

        InputEvent ev = input_wait_press();
        if (ev == INPUT_UP) { if (v < max_v) v++; }
        else if (ev == INPUT_DOWN) { if (v > min_v) v--; }
        else if (ev == INPUT_SELECT) { return v; }
        else if (ev == INPUT_CANCEL) { return default_v; }
    }
}

static int pick_depth(int min_d, int max_d, int default_d) {
    return pick_number("Depth", min_d, max_d, default_d);
}

static int pick_seconds(int min_s, int max_s, int default_s) {
    return pick_number("Seconds", min_s, max_s, default_s);
}

// ---- shared move helpers (make/unmake + king-safety filter, same pattern
// checkpoint 3's timed_root_perft already used) ----

static int count_legal_moves(Board *b) {
    int side = b->side_to_move, opp = (side == WHITE) ? BLACK : WHITE;
    generate_pseudo_moves(b, 0);
    MoveList *list = &move_pool[0];
    int count = 0;
    for (int i = 0; i < list->count; i++) {
        UndoInfo undo;
        make_move(b, list->moves[i], &undo);
        int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        if (!is_square_attacked(b, king_sq, opp)) count++;
        unmake_move(b, list->moves[i], &undo);
    }
    return count;
}

// Finds a legal move from `from` to `to`. Auto-queen: if that square pair
// is only reachable via promotion, the queen-promotion variant is what
// matches (under-promotion isn't offered in this checkpoint).
static int find_legal_move(Board *b, int from, int to, Move *out) {
    int side = b->side_to_move, opp = (side == WHITE) ? BLACK : WHITE;
    generate_pseudo_moves(b, 0);
    MoveList *list = &move_pool[0];
    for (int i = 0; i < list->count; i++) {
        Move m = list->moves[i];
        if (move_from(m) != from || move_to(m) != to) continue;
        if (move_is_promotion(m) && move_promotion_piece_type(m) != QUEEN) continue;
        UndoInfo undo;
        make_move(b, m, &undo);
        int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        int legal = !is_square_attacked(b, king_sq, opp);
        unmake_move(b, m, &undo);
        if (legal) { *out = m; return 1; }
    }
    return 0;
}

// last_move_str/last_move_who/last_engine_nodes/last_engine_time_ms/
// have_engine_info: set by report_move() (and the two spots that call
// find_best_move() directly) so show_info_screen() can display a snapshot
// of the game state whenever the player asks for it via the A+B combo,
// without having to re-run anything. Declared up here (rather than right
// before show_info_screen(), which is where they're mostly used) because
// broadcast_move() below also reads last_search_depth/last_engine_nodes/
// last_engine_time_ms to fill in the PC viewer's stats fields.
static char last_move_str[8];
static char last_move_who[16];
static long long last_engine_nodes = 0;
static long long last_engine_time_ms = 0; // wall-clock time the most recent
                                           // engine_move() call actually
                                           // took, via SysTick -- NOT the
                                           // time budget it was given (that's
                                           // only meaningful in "by time"
                                           // mode); this is how long it
                                           // genuinely ran, in either mode.
static int have_engine_info = 0; // 0 until the engine has moved at least once

// ---- PC viewer protocol ----
//
// A second, MACHINE-readable stream alongside the plain human-readable
// lines above (report_move() etc.), for a PC-side program to follow
// self-play live (see tools/pc_viewer/). The plain lines above are for a
// person watching a terminal, who can shrug off the odd dropped/garbled
// character (see uart.h's file history on this link's reliability); a
// program parsing the same stream can't, so this uses uart.h's
// checksummed, repeat-for-reliability helpers (uart_put_checked_text() /
// uart_send_text_result()) instead of the plain uart_write() the rest of
// this file uses -- same "repeat and vote" reasoning uart.h's own comment
// gives for uart_send_result().
//
// Protocol (one event per line, each line sent PROTO_REPEATS times back to
// back -- the viewer discards a checksum failure and de-duplicates
// identical consecutive lines, so any one intact copy getting through is
// enough):
//   GAME_START
//   MOVE <uci> <score> <depth> <nodes> <time_ms>
//                            -- <uci> is move_to_str()'s own format (e.g.
//                               "e2e4", "e7e8q"); <score> is the mover's
//                               own eval right after this move, signed
//                               decimal, from last_best_score. <depth> is
//                               the deepest iterative-deepening depth that
//                               fully completed (last_search_depth; 0 for a
//                               book move), <nodes> the node count that
//                               search took (last_engine_nodes), <time_ms>
//                               the wall-clock milliseconds it actually ran
//                               (last_engine_time_ms) -- all three straight
//                               off the same stats the on-device info
//                               screen shows, just broadcast with every
//                               move instead of only on request.
//   GAME_END <code>         -- one of CHECKMATE_WHITE, CHECKMATE_BLACK,
//                               STALEMATE, STOPPED (STOPPED = person
//                               pressed B before the game finished).
// The viewer tracks whose turn it is itself (White moves first, then
// alternates) -- <uci> alone is unambiguous once you know that, so the
// protocol doesn't waste bytes repeating the color every line.
#define PROTO_REPEATS 3 // matches uart.h's own "8 copies fail together <1%" reasoning, scaled down since this is a live game feed, not a one-shot result -- a missed move would still show up as the board silently staying one ply behind rather than actual failure, so this trades a slightly higher (but still small) miss chance for not spending 8x the airtime on every single move

static void broadcast_line(const char *text) {
    uart_send_text_result(text, PROTO_REPEATS);
}

static void broadcast_game_start(void) {
    broadcast_line("GAME_START");
}

// Reads last_search_depth/last_engine_nodes/last_engine_time_ms as they
// stand right now -- the caller (self_play(), play_vs_engine()) always
// calls this immediately after engine_move() sets them for the very move
// being broadcast, same way report_move()/show_info_screen() already rely
// on that ordering for the human-readable/on-request paths.
static void broadcast_move(Move m, int score) {
    char mstr[8];
    move_to_str(m, mstr);
    char line[72];
    uint32_t len = 0;
    append_str(line, &len, "MOVE ");
    append_str(line, &len, mstr);
    append_str(line, &len, " ");
    append_sdec(line, &len, score);
    append_str(line, &len, " ");
    append_udec(line, &len, (uint64_t)last_search_depth);
    append_str(line, &len, " ");
    append_udec(line, &len, (uint64_t)last_engine_nodes);
    append_str(line, &len, " ");
    append_udec(line, &len, (uint64_t)last_engine_time_ms);
    line[len] = '\0';
    broadcast_line(line);
}

static void broadcast_game_end(const char *code) {
    char line[32];
    uint32_t len = 0;
    append_str(line, &len, "GAME_END ");
    append_str(line, &len, code);
    line[len] = '\0';
    broadcast_line(line);
}

// Same checkmate/stalemate determination as report_game_end() below, just
// returning a GAME_END protocol code instead of printing -- kept separate
// rather than having report_game_end() return a value, so the existing,
// already-working human-readable path stays untouched.
static const char *game_end_code(Board *b) {
    int side = b->side_to_move, opp = (side == WHITE) ? BLACK : WHITE;
    int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
    int in_check = is_square_attacked(b, king_sq, opp);
    if (!in_check) return "STALEMATE";
    return (side == WHITE) ? "CHECKMATE_BLACK" : "CHECKMATE_WHITE";
}

// Centipawns of advantage needed before "end the game now" picks a winner
// instead of calling it a draw. 300 (three pawns) was Rune's own number --
// comfortably past "probably winning" for this engine's strength, without
// requiring the kind of crushing material lead that would make the decision
// obvious anyway.
#define ADJUDICATION_THRESHOLD_CP 300

// Decides a result for "end the game now" (A button during self-play, or
// the PC viewer's End Game button over UART) -- rather than just abandoning
// the game with no result at all (that's what B/INPUT_CANCEL still does,
// unchanged), this looks at the engine's own most recent evaluation and
// calls it for whoever's clearly ahead.
//
// last_best_score is the mover's OWN-perspective eval from the last move
// actually played (search.h's documented convention), so it has to be
// converted to a White-relative figure before comparing against the
// threshold -- exactly the same conversion tools/pc_viewer/protocol.py
// applies to the score it broadcasts, so "300cp or more decides it" means
// the same thing whether the decision is read here or on the PC screen.
static const char *adjudicate_result(const Board *b, int ply_count) {
    if (ply_count == 0) return "STOPPED"; // no move played yet -- nothing to adjudicate from
    int mover_is_white = (b->side_to_move == BLACK); // side_to_move already flipped past whoever just moved
    int white_relative_score = mover_is_white ? last_best_score : -last_best_score;
    if (white_relative_score >= ADJUDICATION_THRESHOLD_CP) return "ADJUDICATED_WHITE";
    if (white_relative_score <= -ADJUDICATION_THRESHOLD_CP) return "ADJUDICATED_BLACK";
    return "ADJUDICATED_DRAW";
}

static void report_game_end(Board *b, int ply_count) {
    int side = b->side_to_move, opp = (side == WHITE) ? BLACK : WHITE;
    int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
    int in_check = is_square_attacked(b, king_sq, opp);
    int moveno = ply_count / 2 + 1;

    char line[64];
    uint32_t len = 0;
    append_str(line, &len, in_check
        ? (side == WHITE ? "Checkmate! Black wins, move " : "Checkmate! White wins, move ")
        : "Stalemate -- draw, move ");
    append_udec(line, &len, (uint64_t)moveno);
    append_str(line, &len, "\r\n");
    uart_write(line, len);

    char l0[24], l1[32];
    len = 0;
    append_str(l0, &len, in_check ? (side == WHITE ? "Black wins" : "White wins") : "Draw");
    l0[len] = '\0';
    len = 0;
    append_str(l1, &len, in_check ? "Mate, move " : "Stalemate, move ");
    append_udec(l1, &len, (uint64_t)moveno);
    l1[len] = '\0';
    const char *lines[2] = { l0, l1 };
    draw_text_screen(lines, 2);
}

static void report_move(const char *who, Move m, int score, int have_score) {
    char line[64];
    uint32_t len = 0;
    char mstr[8];
    move_to_str(m, mstr);
    append_str(line, &len, who);
    append_str(line, &len, ": ");
    append_str(line, &len, mstr);
    if (have_score) {
        append_str(line, &len, "  score=");
        append_sdec(line, &len, score);
    }
    append_str(line, &len, "\r\n");
    uart_write(line, len);

    len = 0;
    append_str(last_move_who, &len, who);
    last_move_who[len] = '\0';
    int i = 0;
    while (mstr[i]) { last_move_str[i] = mstr[i]; i++; }
    last_move_str[i] = '\0';
}

// Full-screen snapshot: last move played, nodes the engine searched for its
// last move, and its eval -- reached by holding A+B together during your own
// turn (see input.h's INPUT_INFO). Waits for a fresh press before returning
// so the caller can safely re-render the board right after.
static void show_info_screen(void) {
    char l0[24], l1[24], l2[24], l3[24], l4[24];
    const char *lines[6];
    int n = 0;

    if (last_move_who[0] == '\0') {
        lines[n++] = "No moves yet";
    } else {
        uint32_t len = 0;
        append_str(l0, &len, "Last: ");
        append_str(l0, &len, last_move_who);
        append_str(l0, &len, " ");
        append_str(l0, &len, last_move_str);
        l0[len] = '\0';
        lines[n++] = l0;
    }
    if (have_engine_info) {
        uint32_t len = 0;
        append_str(l1, &len, "Nodes: ");
        append_udec(l1, &len, (uint64_t)last_engine_nodes);
        l1[len] = '\0';
        lines[n++] = l1;

        len = 0;
        append_str(l2, &len, "Eval: ");
        append_sdec(l2, &len, last_best_score);
        l2[len] = '\0';
        lines[n++] = l2;

        len = 0;
        append_str(l3, &len, "Depth: ");
        append_udec(l3, &len, (uint64_t)last_search_depth);
        l3[len] = '\0';
        lines[n++] = l3;

        len = 0;
        append_str(l4, &len, "Time: ");
        append_udec(l4, &len, (uint64_t)last_engine_time_ms);
        append_str(l4, &len, "ms");
        l4[len] = '\0';
        lines[n++] = l4;
    }
    lines[n++] = "A or B=back";
    draw_text_screen(lines, n);

    input_wait_press(); // any press dismisses it
}

// ---- Engine strength: by depth or by time (v7 had this; the C port's
// first playable checkpoint fixed PLAY_DEPTH=4 to get something working
// sooner, and never added the choice back) ----

// A hard depth ceiling for time-based search. search.c's find_best_move_timed()
// now polls the time budget every ~1024 nodes *during* a depth, not just
// between depths (see its own comment -- the between-depths-only version
// let a single depth run for minutes past whatever budget was picked,
// since real per-node search cost on this hardware turned out far higher
// than perft's bare make/unmake made it look), and discards any depth that
// gets cut off partway through rather than trust it. So this ceiling is
// back to being a genuine backstop -- depth 10 is nothing realistic
// reaches even with a generous budget -- not the only thing standing
// between a bad pick and a multi-minute wait.
#define TIMED_SEARCH_MAX_DEPTH 10

typedef struct {
    int use_time;  // 0 = fixed depth, 1 = fixed time budget
    int depth;     // meaningful when use_time == 0
    int time_ms;   // meaningful when use_time == 1
} EngineStrength;

// time_check_callback() closes over these file-scope statics rather than
// taking arguments, since search.h's TimeCheckFn is a plain C function
// pointer with no way to carry its own state -- set right before each call
// into find_best_move_timed().
static uint32_t g_search_start_overflow, g_search_start_cvr;
static uint64_t g_search_budget_cycles;

static int time_check_callback(void) {
    uint32_t now_cvr = SYST_CVR, now_overflow = systick_overflow_count;
    uint64_t elapsed = systick_elapsed_cycles(g_search_start_overflow, g_search_start_cvr, now_overflow, now_cvr);
    return elapsed >= g_search_budget_cycles;
}

static EngineStrength choose_engine_strength(void) {
    static const char *items[] = { "By depth", "By time" };
    int choice = run_menu("Engine strength", items, 2);

    EngineStrength s;
    if (choice == 1) {
        int secs = pick_seconds(1, 60, 5);
        s.use_time = 1;
        s.time_ms = secs * 1000;
        char line[48];
        uint32_t len = 0;
        append_str(line, &len, "Engine strength: "); append_udec(line, &len, (uint64_t)secs);
        append_str(line, &len, "s per move\r\n");
        uart_write(line, len);
    } else { // choice == 0, or -1 (cancelled) -- either way, fall back to depth
        int d = pick_depth(1, 6, 4);
        s.use_time = 0;
        s.depth = d;
        char line[48];
        uint32_t len = 0;
        append_str(line, &len, "Engine strength: depth "); append_udec(line, &len, (uint64_t)d);
        append_str(line, &len, "\r\n");
        uart_write(line, len);
    }
    return s;
}

// Runs the engine's move at the chosen strength, updating the info-screen
// stats (last_engine_nodes/have_engine_info/last_best_score) the same way
// either mode.
static Move engine_move(Board *b, const EngineStrength *s) {
    debug_node_count = 0;
    // Wall-clock timing via SysTick, same mechanism time_check_callback()
    // uses for the time budget itself -- but taken unconditionally, in BOTH
    // modes, purely for reporting (last_engine_time_ms). In "by time" mode
    // this just confirms the budget was honored; in "by depth" mode it's
    // the only place that duration is measured at all.
    uint32_t start_overflow = systick_overflow_count;
    uint32_t start_cvr = SYST_CVR;
    Move m;
    if (s->use_time) {
        g_search_start_overflow = start_overflow;
        g_search_start_cvr = start_cvr;
        g_search_budget_cycles = (uint64_t)s->time_ms * (CPU_HZ / 1000UL);
        m = find_best_move_timed(b, TIMED_SEARCH_MAX_DEPTH, time_check_callback);
    } else {
        m = find_best_move(b, s->depth);
    }
    uint32_t end_overflow = systick_overflow_count;
    uint32_t end_cvr = SYST_CVR;
    uint64_t elapsed_cycles = systick_elapsed_cycles(start_overflow, start_cvr, end_overflow, end_cvr);
    last_engine_time_ms = (long long)(elapsed_cycles / (CPU_HZ / 1000UL));
    last_engine_nodes = debug_node_count;
    have_engine_info = 1;
    return m;
}

// ---- Play vs Engine ----

static void play_vs_engine(void) {
    EngineStrength strength = choose_engine_strength();

    Board b;
    board_reset(&b);
    search_init();
    last_move_who[0] = '\0';
    have_engine_info = 0;

    int cursor_sq = 12; // e2
    int selected_sq = -1;
    int ply = 0;

    uart_puts("=== Play vs Engine ===\r\nYou are White. C/D/E/F move cursor, A pick up/confirm.\r\n"
               "B deselects a picked-up piece, or backs out to the menu if nothing's picked up.\r\n"
               "Hold A+B together to see the last move, nodes searched, and eval.\r\n");
    render_board(&b, cursor_sq, selected_sq);

    while (1) {
        if (count_legal_moves(&b) == 0) {
            render_board(&b, -1, -1);
            report_game_end(&b, ply);
            uart_puts("Press B to return to the menu.\r\n");
            while (input_wait_press() != INPUT_CANCEL) {}
            return;
        }

        if (b.side_to_move == WHITE) {
            InputEvent ev = input_wait_press_allow_combo();
            if (ev == INPUT_INFO) {
                show_info_screen();
                render_board(&b, cursor_sq, selected_sq);
                continue;
            }
            int file = cursor_sq % 8, rank = cursor_sq / 8;
            switch (ev) {
                case INPUT_LEFT:  if (file > 0) file--; break;
                case INPUT_RIGHT: if (file < 7) file++; break;
                case INPUT_UP:    if (rank < 7) rank++; break;
                case INPUT_DOWN:  if (rank > 0) rank--; break;
                case INPUT_CANCEL:
                    if (selected_sq >= 0) {
                        selected_sq = -1; // nothing picked up yet: cancel backs out to the menu
                    } else {
                        uart_puts("Returning to menu.\r\n");
                        return;
                    }
                    break;
                case INPUT_SELECT:
                    if (selected_sq < 0) {
                        int8_t piece = b.mailbox[cursor_sq];
                        if (piece > 0) { // White's own piece
                            selected_sq = cursor_sq;
                        } else {
                            uart_puts("Pick one of your own pieces first.\r\n");
                        }
                    } else if (selected_sq == cursor_sq) {
                        selected_sq = -1; // re-confirming the same square deselects it
                    } else {
                        Move m;
                        if (find_legal_move(&b, selected_sq, cursor_sq, &m)) {
                            int8_t moved_piece = b.mailbox[move_from(m)];
                            bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
                            UndoInfo undo;
                            make_move(&b, m, &undo);
                            // Feeds the engine's real in-game repetition
                            // history (search.h) -- without this, the
                            // engine has no way to know a position it's
                            // about to reconsider is one it (or the
                            // player) already reached earlier this game,
                            // and can walk right back into a draw even
                            // from a position it's clearly winning.
                            search_record_move(b.hash, irreversible);
                            report_move("You", m, 0, 0);
                            selected_sq = -1;
                            ply++;
                        } else {
                            uart_puts("Illegal move.\r\n");
                        }
                    }
                    break;
                default: break;
            }
            cursor_sq = rank * 8 + file;
            render_board(&b, cursor_sq, selected_sq);
        } else {
            uart_puts("Engine thinking...\r\n");
            Move m = engine_move(&b, &strength);
            int8_t moved_piece = b.mailbox[move_from(m)];
            bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
            UndoInfo undo;
            make_move(&b, m, &undo);
            search_record_move(b.hash, irreversible);
            report_move("Engine", m, last_best_score, 1);
            ply++;
            render_board(&b, cursor_sq, selected_sq);
        }
    }
}

// ---- Self-play ----

static void self_play(void) {
    EngineStrength strength = choose_engine_strength();

    Board b;
    board_reset(&b);
    search_init();
    last_move_who[0] = '\0';
    have_engine_info = 0;

    uart_puts("=== Self-play ===\r\n"
              "B at any point: stop now, no result.\r\n"
              "A at any point, or the PC viewer's End Game button: end now "
              "and score it (300cp+ ahead wins, else a draw).\r\n");
    render_board(&b, -1, -1);
    broadcast_game_start();

    // Drop anything already sitting in the RX buffer from before this game
    // started -- an End Game press left over from the PREVIOUS self-play
    // session (or any stray byte) must not instantly end this new one.
    uart_rx_init();

    int ply = 0;
    int stopped = 0;
    int adjudicated = 0;
    const char *adjudicated_code = NULL;
    while (1) {
        if (count_legal_moves(&b) == 0) {
            break;
        }
        // Non-blocking: a single button press (or, for the PC viewer's End
        // Game button, a single incoming UART byte) between moves ends
        // things early without making the player wait for or step through
        // every move by hand.
        InputEvent ev = input_poll_edge();
        if (ev == INPUT_CANCEL) {
            uart_puts("Self-play stopped.\r\n");
            stopped = 1;
            break;
        }
        char rx_byte;
        bool end_requested = (ev == INPUT_SELECT) || (uart_try_read_byte(&rx_byte) && rx_byte == 'E');
        if (end_requested) {
            adjudicated_code = adjudicate_result(&b, ply);
            adjudicated = 1;
            uart_puts("Self-play ended early -- adjudicated.\r\n");
            break;
        }

        Move m = engine_move(&b, &strength);
        int8_t moved_piece = b.mailbox[move_from(m)];
        bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
        UndoInfo undo;
        make_move(&b, m, &undo);
        search_record_move(b.hash, irreversible);
        report_move(b.side_to_move == BLACK ? "White" : "Black", m, last_best_score, 1);
        broadcast_move(m, last_best_score);
        ply++;
        render_board(&b, -1, -1);
    }

    if (stopped) {
        broadcast_game_end("STOPPED");
        return; // already asked to leave -- don't make them press B twice
    }
    if (adjudicated) {
        broadcast_game_end(adjudicated_code);
        return; // result already decided and sent -- nothing more to report
    }
    broadcast_game_end(game_end_code(&b));
    report_game_end(&b, ply);
    uart_puts("Press B to return to the menu.\r\n");
    while (input_wait_press() != INPUT_CANCEL) {}
}

// ---- Testing: perft ----

static void run_perft_test(void) {
    // Perft just counts nodes (no search/eval/pruning), so there's no
    // "known correct" ceiling the way there might be for a reference table
    // -- it's self-consistent at any depth, it just gets slow fast: depth 7
    // from the start position is ~3.2M nodes (a few minutes here), depth 8
    // is ~85M (order of an hour or more). SysTick timing (systick.h) stays
    // correct no matter how long it runs. 9 is a generous but not unlimited
    // ceiling -- ask for it and it'll run, just budget real time for it.
    int depth = pick_depth(1, 9, 4);
    uart_puts("Running perft from the start position...\r\n");
    {
        static const char *running_lines[2] = { "Perft running", "please wait" };
        draw_text_screen(running_lines, 2);
    }

    Board b;
    board_reset(&b);

    uint32_t o0 = systick_overflow_count, c0 = SYST_CVR;
    long long nodes = perft(&b, depth, 0);
    uint32_t c1 = SYST_CVR, o1 = systick_overflow_count;

    uint64_t elapsed = systick_elapsed_cycles(o0, c0, o1, c1);
    uint64_t nps = elapsed ? ((uint64_t)nodes * CPU_HZ) / elapsed : 0;

    char line[96];
    uint32_t len = 0;
    append_str(line, &len, "perft depth=");
    append_udec(line, &len, (uint64_t)depth);
    append_str(line, &len, " nodes=");
    append_udec(line, &len, (uint64_t)nodes);
    append_str(line, &len, " nps=");
    append_udec(line, &len, nps);
    append_str(line, &len, "\r\n");
    uart_write(line, len);

    char l0[24], l1[24], l2[24];
    len = 0; append_str(l0, &len, "Perft d="); append_udec(l0, &len, (uint64_t)depth); l0[len] = '\0';
    len = 0; append_str(l1, &len, "nodes="); append_udec(l1, &len, (uint64_t)nodes); l1[len] = '\0';
    len = 0; append_str(l2, &len, "nps="); append_udec(l2, &len, nps); l2[len] = '\0';
    const char *screen_lines[3] = { l0, l1, l2 };
    draw_text_screen(screen_lines, 3);
    uart_puts("Press B to return.\r\n");
    while (input_wait_press() != INPUT_CANCEL) {}
}

// ---- Testing: the 4 Ondsel test positions ----

#define NUM_TEST_POSITIONS 4
static const char *TEST_FENS[NUM_TEST_POSITIONS] = {
    "r2qkb1r/ppp1ppp1/2n4p/3p4/3P1nP1/2N1PN2/PPP2PP1/R2QKB1R b KQkq - 0 1",
    "r2qkb1r/1pp1ppp1/p1n1n2p/1B1p4/3P2P1/2N1PN2/PPP2PP1/R2QK2R w KQkq - 0 1",
    "1r2kb1r/2p1ppp1/p1p1n2p/3p4/3PPqP1/2N2N2/PPP2PP1/1R1Q1RK1 w k - 0 1",
    "1r2kb1r/2p1ppp1/p1p1n2p/3p4/3PP1q1/2NQ1N2/PPP2PP1/1R3RK1 w k - 0 1",
};
static const char *TEST_LABELS[NUM_TEST_POSITIONS] = {
    "8...Ne6?",
    "10.Bxc6+ or Ba4?",
    "14.Qd3 or exd5?",
    "15.exd5 or ...?",
};

// Runs a single one of the 4 named positions at a chosen depth and reports
// move/score/nodes/time/nps, both over UART and as a full OLED screen.
static void run_one_test_position(int idx, int depth) {
    Board b;
    board_load_fen(&b, TEST_FENS[idx]);
    search_init();
    debug_node_count = 0;

    {
        static const char *thinking_lines[2] = { "Thinking", "please wait" };
        draw_text_screen(thinking_lines, 2);
    }

    uint32_t o0 = systick_overflow_count, c0 = SYST_CVR;
    Move m = find_best_move(&b, depth);
    uint32_t c1 = SYST_CVR, o1 = systick_overflow_count;

    uint64_t elapsed = systick_elapsed_cycles(o0, c0, o1, c1);
    uint64_t time_ms = elapsed / (CPU_HZ / 1000UL);
    uint64_t nps = elapsed ? ((uint64_t)debug_node_count * CPU_HZ) / elapsed : 0;

    char mstr[8];
    move_to_str(m, mstr);

    char line[160];
    uint32_t len = 0;
    append_str(line, &len, "pos ");
    append_udec(line, &len, (uint64_t)idx);
    append_str(line, &len, " (");
    append_str(line, &len, TEST_LABELS[idx]);
    append_str(line, &len, "): move=");
    append_str(line, &len, mstr);
    append_str(line, &len, " score=");
    append_sdec(line, &len, last_best_score);
    append_str(line, &len, " nodes=");
    append_udec(line, &len, (uint64_t)debug_node_count);
    append_str(line, &len, " time_ms=");
    append_udec(line, &len, time_ms);
    append_str(line, &len, " nps=");
    append_udec(line, &len, nps);
    append_str(line, &len, "\r\n");
    uart_write(line, len);

    char l0[24], l1[24], l2[24], l3[24];
    len = 0; append_str(l0, &len, "mv="); append_str(l0, &len, mstr); l0[len] = '\0';
    len = 0; append_str(l1, &len, "score="); append_sdec(l1, &len, last_best_score); l1[len] = '\0';
    len = 0; append_str(l2, &len, "nodes="); append_udec(l2, &len, (uint64_t)debug_node_count); l2[len] = '\0';
    len = 0; append_str(l3, &len, "nps="); append_udec(l3, &len, nps); l3[len] = '\0';
    const char *result_lines[4] = { l0, l1, l2, l3 };
    draw_text_screen(result_lines, 4);
    uart_puts("Press B to return.\r\n");
    while (input_wait_press() != INPUT_CANCEL) {}
}

// Submenu listing the 4 named positions by their labels (like the original
// checkpoint 7 report) -- pick one, pick a depth, see its result, land back
// here to pick another.
static void test_positions_menu(void) {
    static const char *items[NUM_TEST_POSITIONS + 1];
    for (int i = 0; i < NUM_TEST_POSITIONS; i++) items[i] = TEST_LABELS[i];
    items[NUM_TEST_POSITIONS] = "Back";

    for (;;) {
        int choice = run_menu("Test positions", items, NUM_TEST_POSITIONS + 1);
        if (choice < 0 || choice == NUM_TEST_POSITIONS) break; // cancel or "Back"
        int depth = pick_depth(1, 6, 4);
        run_one_test_position(choice, depth);
    }
}

static void testing_menu(void) {
    static const char *items[] = { "Perft", "Test positions", "Back" };
    for (;;) {
        int choice = run_menu("Testing", items, 3);
        if (choice == 0) run_perft_test();
        else if (choice == 1) test_positions_menu();
        else break; // -1 (cancel) or "Back"
    }
}

// ---- top-level menu ----

int main(void) {
    P0_DIRSET = (1U << PIN_ROW1) | (1U << PIN_COL1);
    P0_OUTCLR = (1U << PIN_COL1);

    clock_start_hfxo();
    uart_init();
    uart_rx_init();
    systick_init();
    oled_init();
    input_init();
    init_attack_tables();

    uart_puts("microbit v2 Ondsel: starting\r\n");
    P0_OUTSET = (1U << PIN_ROW1);

    {
        static const char *splash_lines[2] = { "Ondsel Chess", "starting..." };
        draw_text_screen(splash_lines, 2);
    }

    static const char *items[] = { "Play vs Engine", "Self-play", "Testing" };
    for (;;) {
        int choice = run_menu("Ondsel Chess", items, 3);
        if (choice == 0) play_vs_engine();
        else if (choice == 1) self_play();
        else if (choice == 2) testing_menu();
        // choice == -1 (cancel at the top level): nothing to cancel to,
        // just redraw the menu on the next loop iteration.
    }

    return 0;
}
