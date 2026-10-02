// ============================================================================
// SEARCH -- deciding which move to actually play.
// ============================================================================
// evaluate() (eval.h) can only judge the position sitting in front of it
// right now; it has no idea a seemingly-fine move loses a piece next turn,
// or that a seemingly-bad one wins the game in three. SEARCH is what looks
// ahead: it plays out possible sequences of moves for both sides, many
// moves deep, and uses evaluate() only at the far end of each line to judge
// how that line would turn out. See search.c's own comments for exactly how
// (negamax + alpha-beta, quiescence search, move ordering, null-move
// pruning, and real in-game repetition awareness).
// ============================================================================
#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "movegen.h"

// A "mate score": deliberately far outside any value a real material/PST
// evaluation could ever produce, so "found a forced checkmate" is always
// judged as better than "won a queen" by search, no matter how the rest of
// the scoring is tuned. Actual mate scores returned by search are this
// value minus the ply at which the mate occurs (see search.c), so that a
// mate found SOONER always scores higher than a mate found later -- the
// engine always prefers the fastest available forced win, and (equally
// important on the defending side) always prefers to delay an unavoidable
// loss as long as possible, rather than being indifferent between "mate in
// 1" and "mate in 10".
#define MATE_VALUE 29000

// Clears the killer-move and history tables (search.c's move-ordering
// memory) and resets the in-game repetition history to empty. Must be
// called once at the start of every new game -- search_record_move() below
// explains why the repetition history in particular needs a fresh start
// per game, not per move.
void search_init(void);

// Runs a fixed-depth search and returns the best move found, searching
// exactly `max_depth` plies deep (via iterative deepening -- see search.c).
// No time budget at all; the caller accepts however long a full depth-N
// search takes.
Move find_best_move(Board *b, int max_depth);

// ---------------------------------------------------------------------------
// REAL IN-GAME REPETITION AWARENESS.
//
// THE PROBLEM: a fresh call to find_best_move() only ever sees the CURRENT
// board position -- it has no memory of how the game actually got here.
// Without more information, search has no way to notice "if I play this
// move, I'm walking back into a position this exact game already reached
// earlier" -- and, this project's own match testing showed, it regularly
// does exactly that, throwing away a winning advantage for a draw purely
// because nothing told it repeating loses ground a decisive line would
// have kept. A fixed-depth search alone fundamentally can't see this on
// its own: the repetition only exists in the game's real move HISTORY, not
// in anything visible from the current board position by itself.
//
// THE FIX: the caller (main_play_test.c's game loop, or a host match
// harness) calls search_record_move() after every move that's actually
// played for real, by either side, passing the resulting position's
// Zobrist hash and whether that move was IRREVERSIBLE (a pawn move or a
// capture -- after either, no earlier position can ever be reached again,
// since pawns can't move backward and captured material can't come back;
// this is exactly the same idea chess's real 50-move-rule and
// threefold-repetition rules use to bound how far back a repeat needs to
// be checked). search_init() clears this history at the start of each new
// game, so callers don't need a second, separately-remembered reset call
// of their own.
//
// Internally, search() (search.c) EXTENDS this same history array with its
// own hypothetical line for the duration of one find_best_move() call, then
// leaves it exactly as it found it once that call returns. This means a
// move that would repeat a position -- whether that repeated position is
// one from REAL history, or one only reachable within the very line being
// searched right now -- gets correctly scored as the draw it actually is,
// instead of being searched (and potentially chosen) as though no
// repetition were about to happen.
// ---------------------------------------------------------------------------
void search_record_move(uint64_t hash_after_move, bool was_irreversible);

// On by default (unlike the speculative strength ideas below), because
// this is a genuine CORRECTNESS fix for the problem described above, not a
// strength experiment still awaiting its own A/B proof. Still a toggle
// (not simply hardwired on) so a before/after match can quantify exactly
// how much it helps, and so a regression test can confirm the OLD (blind
// to repetition) behavior is exactly reproducible with it switched off.
extern int repetition_avoidance_enabled;

// ---------------------------------------------------------------------------
// NULL-MOVE PRUNING -- OFF by default, same "prove it before shipping it"
// convention as eval.h's eval_mobility_enabled/eval_endgame_heuristics_enabled.
// See search.c's own comment above the null-move code itself for what the
// technique actually does and why it works.
//
// IMPORTANT, EASY TO FORGET: this flag lives here and defaults to 0 purely
// to make a clean, apples-to-apples A/B comparison possible (a host match
// harness flips it on for one side, off for the other, with everything
// else identical). Nothing in the actual device firmware
// (main_play_test.c) currently sets this to 1 -- so even after null-move
// is proven to help, playing a real game on the micro:bit itself won't
// benefit from it until something in the real firmware's startup path
// explicitly turns it on. That's a deliberate, temporary state while the
// A/B match is still running, not a bug -- but it needs remembering to
// actually flip once the result is in.
// ---------------------------------------------------------------------------
extern int null_move_enabled;

// Time-budgeted variant: iterative deepening runs exactly the same as
// find_best_move(), but after each depth FULLY completes, time_up() is
// polled -- returning nonzero stops the search there, keeping that depth's
// result. Never returns a PARTIAL/incomplete depth's result (alpha-beta
// search can't stand behind a result from a depth that got interrupted
// partway through -- see search.c's g_search_aborted handling). max_depth
// is still an absolute ceiling regardless of the time budget, since each
// extra ply of depth grows the work exponentially, and a generous time
// budget on a fast, simple position could otherwise search far deeper than
// is useful. Deliberately hardware-agnostic: this file has no notion of a
// clock at all; the caller supplies one via the callback (see
// main_play_test.c's time_check_callback(), which closes over a
// SysTick-based deadline through file-scope statics, since a plain C
// function pointer can't carry its own state).
typedef int (*TimeCheckFn)(void);
Move find_best_move_timed(Board *b, int max_depth, TimeCheckFn time_up);

// Total nodes visited by the most recent search call -- exposed purely for
// benchmarking/reporting (nodes-per-second figures, comparing search
// configurations against each other).
extern long long debug_node_count;

// The negamax score (relative to the side to move, i.e. positive always
// means "good for whoever's turn it was"), backing the move that
// find_best_move()/find_best_move_timed() most recently returned, at its
// deepest FULLY completed iteration. Exposed so callers can report or
// check the search's actual evaluation of the position, not just which
// move it chose.
extern int last_best_score;

// The deepest iterative-deepening depth that FULLY completed backing that
// same most-recent result (0 if the move came straight from the opening
// book, which does no search at all). Same "only updated once a depth
// actually finishes" guarantee as last_best_score -- an aborted deeper
// attempt never gets reported as if it had completed.
extern int last_search_depth;

#endif
