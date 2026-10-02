// ============================================================================
// ATTACK TABLE GENERATION.
// ============================================================================
#include "attacks.h"

Bitboard knight_attacks[64];
Bitboard king_attacks[64];
Bitboard pawn_attacks_white[64];
Bitboard pawn_attacks_black[64];

// Builds all four leaper attack tables by COMPOSING the shift primitives
// from bitboard.h, rather than hand-deriving each knight/king move as a
// magic offset number. The reasoning: shift_north()/shift_east()/etc. are
// simple enough to trust by inspection (and are exercised constantly by
// everything else in the engine, so any bug in them would show up
// everywhere, not hide here). Building a king's attack pattern as "OR
// together all 8 single-step shifts" and a knight's as "OR together all 8
// combinations of two-steps-then-one-step-perpendicular" inherits that same
// correctness for free, instead of introducing a second, independent
// implementation (raw square-offset arithmetic) that could have its own,
// different bug.
void init_attack_tables(void) {
    for (int sq = 0; sq < 64; sq++) {
        Bitboard b = bb_square(sq); // a bitboard with just this one square set

        // KING: one step in each of the 8 compass directions.
        Bitboard k = 0;
        k |= shift_north(b); k |= shift_south(b);
        k |= shift_east(b); k |= shift_west(b);
        k |= shift_north_east(b); k |= shift_north_west(b);
        k |= shift_south_east(b); k |= shift_south_west(b);
        king_attacks[sq] = k;

        // KNIGHT: the classic "L" shape, expressed as 8 separate
        // two-steps-one-way-then-one-step-perpendicular combinations
        // (NNE, NNW, SSE, SSW, ENE, ESE, WNW, WSW). Each name spells out
        // its own composition, e.g. "NNE" = shift north twice, then east
        // once.
        Bitboard n = 0;
        n |= shift_east(shift_north(shift_north(b)));   // NNE
        n |= shift_west(shift_north(shift_north(b)));   // NNW
        n |= shift_east(shift_south(shift_south(b)));   // SSE
        n |= shift_west(shift_south(shift_south(b)));   // SSW
        n |= shift_north(shift_east(shift_east(b)));     // ENE
        n |= shift_south(shift_east(shift_east(b)));     // ESE
        n |= shift_north(shift_west(shift_west(b)));     // WNW
        n |= shift_south(shift_west(shift_west(b)));     // WSW
        knight_attacks[sq] = n;

        // PAWNS: attack diagonally forward only (never straight ahead --
        // that's a push, a completely different kind of move handled
        // separately in movegen.c). "Forward" means opposite directions
        // for the two colors, hence two separate tables.
        pawn_attacks_white[sq] = shift_north_east(b) | shift_north_west(b);
        pawn_attacks_black[sq] = shift_south_east(b) | shift_south_west(b);
    }
}

// Walks one ray of squares from `sq` in a single direction (given as a
// shift-function pointer, e.g. shift_north) until either the board edge or
// an occupied square is hit, returning every square passed through
// (INCLUDING the blocking square itself, if any -- a slider can always
// capture whatever it first runs into, friendly-piece filtering happens
// later in movegen.c, not here).
//
// This is a plain "walk one square at a time and stop at the first
// blocker" implementation -- the conceptually simplest correct way to
// compute a sliding piece's reach, and the natural generalization of "keep
// applying the same shift until something stops you". The standard faster
// alternative is a "magic bitboard": a precomputed perfect-hash lookup
// table that turns this whole walk into one multiply and one array read,
// the same way the leaper tables above turn king/knight moves into a
// single lookup. That table costs real memory, though -- several hundred
// KB even in a compact form -- and staying memory-conservative is a
// genuine hardware constraint on a 128KB-RAM chip, not just a nice-to-have.
// This ray-walk is the correct, working choice for now; magic bitboards
// remain a well-scoped future upgrade once the engine is proven on real
// hardware and memory can be budgeted for it deliberately.
static Bitboard ray(int sq, Bitboard occ, Bitboard (*step)(Bitboard)) {
    Bitboard acc = 0;
    Bitboard cur = bb_square(sq);
    while (1) {
        cur = step(cur);
        if (cur == 0) break;      // fell off the edge of the board
        acc |= cur;
        if (cur & occ) break;     // hit a piece -- can capture it, but no further
    }
    return acc;
}

// A rook slides along ranks and files: the union of all four straight-line
// rays.
Bitboard rook_attacks(int sq, Bitboard occ) {
    return ray(sq, occ, shift_north) | ray(sq, occ, shift_south) |
           ray(sq, occ, shift_east) | ray(sq, occ, shift_west);
}

// A bishop slides along diagonals: the union of all four diagonal rays.
Bitboard bishop_attacks(int sq, Bitboard occ) {
    return ray(sq, occ, shift_north_east) | ray(sq, occ, shift_north_west) |
           ray(sq, occ, shift_south_east) | ray(sq, occ, shift_south_west);
}

// A queen moves like a rook and a bishop combined -- so its attack set is
// simply the union of both.
Bitboard queen_attacks(int sq, Bitboard occ) {
    return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}
