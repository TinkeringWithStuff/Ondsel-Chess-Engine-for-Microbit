// Host-only devloop tool: replays an EXACT position (a UCI move prefix from
// the startpos, as recorded in a Divergence's moves_before_uci) and
// re-queries both Ondsel (at a configurable node budget) and Stockfish (at
// a configurable depth/multipv) on it. This is the "put the same position
// into the harness again" step from RUNBOOK.md's methodology -- used both
// to verify whether a just-implemented idea actually changed Ondsel's
// choice at the exact position that motivated it, and, with a much larger
// --node-budget, as the SEARCH-vs-EVAL diagnostic (if a big budget alone
// finds Stockfish's move, the problem was search depth, not a missing eval
// term; if even a huge budget still prefers the wrong move, it's a genuine
// eval blind spot).
//
// Code here is deliberately copy-adapted from play_vs_stockfish.c (same
// notation helpers, same Stockfish-subprocess plumbing) rather than shared
// via a header, consistent with this project's established host-tool
// convention (see book.c's own comment on why each tool carries its own
// copy).
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include "board.h"
#include "movegen.h"
#include "attacks.h"
#include "eval.h"
#include "search.h"
#include "book.h"
#include "zobrist.h"

static const char PIECE_LETTER[7] = { '?', 'P', 'N', 'B', 'R', 'Q', 'K' };

static void square_name(int sq, char *out) {
    out[0] = (char)('a' + (sq % 8));
    out[1] = (char)('1' + (sq / 8));
    out[2] = '\0';
}

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

static void move_to_uci(Move m, char *out) {
    char from[3], to[3];
    square_name(move_from(m), from);
    square_name(move_to(m), to);
    int len = 0;
    out[len++] = from[0]; out[len++] = from[1];
    out[len++] = to[0]; out[len++] = to[1];
    if (move_is_promotion(m)) {
        static const char promo_letter[4] = { 'n', 'b', 'r', 'q' };
        out[len++] = promo_letter[move_promotion_piece_type(m) - KNIGHT];
    }
    out[len] = '\0';
}

static Move parse_uci_move(Board *b, const char *uci) {
    if (strlen(uci) < 4) return 0;
    int from = (uci[0] - 'a') + (uci[1] - '1') * 8;
    int to = (uci[2] - 'a') + (uci[3] - '1') * 8;
    int promo = 0;
    if (uci[4]) {
        switch (uci[4]) {
            case 'n': promo = KNIGHT; break;
            case 'b': promo = BISHOP; break;
            case 'r': promo = ROOK; break;
            case 'q': promo = QUEEN; break;
        }
    }
    Move candidates[218];
    int n = legal_moves_now(b, candidates);
    for (int i = 0; i < n; i++) {
        Move m = candidates[i];
        if (move_from(m) != from || move_to(m) != to) continue;
        if (promo != 0 && (!move_is_promotion(m) || move_promotion_piece_type(m) != promo)) continue;
        if (promo == 0 && move_is_promotion(m)) continue;
        return m;
    }
    return 0;
}

typedef struct {
    pid_t pid;
    FILE *to_engine;
    FILE *from_engine;
} Engine;

static Engine sf_start(const char *path) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) { perror("pipe"); exit(1); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(path, "stockfish", (char *)NULL);
        perror("execl");
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    Engine e;
    e.pid = pid;
    e.to_engine = fdopen(in_pipe[1], "w");
    e.from_engine = fdopen(out_pipe[0], "r");
    if (!e.to_engine || !e.from_engine) { perror("fdopen"); exit(1); }
    setvbuf(e.to_engine, NULL, _IOLBF, 0);
    return e;
}

static void sf_send(Engine *e, const char *cmd) {
    fprintf(e->to_engine, "%s\n", cmd);
    fflush(e->to_engine);
}

static void sf_wait_for(Engine *e, const char *token) {
    char line[4096];
    while (fgets(line, sizeof(line), e->from_engine)) {
        if (strncmp(line, token, strlen(token)) == 0) return;
    }
}

static void sf_init(Engine *e) {
    sf_send(e, "uci");
    sf_wait_for(e, "uciok");
    sf_send(e, "isready");
    sf_wait_for(e, "readyok");
}

static void sf_set_multipv(Engine *e, int k) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "setoption name MultiPV value %d", k);
    sf_send(e, cmd);
    sf_send(e, "isready");
    sf_wait_for(e, "readyok");
}

static int parse_score(const char *cp_tok, const char *mate_tok) {
    if (mate_tok) {
        int m = atoi(mate_tok);
        int mag = 30000 - 10 * abs(m);
        return m >= 0 ? mag : -mag;
    }
    return atoi(cp_tok);
}

typedef struct {
    char move_uci[8];
    int score_cp;
    bool seen;
} SfResult;

