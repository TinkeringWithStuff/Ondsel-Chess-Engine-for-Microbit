// Regression gate for the overnight Ondsel-vs-Stockfish devloop (see
// RUNBOOK.md): plays CURRENT (this working tree's src/engine, compiled
// normally) against a BASELINE git ref (compiled separately and renamed
// with the old_ prefix by build_baseline.sh, then linked into this same
// binary) -- so every devloop "idea" can be checked for regressions
// in-process, fast, with no subprocess/UCI overhead.
//
// Modeled directly on this project's earlier match_orig_vs_rewrite.c (see
// RUNBOOK.md for why that file exists): same equal-node-budget design,
// same play_game() structure, same win/loss/draw bookkeeping.
//
// OPENINGS -- IMPORTANT HISTORY (see tools/devloop/JOURNAL.md's
// "corrected null-move result" entry): this tool originally only had
// random_opening() (uniformly-random LEGAL moves from the startpos),
// written under the mistaken belief that this project's real opening-
// book file "doesn't exist in this checkout". It does --
// tools/devloop/openingbook/8moves_v3_movesonly.txt, copied in from this
// same project's earlier (pre-devloop) search-testing work, is 34,700
// lines of real, sensible opening theory, exactly what the project's own
// earlier match_nullmove_book.c used. Uniformly-random legal moves can
// (and did) produce bizarre, unrepresentative positions that distort a
// test's result -- confirmed directly: a feature (null-move pruning)
// that this tool's random-opening mode made look like a clear net loser
// turned out to be a clear net WINNER once tested with real book
// openings, reproducing a result from this project's own earlier
// history. --book <path> (see main()) switches to real book lines,
// drawn and shuffled the same way match_nullmove_book.c did; without
// --book, random_opening() is still available as a fallback but should
// no longer be trusted alone for a real keep/reject decision -- use it
// only for a quick sanity check during development, not the actual gate.
//
// Usage:
//   match_regression --baseline-ref <git-ref> [--games N] [--node-budget N]
//                     [--seed N] [--opening-plies N] [--out path.pgn]
//                     [--book path/to/book_movesonly.txt]
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

// ---------------------------------------------------------------------------
// REAL OPENING BOOK support (see the file header comment above for why
// this was added). Lifted directly from match_nullmove_book.c's own
// load_book()/tokenize_opening() -- same file format (one line per
// opening, SAN moves with move numbers, e.g. "1. Nf3 d5 2. g3 c6 ..."),
// same approach.
// ---------------------------------------------------------------------------
#define MAX_BOOK_LINES 40000
#define MAX_OPENING_PLIES 32
static char *g_book_lines[MAX_BOOK_LINES];
static int g_book_n = 0;
static char g_book_storage[8 * 1024 * 1024];
static int *g_book_shuffled_idx = NULL;

static void load_book(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "match_regression: could not open opening book '%s'\n", path); exit(1); }
    size_t used = 0;
    char line[2048];
    while (fgets(line, sizeof(line), f) && g_book_n < MAX_BOOK_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        if (used + len + 1 > sizeof(g_book_storage)) { fprintf(stderr, "match_regression: book storage exhausted\n"); break; }
        memcpy(g_book_storage + used, line, len + 1);
        g_book_lines[g_book_n++] = g_book_storage + used;
        used += len + 1;
    }
    fclose(f);
    fprintf(stderr, "Loaded %d opening lines from %s\n", g_book_n, path);
}

static int tokenize_opening(const char *line, char tokens[][16]) {
    char buf[2048];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int count = 0;
    char *tok = strtok(buf, " \t");
    while (tok && count < MAX_OPENING_PLIES) {
        char *dot = strchr(tok, '.');
        if (dot) {
            int nl = (int)(dot - tok);
            bool is_num = nl > 0;
            for (int i = 0; i < nl; i++) if (!isdigit((unsigned char)tok[i])) is_num = false;
            if (is_num) {
                char *rest = dot + 1;
                if (*rest == '\0') { tok = strtok(NULL, " \t"); continue; }
                tok = rest;
            }
        }
        strncpy(tokens[count], tok, 15);
        tokens[count][15] = '\0';
        count++;
        tok = strtok(NULL, " \t");
    }
    return count;
}

