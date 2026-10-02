// ============================================================================
// SEARCH: negamax + alpha-beta, with move ordering, quiescence search,
// check extensions, null-move pruning, and real in-game repetition
// awareness, wrapped in iterative deepening.
// ============================================================================
//
// THE BIG PICTURE, FOR ANYONE NEW TO HOW A CHESS ENGINE ACTUALLY "THINKS":
//
// A chess position can be thought of as the root of a tree: from here,
// every legal move leads to a new position, which itself has its own
// legal moves leading to further positions, and so on. Play this out far
// enough and eventually every line ends in checkmate, stalemate, or (in
// practice, since games can't be searched to their literal end) a
// position the engine decides to just evaluate directly with evaluate()
// (eval.c) instead of searching further. The engine's whole job is to
// explore this tree far enough to be confident about which of its ROOT
// moves — the very next move it's actually choosing right now — leads to
// the best outcome, assuming the opponent also always plays their own best
// response at every step along the way.
//
// NEGAMAX is the specific bookkeeping trick used to explore that tree.
// A naive "minimax" implementation needs separate code paths for "White is
// choosing, so pick the move that MAXIMIZES the score" and "Black is
// choosing, so pick the move that MINIMIZES it" — doubling the logic for
// no real benefit. Negamax's insight: if scores are always reported from
// the CURRENT PLAYER'S OWN perspective (positive always means "good for
// whoever's turn it is right now", never "good for White specifically"),
// then both players are always doing the exact same thing — maximizing
// their own score — and a child node's score just needs to be NEGATED
// before being compared at the parent, since "good for my opponent" is by
// definition "bad for me" by exactly that same amount. That's the entire
// trick behind every `-search(...)` call below: recurse, then flip the
// sign, and the same one function correctly handles both sides without
// ever needing to know or care which color is actually moving.
//
// ALPHA-BETA PRUNING is what makes exploring this tree computationally
// feasible at all — searching EVERY line to a useful depth would be
// astronomically slow (chess's branching factor is roughly 35 legal moves
// per position). Alpha-beta tracks two running bounds while it searches:
// `alpha`, the best score the searching side has already guaranteed
// itself somewhere else in the tree, and `beta`, the best score the
// OPPONENT has already guaranteed themselves. The moment a move is found
// that scores >= beta, searching any of that position's remaining sibling
// moves is provably pointless: the opponent, one ply up, would never
// choose to allow reaching this position in the first place (they already
// have a response elsewhere that's at least this good for them), so
// nothing found here can ever actually affect the final chosen line. This
// is what the `if (alpha >= beta) break;` lines below actually mean —
// they're not an approximation or a heuristic; skipping those moves is
// GUARANTEED not to change the final answer, only how fast it's found.
// With good move ordering (see order_moves() below), alpha-beta routinely
// lets the engine search several times deeper than brute-force minimax
// could, for the same amount of work.
// ============================================================================
#include <string.h>
#include "search.h"
#include "attacks.h"
#include "eval.h"
#include "see.h"
#include "book.h"

#define MAX_CHECK_EXTENSIONS 4
#define MAX_QUIESCE_CHECK_DEPTH 6
#define KILLER_SLOTS 32
#define HISTORY_TABLE_SIZE 384

// --- Move-ordering memory: see order_moves()'s own comment for what these
// are actually used for. Both persist for the WHOLE search call (all
// iterative-deepening depths), not just one node, since "this move caused
// a cutoff somewhere" or "this move has historically been good" remains
// useful information from one part of the tree to another.
static Move killer_move1[KILLER_SLOTS];
static Move killer_move2[KILLER_SLOTS];
static int history_table[HISTORY_TABLE_SIZE];

long long debug_node_count = 0;
int null_move_enabled = 1;
int repetition_avoidance_enabled = 1;
int last_best_score = 0;
int last_search_depth = 0; // the deepest iterative-deepening depth that fully
                           // completed in the most recent find_best_move()/
                           // find_best_move_timed() call -- set alongside
                           // last_best_score below, same "only once a depth
                           // fully completes" guarantee. Purely for
                           // reporting (info screen, PC viewer stats).

