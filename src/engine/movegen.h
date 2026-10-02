// ============================================================================
// MOVE REPRESENTATION AND GENERATION.
// ============================================================================
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"

// A move is packed into a single 16-bit integer: 6 bits for the origin
// square (0-63), 6 bits for the destination square (0-63), and 4 bits for
// a "flag" saying what KIND of move it is (quiet, capture, castle,
// en passant, or one of the 8 promotion variants). Why bother packing
// instead of using a small struct with separate from/to/flag fields? On a
// memory-constrained target, a Move being 2 bytes instead of, say, 12
// (three ints) means move lists (MoveList below) and the various per-move
// scratch arrays throughout search.c cost 6x less RAM and fit better in
// cache -- a real, measurable win on hardware this tight, not premature
// optimization. This is the standard scheme used by chessprogramming.org
// and most open-source engines, chosen deliberately for that reason: it's
// well-known and widely implemented, so there's a large body of prior art
// to check this against instead of inventing (and debugging) something
// novel.
typedef uint16_t Move;

enum {
    MOVE_QUIET = 0, MOVE_DOUBLE_PAWN_PUSH = 1, MOVE_KING_CASTLE = 2, MOVE_QUEEN_CASTLE = 3,
    MOVE_CAPTURE = 4, MOVE_EP_CAPTURE = 5,
    MOVE_PROMO_KNIGHT = 8, MOVE_PROMO_BISHOP = 9, MOVE_PROMO_ROOK = 10, MOVE_PROMO_QUEEN = 11,
    MOVE_PROMO_KNIGHT_CAPTURE = 12, MOVE_PROMO_BISHOP_CAPTURE = 13,
    MOVE_PROMO_ROOK_CAPTURE = 14, MOVE_PROMO_QUEEN_CAPTURE = 15,
};

static inline Move encode_move(int from, int to, int flag) { return (Move)(from | (to << 6) | (flag << 12)); }
static inline int move_from(Move m) { return m & 0x3F; }
static inline int move_to(Move m) { return (m >> 6) & 0x3F; }
static inline int move_flag(Move m) { return (m >> 12) & 0xF; }

// A move is a capture if its flag says so directly (MOVE_CAPTURE), if it's
// the special en passant case (MOVE_EP_CAPTURE -- the captured piece isn't
// even ON the destination square, see see.c's handling of this), or if
// it's one of the four "promote AND capture" flag values (12-15, i.e.
// MOVE_PROMO_KNIGHT_CAPTURE and above).
static inline bool move_is_capture(Move m) { int f = move_flag(m); return f == MOVE_CAPTURE || f == MOVE_EP_CAPTURE || f >= MOVE_PROMO_KNIGHT_CAPTURE; }

// Every promotion flag (capturing or not) is numerically >= MOVE_PROMO_KNIGHT (8).
static inline bool move_is_promotion(Move m) { return move_flag(m) >= MOVE_PROMO_KNIGHT; }

// The low 2 bits of a promotion flag directly encode WHICH piece to
// promote to: 0=knight, 1=bishop, 2=rook, 3=queen (both the plain and
// capturing promotion flags share this same low-2-bit pattern -- compare
// MOVE_PROMO_KNIGHT=8 (0b1000) with MOVE_PROMO_KNIGHT_CAPTURE=12 (0b1100):
// only bit 2 differs, marking "is this also a capture", while bits 0-1
// stay "00" for knight in both). Adding 2 maps that 0-3 range onto this
// engine's own KNIGHT..QUEEN piece-type enum values (2-5, see board.h).
static inline int move_promotion_piece_type(Move m) { return (move_flag(m) & 3) + 2; }

// Fixed-size move-list pool, indexed by SEARCH PLY rather than allocated
// per call. Every ply of the search tree gets its own permanent slot
// (MAX_PLY of them), reused across the whole program's life -- no malloc,
// ever, which matters on a target with no heap to speak of. This also
// means a move list generated at one ply stays valid and untouched while
// deeper plies generate their OWN lists in their OWN slots, which is
// exactly the access pattern move generation needs: "list the moves here,
// recurse into one of them, come back and look at the same list again to
// try the next one".
#define MAX_PLY 32
#define MAX_MOVES_PER_PLY 218  // true worst-case legal move count in any reachable chess position

typedef struct {
    Move moves[MAX_MOVES_PER_PLY];
    int count;
} MoveList;

extern MoveList move_pool[MAX_PLY];

// Is `sq` attacked by any piece belonging to `by_side`? This is the single
// most important question in the whole engine for LEGALITY: after
// tentatively making a move, checking whether your own king's square is
// attacked by the opponent is exactly how "is this move legal" gets
// answered (a move that leaves your own king in check is illegal, full
// stop) -- see every `is_square_attacked(b, king_sq, opponent)` call
// throughout movegen.c and search.c. It's also how castling's "can't
// castle through or out of check" rule and checkmate/stalemate detection
// both work.
bool is_square_attacked(const Board *b, int sq, int by_side);

// Fills move_pool[ply] with every PSEUDO-legal move for the side to move
// in the given position. "Pseudo-legal" means "obeys each piece's normal
// movement rules" but NOT necessarily "doesn't leave your own king in
// check" -- that final legality check is deliberately left to the caller
// (via make_move + is_square_attacked + unmake_move if illegal), rather
// than done here, because checking king safety requires actually playing
// the move first anyway; doing it as a separate pass after generation
// avoids paying that cost for moves nobody ends up trying (e.g. once
// alpha-beta cuts off the rest of a move list).
void generate_pseudo_moves(Board *b, int ply);

// Everything needed to reverse exactly one make_move() call: whatever
// piece was captured (if any), the castling rights and en passant square
// as they were BEFORE the move, and a full snapshot of the hash from
// before the move (see zobrist.h's comment on why unmake_move() can just
// restore this directly instead of reversing any XOR math). One of these
// exists per ply currently on the search stack, the same fixed-array-
// instead-of-malloc idiom as move_pool above.
typedef struct {
    int8_t captured_piece;
    int ep_square;
    bool castle_wk, castle_wq, castle_bk, castle_bq;
    uint64_t hash_before;
} UndoInfo;

// Plays a move on the board, updating mailbox, bitboards, occupancy,
// castling rights, en passant state, and the Zobrist hash, all in one
// call. Does NOT check legality (see generate_pseudo_moves()'s comment) --
// it's entirely the caller's job to verify, after calling this, that the
// mover's own king isn't left in check, and to call unmake_move() to back
// out if it is.
void make_move(Board *b, Move m, UndoInfo *undo);

// Exactly reverses the most recent make_move() call, using the UndoInfo
// that call filled in. Must be called with the SAME move and UndoInfo, and
// nothing else may have called make_move() again in between (this is a
// strict stack discipline: make/unmake pairs must nest correctly, exactly
// like a normal function call stack, since UndoInfo only remembers "the
// state one move ago", not a full history).
void unmake_move(Board *b, Move m, const UndoInfo *undo);

// Counts leaf nodes at exactly `depth` plies from the current position --
// no evaluation, no pruning, just "how many distinct legal game states are
// reachable in exactly this many moves". This exists purely as a
// correctness check: perft's expected node counts from the standard
// starting position (and various other well-known test positions) are
// published and well-known, so running perft and comparing against those
// published numbers is one of the most reliable ways to catch a move
// generation bug (an illegal move being generated, a legal move being
// missed, castling rights or en passant being tracked wrong, etc.) --
// any such bug almost always shows up as a wrong node count at some depth,
// even when the bug is too subtle to notice by eye from a handful of
// example positions.
long long perft(Board *b, int depth, int ply);

#endif
