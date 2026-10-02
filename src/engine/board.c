// ============================================================================
// BOARD SETUP: clearing, placing pieces, and loading a position from FEN.
// ============================================================================
// Nothing in this file runs on a hot path -- a position is only ever loaded
// once per game (or once per test position), then make_move()/unmake_move()
// (movegen.c) take over and maintain the board incrementally move by move.
// So unlike search.c/movegen.c, there's no pressure here to avoid ordinary,
// readable C library calls like strchr(); this is exactly the kind of code
// where clarity costs nothing.
// ============================================================================
#include <string.h>
#include <stdio.h>
#include "board.h"
#include "zobrist.h"

// FEN (Forsyth-Edwards Notation) piece letters, in the standard order,
// paired up with this engine's own signed piece-code numbering (positive =
// White, negative = Black -- see board.h's enum). strchr() below finds
// which letter matched; its index into this string is also the index into
// FEN_PIECE_VALUES, so the two arrays are deliberately kept in lockstep.
static const char FEN_PIECE_CHARS[] = "PNBRQKpnbrqk";
static const int8_t FEN_PIECE_VALUES[] = {1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6};

// Wipes a board back to "completely empty, nothing has ever been placed" --
// every mailbox slot 0 (EMPTY), every bitboard 0, no castling rights, no en
// passant square. board_load_fen() always starts from this so a second FEN
// load onto a reused Board struct can never leak stale state from whatever
// position was there before.
void board_clear(Board *b) {
    memset(b, 0, sizeof(*b));
    b->ep_square = -1; // 0 would mean "a1", which is a real square -- -1 means "none"
}

// Puts one piece on one square, in BOTH representations at once (mailbox
// AND the matching bitboard) -- see board.h's comment on why the engine
// keeps both. SETUP ONLY: this assumes the target square is already empty
// and doesn't touch occupancy or the hash, which is fine for building up a
// position square-by-square from a FEN string, but would be wrong mid-game
// (that's what make_move()/unmake_move() in movegen.c are for -- they
// handle removing whatever was already on a square, updating occupancy,
// and maintaining the hash incrementally).
void board_place(Board *b, int sq, int piece_value) {
    b->mailbox[sq] = (int8_t)piece_value;
    int color = piece_value > 0 ? WHITE : BLACK;
    int ptype = piece_value > 0 ? piece_value : -piece_value;
    b->piece_bb[piece_index(color, ptype)] |= bb_square(sq);
}

// Rebuilds the derived occupancy bitboards (board.h's comment explains why
// they're stored rather than recomputed on every query) from piece_bb,
// which is the actual source of truth. Called once after a FEN's pieces
// are all placed, and again after every make_move()/unmake_move() in
// movegen.c, since those touch piece_bb directly and need occupancy to
// reflect the result.
void board_recompute_occupancy(Board *b) {
    b->occ[WHITE] = 0;
    b->occ[BLACK] = 0;
    for (int t = 0; t < 6; t++) b->occ[WHITE] |= b->piece_bb[piece_index(WHITE, t + 1)];
    for (int t = 0; t < 6; t++) b->occ[BLACK] |= b->piece_bb[piece_index(BLACK, t + 1)];
    b->occ_all = b->occ[WHITE] | b->occ[BLACK];
}

