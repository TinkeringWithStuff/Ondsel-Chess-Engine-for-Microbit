// Regression gate for the overnight Ondsel-vs-Stockfish devloop (see
// RUNBOOK.md): plays CURRENT (this working tree's src/engine, compiled
// normally) against a BASELINE git ref (compiled separately and renamed
// with the old_ prefix by build_baseline.sh, then linked into this same
// binary) -- so every devloop "idea" can be checked for regressions
// in-process, fast, with no subprocess/UCI overhead.
//
// Modeled directly on this project's earlier match_orig_vs_rewrite.c (see
// RUNBOOK.md for why that file exists): same equal-node-budget design,
// same play_game() structure, same win/loss/draw bookkeeping. The main
// difference is openings: that file drew from a pre-built opening book
// file that doesn't exist in this checkout, so this version generates its
// own short random-but-seeded openings instead (see random_opening()) --
// this is purely for START-POSITION VARIETY so a 40-game match isn't 40
// repeats of the exact same game; it is NOT an engine heuristic and must
// never be confused with "hardcoding a move" (RUNBOOK.md's anti-cheating
// rule is about how ONDSEL ITSELF decides moves during real search, not
// about how test positions are chosen).
//
// Usage:
//   match_regression --baseline-ref <git-ref> [--games N] [--node-budget N]
//                     [--seed N] [--opening-plies N] [--out path.pgn]
//                     [--baseline-obj path.o already built by build_baseline.sh]
//
// Exit code 0 always (summary is for a human/script to read from stdout);
// use the printed summary line (machine-parseable, see below) to decide
// pass/fail.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "board.h"
#include "movegen.h"
#include "attacks.h"
#include "eval.h"
#include "search.h"
#include "zobrist.h"
#include "book.h"
#include "old_extern.h"

#define NODE_BUDGET_DEFAULT 134638
#define MAX_SEARCH_DEPTH 20
#define MAX_PLIES 300

static long long g_node_budget_target;
// Both default to OFF, matching the plain apples-to-apples eval-only
// comparison this tool was originally written for. Set via
// --current-null-move / --baseline-null-move (see main()) to also A/B a
// SEARCH feature (e.g. null-move pruning) instead of an eval change --
// see RUNBOOK.md / JOURNAL.md cycle 4 for why this was added.
static int g_current_null_move = 0;
static int g_baseline_null_move = 0;
static int current_node_budget_reached(void) { return debug_node_count >= g_node_budget_target; }
static int old_node_budget_reached(void) { return old_debug_node_count >= g_node_budget_target; }

static void square_name(int sq, char *out) {
    out[0] = (char)('a' + (sq % 8));
    out[1] = (char)('1' + (sq / 8));
    out[2] = '\0';
}

static const char PIECE_LETTER[7] = { '?', 'P', 'N', 'B', 'R', 'Q', 'K' };

// Pure board-utility helpers (legality/SAN/checkmate bookkeeping) built on
// CURRENT's move generator only. Both engine builds share logically
// identical move generation (same as match_orig_vs_rewrite.c's reasoning),
// so using only one copy here doesn't favor either side in the actual
// search/eval decisions being compared.
static int legal_moves_now(Board *b, Move *out) {
    int side = b->side_to_move;
    int opponent = side == WHITE ? BLACK : WHITE;
    generate_pseudo_moves(b, 0);
    MoveList *list = &move_pool[0];
    int n = 0;
    for (int i = 0; i < list->count; i++) {
        Move m = list->moves[i];
        UndoInfo undo;
        make_move(b, m, &undo);
        int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        if (!is_square_attacked(b, king_sq, opponent)) out[n++] = m;
        unmake_move(b, m, &undo);
    }
    return n;
}

