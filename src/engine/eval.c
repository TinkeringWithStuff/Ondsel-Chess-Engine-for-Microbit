// ============================================================================
// STATIC EVALUATION: material + piece-square tables, plus two optional,
// currently-disabled terms (mobility, endgame heuristics -- see eval.h).
// ============================================================================
// The material+PST values here are a "simplified evaluation function" --
// hand-tuned numbers, not learned or auto-tuned, but sourced from widely
// used chess-programming reference values and already exercised on real
// hardware by this engine's own earlier checkpoints. There's no principled
// reason to re-tune them from scratch; they're a solid, well-understood
// baseline, and any future improvement to evaluation should be A/B tested
// against exactly this baseline the same way null-move pruning is being
// tested against the plain search.
// ============================================================================
#include "eval.h"
#include "bitboard.h"
#include "attacks.h"

// Standard approximate values in centipawns (1 pawn = 100). Note the queen
// isn't simply "3x a rook" or similar -- these are the conventional
// chess-programming values, chosen because a queen is disproportionately
// powerful for its nominal "material count", not because of any formula.
// The king's "value" (20000) is never actually used in material scoring
// (both sides always have exactly one, so it cancels out) -- it exists
// here only so PIECE_VALUE[KING] is a large, safely-nonzero number if
// anything (e.g. see.c, when the king itself is the cheapest available
// "attacker" in a simulated exchange) ever indexes into this table with a
// king's piece type.
const int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 20000};

int eval_mobility_enabled = 0;

// ---------------------------------------------------------------------------
// MOBILITY: "how many squares does this side attack, total" (see eval.h's
// comment on why this is a useful proxy for piece activity).
//
// IMPORTANT SIMPLIFICATION: this counts every square a piece COULD move
// to as if the move were pseudo-legal, not real legal-move mobility. A
// pinned piece's "attacks" still count here, and a king's own nominally-
// attacked squares include ones actually defended by the enemy (moving
// there would be illegal, but this function doesn't know that). That's a
// deliberate cost trade-off, not an oversight: computing TRUE legal
// mobility would mean generating every pseudo-move and running the full
// make-move/king-safety-check/unmake cycle on each one -- roughly as
// expensive as a whole extra search node -- and this function gets called
// on the large majority of nodes the whole search visits (evaluate() also
// backs quiescence's stand-pat check). The cheap approximation used here
// -- plain bitwise OR + popcount over the same attack tables
// is_square_attacked()/see() already use, no per-move loop, no make/unmake
// at all -- is the standard trade-off real engines make for exactly this
// reason.
// ---------------------------------------------------------------------------
static Bitboard attacked_squares(const Board *b, int color) {
    Bitboard occ = b->occ_all;
    Bitboard attacked = 0;

    Bitboard pawns = b->piece_bb[piece_index(color, PAWN)];
    while (pawns) {
        int sq = bb_pop_lsb(&pawns);
        attacked |= (color == WHITE) ? pawn_attacks_white[sq] : pawn_attacks_black[sq];
    }
    Bitboard knights = b->piece_bb[piece_index(color, KNIGHT)];
    while (knights) attacked |= knight_attacks[bb_pop_lsb(&knights)];
    Bitboard bishops = b->piece_bb[piece_index(color, BISHOP)];
    while (bishops) attacked |= bishop_attacks(bb_pop_lsb(&bishops), occ);
    Bitboard rooks = b->piece_bb[piece_index(color, ROOK)];
    while (rooks) attacked |= rook_attacks(bb_pop_lsb(&rooks), occ);
    Bitboard queens = b->piece_bb[piece_index(color, QUEEN)];
    while (queens) attacked |= queen_attacks(bb_pop_lsb(&queens), occ);
    Bitboard kings = b->piece_bb[piece_index(color, KING)];
    while (kings) attacked |= king_attacks[bb_pop_lsb(&kings)];

    return attacked;
}

