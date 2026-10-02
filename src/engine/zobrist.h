// ============================================================================
// ZOBRIST HASHING -- turning a whole chess position into one 64-bit number
// that (almost certainly) uniquely identifies it.
// ============================================================================
//
// THE PROBLEM THIS SOLVES:
// The engine needs to answer "have I already reached this exact position
// earlier in this game (or in the hypothetical line I'm searching right
// now)?" -- that's what detects repetition draws (search.c's
// is_repetition()), and it's the same question a transposition table would
// need answered to detect "I've already analyzed this position via a
// different move order". Comparing two full Board structs field-by-field
// every time would work, but is far too slow to do millions of times per
// search. What's needed instead is a cheap FINGERPRINT: a single number
// that's cheap to compute, cheap to compare, and -- crucially -- cheap to
// keep up to date as the position changes one move at a time.
//
// THE TRICK: XOR each of the position's "facts" into one running 64-bit
// number:
//   - one random 64-bit number per (piece, square) combination that's
//     currently occupied (zobrist_piece[][])
//   - one random number XORed in whenever it's Black's turn (zobrist_side)
//   - one random number for the current castling-rights combination
//     (zobrist_castle[])
//   - one random number for the current en passant file, if any
//     (zobrist_ep_file[])
// XOR has a magic property that makes this work: XORing the same value in
// TWICE cancels out to a no-op (A XOR X XOR X == A). That means "remove
// this piece from this square" and "add this piece to this square" are the
// EXACT SAME OPERATION (XOR that piece/square's random number into the
// hash) -- removing is just adding again, since two consecutive XORs of
// the same value cancel. This is what makes the hash maintainable
// INCREMENTALLY: make_move() doesn't need to know how to "undo" a hash
// contribution, because XORing the departure square, the arrival square,
// and (if applicable) a captured piece's square, in any order, produces
// exactly the same final hash as computing it from scratch on the new
// position. unmake_move() doesn't even need to replay any of that logic in
// reverse -- it just restores the pre-move hash value that was snapshotted
// before the move was made (see movegen.h's UndoInfo).
//
// WHY THIS IS ONLY "ALMOST CERTAINLY" UNIQUE: with 64 bits of randomness,
// two genuinely different positions colliding to the same hash by sheer
// chance is astronomically unlikely (on the order of 1 in 2^64) but not
// mathematically impossible. Every real chess engine accepts this trade-off
// -- the alternative (comparing full board state) is far too slow to be
// worth guarding against a probability this small.
// ============================================================================
#ifndef ZOBRIST_H
#define ZOBRIST_H

#include <stdint.h>
#include "board.h"

extern uint64_t zobrist_piece[12][64];   // [piece_index(color,type)][square]
extern uint64_t zobrist_side;             // XORed in whenever it's Black's turn
extern uint64_t zobrist_castle[16];       // indexed by (wk<<3|wq<<2|bk<<1|bq)
extern uint64_t zobrist_ep_file[8];       // indexed by file only (rank is implied by side to move)

// Fills every table above with a FIXED, deterministic pseudo-random
// sequence (see zobrist.c's splitmix64) -- deterministic on purpose. The
// actual bit patterns don't need to be "truly" random, only spread out
// enough that unrelated positions essentially never collide; what matters
// far more is that they're the SAME every run, on every machine, so a
// host-computed hash always agrees with the embedded build's hash, and a
// recorded match/PGN can be reproduced exactly.
//
// Must be called once, before any position is loaded -- board_load_fen()
// takes care of this automatically on first use, so callers don't need to
// remember a separate init step.
void zobrist_init(void);

// Computes a position's hash directly from its actual current fields
// (mailbox contents, side to move, castling rights, en passant square),
// completely independent of any move history. This is the "ground truth"
// used exactly twice: once to seed Board.hash when a position is first
// loaded from FEN (board_load_fen() has no prior move to derive a hash
// from incrementally), and available to any test that wants to check the
// incrementally-maintained hash hasn't drifted from what a fresh
// from-scratch computation would give.
uint64_t zobrist_hash_from_scratch(const Board *b);

#endif
