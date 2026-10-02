// ============================================================================
// SEE -- Static Exchange Evaluation.
// ============================================================================
// THE QUESTION SEE ANSWERS: "if I capture on this square, and then both
// sides keep recapturing with their cheapest available attacker each time,
// who comes out ahead materially, and by how much?" -- all WITHOUT actually
// searching any of those follow-up moves. It's a fast, purely
// material-counting simulation of just the capture sequence on one square,
// used everywhere the engine needs a quick "is this capture likely to be
// good or bad" answer without paying for a real search: move ordering
// (try promising captures first -- see search.c's order_moves()) and
// quiescence pruning (don't even bother searching a capture that a trade-
// off sequence would clearly lose -- search.c's quiescence()).
//
// See.c's own comment walks through exactly how the simulated exchange is
// computed.
// ============================================================================
#ifndef SEE_H
#define SEE_H

#include "board.h"

// is_ep: true if this is an en passant capture. This can't be inferred
// from the board alone -- b->mailbox[to] is EMPTY for a real en passant
// capture, since the victim pawn sits one square behind `to`, not on it --
// so the caller (which already knows this from the move's flag) has to say
// so explicitly.
int see(const Board *b, int from, int to, bool is_ep);

#endif