// Parses one SAN token (e.g. "Nf3", "Bxh3", "O-O", "e8=Q+") into the legal
// Move it refers to in the current position. Same approach as this
// project's earlier match_orig_vs_rewrite.c parse_san().
static Move parse_san(Board *b, const char *san_in) {
    char san[16];
    strncpy(san, san_in, sizeof(san) - 1);
    san[sizeof(san) - 1] = '\0';
    int len = (int)strlen(san);
    while (len > 0 && (san[len - 1] == '+' || san[len - 1] == '#')) san[--len] = '\0';

    Move candidates[218];
    int n = legal_moves_now(b, candidates);

    if (strcmp(san, "O-O") == 0) {
        for (int i = 0; i < n; i++) if (move_flag(candidates[i]) == MOVE_KING_CASTLE) return candidates[i];
        return 0;
    }
    if (strcmp(san, "O-O-O") == 0) {
        for (int i = 0; i < n; i++) if (move_flag(candidates[i]) == MOVE_QUEEN_CASTLE) return candidates[i];
        return 0;
    }

    int promo_piece = 0;
    char *eq = strchr(san, '=');
    if (eq) {
        switch (eq[1]) {
            case 'N': promo_piece = KNIGHT; break;
            case 'B': promo_piece = BISHOP; break;
            case 'R': promo_piece = ROOK; break;
            case 'Q': promo_piece = QUEEN; break;
        }
        *eq = '\0';
        len = (int)strlen(san);
    }

    int piece_type = PAWN;
    int p = 0;
    if (isupper((unsigned char)san[0])) {
        switch (san[0]) {
            case 'N': piece_type = KNIGHT; break;
            case 'B': piece_type = BISHOP; break;
            case 'R': piece_type = ROOK; break;
            case 'Q': piece_type = QUEEN; break;
            case 'K': piece_type = KING; break;
        }
        p = 1;
    }

    int to_sq = (san[len-2]-'a') + (san[len-1]-'1')*8;

    int disambig_file = -1, disambig_rank = -1;
    for (int i = p; i < len - 2; i++) {
        char c = san[i];
        if (c == 'x') continue;
        if (c >= 'a' && c <= 'h') disambig_file = c - 'a';
        else if (c >= '1' && c <= '8') disambig_rank = c - '1';
    }

    Move found = 0;
    int found_count = 0;
    for (int i = 0; i < n; i++) {
        Move m = candidates[i];
        int from = move_from(m), to = move_to(m);
        if (to != to_sq) continue;
        int8_t piece = b->mailbox[from];
        int mt = piece > 0 ? piece : -piece;
        if (mt != piece_type) continue;
        if (move_is_promotion(m) && promo_piece != 0 && move_promotion_piece_type(m) != promo_piece) continue;
        if (disambig_file >= 0 && (from % 8) != disambig_file) continue;
        if (disambig_rank >= 0 && (from / 8) != disambig_rank) continue;
        found = m;
        found_count++;
    }
    if (found_count != 1) return 0; // ambiguous/unparseable -- caller treats as opening-replay failure
    return found;
}

static void shuffle_indices(int *idx, int n, unsigned int seed) {
    unsigned int state = seed;
    for (int i = n - 1; i > 0; i--) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        int j = (int)(state % (unsigned)(i + 1));
        int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }
}

// Replays book opening line `book_idx` (an index into g_book_lines) from
// the startpos via real SAN parsing, same purpose as random_opening() but
// drawing from real opening theory instead of uniformly-random legal
// moves. Returns the UCI moves actually played.
static int book_opening(Board *b, int book_idx, char uci_out[][6]) {
    board_reset(b);
    char tokens[MAX_OPENING_PLIES][16];
    int n_tokens = tokenize_opening(g_book_lines[book_idx], tokens);
    int played = 0;
    for (int i = 0; i < n_tokens; i++) {
        Move m = parse_san(b, tokens[i]);
        if (m == 0) break; // stop at first unparseable token, play on from here
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
// book_idx: when g_book_n > 0 (a --book file was loaded), this is the
// index into g_book_shuffled_idx to use for a real book opening instead
// of random_opening(); ignored otherwise.
static int play_game(int n_opening_plies, unsigned long long opening_seed, bool current_is_white,
                      long long node_budget, FILE *pgn, int game_no, int book_idx) {
    Board opening_board;
    rng_state = opening_seed ? opening_seed : 0x9e3779b97f4a7c15ULL;
    char opening_uci[64][6];
    int opening_plies = (g_book_n > 0)
        ? book_opening(&opening_board, g_book_shuffled_idx[book_idx % g_book_n], opening_uci)
        : random_opening(&opening_board, n_opening_plies, opening_uci);

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
    const char *book_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--baseline-ref") == 0 && i + 1 < argc) baseline_ref = argv[++i];
        else if (strcmp(argv[i], "--games") == 0 && i + 1 < argc) n_pairs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--node-budget") == 0 && i + 1 < argc) node_budget = atoll(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--opening-plies") == 0 && i + 1 < argc) opening_plies = atoi(argv[++i]);
        else if (strcmp(argv[i], "--current-null-move") == 0) g_current_null_move = 1;
        else if (strcmp(argv[i], "--baseline-null-move") == 0) g_baseline_null_move = 1;
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_pgn_path = argv[++i];
        else if (strcmp(argv[i], "--book") == 0 && i + 1 < argc) book_path = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    (void)baseline_ref; // informational only here; the baseline build itself happens in build_baseline.sh

    init_attack_tables();
    zobrist_init();
    old_init_attack_tables();
    old_zobrist_init();

    if (book_path) {
        load_book(book_path);
        g_book_shuffled_idx = malloc(sizeof(int) * (size_t)g_book_n);
        for (int i = 0; i < g_book_n; i++) g_book_shuffled_idx[i] = i;
        shuffle_indices(g_book_shuffled_idx, g_book_n, (unsigned int)seed);
    }

    FILE *pgn = out_pgn_path ? fopen(out_pgn_path, "w") : NULL;
    if (out_pgn_path && !pgn) { perror("fopen"); return 1; }

    int current_wins = 0, baseline_wins = 0, draws = 0, adjudicated = 0;
    int game_no = 1;
    for (int i = 0; i < n_pairs; i++) {
        unsigned long long opening_seed = seed + (unsigned long long)i * 2654435761ULL;
        for (int side = 0; side < 2; side++) {
            bool current_is_white = (side == 0);
            int result = play_game(opening_plies, opening_seed, current_is_white, node_budget, pgn, game_no++, i);
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