// ---------------------------------------------------------------------------
// ENDGAME HEURISTICS: four textbook chess principles the plain material +
// PST evaluation below has no concept of at all (the only "endgame
// awareness" it has otherwise is swapping which king PST is used -- see
// pst_for()). Each one is a well-known rule any chess-programming
// introduction covers:
//   1. A passed pawn (no enemy pawn can ever stop or capture it by
//      advancing straight or diagonally) is dangerous, and more dangerous
//      the further it's already advanced.
//   2. In the endgame specifically (not the middlegame, where the king
//      wants safety instead), the king should actively move toward the
//      action -- escorting its own passed pawns forward, or rushing to
//      blockade the opponent's.
//   3. A rook is more active on a file with no pawns blocking its view
//      (semi-open: no OWN pawn on the file; fully open: no pawns from
//      EITHER side).
//   4. The Tarrasch rule: a rook belongs BEHIND a passed pawn on the same
//      file -- behind its own pawn, to support pushing it forward; behind
//      an enemy pawn, to blockade/chase it as it advances. "Behind" is
//      deliberately colorblind here: the geometry is identical for either
//      side, only which side benefits changes.
// ---------------------------------------------------------------------------
int eval_endgame_heuristics_enabled = 0;

// Precomputed once: passed_pawn_mask[color][sq] is every square, on the
// files immediately adjacent to and including `sq`'s own file, that lies
// AHEAD of `sq` from that color's perspective. A pawn on `sq` is "passed"
// exactly when none of the enemy's pawns occupy any square in this mask --
// i.e. no enemy pawn can ever block or capture it on its way to promotion,
// no matter how the game continues on other files.
static Bitboard passed_pawn_mask[2][64];
static bool endgame_tables_ready = false;

// Lazily built on first use rather than requiring every one of this
// project's many entry points to remember a separate init call -- cheap
// (64 squares, computed once, ever) and a forgotten init call silently
// leaving these masks zeroed would be a much worse failure mode (every
// pawn would incorrectly look "passed") than one extra boolean check per
// evaluate() call.
static void init_endgame_tables(void) {
    for (int sq = 0; sq < 64; sq++) {
        int file = sq % 8, rank = sq / 8;
        Bitboard white_mask = 0, black_mask = 0;
        for (int f = file - 1; f <= file + 1; f++) {
            if (f < 0 || f > 7) continue;
            for (int r = rank + 1; r <= 7; r++) white_mask |= bb_square(r * 8 + f);
            for (int r = rank - 1; r >= 0; r--) black_mask |= bb_square(r * 8 + f);
        }
        passed_pawn_mask[WHITE][sq] = white_mask;
        passed_pawn_mask[BLACK][sq] = black_mask;
    }
    endgame_tables_ready = true;
}

// Bonus by rank, indexed 0 (a pawn's own back rank) to 7 (the far side),
// always from a WHITE pawn's point of view -- a black pawn's bonus is
// looked up using (7 - rank) instead, mirroring the table. Ranks 0 and 7
// never actually hold a pawn in a legal position (rank 7 for White would
// mean it should already have promoted), so those two zero entries are
// harmless placeholders, never actually read for a real pawn.
static const int PASSED_PAWN_BONUS[8] = { 0, 10, 15, 25, 45, 75, 120, 0 };

#define KING_PASSED_PAWN_WEIGHT 4
#define ROOK_SEMI_OPEN_FILE_BONUS 10
#define ROOK_OPEN_FILE_BONUS 10 // additional, on top of semi-open, if NEITHER side has a pawn on the file
#define ROOK_BEHIND_PASSED_PAWN_BONUS 15

// Chebyshev distance: the number of KING MOVES needed to get from one
// square to another (a king can move diagonally, so this is
// max(file distance, rank distance), not ordinary "taxicab" distance).
// Used for "how many moves would it take this king to reach/escort this
// passed pawn".
static inline int chebyshev_distance(int sq1, int sq2) {
    int f1 = sq1 % 8, r1 = sq1 / 8, f2 = sq2 % 8, r2 = sq2 / 8;
    int df = f1 > f2 ? f1 - f2 : f2 - f1;
    int dr = r1 > r2 ? r1 - r2 : r2 - r1;
    return df > dr ? df : dr;
}

