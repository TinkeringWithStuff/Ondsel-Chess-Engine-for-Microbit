// ============================================================================
// STATIC EVALUATION -- "without searching any further, who's better off in
// this exact position, and by how much?"
// ============================================================================
// Every leaf of the search tree (and every quiescence stand-pat check) needs
// this question answered, so evaluate() gets called an enormous number of
// times per move chosen -- it has to be both cheap AND give the search
// something meaningful to work with. See eval.c for what it actually
// measures (currently: material + piece-square tables, with two more
// speculative terms wired in but switched off pending their own proof that
// they help).
// ============================================================================
#ifndef EVAL_H
#define EVAL_H

#include "board.h"

// Standard approximate piece values in "centipawns" (1 pawn = 100), indexed
// by piece type (PAWN..KING; index 0/EMPTY unused). Shared with see.c
// rather than each file keeping its own copy, so there's exactly one source
// of truth for "how much is a knight worth" -- if these ever get tuned,
// both material scoring AND exchange evaluation update together
// automatically, instead of risking the two silently drifting apart.
extern const int PIECE_VALUE[7];

// Returns a score in centipawns from WHITE's perspective: positive means
// White is better, negative means Black is better, 0 is dead equal.
// (Callers that need the score from the SIDE TO MOVE's perspective instead
// -- which is what negamax search actually wants -- negate this
// themselves; see search.c.)
int evaluate(const Board *b);

// ---------------------------------------------------------------------------
// Two additional evaluation terms exist below, both DELIBERATELY switched
// off by default. This isn't an oversight -- it's the same
// build-it-then-prove-it-with-an-A/B-match discipline this project used for
// null-move pruning (see search.h's null_move_enabled): each term is fully
// implemented and ready to test, but hasn't yet been proven on an actual
// engine-vs-engine match to make the engine stronger rather than weaker, so
// it stays off until that evidence exists. Flipping either on is a one-line
// change once someone runs that comparison -- see main_play_test.c for
// where the engine's real settings are chosen.
// ---------------------------------------------------------------------------

// MOBILITY: "how many squares can this side's pieces currently attack,
// total" is a classic proxy for how active/well-placed a side's pieces
// are, independent of material -- a side with cramped, blocked pieces is
// usually worse off than the raw material count alone would suggest. When
// enabled, adds (white's attacked-square count - black's) * MOBILITY_WEIGHT
// to the score.
extern int eval_mobility_enabled;
#define MOBILITY_WEIGHT 2

// ENDGAME HEURISTICS: several classic, textbook endgame principles (a
// passed pawn is dangerous and gets more dangerous the further it's
// advanced; the king should actively help escort/stop passed pawns once
// material has been traded down; a rook wants an open file; a rook belongs
// behind a passed pawn, its own or the enemy's) that the plain material +
// PST evaluation below has no notion of at all. See eval.c's
// endgame_heuristics_score() for the actual scoring.
extern int eval_endgame_heuristics_enabled;

#endif
