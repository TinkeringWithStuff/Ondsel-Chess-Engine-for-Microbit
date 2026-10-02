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
// Two additional evaluation terms exist below. This project's standing
// discipline (same as null-move pruning, search.h's null_move_enabled) is
// build-it-then-prove-it-with-an-A/B-match: a term stays off until an
// actual engine-vs-engine regression match shows it makes the engine
// stronger, not weaker -- see main_play_test.c for where the engine's real
// settings are chosen.
// ---------------------------------------------------------------------------

// MOBILITY: "how many squares can this side's pieces currently attack,
// total" is a classic proxy for how active/well-placed a side's pieces
// are, independent of material -- a side with cramped, blocked pieces is
// usually worse off than the raw material count alone would suggest. When
// enabled, adds (white's attacked-square count - black's) * MOBILITY_WEIGHT
// to the score. Still OFF: checked directly (by hand, via a static-eval
// probe, not even a full match) against two different real Ondsel-vs-
// Stockfish divergences during the devloop (see tools/devloop/JOURNAL.md,
// cycles 0-1) and its effect size was too small (single-digit to low-
// double-digit centipawns) to explain either one -- never got as far as
// an actual regression match.
extern int eval_mobility_enabled;
#define MOBILITY_WEIGHT 2

// ENDGAME HEURISTICS: several classic, textbook endgame principles (a
// passed pawn is dangerous and gets more dangerous the further it's
// advanced; the king should actively help escort/stop passed pawns once
// material has been traded down; a rook wants an open file; a rook belongs
// behind a passed pawn, its own or the enemy's) that the plain material +
// PST evaluation below has no notion of at all. See eval.c's
// endgame_heuristics_score() for the actual scoring.
//
// ON by default as of the devloop's cycle 5 (see tools/devloop/JOURNAL.md):
// a direct 3-seed, 40-games-per-seed regression match (120 games total)
// against the previous baseline (this term OFF) came back 57 wins / 44
// losses / 19 draws for ON -- current won 2 of the 3 individual 40-game
// matches outright and lost the third by only one game, a consistent
// enough signal across independently-seeded samples to trust.
extern int eval_endgame_heuristics_enabled;

#endif