static int endgame_heuristics_score(const Board *b, bool endgame) {
    if (!endgame_tables_ready) init_endgame_tables();
    int score = 0;

    Bitboard white_pawns = b->piece_bb[piece_index(WHITE, PAWN)];
    Bitboard black_pawns = b->piece_bb[piece_index(BLACK, PAWN)];
    int white_king_sq = bb_lsb_index(b->piece_bb[piece_index(WHITE, KING)]);
    int black_king_sq = bb_lsb_index(b->piece_bb[piece_index(BLACK, KING)]);

    // --- Passed pawns (and, in the endgame, king activity toward them).
    for (int color = 0; color <= 1; color++) {
        int sign = color == WHITE ? 1 : -1; // White's bonuses add to the score, Black's subtract
        Bitboard pawns = color == WHITE ? white_pawns : black_pawns;
        Bitboard enemy_pawns = color == WHITE ? black_pawns : white_pawns;
        Bitboard bb = pawns;
        while (bb) {
            int sq = bb_pop_lsb(&bb);
            if (passed_pawn_mask[color][sq] & enemy_pawns) continue; // an enemy pawn can still stop it -- not passed
            int rank = sq / 8;
            int bonus_rank = color == WHITE ? rank : 7 - rank;
            score += sign * PASSED_PAWN_BONUS[bonus_rank];

            // King activity toward passed pawns is specifically an
            // ENDGAME idea -- in the middlegame the king wants safety
            // (tucked behind its own pawns), not a trip up the board.
            if (endgame) {
                int white_dist = chebyshev_distance(white_king_sq, sq);
                int black_dist = chebyshev_distance(black_king_sq, sq);
                // (7 - distance) turns "closer is better" into "closer
                // scores higher"; the difference rewards whichever king is
                // relatively nearer to this particular passed pawn.
                score += ((7 - white_dist) - (7 - black_dist)) * KING_PASSED_PAWN_WEIGHT;
            }
        }
    }

    // --- Rook file activity: semi-open (no OWN pawn on the file) and,
    // additionally, fully open (no pawns from either side).
    for (int color = 0; color <= 1; color++) {
        int sign = color == WHITE ? 1 : -1;
        Bitboard rooks = b->piece_bb[piece_index(color, ROOK)];
        while (rooks) {
            int sq = bb_pop_lsb(&rooks);
            int file = sq % 8;
            Bitboard file_bb = FILE_A << file;
            bool own_pawn_on_file = (file_bb & (color == WHITE ? white_pawns : black_pawns)) != 0;
            bool enemy_pawn_on_file = (file_bb & (color == WHITE ? black_pawns : white_pawns)) != 0;
            if (!own_pawn_on_file) {
                score += sign * ROOK_SEMI_OPEN_FILE_BONUS;
                if (!enemy_pawn_on_file) score += sign * ROOK_OPEN_FILE_BONUS;
            }
        }
    }

    // --- Tarrasch rule: a rook (either side's) behind a passed pawn
    // (either side's) on the same file. "Behind" means on the side the
    // pawn is moving AWAY from, so the rook's control of the file is never
    // blocked by its own pawn as that pawn advances -- whether it's
    // supporting its own pawn's push from behind, or trailing an enemy
    // pawn ready to capture it the moment it has to stop.
    for (int color = 0; color <= 1; color++) {
        Bitboard pawns = color == WHITE ? white_pawns : black_pawns;
        Bitboard enemy_pawns = color == WHITE ? black_pawns : white_pawns;
        Bitboard bb = pawns;
        while (bb) {
            int psq = bb_pop_lsb(&bb);
            if (passed_pawn_mask[color][psq] & enemy_pawns) continue; // not passed
            int pfile = psq % 8, prank = psq / 8;
            for (int rc = 0; rc <= 1; rc++) {
                int rsign = rc == WHITE ? 1 : -1;
                Bitboard rooks = b->piece_bb[piece_index(rc, ROOK)];
                while (rooks) {
                    int rsq = bb_pop_lsb(&rooks);
                    if (rsq % 8 != pfile) continue; // must be the same file as the pawn
                    int rrank = rsq / 8;
                    bool behind = color == WHITE ? (rrank < prank) : (rrank > prank);
                    if (behind) score += rsign * ROOK_BEHIND_PASSED_PAWN_BONUS;
                }
            }
        }
    }

    return score;
}

