// ============================================================================
// STATIC EXCHANGE EVALUATION -- simulating a whole capture sequence on one
// square using only material values, no real search.
// ============================================================================
//
// THE ALGORITHM, IN PLAIN TERMS:
// Imagine every piece that could possibly capture on the target square,
// from BOTH sides, lined up in increasing order of value (pawns first,
// then knights/bishops, then rooks, then the queen, then the king last --
// each side always wants to use its CHEAPEST attacker first, since that
// risks the least material if the exchange stops there). The two sides
// then alternate "capturing" with their next-cheapest piece, one at a
// time, for as long as either side still has an attacker AND still wants
// to continue. "Still wants to continue" is the whole trick: a rational
// player stops recapturing the moment doing so would lose material rather
// than gain it -- so after simulating the full chain of possible captures,
// the code walks it back to front doing exactly what a real player would:
// at each step, choose the better of "stop here" vs. "let the next
// exchange happen", exactly the same MINIMAX idea search.c's negamax uses
// for the whole game tree, just applied to this one tiny, purely material
// sub-problem instead.
//
// THE X-RAY DETAIL: after an attacker is "used", removing it from the
// occupancy bitboard (not just noting it's been used) matters because it
// can reveal a SLIDING piece that was standing behind it on the same
// line -- e.g. a rook behind a knight, both defending the same square: the
// knight has to be captured (and removed from the board) before the rook's
// own attack on that square becomes visible to bishop_attacks()/
// rook_attacks(). Using a real, shrinking copy of the occupancy bitboard
// (rather than some other kind of "already used" tracking) is what makes
// this x-ray effect fall out automatically, for free, from the same attack
// functions the rest of the engine already uses.
// ============================================================================
#include "see.h"
#include "eval.h"
#include "attacks.h"

int see(const Board *b, int from, int to, bool is_ep) {
    int gain[32]; // gain[d] = net material change after the d-th capture in the sequence
    Bitboard occ = b->occ_all;
    int mover = b->mailbox[from];

    // En passant's victim is never on `to` -- it's the pawn just behind
    // it, still sitting where it landed on its double push (one rank below
    // `to` for a white capturer, one rank above for a black one). For
    // every other kind of capture, b->mailbox[to] IS exactly the piece
    // being captured.
    int captured_sq = to;
    if (is_ep) captured_sq = (mover > 0) ? to - 8 : to + 8;
    int target = b->mailbox[captured_sq];

    // gain[0]: the very first capture's value -- whatever sits on the
    // actual victim square. (target == EMPTY would only happen if this
    // were called on a non-capturing move, which no caller in this
    // codebase does.)
    gain[0] = target == EMPTY ? 0 : PIECE_VALUE[target > 0 ? target : -target];
    int current_attacker_value = PIECE_VALUE[mover > 0 ? mover : -mover];

    occ &= ~bb_square(from); // the first attacker has now "moved" onto the target square
    // The captured pawn actually leaves the board on an en passant
    // capture, which can unmask a slider that was standing behind IT too
    // -- same reasoning as every other removal in this loop clearing occ,
    // not just the mailbox array (which this function doesn't even touch;
    // it works entirely off bitboards and PIECE_VALUE).
    if (is_ep) occ &= ~bb_square(captured_sq);
    int side = mover > 0 ? BLACK : WHITE; // whoever recaptures next
    int d = 0;

    // Build the full exchange sequence: repeatedly find the CHEAPEST
    // remaining attacker the side-to-recapture has on `to`, "play" that
    // capture (record the running gain, swap sides, shrink the occupancy),
    // and stop only when a side has no attacker left at all.
    while (1) {
        int attacker_sq = -1;
        int attacker_val = 0;

        // Check attacker types cheapest-first: pawn, knight, bishop, rook,
        // queen, king. The first one found is used -- exactly the
        // "recapture with your least valuable piece" assumption the whole
        // algorithm is built on.
        Bitboard pawn_attackers = (side == WHITE)
            ? (pawn_attacks_black[to] & b->piece_bb[piece_index(WHITE, PAWN)] & occ)
            : (pawn_attacks_white[to] & b->piece_bb[piece_index(BLACK, PAWN)] & occ);
        if (pawn_attackers) {
            attacker_sq = bb_lsb_index(pawn_attackers); attacker_val = PIECE_VALUE[PAWN];
        } else {
            Bitboard knights = knight_attacks[to] & b->piece_bb[piece_index(side, KNIGHT)] & occ;
            if (knights) {
                attacker_sq = bb_lsb_index(knights); attacker_val = PIECE_VALUE[KNIGHT];
            } else {
                Bitboard bishops = bishop_attacks(to, occ) & b->piece_bb[piece_index(side, BISHOP)] & occ;
                if (bishops) {
                    attacker_sq = bb_lsb_index(bishops); attacker_val = PIECE_VALUE[BISHOP];
                } else {
                    Bitboard rooks = rook_attacks(to, occ) & b->piece_bb[piece_index(side, ROOK)] & occ;
                    if (rooks) {
                        attacker_sq = bb_lsb_index(rooks); attacker_val = PIECE_VALUE[ROOK];
                    } else {
                        Bitboard queens = queen_attacks(to, occ) & b->piece_bb[piece_index(side, QUEEN)] & occ;
                        if (queens) {
                            attacker_sq = bb_lsb_index(queens); attacker_val = PIECE_VALUE[QUEEN];
                        } else {
                            Bitboard kings = king_attacks[to] & b->piece_bb[piece_index(side, KING)] & occ;
                            if (kings) { attacker_sq = bb_lsb_index(kings); attacker_val = PIECE_VALUE[KING]; }
                        }
                    }
                }
            }
        }

        if (attacker_sq < 0) break; // this side has nothing left that can recapture -- exchange is over

        d++;
        // This capture's net gain, from the CURRENT recapturer's
        // perspective, is "the value of the piece I'm capturing" minus
        // "the value of the piece I'm using to do it" -- because that
        // attacker is now sitting on the target square too, itself
        // capturable by whatever the other side plays next.
        gain[d] = current_attacker_value - gain[d - 1];
        current_attacker_value = attacker_val;
        occ &= ~bb_square(attacker_sq); // this attacker has "moved" onto the target square; remove it from occ (may reveal an x-ray attacker behind it)
        side = side == WHITE ? BLACK : WHITE;
    }

    // Walk the simulated sequence back to front, applying real minimax
    // logic: at each point, a rational player only continues the exchange
    // if doing so is BETTER than stopping. `gain[d-1]` represents "the
    // value of stopping the exchange one capture earlier"; comparing it
    // against `-gain[d]` (the continuation's value, negated -- the same
    // negamax sign-flip search.c's real search uses, since gain[] was
    // built from alternating sides' perspectives) and keeping the better
    // of the two collapses the whole simulated exchange down to a single
    // final number: the actual best result for whoever captures first,
    // assuming perfectly rational recapture decisions on both sides.
    while (d > 0) {
        int a = -gain[d - 1];
        int bb = gain[d];
        gain[d - 1] = -(a > bb ? a : bb);
        d--;
    }

    return gain[0];
}
