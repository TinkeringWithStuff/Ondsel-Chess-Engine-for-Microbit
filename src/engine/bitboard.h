// ============================================================================
// BITBOARDS -- the foundational data structure this whole engine is built on.
// ============================================================================
//
// THE BIG IDEA:
// A "bitboard" represents one fact about the 64 squares of a chess board
// using a single 64-bit integer -- one bit per square, bit N set (1) means
// "this fact is true on square N", clear (0) means "false". A chess engine
// keeps a separate bitboard for each (color, piece type) pair -- e.g. one
// bitboard just for "where are White's pawns", another for "where is the
// Black king" -- twelve in total (see board.h's piece_bb[12]).
//
// WHY THIS BEATS A NAIVE "8x8 ARRAY OF PIECE CODES" DESIGN:
// With an 8x8 array, answering "which squares does this piece attack" or
// "is this square attacked by anything" means looping over up to 64 squares
// checking each one by hand. With bitboards, the same questions become
// single machine instructions operating on ALL 64 squares simultaneously:
//   - "are there any white pawns at all?"      -> is the bitboard nonzero?
//   - "how many black knights are on the board?" -> popcount (one CPU op)
//   - "which squares do these rooks attack?"   -> a handful of bitwise
//                                                 shifts and ORs (see
//                                                 attacks.c)
//   - "is square X attacked by anything?"      -> AND the attack pattern
//                                                 against the enemy
//                                                 bitboards, test for zero
// This is the single biggest reason bitboard engines are fast: operations
// that would be O(64) loops in an array-based engine become O(1) bitwise
// arithmetic here, and modern CPUs (including the Cortex-M4 this project
// targets) have dedicated single-cycle instructions for exactly this kind
// of work (population count, count-leading/trailing-zeros, etc.).
//
// SQUARE NUMBERING: square = rank*8 + file (file 0 = a, rank 0 = rank 1),
// so a1 = 0, h1 = 7, a8 = 56, h8 = 63. Bit N of a Bitboard corresponds to
// square N. "North" (toward Black's side, increasing rank) is +8; "east"
// (toward the h-file) is +1.
//
// A NOTE ON PROJECT HISTORY, FOR ANYONE COMPARING THIS TO THE OLDER
// JAVASCRIPT (MAKECODE) VERSION OF ONDSEL:
// That version had to split every 64-bit board into two separate 32-bit
// halves (Lo/Hi) with hand-written carry logic, because MakeCode's
// JavaScript-derived bytecode interpreter only has 32-bit bitwise
// operators. None of that plumbing is real chess-engine design -- it was
// working around one specific interpreter's limitation. Plain C has real
// 64-bit integers (uint64_t), so this port just uses them directly; the
// compiler (not a human) generates whatever multi-instruction sequence the
// target CPU needs under the hood, and does it without introducing a
// hand-written bug in the process.
// ============================================================================
#ifndef BITBOARD_H
#define BITBOARD_H

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t Bitboard;

// File/rank masks: a Bitboard with every square of one file or rank set.
// Used constantly to stop a piece's move from "wrapping around" the edge
// of the board -- e.g. a rook shifted one square east from h4 would land
// on a5 if we didn't explicitly mask h-file pieces out first (see
// shift_east() below).
#define FILE_A 0x0101010101010101ULL
#define FILE_H 0x8080808080808080ULL
#define NOT_FILE_A (~FILE_A)
#define NOT_FILE_H (~FILE_H)
#define RANK_1 0x00000000000000FFULL
#define RANK_8 0xFF00000000000000ULL

// ---------------------------------------------------------------------------
// Bit-counting primitives.
//
// These three operations -- "how many bits are set", "index of the lowest
// set bit", and "index of the lowest set bit, then clear it" -- are used
// so often (once per piece per move-generation call, many times per search
// node) that their speed matters more than almost anything else in the
// engine. All three compile to a single hardware instruction on a real
// CPU, which is the entire point of representing the board this way.
// ---------------------------------------------------------------------------

// Counts how many bits are set -- i.e. how many pieces are on this
// bitboard. __builtin_popcountll is a compiler intrinsic that becomes one
// POPCNT-equivalent instruction on hardware that has one; used here for
// things like "how much non-pawn material does each side have" (eval.c's
// endgame detection).
static inline int bb_popcount(Bitboard b) {
    return __builtin_popcountll(b);
}