// ---------------------------------------------------------------------------
// PIECE-SQUARE TABLES (PSTs): a fixed bonus/penalty added per piece based
// purely on WHICH SQUARE it occupies, independent of the rest of the
// position. This is how the evaluation encodes basic positional principles
// without any actual "reasoning" -- e.g. the knight table strongly
// penalizes the board's rim ("a knight on the rim is dim": far fewer
// squares reachable from a corner than from the center), the pawn table
// rewards advancing (especially toward the center in the early ranks), and
// the king gets TWO different tables (see PST_KING_MID vs PST_KING_END)
// because "where a king wants to be" flips entirely between the
// middlegame (tucked away behind pawns, safety first) and the endgame
// (marching toward the center, where it's needed as an active fighting
// piece once there aren't enough enemy pieces left to attack it).
//
// Each table is written from WHITE's point of view, with index 0 = a1 and
// increasing left-to-right, rank by rank, up to index 63 = h8 -- i.e. the
// table reads top-to-bottom on the page as rank 8 down to rank 1, matching
// how a human would sketch a board bonus map by hand. evaluate() below
// flips the lookup for Black (and for White) into actual square indices
// via `is_white ? (7 - rank) * 8 + file : rank * 8 + file`.
// ---------------------------------------------------------------------------
static const int PST_PAWN[64] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
    5, 5, 10, 25, 25, 10, 5, 5,
    0, 0, 0, 20, 20, 0, 0, 0,
    5, -5, -10, 0, 0, -10, -5, 5,
    5, 10, 10, -20, -20, 10, 10, 5,
    0, 0, 0, 0, 0, 0, 0, 0,
};
static const int PST_KNIGHT[64] = {
    -50, -40, -30, -30, -30, -30, -40, -50,
    -40, -20, 0, 0, 0, 0, -20, -40,
    -30, 0, 10, 15, 15, 10, 0, -30,
    -30, 5, 15, 20, 20, 15, 5, -30,
    -30, 0, 15, 20, 20, 15, 0, -30,
    -30, 5, 10, 15, 15, 10, 5, -30,
    -40, -20, 0, 5, 5, 0, -20, -40,
    -50, -40, -30, -30, -30, -30, -40, -50,
};
static const int PST_BISHOP[64] = {
    -20, -10, -10, -10, -10, -10, -10, -20,
    -10, 0, 0, 0, 0, 0, 0, -10,
    -10, 0, 5, 10, 10, 5, 0, -10,
    -10, 5, 5, 10, 10, 5, 5, -10,
    -10, 0, 10, 10, 10, 10, 0, -10,
    -10, 10, 10, 10, 10, 10, 10, -10,
    -10, 5, 0, 0, 0, 0, 5, -10,
    -20, -10, -10, -10, -10, -10, -10, -20,
};
static const int PST_ROOK[64] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    5, 10, 10, 10, 10, 10, 10, 5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    0, 0, 0, 5, 5, 0, 0, 0,
};
static const int PST_QUEEN[64] = {
    -20, -10, -10, -5, -5, -10, -10, -20,
    -10, 0, 0, 0, 0, 0, 0, -10,
    -10, 0, 5, 5, 5, 5, 0, -10,
    -5, 0, 5, 5, 5, 5, 0, -5,
    0, 0, 5, 5, 5, 5, 0, -5,
    -10, 5, 5, 5, 5, 5, 0, -10,
    -10, 0, 5, 0, 0, 0, 0, -10,
    -20, -10, -10, -5, -5, -10, -10, -20,
};
// King, MIDDLEGAME: strongly rewards staying on the back rank behind
// pawn cover (the bottom two rows here, which map to ranks 1-2 for White)
// and strongly penalizes the center and especially the far/enemy side of
// the board, where the king would be exposed to attack.
static const int PST_KING_MID[64] = {
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -20, -30, -30, -40, -40, -30, -30, -20,
    -10, -20, -20, -20, -20, -20, -20, -10,
    20, 20, 0, 0, 0, 0, 20, 20,
    20, 30, 10, 0, 0, 10, 30, 20,
};
// King, ENDGAME: the exact opposite philosophy -- the center is now
// rewarded (a central king can reach either side of the board fastest to
// support its own pawns or stop the enemy's) and the corners are
// penalized instead.
static const int PST_KING_END[64] = {
    -50, -40, -30, -20, -20, -30, -40, -50,
    -30, -20, -10, 0, 0, -10, -20, -30,
    -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -30, 0, 0, 0, 0, -30, -30,
    -50, -30, -30, -30, -30, -30, -30, -50,
};