// ---------------------------------------------------------------------------
// REAL IN-GAME REPETITION HISTORY -- see search.h's search_record_move()
// comment for the full explanation of why this exists and how it's used.
// Sized for every real ply a full game could reach (300, matching every
// host match harness's own MAX_PLIES cap) PLUS this engine's own maximum
// search depth (MAX_PLY, from movegen.h), since search() below extends
// this same array with its own hypothetical line while it's running, on
// top of whatever real moves have already been played.
// ---------------------------------------------------------------------------
#define MAX_GAME_HISTORY (300 + MAX_PLY)
static uint64_t game_history[MAX_GAME_HISTORY];
static int game_history_count = 0;       // real half-moves actually played so far this game
static int game_history_irreversible = 0; // earliest index that could still possibly recur

void search_init(void) {
    memset(killer_move1, 0, sizeof(killer_move1));
    memset(killer_move2, 0, sizeof(killer_move2));
    memset(history_table, 0, sizeof(history_table));
    game_history_count = 0;
    game_history_irreversible = 0;
}

void search_record_move(uint64_t hash_after_move, bool was_irreversible) {
    if (game_history_count < MAX_GAME_HISTORY) {
        game_history[game_history_count] = hash_after_move;
    }
    game_history_count++;
    // An irreversible move (pawn move or capture) means nothing before
    // this point in the game can ever be reached again, so the search for
    // repeats never needs to look earlier than here — exactly the same
    // idea chess's own 50-move-rule and threefold-repetition rules rely
    // on.
    if (was_irreversible) game_history_irreversible = game_history_count - 1;
}

