// Extern declarations for the BASELINE engine build (the git ref being
// regression-tested against -- normally the last commit the devloop
// trusted), whose global symbols were renamed with the old_ prefix by
// build_baseline.sh via `objcopy --redefine-syms`, so both the baseline
// build and the CURRENT engine build (compiled normally, unprefixed) can
// be linked into one process for a head-to-head match -- see
// match_regression.c.
//
// Modeled directly on this project's earlier orig_extern.h (see
// RUNBOOK.md): types (Board, Move, MoveList, UndoInfo, ...) are shared --
// every git revision of this project defines them identically enough for
// this purpose -- so the CURRENT tree's own headers are used for the
// struct/type definitions, and this file only re-declares the renamed
// baseline functions/globals that build_baseline.sh actually renamed (see
// its output's symbols.txt for the authoritative list; keep this file in
// sync with search.h/eval.h/book.h if new globals are added there).
#ifndef OLD_EXTERN_H
#define OLD_EXTERN_H
#include "board.h"
#include "movegen.h"
#include "search.h"

void old_board_reset(Board *b);
void old_board_load_fen(Board *b, const char *fen);

bool old_is_square_attacked(const Board *b, int sq, int by_side);
void old_generate_pseudo_moves(Board *b, int ply);
void old_make_move(Board *b, Move m, UndoInfo *undo);
void old_unmake_move(Board *b, Move m, const UndoInfo *undo);
extern MoveList old_move_pool[MAX_PLY];

void old_search_init(void);
Move old_find_best_move(Board *b, int max_depth);
Move old_find_best_move_timed(Board *b, int max_depth, TimeCheckFn time_up);
void old_search_record_move(uint64_t hash_after_move, bool was_irreversible);
extern int old_repetition_avoidance_enabled;
extern int old_null_move_enabled;
extern long long old_debug_node_count;
extern int old_last_best_score;
extern int old_last_search_depth;

extern int old_book_enabled;

void old_init_attack_tables(void);
void old_zobrist_init(void);

#endif