static void move_to_san(Board *b, Move m, char *out) {
    int from = move_from(m), to = move_to(m);
    int8_t piece = b->mailbox[from];
    int ptype = piece > 0 ? piece : -piece;
    bool is_capture = move_is_capture(m);
    char destN[3];
    square_name(to, destN);
    int len = 0;

    if (move_flag(m) == MOVE_KING_CASTLE) { strcpy(out, "O-O"); len = 3; }
    else if (move_flag(m) == MOVE_QUEEN_CASTLE) { strcpy(out, "O-O-O"); len = 5; }
    else if (ptype == PAWN) {
        if (is_capture) {
            char fromN[3]; square_name(from, fromN);
            out[len++] = fromN[0];
            out[len++] = 'x';
        }
        out[len++] = destN[0]; out[len++] = destN[1];
        if (move_is_promotion(m)) {
            out[len++] = '=';
            out[len++] = PIECE_LETTER[move_promotion_piece_type(m)];
        }
    } else {
        out[len++] = PIECE_LETTER[ptype];
        Move candidates[218];
        int n = legal_moves_now(b, candidates);
        bool file_clash = false, rank_clash = false, any_clash = false;
        int from_file = from % 8, from_rank = from / 8;
        for (int i = 0; i < n; i++) {
            Move other = candidates[i];
            if (other == m) continue;
            if (move_to(other) != to) continue;
            int8_t op = b->mailbox[move_from(other)];
            int ot = op > 0 ? op : -op;
            if (ot != ptype) continue;
            any_clash = true;
            if (move_from(other) % 8 == from_file) file_clash = true;
            if (move_from(other) / 8 == from_rank) rank_clash = true;
        }
        if (any_clash) {
            char fromN[3]; square_name(from, fromN);
            if (!file_clash) out[len++] = fromN[0];
            else if (!rank_clash) out[len++] = fromN[1];
            else { out[len++] = fromN[0]; out[len++] = fromN[1]; }
        }
        if (is_capture) out[len++] = 'x';
        out[len++] = destN[0]; out[len++] = destN[1];
    }
    out[len] = '\0';

    UndoInfo undo;
    make_move(b, m, &undo);
    int opp_side = b->side_to_move;
    int opp_for_check = opp_side == WHITE ? BLACK : WHITE;
    int king_sq = bb_lsb_index(b->piece_bb[piece_index(opp_side, KING)]);
    bool in_check = is_square_attacked(b, king_sq, opp_for_check);
    if (in_check) {
        Move tmp[218];
        bool mate = legal_moves_now(b, tmp) == 0;
        out[len] = mate ? '#' : '+';
        out[len + 1] = '\0';
    }
    unmake_move(b, m, &undo);
}

static unsigned long long position_signature(const Board *b) {
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < 64; i++) {
        h ^= (unsigned char)(b->mailbox[i] + 128);
        h *= 1099511628211ULL;
    }
    h ^= (unsigned)b->side_to_move; h *= 1099511628211ULL;
    h ^= (unsigned)(b->castle_wk << 3 | b->castle_wq << 2 | b->castle_bk << 1 | b->castle_bq);
    h *= 1099511628211ULL;
    h ^= (unsigned)(b->ep_square + 1); h *= 1099511628211ULL;
    return h;
}

// Simple xorshift PRNG, seeded explicitly, so a given --seed always
// reproduces exactly the same set of openings -- useful for debugging a
// specific match result later.
static unsigned long long rng_state;
static unsigned int rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned int)(rng_state & 0xffffffffu);
}

// Plays `n_plies` uniformly-random LEGAL half-moves from the startpos, via
// CURRENT's own move generator (board-utility use only, see note above).
// Returns the UCI move list actually played (for the PGN/logging) and
// leaves *b at the resulting position. If a side has no legal moves before
// n_plies is reached (vanishingly unlikely this early but defensive
// anyway), stops early.
static int random_opening(Board *b, int n_plies, char uci_out[][6]) {
    board_reset(b);
    int played = 0;
    for (int i = 0; i < n_plies; i++) {
        Move legal[218];
        int n = legal_moves_now(b, legal);
        if (n == 0) break;
        Move m = legal[rng_next() % (unsigned)n];
        UndoInfo undo;
        make_move(b, m, &undo);
        int from = move_from(m), to = move_to(m);
        char f[3], t[3];
        square_name(from, f); square_name(to, t);
        snprintf(uci_out[played], 6, "%s%s", f, t);
        played++;
    }
    return played;
}