// Parses a FEN string into a Board. FEN packs a full position into one line
// of text as six space-separated fields; this engine only needs the first
// four (later engines' search sometimes cares about halfmove-clock/
// fullmove-number too, but this project tracks those separately in each
// host match harness's own game loop instead, since they're about the GAME
// history, not the position itself).
void board_load_fen(Board *b, const char *fen) {
    board_clear(b);
    const char *p = fen;
    int rank = 7, file = 0; // FEN starts describing rank 8 (rank index 7), a-file first

    // --- Field 1: piece placement, rank 8 down to rank 1, each rank
    // separated by '/'. A digit N means "N empty squares here"; anything
    // else is a piece letter.
    while (*p && *p != ' ') {
        char ch = *p++;
        if (ch == '/') { rank--; file = 0; continue; }
        if (ch >= '1' && ch <= '8') { file += ch - '0'; continue; }
        const char *pc = strchr(FEN_PIECE_CHARS, ch);
        if (pc) {
            int idx = (int)(pc - FEN_PIECE_CHARS);
            board_place(b, rank * 8 + file, FEN_PIECE_VALUES[idx]);
        }
        file++;
    }
    board_recompute_occupancy(b);
    if (*p == ' ') p++;

    // --- Field 2: side to move ('w' or 'b').
    b->side_to_move = (*p == 'b') ? BLACK : WHITE;
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;

    // --- Field 3: castling rights -- any combination of K/Q/k/q, or '-'
    // for none. Order in the string doesn't matter; each letter just turns
    // on the matching right.
    b->castle_wk = b->castle_wq = b->castle_bk = b->castle_bq = false;
    while (*p && *p != ' ') {
        if (*p == 'K') b->castle_wk = true;
        else if (*p == 'Q') b->castle_wq = true;
        else if (*p == 'k') b->castle_bk = true;
        else if (*p == 'q') b->castle_bq = true;
        p++;
    }
    if (*p == ' ') p++;

    // --- Field 4: en passant target square in algebraic notation (e.g.
    // "e3"), or '-' for none.
    b->ep_square = -1;
    if (*p && *p != '-' && *p != ' ') {
        int ep_file = p[0] - 'a';
        int ep_rank = p[1] - '1';
        if (ep_file >= 0 && ep_file <= 7 && ep_rank >= 0 && ep_rank <= 7) {
            b->ep_square = ep_rank * 8 + ep_file;
        }
    }

    // Zobrist's random tables are lazily filled in on first use (a single
    // splitmix64-seeded pass -- see zobrist.c) rather than requiring every
    // one of this project's many entry points (main_play_test.c,
    // main_perft.c, every host test/match harness) to separately remember
    // to call zobrist_init() before doing anything else. One guaranteed
    // init site here beats a dozen easy-to-forget ones scattered
    // elsewhere.
    static bool zobrist_ready = false;
    if (!zobrist_ready) { zobrist_init(); zobrist_ready = true; }

    // From here on, make_move()/unmake_move() maintain this hash
    // incrementally (a handful of XORs per move) -- computing it from
    // scratch is only ever done here, once per loaded position, since FEN
    // loading itself is rare (once per game) and doesn't need to be fast.
    b->hash = zobrist_hash_from_scratch(b);
}

static const char START_FEN[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Sets up the standard chess starting position. Just board_load_fen() with
// a hardcoded FEN -- given as its own function because "reset to the start
// of a new game" is a common enough operation to deserve a name that says
// so, rather than making every caller carry the FEN string around.
void board_reset(Board *b) {
    board_load_fen(b, START_FEN);
}

// Debug-only: renders the board as plain text (uppercase = White,
// lowercase = Black, '.' = empty), one row per rank, rank 8 first. Not
// used by search or move generation at all -- purely a human-readability
// aid for anyone eyeballing a position while debugging on a host machine
// (there's no room, and no real use, for this on the micro:bit's own OLED
// output, which uses board_render.h's dedicated graphical renderer
// instead).
void board_to_text(const Board *b, char *out, size_t out_size) {
    static const char DISP_WHITE[] = ".PNBRQK";
    static const char DISP_BLACK[] = ".pnbrqk";
    size_t n = 0;
    for (int r = 7; r >= 0 && n + 17 < out_size; r--) {
        for (int f = 0; f < 8; f++) {
            int pv = b->mailbox[r * 8 + f];
            char ch = pv < 0 ? DISP_BLACK[-pv] : DISP_WHITE[pv];
            out[n++] = ch;
            out[n++] = ' ';
        }
        out[n++] = '\n';
    }
    out[n] = '\0';
}
