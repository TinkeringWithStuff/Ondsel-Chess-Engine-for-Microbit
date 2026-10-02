// ============================================================================
// ATTACK TABLES -- "which squares can a piece of this type, on this square,
// reach?" -- the question every other part of the engine (move generation,
// king-safety/check detection, SEE, mobility eval) ultimately boils down to.
// ============================================================================
//
// Chess pieces fall into two very different categories for this purpose:
//
//   LEAPERS (king, knight, and -- with a slight twist -- pawns): their
//   reach never depends on what else is on the board. A knight on e4
//   always attacks the same 8 squares whether the board is empty or full.
//   So their attack patterns can be precomputed ONCE, for all 64 starting
//   squares, into a lookup table -- see the four `extern Bitboard [64]`
//   tables below, filled in once by init_attack_tables() and then just
//   read for the rest of the program's life. Table lookup is as fast as
//   attack generation gets: one array read, no computation at all.
//
//   SLIDERS (bishop, rook, queen): their reach DOES depend on the board --
//   a rook's reach along a file stops at the first piece (friendly or
//   enemy) in its way. That can't be precomputed independent of the
//   position, so rook_attacks()/bishop_attacks()/queen_attacks() take the
//   current occupancy bitboard as a parameter and compute the answer
//   fresh each time (attacks.c's ray-walk). This is the one place this
//   engine pays a real per-call cost instead of a table lookup -- see
//   attacks.c's own comment on magic bitboards, the standard technique for
//   turning even this into a lookup, and why it wasn't used here.
// ============================================================================
#ifndef ATTACKS_H
#define ATTACKS_H

#include "bitboard.h"

// Precomputed leaper attack tables, indexed by the piece's own square.
// pawn_attacks_white[sq] means "the squares a WHITE pawn on sq attacks"
// (diagonally forward, toward higher ranks); pawn_attacks_black[sq] is the
// mirror image (diagonally forward for Black, toward lower ranks). Pawns
// need their own two tables rather than sharing one, since -- unlike every
// other piece -- a pawn's attack direction depends on its color.
extern Bitboard knight_attacks[64];
extern Bitboard king_attacks[64];
extern Bitboard pawn_attacks_white[64];
extern Bitboard pawn_attacks_black[64];

// Fills in all four leaper tables above. Must be called once, before
// anything else in the engine runs -- every entry point (main_play_test.c,
// main_perft.c, every host test/match harness) calls this first thing.
void init_attack_tables(void);

// Slider attacks: computed fresh from the current occupancy every call
// (see attacks.c's ray()), since -- unlike the leaper tables above -- a
// slider's reach genuinely depends on what's blocking it right now.
Bitboard rook_attacks(int sq, Bitboard occ);
Bitboard bishop_attacks(int sq, Bitboard occ);
Bitboard queen_attacks(int sq, Bitboard occ); // queen = rook + bishop combined

#endif