// result: 1 = current wins, -1 = baseline wins, 0 = draw, 2 = adjudicated/aborted
static int play_game(int n_opening_plies, unsigned long long opening_seed, bool current_is_white,
                      long long node_budget, FILE *pgn, int game_no) {
    Board opening_board;
    rng_state = opening_seed ? opening_seed : 0x9e3779b97f4a7c15ULL;
    char opening_uci[64][6];
    int opening_plies = random_opening(&opening_board, n_opening_plies, opening_uci);

    Board b;
    board_reset(&b);
    search_init();
    old_search_init();
    // Host builds (no ONDSEL_DEVICE_BUILD) use the dynamic book table; keep
    // it OFF here so both sides are judged purely on search+eval, exactly
    // like play_vs_stockfish.c -- the devloop's whole point is improving
    // search/eval, not book coverage.
    book_enabled = 0;
    old_book_enabled = 0;

    unsigned long long history[64 + MAX_PLIES + 4];
    int hist_n = 0;
    char movetext[64 + MAX_PLIES + 4][16];
    int move_count = 0;

    for (int i = 0; i < opening_plies; i++) {
        char from_s[3] = { opening_uci[i][0], opening_uci[i][1], '\0' };
        char to_s[3] = { opening_uci[i][2], opening_uci[i][3], '\0' };
        int from = (from_s[0]-'a') + (from_s[1]-'1')*8;
        int to = (to_s[0]-'a') + (to_s[1]-'1')*8;
        Move legal[218];
        int n = legal_moves_now(&b, legal);
        Move m = 0;
        for (int k = 0; k < n; k++) if (move_from(legal[k]) == from && move_to(legal[k]) == to) { m = legal[k]; break; }
        if (m == 0) { fprintf(stderr, "Game %d: opening replay failed at ply %d -- aborting opening, playing on\n", game_no, i); break; }
        int8_t moved_piece = b.mailbox[move_from(m)];
        bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
        char san[8];
        move_to_san(&b, m, san);
        UndoInfo undo;
        make_move(&b, m, &undo);
        strcpy(movetext[move_count++], san);
        history[hist_n++] = position_signature(&b);
        search_record_move(b.hash, irreversible);
        old_search_record_move(b.hash, irreversible);
    }

    int halfmove_clock = 0;
    int result = 0;
    const char *result_str = "1/2-1/2";
    const char *termination = "";

    for (int ply = 0; ply < MAX_PLIES; ply++) {
        Move legal[218];
        int n = legal_moves_now(&b, legal);
        if (n == 0) {
            int side = b.side_to_move, opp = side == WHITE ? BLACK : WHITE;
            int king_sq = bb_lsb_index(b.piece_bb[piece_index(side, KING)]);
            bool in_check = is_square_attacked(&b, king_sq, opp);
            if (in_check) {
                result = (side == WHITE) ? -1 : 1;
                result_str = (side == WHITE) ? "0-1" : "1-0";
                termination = "checkmate";
            } else {
                result = 0; result_str = "1/2-1/2"; termination = "stalemate";
            }
            break;
        }
        if (halfmove_clock >= 100) {
            result = 0; result_str = "1/2-1/2"; termination = "50-move rule";
            break;
        }
        if (hist_n > 0) {
            int reps = 0;
            for (int i = 0; i < hist_n; i++) if (history[i] == history[hist_n - 1]) reps++;
            if (reps >= 3) {
                result = 0; result_str = "1/2-1/2"; termination = "threefold repetition";
                break;
            }
        }

        bool white_to_move = (b.side_to_move == WHITE);
        bool use_current = (white_to_move == current_is_white);

        Move m;
        if (use_current) {
            null_move_enabled = g_current_null_move;
            g_node_budget_target = node_budget;
            debug_node_count = 0;
            m = find_best_move_timed(&b, MAX_SEARCH_DEPTH, current_node_budget_reached);
        } else {
            old_null_move_enabled = g_baseline_null_move;
            g_node_budget_target = node_budget;
            old_debug_node_count = 0;
            m = old_find_best_move_timed(&b, MAX_SEARCH_DEPTH, old_node_budget_reached);
        }
        if (m == 0) { result = 2; termination = "engine returned no move"; break; }

        int8_t moved_piece = b.mailbox[move_from(m)];
        bool is_pawn_move = (moved_piece == PAWN || moved_piece == -PAWN);
        bool is_capture = move_is_capture(m);

        char san[8];
        move_to_san(&b, m, san);

        UndoInfo undo;
        make_move(&b, m, &undo);

        strcpy(movetext[move_count++], san);
        halfmove_clock = (is_pawn_move || is_capture) ? 0 : halfmove_clock + 1;
        history[hist_n++] = position_signature(&b);
        search_record_move(b.hash, is_pawn_move || is_capture);
        old_search_record_move(b.hash, is_pawn_move || is_capture);
    }
    if (move_count >= opening_plies + MAX_PLIES - 1 && result != 1 && result != -1 && result != 0) {
        result = 2; result_str = "*"; termination = "ply cap reached";
    }

    const char *white_name = current_is_white ? "Current" : "Baseline";
    const char *black_name = current_is_white ? "Baseline" : "Current";
    if (pgn) {
        fprintf(pgn, "[Event \"Devloop regression gate\"]\n");
        fprintf(pgn, "[Round \"%d\"]\n", game_no);
        fprintf(pgn, "[White \"%s\"]\n", white_name);
        fprintf(pgn, "[Black \"%s\"]\n", black_name);
        fprintf(pgn, "[Result \"%s\"]\n", result_str);
        fprintf(pgn, "[Termination \"%s\"]\n", termination[0] ? termination : "ply cap reached");
        fprintf(pgn, "[NodeBudget \"%lld\"]\n", node_budget);
        fprintf(pgn, "\n");
        int col = 0;
        for (int i = 0; i < move_count; i++) {
            char buf[24];
            int blen = 0;
            if (i % 2 == 0) blen = snprintf(buf, sizeof(buf), "%d. %s ", i / 2 + 1, movetext[i]);
            else blen = snprintf(buf, sizeof(buf), "%s ", movetext[i]);
            fputs(buf, pgn);
            col += blen;
            if (col > 70) { fputc('\n', pgn); col = 0; }
        }
        fprintf(pgn, "%s\n\n", result_str);
    }

    fprintf(stderr, "Game %4d: White=%-10s Black=%-10s -> %-8s (%s, %d plies)\n",
            game_no, white_name, black_name, result_str, termination[0] ? termination : "ply cap reached", move_count);

    return result;
}