// Picks which table applies to a given piece type -- only the king has two
// (see above); every other piece type's positional preferences don't
// change between middlegame and endgame enough to bother with a second
// table.
static const int *pst_for(int ptype, bool endgame) {
    switch (ptype) {
        case PAWN: return PST_PAWN;
        case KNIGHT: return PST_KNIGHT;
        case BISHOP: return PST_BISHOP;
        case ROOK: return PST_ROOK;
        case QUEEN: return PST_QUEEN;
        default: return endgame ? PST_KING_END : PST_KING_MID;
    }
}

// The main evaluation function -- called an enormous number of times per
// search (every leaf, every quiescence stand-pat check), so it's built
// entirely from cheap bitboard operations (popcount, bit-scanning), never
// anything that loops over all 64 squares of the mailbox array. Returns a
// score in centipawns from WHITE's perspective (positive = White better).
int evaluate(const Board *b) {
    int score = 0;
    int non_pawn_material = 0; // used below to detect "is this an endgame"

    // --- Material: sum each side's piece values, White positive, Black
    // negative. Tracking non-pawn material (knights/bishops/rooks/queens,
    // NOT pawns or kings) separately is what lets the code below decide
    // whether the position "is an endgame" -- pawn count alone wouldn't be
    // a good signal (a pure king-and-pawn endgame and a middlegame full of
    // pawns look identical by pawn count, but very different by piece
    // count).
    for (int color = 0; color <= 1; color++) {
        int sign = color == WHITE ? 1 : -1;
        for (int ptype = PAWN; ptype <= KING; ptype++) {
            int count = bb_popcount(b->piece_bb[piece_index(color, ptype)]);
            int value = PIECE_VALUE[ptype];
            score += sign * value * count;
            if (ptype != PAWN && ptype != KING) non_pawn_material += value * count;
        }
    }

    // Threshold chosen so "roughly a rook and a minor piece or less,
    // combined across BOTH sides" counts as the endgame -- e.g. once
    // most of the heavy pieces have been traded off. This single boolean
    // controls both which king PST is used just below, and (if enabled)
    // endgame_heuristics_score()'s king-activity term above.
    bool endgame = non_pawn_material <= 1300;

    // --- Piece-square tables: add each piece's positional bonus for the
    // square it actually occupies.
    for (int color = 0; color <= 1; color++) {
        bool is_white = color == WHITE;
        int sign = is_white ? 1 : -1;
        for (int ptype = PAWN; ptype <= KING; ptype++) {
            const int *table = pst_for(ptype, endgame);
            Bitboard bb = b->piece_bb[piece_index(color, ptype)];
            while (bb) {
                int sq = bb_pop_lsb(&bb);
                int rank = sq / 8, file = sq % 8;
                // Every table above is written from White's point of view
                // with row 0 = rank 8; a WHITE piece on rank `r` looks up
                // row (7 - r) of the table, while a BLACK piece looks up
                // its own rank directly -- the net effect is that the same
                // table is mirrored vertically for Black, which is exactly
                // right, since "advance toward the enemy" means increasing
                // rank for White but decreasing rank for Black.
                int t_idx = is_white ? (7 - rank) * 8 + file : rank * 8 + file;
                score += sign * table[t_idx];
            }
        }
    }

    // --- Optional terms, both off by default (see eval.h).
    if (eval_mobility_enabled) {
        int white_mobility = bb_popcount(attacked_squares(b, WHITE));
        int black_mobility = bb_popcount(attacked_squares(b, BLACK));
        score += (white_mobility - black_mobility) * MOBILITY_WEIGHT;
    }

    if (eval_endgame_heuristics_enabled) {
        score += endgame_heuristics_score(b, endgame);
    }

    return score;
}
