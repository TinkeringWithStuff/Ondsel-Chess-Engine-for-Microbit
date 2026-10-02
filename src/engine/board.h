// ============================================================================
// BOARD REPRESENTATION -- what a "chess position" actually is, in memory.
// ============================================================================
//
// This engine represents a position TWICE, redundantly, at the same time:
//
//   1. `mailbox[64]`: a plain array, one slot per square, holding a signed
//      piece code (+1..+6 for White pawn..king, -1..-6 for Black, 0 for
//      empty). This is the "obvious" representation -- given a square, it
//      answers "what's on it?" in one array read. Humans (and debug
//      printers) think in these terms.
//
//   2. `piece_bb[12]`: twelve bitboards (see bitboard.h), one per
//      (color, piece type) combination -- "where are White's pawns",
//      "where is the Black king", etc. This is the representation the
//      move generator and attack-detection code actually compute with,
//      because bitboard operations answer whole-board questions ("which
//      squares does this rook attack") in a handful of instructions
//      instead of a 64-iteration loop.
//
// Why keep both instead of picking one? Different questions are cheap in
// different representations. "What's on e4?" is instant with mailbox and
// mildly annoying with bitboards (you'd have to test all 12 bitboards at
// that square). "Generate all of White's rook moves" is instant with
// bitboards and would mean scanning the whole mailbox array with bitboards
// too. Both make_move() and unmake_move() (movegen.c) keep the two views
// in sync on every move, so the small bookkeeping cost is paid once per
// move instead of once per query.
//
// `occ[2]` and `occ_all` are a third, DERIVED view: "all of White's
// pieces", "all of Black's pieces", and "every occupied square", each just
// the OR of the six bitboards belonging to that color (or both colors).
// These get asked constantly during move generation ("can this piece move
// here, or is it blocked/occupied by my own piece") and during sliding-
// attack generation (attacks.c's ray-walk needs to know the *combined*
// occupancy to know where to stop), so they're kept as their own field
// rather than recomputed from piece_bb every time they're needed.
// ============================================================================
#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include "bitboard.h"

enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum { WHITE = 0, BLACK = 1 };

typedef struct {
    int8_t mailbox[64];          // signed piece code, +1..+6 white, -1..-6 black, 0 empty
    Bitboard piece_bb[12];       // pieceIndex(color,type) = color*6 + (type-1)
    Bitboard occ[2];             // white/black occupancy, derived from piece_bb
    Bitboard occ_all;            // occ[WHITE] | occ[BLACK], derived

    int side_to_move;            // WHITE or BLACK: whose turn it is

    // Castling rights. Each stays true until either that king or that
    // specific rook has moved (or that rook gets captured) -- see
    // movegen.c's update_castling_rights_for_square(), called on every
    // move's `from` AND `to` square (the `to` check is what catches a rook
    // being captured on its home square without ever having moved itself).
    bool castle_wk, castle_wq, castle_bk, castle_bq;

    // The square a pawn just double-stepped THROUGH, if the immediately
    // preceding move was a two-square pawn push -- i.e. the one square an
    // enemy pawn could capture onto via en passant right now. -1 if the
    // last move wasn't a double push (en passant is only ever legal on the
    // very next move after the double push that created the opportunity).
    int ep_square;

    // Zobrist hash: a single 64-bit number that (with overwhelmingly high
    // probability) uniquely identifies this exact position -- same
    // pieces, same side to move, same castling rights, same en passant
    // square. See zobrist.h for how it's built and why XOR is the right
    // operation for maintaining it. Computed from scratch once, by
    // board_load_fen(); after that, make_move()/unmake_move() maintain it
    // incrementally (a handful of XORs per move, not a full recompute),
    // since that's the hash actually checked millions of times per search
    // for repetition detection and would be far too slow to rebuild from
    // scratch at every node.
    uint64_t hash;
} Board;

// Where a (color, piece_type) bitboard lives in piece_bb[12]: white pieces
// occupy indices 0-5 (pawn..king), black pieces 6-11, in the same
// pawn..king order. This is just an arbitrary but consistent numbering --
// nothing about chess requires this layout, it's simply a convention this
// whole codebase agrees on.
static inline int piece_index(int color, int piece_type) { return color * 6 + (piece_type - 1); }

void board_clear(Board *b);
void board_place(Board *b, int sq, int piece_value);   // setup only, target must be empty
void board_recompute_occupancy(Board *b);
void board_load_fen(Board *b, const char *fen);
void board_reset(Board *b);
void board_to_text(const Board *b, char *out, size_t out_size); // debug dump, ~600 bytes

#endif