int main(int argc, char **argv) {
    const char *baseline_ref = NULL;
    int n_pairs = 20; // x2 colors = 2*n_pairs games
    long long node_budget = NODE_BUDGET_DEFAULT;
    unsigned long long seed = 42;
    int opening_plies = 4;
    const char *out_pgn_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--baseline-ref") == 0 && i + 1 < argc) baseline_ref = argv[++i];
        else if (strcmp(argv[i], "--games") == 0 && i + 1 < argc) n_pairs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--node-budget") == 0 && i + 1 < argc) node_budget = atoll(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--opening-plies") == 0 && i + 1 < argc) opening_plies = atoi(argv[++i]);
        else if (strcmp(argv[i], "--current-null-move") == 0) g_current_null_move = 1;
        else if (strcmp(argv[i], "--baseline-null-move") == 0) g_baseline_null_move = 1;
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_pgn_path = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    (void)baseline_ref; // informational only here; the baseline build itself happens in build_baseline.sh

    init_attack_tables();
    zobrist_init();
    old_init_attack_tables();
    old_zobrist_init();

    FILE *pgn = out_pgn_path ? fopen(out_pgn_path, "w") : NULL;
    if (out_pgn_path && !pgn) { perror("fopen"); return 1; }

    int current_wins = 0, baseline_wins = 0, draws = 0, adjudicated = 0;
    int game_no = 1;
    for (int i = 0; i < n_pairs; i++) {
        unsigned long long opening_seed = seed + (unsigned long long)i * 2654435761ULL;
        for (int side = 0; side < 2; side++) {
            bool current_is_white = (side == 0);
            int result = play_game(opening_plies, opening_seed, current_is_white, node_budget, pgn, game_no++);
            int current_result = current_is_white ? result : -result;
            if (result == 2) adjudicated++;
            else if (current_result == 1) current_wins++;
            else if (current_result == -1) baseline_wins++;
            else draws++;
        }
    }

    if (pgn) fclose(pgn);

    int total = n_pairs * 2;
    fprintf(stderr, "\n=== Match summary (%d games, node budget %lld/move, %d openings x 2 colors, seed %llu) ===\n",
            total, node_budget, n_pairs, seed);
    fprintf(stderr, "Current: %d wins, Baseline: %d wins, Draws: %d, Adjudicated/aborted: %d\n",
            current_wins, baseline_wins, draws, adjudicated);

    // Machine-parseable summary line for scripts/journal entries.
    printf("MATCH_RESULT current_wins=%d baseline_wins=%d draws=%d adjudicated=%d total=%d\n",
           current_wins, baseline_wins, draws, adjudicated, total);

    return 0;
}