// Index (0-63) of the least-significant set bit -- i.e. "find me A piece
// on this bitboard, I don't care which one, just give me its square".
// UNDEFINED if b == 0 (there is no lowest set bit in an empty board) --
// every caller in this codebase already knows the bitboard it's asking
// about is nonempty (e.g. "the king bitboard" is never empty mid-game)
// before calling this.
//
// IMPLEMENTATION NOTE (why this isn't just __builtin_ctzll(b)): that
// builtin is correct on any target, but the nRF52833 (Cortex-M4) has no
// native 64-bit "count trailing zeros" instruction, so the compiler has to
// lower a 64-bit ctz into an actual function call into its own support
// library (__ctzdi2) -- a real call/return, not a single instruction. This
// project's own on-hardware profiling found that call to be the single
// hottest line in the whole engine, because bb_pop_lsb() (below) calls it
// once per legal move considered, at every node. The Cortex-M4 DOES have
// single-cycle 32-bit RBIT (bit-reverse) + CLZ (count-leading-zeros)
// instructions, and __builtin_ctz (no "ll") on a plain 32-bit value
// compiles straight to that pair with no function call at all. So: split
// the 64-bit board into its low and high 32-bit halves by hand, and run
// the fast 32-bit builtin on whichever half actually has the bit we want.
// Same answer as ctzll, entirely inline.
static inline int bb_lsb_index(Bitboard b) {
    uint32_t lo = (uint32_t)b;
    if (lo) {
        return __builtin_ctz(lo);
    }
    uint32_t hi = (uint32_t)(b >> 32);
    return 32 + __builtin_ctz(hi);
}

// The classic "pop lsb" idiom: read the index of the lowest set bit, AND
// clear that bit from the board, in one step. This is how every move
// generator in this project walks "for each piece of this type, do X" --
// see movegen.c's `while (bb) { int sq = bb_pop_lsb(&bb); ... }` loops.
// `*b &= *b - 1` is the standard trick for clearing the lowest set bit:
// subtracting 1 flips every trailing zero to 1 and the lowest set bit to
// 0, so ANDing with the original clears exactly that one bit and nothing
// else.
static inline int bb_pop_lsb(Bitboard *b) {
    int idx = bb_lsb_index(*b);
    *b &= *b - 1;
    return idx;
}

// Is square `sq` set on this bitboard?
static inline bool bb_test_bit(Bitboard b, int sq) {
    return (b >> sq) & 1ULL;
}

// A bitboard with exactly one bit set -- the one at square `sq`. Used to
// build up or test against individual squares (e.g. "is the en passant
// square attacked").
static inline Bitboard bb_square(int sq) {
    return 1ULL << sq;
}

// ---------------------------------------------------------------------------
// Directional shifts -- "take every piece on this bitboard and slide it
// one square in direction X, all at once".
//
// These are the actual building blocks every other piece of move-related
// code is made from: king/knight/pawn attack tables (attacks.c) are built
// by composing a handful of these; sliding-piece attacks (bishops, rooks,
// queens) are generated by repeatedly applying one of these in a loop
// until a piece or the board edge is hit (attacks.c's ray()).
//
// THE WRAP-AROUND PROBLEM, AND WHY EVERY EAST/WEST SHIFT MASKS A FILE
// FIRST: a bitboard is just one flat 64-bit number, but the board it
// represents wraps around every 8 bits (one rank). Shifting the whole
// number left or right by 1 to mean "move everything one square east/
// west" is only correct for pieces that AREN'T on the edge file being
// shifted toward -- a piece on h4 (bit 31) shifted "east" by the naive
// `b << 1` lands on bit 32, which is a1... sorry, a5 -- the far edge of
// the NEXT rank, not off the board where it should have vanished. Masking
// with NOT_FILE_H *before* shifting east removes any h-file bits first, so
// they simply disappear instead of wrapping onto the next rank. North/
// south shifts (`<< 8` / `>> 8`) never have this problem, since a whole
// rank shifting by exactly 8 bits can never spill into a different file.
// ---------------------------------------------------------------------------
static inline Bitboard shift_north(Bitboard b) { return b << 8; }
static inline Bitboard shift_south(Bitboard b) { return b >> 8; }
static inline Bitboard shift_east(Bitboard b) { return (b & NOT_FILE_H) << 1; }
static inline Bitboard shift_west(Bitboard b) { return (b & NOT_FILE_A) >> 1; }
static inline Bitboard shift_north_east(Bitboard b) { return (b & NOT_FILE_H) << 9; }
static inline Bitboard shift_north_west(Bitboard b) { return (b & NOT_FILE_A) << 7; }
static inline Bitboard shift_south_east(Bitboard b) { return (b & NOT_FILE_H) >> 7; }
static inline Bitboard shift_south_west(Bitboard b) { return (b & NOT_FILE_A) >> 9; }

#endif