// True if `hash` (just reached at history index `at_index`) already
// occurred earlier at an index of the SAME PARITY (i.e. the same side was
// to move both times — a position can only meaningfully "repeat" when it's
// the same player's turn to act in it both times), no earlier than
// `irreversible`.
//
// Matching on the FIRST occurrence, rather than waiting for a literal
// third repetition before treating a line as a draw, is the standard,
// deliberately conservative convention essentially every chess engine
// uses during search (as opposed to what the actual GAME RULES require to
// officially claim a draw, which is genuinely three occurrences). It's
// correct for search's purposes either way the game is actually going: a
// winning side will never voluntarily choose a 0-scoring branch while a
// better one exists, so treating a repeat as an immediate draw never
// costs a winning side anything; and a losing or equal side is completely
// correct to treat "I have a way to force a repeat" as being every bit as
// good as an actual draw, without needing to search out the fully literal
// third occurrence to know that.
static bool is_repetition(uint64_t hash, int at_index, int irreversible) {
    for (int i = at_index - 2; i >= irreversible; i -= 2) {
        if (game_history[i] == hash) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// MID-SEARCH TIME ABORT (for find_best_move_timed()).
//
// Checking the time budget only BETWEEN depths (i.e. only after a whole
// depth of iterative deepening finishes) turned out not to be
// fine-grained enough on real hardware: measured node counts from the
// start position are only around 3.7k / 30k / 135k nodes at depth 4/5/6,
// but a single search() *node* here costs far more than perft's bare
// make/unmake (move generation, SEE-ordered move loop, a recursive call)
// — real hardware measurements showed depth 6 ALONE sometimes taking
// several minutes, many times over any reasonable per-move time budget,
// simply because the code had already fully committed to searching that
// entire depth before it next checked the clock.
//
// So the clock is now ALSO polled periodically DURING a depth (not just
// between depths), via a countdown checked every TIME_CHECK_NODE_INTERVAL
// nodes — frequent enough to react within a fraction of a second, but
// cheap enough (one function call per few thousand nodes) that it doesn't
// meaningfully add to the cost of a node itself.
// ---------------------------------------------------------------------------
#define TIME_CHECK_NODE_INTERVAL 1024
static TimeCheckFn g_time_up = 0;
static volatile bool g_search_aborted = false;

static inline bool time_check_should_abort(void) {
    if (g_search_aborted) return true;
    if (g_time_up && (debug_node_count % TIME_CHECK_NODE_INTERVAL) == 0) {
        if (g_time_up()) g_search_aborted = true;
    }
    return g_search_aborted;
}

// ---------------------------------------------------------------------------
// MOVE ORDERING.
//
// Alpha-beta's ability to skip huge parts of the search tree (see this
// file's top comment) depends entirely on trying the BEST moves first —
// finding a strong move immediately means far more subsequent alpha-beta
// cutoffs than stumbling onto it last. order_moves() scores every move at
// a node using cheap heuristics (never a real search — that would defeat
// the purpose) and sorts them so the most promising ones are tried first:
//
//   - CAPTURES are scored by SEE (see.c): a capture that wins material in
//     the full simulated exchange gets a big bonus roughly proportional to
//     how much it wins.
//   - PROMOTIONS get a flat bonus (promoting is almost always worth
//     investigating first).
//   - KILLER MOVES: quiet (non-capture, non-promotion) moves that caused
//     an alpha-beta cutoff at THIS SAME DEPTH in a different branch of the
//     tree recently. The intuition: chess positions at similar points in
//     the search often share good responses (a good developing move, a
//     useful defensive resource) even when the exact position differs, so
//     a move that worked well as a cutoff once nearby is worth trying
//     early again. Two slots per depth (killer_move1/2) keep the two most
//     recent such moves.
//   - HISTORY HEURISTIC: a running tally, indexed by (piece type,
//     destination square), of how often a quiet move from roughly that
//     shape has caused cutoffs across the WHOLE search so far (not just
//     one depth) — a longer-memory, coarser-grained cousin of the killer
//     heuristic.
//   - The current iteration's own best move so far (root_pv_move) is given
//     the largest bonus of all, when asked for (use_root_pv) — since
//     iterative deepening re-searches the same root position at
//     increasing depths, the previous (shallower) depth's best move is an
//     excellent first guess for the new depth too.
// ---------------------------------------------------------------------------
typedef struct {
    int order[MAX_MOVES_PER_PLY]; // indices into move_pool[ply], after sorting
    int score[MAX_MOVES_PER_PLY]; // that move's ordering score, kept alongside so
                                    // quiescence() can read a capture's already-computed
                                    // SEE value back out without a second see() call
} OrderInfo;
static OrderInfo order_pool[MAX_PLY];

static void order_moves(const Board *b, int ply, int depth_remaining, bool use_root_pv, Move root_pv_move) {
    MoveList *list = &move_pool[ply];
    OrderInfo *oi = &order_pool[ply];
    int n = list->count;

    for (int i = 0; i < n; i++) {
        oi->order[i] = i;
        Move m = list->moves[i];
        int v = 0;
        bool is_cap = move_is_capture(m);
        bool is_promo = move_is_promotion(m);
        if (is_cap) v += 20000 + see(b, move_from(m), move_to(m), move_flag(m) == MOVE_EP_CAPTURE);
        if (is_promo) v += 800;
        if (!is_cap && !is_promo) {
            // Killer moves: only meaningful "at this depth", since a
            // killer recorded far away in the tree at a very different
            // remaining-depth isn't a relevant comparison.
            if (depth_remaining >= 0 && depth_remaining < KILLER_SLOTS) {
                if (m == killer_move1[depth_remaining]) v += 6000;
                else if (m == killer_move2[depth_remaining]) v += 5000;
            }
            // History heuristic: indexed by (piece type, destination
            // square) — deliberately coarse (not by origin square too),
            // since the idea is "moves of roughly this shape tend to be
            // good", not "this exact move was good before". Capped so a
            // single very-frequently-updated entry can never dominate
            // ordering outright, which would defeat the point of still
            // trying captures/promotions/killers ahead of it.
            int ptype = b->mailbox[move_from(m)];
            ptype = ptype > 0 ? ptype : -ptype;
            int hist_val = history_table[(ptype - 1) * 64 + move_to(m)];
            if (hist_val > 4000) hist_val = 4000;
            v += hist_val;
        }
        if (use_root_pv && m == root_pv_move) v += 1000000; // always tried first, above everything else
        oi->score[i] = v;
    }

    // Plain insertion sort, descending by score. Chosen over a fancier
    // sort deliberately: move lists here are short (rarely more than a few
    // dozen moves), and insertion sort is both simple to trust and, for
    // lists this size, competitive with or faster than anything more
    // complex — there's no real algorithmic win available from a "better"
    // sort at this scale.
    for (int x = 1; x < n; x++) {
        int key_order = oi->order[x], key_val = oi->score[x];
        int y = x - 1;
        while (y >= 0 && oi->score[y] < key_val) {
            oi->order[y + 1] = oi->order[y];
            oi->score[y + 1] = oi->score[y];
            y--;
        }
        oi->order[y + 1] = key_order;
        oi->score[y + 1] = key_val;
    }
}

// ---------------------------------------------------------------------------
// QUIESCENCE SEARCH.
//
// THE PROBLEM IT SOLVES: if the main search simply stopped dead at depth 0
// and called evaluate() on whatever position it happened to land on, the
// engine would frequently misjudge positions badly — e.g. it might stop
// right after grabbing a pawn with its queen, evaluate "I'm up a pawn,
// great", and completely miss that the very next move loses the queen to
// a pawn recapture. This is the classic "horizon effect": a fixed search
// depth creates an artificial cutoff (the "horizon") that can hide a
// disaster or a windfall just one move beyond it.
//
// THE FIX: once the main search reaches depth 0, instead of evaluating
// immediately, keep searching — but ONLY "loud"/tactical moves (captures,
// promotions, and check evasions when in check), never quiet developing
// moves, until the position reaches a "quiet" (quiescent) state with no
// immediate tactics left to resolve. This keeps the extra search cheap
// (there are usually far fewer captures than total legal moves in a
// position) while eliminating exactly the kind of short-term tactical
// blindness a hard depth cutoff would otherwise cause.
//
// STAND-PAT: at every quiescence node, `evaluate()`'s score is available
// immediately as a baseline — "the value of simply stopping here and not
// capturing anything further" (the term "stand pat" is borrowed from
// poker: choosing not to act). Since capturing is always optional, alpha
// is raised to at least the stand-pat score before any capture is even
// tried, and if stand-pat already exceeds beta, the whole node cuts off
// immediately — there's no reason to search further here at all if simply
// doing nothing already refutes whatever the opponent was hoping for. The
// ONE exception is being in check: a king in check can't just "stand pat"
// and ignore the threat, so allow_check_evasion instead searches every
// legal response (not just captures) up to MAX_QUIESCE_CHECK_DEPTH plies
// of forced check evasions, mirroring the main search's own check
// handling.
//
// SEE-BASED PRUNING: captures that SEE (see.c) judges as clearly losing
// material are skipped rather than searched — if simulating the full
// capture sequence already shows it loses material, actually searching it
// deeper essentially never changes that conclusion, and skipping it saves
// real work. This is why order_moves()'s already-computed SEE score is
// read back out of oi->score instead of calling see() a second time here.
// ---------------------------------------------------------------------------
static int quiescence(Board *b, int ply, int alpha, int beta, int check_ply) {
    debug_node_count++;
    if (time_check_should_abort()) return alpha; // discarded by the root loop once it unwinds -- see time_check_should_abort()
    int side = b->side_to_move;
    int opponent = side == WHITE ? BLACK : WHITE;
    int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
    bool in_check = is_square_attacked(b, king_sq, opponent);
    bool allow_check_evasion = in_check && check_ply < MAX_QUIESCE_CHECK_DEPTH;

    if (!allow_check_evasion) {
        // Negamax convention (this file's top comment): evaluate() always
        // returns a WHITE-relative score, so it's negated here for Black
        // to keep "positive = good for whoever's turn it is" true
        // everywhere in the search.
        int stand_pat = side == WHITE ? evaluate(b) : -evaluate(b);
        if (stand_pat >= beta) return beta;      // "doing nothing" already refutes the opponent's hope here -- cut off
        if (stand_pat > alpha) alpha = stand_pat; // otherwise, "doing nothing" is at least this good; only look for captures that beat it
    }

    if (ply + 1 >= MAX_PLY) return alpha; // hard safety ceiling on the fixed-size move_pool/order_pool arrays

    generate_pseudo_moves(b, ply);
    order_moves(b, ply, -1, false, 0); // depth_remaining=-1: no killer-slot lookup makes sense in quiescence
    MoveList *list = &move_pool[ply];
    OrderInfo *oi = &order_pool[ply];
    int legal_count = 0;

    for (int k = 0; k < list->count; k++) {
        int idx = oi->order[k];
        Move m = list->moves[idx];
        bool is_tactical = move_is_capture(m) || move_is_promotion(m);
        // Moves are sorted with all tactical moves ahead of all quiet
        // ones (order_moves() always scores captures/promotions higher),
        // so the moment a non-tactical move is reached, every remaining
        // move in the list is also non-tactical -- safe to stop here
        // entirely rather than checking each one individually.
        if (!allow_check_evasion && !is_tactical) break;
        if (!allow_check_evasion && move_is_capture(m)) {
            // Recover the plain SEE value from the combined ordering
            // score computed earlier (undoing the +20000 capture bonus
            // and, if applicable, the +800 promotion bonus) rather than
            // calling see() again.
            int see_val = oi->score[k] - 20000 - (move_is_promotion(m) ? 800 : 0);
            if (see_val < 0) continue; // a clearly losing capture -- not worth searching further
        }
        UndoInfo undo;
        make_move(b, m, &undo);
        int king_sq_now = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        if (is_square_attacked(b, king_sq_now, opponent)) { unmake_move(b, m, &undo); continue; } // illegal -- own king left in check
        legal_count++;
        int s = -quiescence(b, ply + 1, -beta, -alpha, allow_check_evasion ? check_ply + 1 : 0);
        unmake_move(b, m, &undo);

        if (s >= beta) return beta;
        if (s > alpha) alpha = s;
    }

    // No legal check-evasion at all means checkmate (only reachable when
    // allow_check_evasion was true to begin with, since otherwise
    // legal_count only counts captures that were actually tried, which
    // says nothing about whether OTHER, non-capturing legal moves exist).
    if (allow_check_evasion && legal_count == 0) return -(MATE_VALUE - ply);
    return alpha;
}

// ---------------------------------------------------------------------------
// NULL-MOVE PRUNING.
//
// THE IDEA: pretend, just for a moment, that the side to move passes their
// turn entirely — plays no move at all, not even a bad one — and let the
// opponent respond. Search THAT hypothetical line, at a reduced depth,
// with a minimal search window placed just above beta (this is a "null
// window" search: it only asks "is this at least beta", not "exactly how
// good is this", which is far cheaper to answer). If the opponent STILL
// can't reach beta even after being handed a completely free extra move,
// the reasoning goes, the actual position must already be so good for the
// side to move that playing any REAL move will almost certainly clear
// beta too — so the whole node can be pruned immediately, without
// searching any of the side to move's own actual candidate moves at all.
//
// WHY THIS WORKS, MOST OF THE TIME: in the vast majority of chess
// positions, having an extra free tempo (an extra move with no
// corresponding cost) can only help, never hurt — "null move" is possibly
// the single most direct way to test "how good is this position,
// approximately, without spending any real search effort enumerating
// actual moves." That's also exactly why it's NOT a sound technique in the
// strict alpha-beta sense (a real proof that pruning here can never change
// the final answer, the way the ordinary `alpha >= beta` cutoffs are) — it
// can occasionally prune away a genuinely correct move, which is why every
// A/B match testing this against the baseline exists in the first place:
// to measure whether the speed gained (searching deeper elsewhere, since
// far fewer nodes get spent on positions this reasoning applies to) is
// worth more, on balance, than the rare cases where it's wrong.
//
// THE ZUGZWANG EXCEPTION: the entire premise above — "an extra free tempo
// can only help" — is exactly false in zugzwang positions, where ANY move
// at all, even a completely free one, makes things WORSE (having to move
// is itself the disadvantage). King-and-pawn endgames are the classic
// example, and this project's own testing separately found king-and-pawn
// endgames to already be this engine's weakest phase — so skipping
// null-move whenever the side to move has no pieces left besides pawns and
// its king (has_non_pawn_material() below) isn't just textbook caution
// borrowed from chess engine theory, it's specifically protecting the one
// phase where getting this wrong would hurt the most.
//
// Other safety conditions below: never try it while already in check
// (there's no such thing as a legal "pass" out of check), never two null
// moves in a row (`allow_null` — testing "what if nobody moved, twice in a
// row" is simply wasted work, not a meaningfully stronger signal), and
// never near mate-score bounds (avoids null-move interfering with the
// engine's ability to correctly recognize forced mates).
// ---------------------------------------------------------------------------
static bool has_non_pawn_material(const Board *b, int color) {
    return (b->piece_bb[piece_index(color, KNIGHT)] | b->piece_bb[piece_index(color, BISHOP)] |
            b->piece_bb[piece_index(color, ROOK)] | b->piece_bb[piece_index(color, QUEEN)]) != 0;
}

#define NULL_MOVE_MIN_DEPTH 3  // too shallow a remaining depth isn't worth the overhead of a reduced sub-search
#define NULL_MOVE_REDUCTION 2  // how much shallower the "opponent gets a free move" probe searches, vs. the real remaining depth

// The main search function. Negamax + alpha-beta over the real move tree,
// with the null-move probe (above), check extensions, and repetition
// awareness woven in. `ply` is this node's depth from the SEARCH ROOT (used
// to index move_pool/order_pool and to compute mate-distance scores);
// `depth` is how many plies of REAL search remain before dropping into
// quiescence; `ext_count` tracks how many check extensions have already
// been spent on this line (capped by MAX_CHECK_EXTENSIONS, so a long
// sequence of checks can't make the search effectively unbounded);
// `allow_null` is false only on the one recursive call the null-move probe
// itself makes (see above); `hist_irr` is the earliest game_history[]
// index that could still possibly recur from this point on (threaded
// through recursion exactly like ext_count, and updated only when the move
// just made was itself irreversible).
static int search(Board *b, int ply, int depth, int alpha, int beta, int ext_count, bool allow_null, int hist_irr) {
    debug_node_count++;
    if (time_check_should_abort()) return alpha; // discarded by the root loop once it unwinds -- see time_check_should_abort()
    int side = b->side_to_move;
    int opponent = side == WHITE ? BLACK : WHITE;
    int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
    bool node_in_check = is_square_attacked(b, king_sq, opponent);

    // CHECK EXTENSIONS: a position where the side to move is in check
    // deserves one extra ply of real search rather than being treated the
    // same as any other node, because a check drastically narrows the
    // reasonable replies (often to just a handful of legal moves) and
    // forced sequences through check are exactly where a fixed depth is
    // most likely to cut off a critical line right before it resolves
    // (e.g. missing a forced mate, or missing that a seemingly-dangerous
    // check actually leads nowhere). Capped by MAX_CHECK_EXTENSIONS so a
    // deliberately drawn-out sequence of checks can't make one line of the
    // search unboundedly deep.
    int effective_depth = depth;
    int next_ext_count = ext_count;
    if (node_in_check && ext_count < MAX_CHECK_EXTENSIONS) {
        effective_depth = depth + 1;
        next_ext_count = ext_count + 1;
    }

    if (effective_depth <= 0) return quiescence(b, ply, alpha, beta, 0);
    if (ply + 1 >= MAX_PLY) return side == WHITE ? evaluate(b) : -evaluate(b); // hard safety ceiling on the fixed-size arrays

    // --- Null-move pruning attempt (see this section's own comment
    // above for the full explanation).
    if (null_move_enabled && allow_null && !node_in_check && depth >= NULL_MOVE_MIN_DEPTH &&
        has_non_pawn_material(b, side) &&
        beta < MATE_VALUE - MAX_PLY && beta > -(MATE_VALUE - MAX_PLY)) {
        int saved_ep = b->ep_square;
        b->ep_square = -1;          // a "pass" forfeits any en passant opportunity, same as a real move would
        b->side_to_move = opponent; // the entire "null move": just hand the turn over, no piece moves at all
        int null_score = -search(b, ply + 1, depth - 1 - NULL_MOVE_REDUCTION, -beta, -beta + 1, ext_count, false, hist_irr);
        b->side_to_move = side;
        b->ep_square = saved_ep;
        if (null_score >= beta) return beta; // even a free move wasn't enough for the opponent to catch up -- prune
    }

    generate_pseudo_moves(b, ply);
    order_moves(b, ply, depth, false, 0);
    MoveList *list = &move_pool[ply];
    OrderInfo *oi = &order_pool[ply];
    int best = -32000; // deliberately far below any real evaluation, so the very first legal move always improves on it
    int legal_count = 0;

    for (int k = 0; k < list->count; k++) {
        int idx = oi->order[k];
        Move m = list->moves[idx];
        int8_t moved_piece = b->mailbox[move_from(m)]; // must be read BEFORE make_move(), which moves it
        bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
        UndoInfo undo;
        make_move(b, m, &undo);
        int king_sq_now = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        if (is_square_attacked(b, king_sq_now, opponent)) { unmake_move(b, m, &undo); continue; } // illegal -- own king left in check
        legal_count++;

        // --- Repetition check: does the position this move JUST reached
        // already exist earlier, either in real game history or earlier
        // within this very same hypothetical line being searched right
        // now? See search.h's search_record_move() comment for the full
        // explanation of why this needs checking at all.
        int child_ply = ply + 1;
        int hidx = game_history_count + child_ply - 1; // this hypothetical position's slot in game_history[]
        int child_irr = irreversible ? hidx : hist_irr;
        int s;
        if (repetition_avoidance_enabled && hidx < MAX_GAME_HISTORY && is_repetition(b->hash, hidx, hist_irr)) {
            s = 0; // this line repeats an already-reached position -- correctly a draw, not something worth searching deeper to "discover" is a draw
        } else {
            if (hidx < MAX_GAME_HISTORY) game_history[hidx] = b->hash; // extend the hypothetical line so a DEEPER repeat can also be detected
            s = -search(b, child_ply, effective_depth - 1, -beta, -alpha, next_ext_count, true, child_irr);
        }
        unmake_move(b, m, &undo);

        if (s > best) best = s;
        if (best > alpha) alpha = best;
        if (alpha >= beta) {
            // Alpha-beta cutoff (see this file's top comment: this is a
            // GUARANTEED prune, not a heuristic one). Before breaking out,
            // remember this move for move ordering elsewhere in the tree
            // -- but only if it's a quiet move; captures/promotions are
            // already prioritized by SEE, so there's nothing extra to
            // learn from a capture causing a cutoff.
            if (!move_is_capture(m) && !move_is_promotion(m)) {
                if (depth >= 0 && depth < KILLER_SLOTS) {
                    if (m != killer_move1[depth]) { killer_move2[depth] = killer_move1[depth]; killer_move1[depth] = m; }
                }
                int ptype = b->mailbox[move_from(m)];
                ptype = ptype > 0 ? ptype : -ptype;
                int hist_idx = (ptype - 1) * 64 + move_to(m);
                history_table[hist_idx] += depth * depth; // cutoffs found deeper in the tree are weighted more heavily
            }
            break;
        }
    }

    // No legal moves at all: checkmate if the king is in check, stalemate
    // (a draw) otherwise. This is the actual TERMINAL-position case that
    // is_repetition()'s draw score above is deliberately mimicking for
    // repeated positions.
    if (legal_count == 0) return node_in_check ? -(MATE_VALUE - ply) : 0;
    return best;
}

// ---------------------------------------------------------------------------
// ITERATIVE DEEPENING.
//
// Rather than searching directly to max_depth in one shot, the engine
// searches to depth 1 first, then depth 2, then depth 3, and so on, up to
// max_depth — repeating work that already looks wasteful (why redo depth 1
// through N-1 before ever reaching depth N?) but that in practice costs
// very little extra, since each additional ply of chess search is
// exponentially more expensive than the last: the shallow depths finish
// almost instantly compared to the deepest one, so re-doing them is cheap
// insurance. In exchange, iterative deepening buys two real advantages:
//
//   1. A GOOD MOVE IS ALWAYS AVAILABLE, at whatever depth was last fully
//      completed, even if the engine runs out of time partway through a
//      deeper depth (this is exactly what makes find_best_move_timed()
//      possible/safe at all — see below).
//   2. EACH DEEPER SEARCH IS SEEDED WITH THE PREVIOUS DEPTH'S BEST MOVE
//      (root_pv_move, fed into order_moves() as use_root_pv), which is
//      usually still an excellent guess at the new depth's best move too
//      — trying it FIRST at the new depth tends to produce far better
//      alpha-beta cutoffs throughout the rest of that depth's search than
//      an uninformed move order would, often making the "redundant"
//      shallower passes pay for themselves many times over in cutoffs
//      gained at the final depth.
// ---------------------------------------------------------------------------

// Shared by find_best_move() and find_best_move_timed(): time_up may be
// NULL (a plain depth-limited search, never checked) or a callback polled
// after each depth fully completes (see search.h's find_best_move_timed
// comment for the full time-budget behavior, including the
// during-a-depth polling handled by time_check_should_abort() above).
static Move find_best_move_impl(Board *b, int max_depth, TimeCheckFn time_up) {
    // --- Opening book: if this exact position is in the book, play its
    // move immediately without spending any node budget or search time on
    // it at all -- that's the entire value proposition of a book (see
    // book.h). last_best_score is set to 0 rather than left stale from a
    // previous call, since a book move by definition has no search score
    // backing it; debug_node_count is reset to 0 for the same reason
    // node-count reporting for this "search" should honestly read zero.
    if (book_enabled) {
        Move book_move = book_probe(b);
        if (book_move != 0) {
            debug_node_count = 0;
            last_best_score = 0;
            last_search_depth = 0;
            return book_move;
        }
    }

    Move best_move_overall = 0;
    bool have_best = false;
    Move root_pv_move = 0;
    bool have_pv = false;
    g_search_aborted = false;
    // Snapshotted once per find_best_move() call: search_record_move()
    // (called by the CALLER, only after it actually plays whichever move
    // this call eventually returns) is the only thing that ever advances
    // game_history_irreversible, so it's genuinely constant for this
    // entire iterative-deepening search, however many depths it runs
    // through.
    int root_irr = game_history_irreversible;

    for (int d = 1; d <= max_depth; d++) {
        int side = b->side_to_move;
        int opponent = side == WHITE ? BLACK : WHITE;

        // Depth 1 always runs to completion regardless of the time
        // budget: it costs only tens of nodes, and this guarantees
        // have_best becomes true before an abort can ever discard a depth
        // — otherwise a pathologically tiny time budget could abort
        // before even the shallowest depth finishes and hand the caller
        // back move 0 (no move at all).
        g_time_up = (d == 1) ? 0 : time_up;

        generate_pseudo_moves(b, 0);
        order_moves(b, 0, d, have_pv, root_pv_move);
        MoveList *list = &move_pool[0];
        OrderInfo *oi = &order_pool[0];

        int root_alpha = -32000;
        int best_score = -32000;
        Move best_move_this_depth = 0;
        bool have_move_this_depth = false;
        bool depth_aborted = false;

        for (int k = 0; k < list->count; k++) {
            int idx = oi->order[k];
            Move m = list->moves[idx];
            int8_t moved_piece = b->mailbox[move_from(m)];
            bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
            UndoInfo undo;
            make_move(b, m, &undo);
            int king_sq_now = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
            if (is_square_attacked(b, king_sq_now, opponent)) { unmake_move(b, m, &undo); continue; }

            // Same repetition-awareness logic as inside search() itself
            // (see there for the full explanation) — the root's own move
            // loop needs it too, since a repetition can be created by the
            // very first move of the search just as easily as by one
            // found deeper in the tree.
            int hidx = game_history_count; // child_ply is always 1 at the root, so this is search()'s hidx formula with child_ply=1
            int child_irr = irreversible ? hidx : root_irr;
            int s;
            if (repetition_avoidance_enabled && hidx < MAX_GAME_HISTORY && is_repetition(b->hash, hidx, root_irr)) {
                s = 0;
            } else {
                if (hidx < MAX_GAME_HISTORY) game_history[hidx] = b->hash;
                s = -search(b, 1, d - 1, -32000, -root_alpha, 0, true, child_irr);
            }
            unmake_move(b, m, &undo);

            // g_search_aborted means the recursive call above didn't
            // really finish — every frame just unwound as fast as
            // possible once the abort flag was seen, so `s` is
            // meaningless. Discard this WHOLE depth (keeping whatever the
            // previous, fully-completed depth found) rather than risk
            // using a score computed from an incomplete search.
            if (g_search_aborted) { depth_aborted = true; break; }

            if (!have_move_this_depth || s > best_score) {
                best_score = s;
                best_move_this_depth = m;
                have_move_this_depth = true;
            }
            if (best_score > root_alpha) root_alpha = best_score;
        }

        if (depth_aborted) break; // keep whatever the previous (fully-completed) depth found

        if (have_move_this_depth) {
            best_move_overall = best_move_this_depth;
            have_best = true;
            root_pv_move = best_move_this_depth; // feeds move ordering at the NEXT, deeper iteration
            have_pv = true;
            last_best_score = best_score;
            last_search_depth = d;
        } else {
            break; // no legal moves at all (checkmate/stalemate) -- nothing more to search
        }

        if (time_up && time_up()) break; // budget exhausted -- stop here, but only AFTER this depth fully completed
    }

    g_time_up = 0; // don't leave a stale callback pointer set for whatever calls into search()/quiescence() next
    return have_best ? best_move_overall : 0;
}

Move find_best_move(Board *b, int max_depth) {
    return find_best_move_impl(b, max_depth, 0);
}

Move find_best_move_timed(Board *b, int max_depth, TimeCheckFn time_up) {
    return find_best_move_impl(b, max_depth, time_up);
}