static void sf_go(Engine *e, const char *position_cmd, int depth, int multipv, SfResult *results) {
    for (int i = 0; i < multipv; i++) results[i].seen = false;

    sf_send(e, position_cmd);
    char go_cmd[32];
    snprintf(go_cmd, sizeof(go_cmd), "go depth %d", depth);
    sf_send(e, go_cmd);

    char line[4096];
    char bestmove[8] = {0};
    while (fgets(line, sizeof(line), e->from_engine)) {
        if (strncmp(line, "bestmove", 8) == 0) {
            sscanf(line, "bestmove %7s", bestmove);
            break;
        }
        if (strncmp(line, "info", 4) != 0) continue;
        if (!strstr(line, " pv ")) continue;

        int mpv = 1;
        char *mpv_tok = strstr(line, "multipv ");
        if (mpv_tok) mpv = atoi(mpv_tok + 8);
        if (mpv < 1 || mpv > multipv) continue;

        char cp_buf[16] = {0}, mate_buf[16] = {0};
        char *cp_tok = strstr(line, "score cp ");
        char *mate_tok = strstr(line, "score mate ");
        if (cp_tok) sscanf(cp_tok + 9, "%15s", cp_buf);
        if (mate_tok) sscanf(mate_tok + 11, "%15s", mate_buf);
        if (!cp_tok && !mate_tok) continue;

        char *pv_tok = strstr(line, " pv ");
        char first_move[8] = {0};
        sscanf(pv_tok + 4, "%7s", first_move);
        if (!first_move[0]) continue;

        SfResult *r = &results[mpv - 1];
        strncpy(r->move_uci, first_move, sizeof(r->move_uci) - 1);
        r->score_cp = parse_score(cp_buf[0] ? cp_buf : NULL, mate_buf[0] ? mate_buf : NULL);
        r->seen = true;
    }
    if (bestmove[0] && multipv >= 1) {
        strncpy(results[0].move_uci, bestmove, sizeof(results[0].move_uci) - 1);
        results[0].move_uci[sizeof(results[0].move_uci) - 1] = '\0';
        results[0].seen = true;
    }
}

#define MAX_SEARCH_DEPTH_CEILING 30
static long long g_node_budget_target;
static int node_budget_reached(void) { return debug_node_count >= g_node_budget_target; }

int main(int argc, char **argv) {
    const char *moves_str = "";
    long long node_budget = 134638;
    int sf_depth = 14;
    int sf_multipv = 3;
    const char *stockfish_path = "tools/devloop/stockfish11";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--moves") && i + 1 < argc) moves_str = argv[++i];
        else if (!strcmp(argv[i], "--node-budget") && i + 1 < argc) node_budget = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--sf-depth") && i + 1 < argc) sf_depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sf-multipv") && i + 1 < argc) sf_multipv = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stockfish") && i + 1 < argc) stockfish_path = argv[++i];
        else {
            fprintf(stderr, "Unknown/incomplete arg: %s\n", argv[i]);
            fprintf(stderr, "Usage: replay_position [--moves \"e2e4 e7e5 ...\"] [--node-budget N]\n"
                            "           [--sf-depth D] [--sf-multipv K] [--stockfish PATH]\n");
            return 1;
        }
    }
    if (sf_multipv > 32) sf_multipv = 32;

    init_attack_tables();
    zobrist_init();
    search_init();
    book_enabled = 0;

    Board b;
    board_reset(&b);

    char moves_buf[4096];
    strncpy(moves_buf, moves_str, sizeof(moves_buf) - 1);
    moves_buf[sizeof(moves_buf) - 1] = '\0';
    char *tok = strtok(moves_buf, " \t");
    int ply = 0;
    while (tok) {
        Move m = parse_uci_move(&b, tok);
        if (m == 0) {
            fprintf(stderr, "replay_position: move '%s' (ply %d) is not legal here -- aborting\n", tok, ply);
            return 1;
        }
        UndoInfo undo;
        int8_t moved_piece = b.mailbox[move_from(m)];
        bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
        make_move(&b, m, &undo);
        search_record_move(b.hash, irreversible);
        ply++;
        tok = strtok(NULL, " \t");
    }

    fprintf(stderr, "Replaying position after %d ply/plies, side to move: %s\n",
            ply, b.side_to_move == WHITE ? "White" : "Black");

    // Ondsel's move at the requested node budget.
    g_node_budget_target = node_budget;
    debug_node_count = 0;
    Move ondsel_best = find_best_move_timed(&b, MAX_SEARCH_DEPTH_CEILING, node_budget_reached);
    char ondsel_san[8], ondsel_uci[8];
    move_to_san(&b, ondsel_best, ondsel_san);
    move_to_uci(ondsel_best, ondsel_uci);

    // Stockfish's view of the same position.
    Engine sf = sf_start(stockfish_path);
    sf_init(&sf);
    sf_set_multipv(&sf, sf_multipv);
    char pos_cmd[4096];
    if (ply == 0) {
        snprintf(pos_cmd, sizeof(pos_cmd), "position startpos");
    } else {
        int len = snprintf(pos_cmd, sizeof(pos_cmd), "position startpos moves %s", moves_str);
        (void)len;
    }
    SfResult results[32];
    sf_go(&sf, pos_cmd, sf_depth, sf_multipv, results);
    sf_send(&sf, "quit");
    int status;
    waitpid(sf.pid, &status, 0);

    // Plain-text report (not JSON -- this tool is for interactive/manual
    // use during the devloop's diagnostic step, read directly by whoever/
    // whatever is driving the loop).
    printf("ONDSEL move=%s (%s) eval=%d depth=%d nodes=%lld node_budget=%lld\n",
           ondsel_uci, ondsel_san, last_best_score, last_search_depth, debug_node_count, node_budget);
    printf("STOCKFISH (depth %d, multipv %d):\n", sf_depth, sf_multipv);
    for (int i = 0; i < sf_multipv; i++) {
        if (!results[i].seen) break;
        printf("  #%d: %s cp=%d\n", i + 1, results[i].move_uci, results[i].score_cp);
    }
    bool ondsel_matches_sf_best = results[0].seen && !strcmp(ondsel_uci, results[0].move_uci);
    printf("ONDSEL_MATCHES_SF_BEST=%s\n", ondsel_matches_sf_best ? "yes" : "no");

    return 0;
}
