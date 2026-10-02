// Host-only devloop tool: plays one full game, Ondsel vs a real Stockfish
// subprocess, and logs a JSON-lines diagnostic trail (one line per ply) plus
// a human-readable PGN. This is the data-collection half of the overnight
// "find where Ondsel and Stockfish disagree, fix it, verify it helped"
// loop described in RUNBOOK.md -- it does NOT decide anything or change any
// engine behavior itself, it only plays and records.
//
// Reuses the Stockfish-subprocess/UCI/notation machinery that
// tools/build_opening_book.c already proved out (fork+exec over a real pipe
// pair, since popen() only gives one direction) rather than reinventing it.
//
// Book is intentionally left OFF (book_enabled stays at its default 0, and
// book_load() is never called) -- the whole point of this exercise is
// Ondsel's own search+eval judgement, not book coverage. See RUNBOOK.md.
//
// MOVE-QUALITY NOTE: this tool does NOT make a second Stockfish query after
// each move to see "how good was that actually" -- it only queries
// Stockfish once per ply, BEFORE the move is made. The "after" figure
// needed for move-quality analysis is just the NEXT ply's "before" figure
// (the position after ply N's move IS the position before ply N+1's move),
// so analysis.py reconstructs it by reading one record ahead rather than
// this tool paying for a redundant second search per ply.
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

// ---------------------------------------------------------------------------
// Notation helpers -- same approach as book.c / build_opening_book.c's own
// copies (each host tool in this project carries its own rather than
// sharing a header; see book.c's comment on why).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Stockfish subprocess -- identical approach to build_opening_book.c's own
// (a real bidirectional pipe pair via fork+exec; popen() only gives one
// direction and UCI needs both).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Move list bookkeeping, for building "position startpos moves ..." and for
// the PGN -- same dual UCI+SAN tracking as build_opening_book.c's Line.
// ---------------------------------------------------------------------------
#define MAX_PLIES_CAP 400
typedef struct {
    char uci[MAX_PLIES_CAP][8];
    char san[MAX_PLIES_CAP][8];
    int count;
} Line;

static void line_position_cmd(const Line *ln, char *out, size_t out_size) {
    if (ln->count == 0) { snprintf(out, out_size, "position startpos"); return; }
    int len = snprintf(out, out_size, "position startpos moves");
    for (int i = 0; i < ln->count; i++) {
        len += snprintf(out + len, out_size - len, " %s", ln->uci[i]);
    }
}

// ---------------------------------------------------------------------------
// Ondsel's own node-budgeted search, same convention as the device/other
// host tools (NODE_BUDGET_DEFAULT elsewhere in this project == 134638).
// ---------------------------------------------------------------------------
#define MAX_SEARCH_DEPTH_CEILING 20
static long long g_node_budget_target;
static int node_budget_reached(void) { return debug_node_count >= g_node_budget_target; }

static Move ondsel_move(Board *b, long long node_budget) {
    g_node_budget_target = node_budget;
    debug_node_count = 0;
    return find_best_move_timed(b, MAX_SEARCH_DEPTH_CEILING, node_budget_reached);
}

int main(int argc, char **argv) {
    int ondsel_color = -1;
    long long node_budget = 134638;
    int sf_depth = 10;
    int sf_multipv = 3;
    int max_plies = 160;
    const char *stockfish_path = "tools/devloop/stockfish11";
    const char *out_prefix = "tools/devloop/work/game";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ondsel-color") && i + 1 < argc) {
            ondsel_color = !strcmp(argv[++i], "white") ? WHITE : BLACK;
        } else if (!strcmp(argv[i], "--node-budget") && i + 1 < argc) {
            node_budget = atoll(argv[++i]);
        } else if (!strcmp(argv[i], "--sf-depth") && i + 1 < argc) {
            sf_depth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--sf-multipv") && i + 1 < argc) {
            sf_multipv = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-plies") && i + 1 < argc) {
            max_plies = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--stockfish") && i + 1 < argc) {
            stockfish_path = argv[++i];
        } else if (!strcmp(argv[i], "--out-prefix") && i + 1 < argc) {
            out_prefix = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            fprintf(stderr,
                "Usage: play_vs_stockfish --ondsel-color white|black\n"
                "           [--node-budget NB] [--sf-depth D] [--sf-multipv K]\n"
                "           [--max-plies N] [--stockfish PATH] [--out-prefix PATH]\n");
            return 1;
        }
    }
    if (ondsel_color != WHITE && ondsel_color != BLACK) {
        fprintf(stderr, "Must pass --ondsel-color white|black\n");
        return 1;
    }

    char jsonl_path[512], pgn_path[512];
    snprintf(jsonl_path, sizeof(jsonl_path), "%s.jsonl", out_prefix);
    snprintf(pgn_path, sizeof(pgn_path), "%s.pgn", out_prefix);
    FILE *jf = fopen(jsonl_path, "w");
    FILE *pf = fopen(pgn_path, "w");
    if (!jf || !pf) { perror("fopen output"); return 1; }

    init_attack_tables();
    zobrist_init();
    search_init();
    book_enabled = 0; // deliberate -- see file header comment

    Engine sf = sf_start(stockfish_path);
    sf_init(&sf);

    Board b;
    board_reset(&b);
    Line ln;
    ln.count = 0;

    const char *result_code = "UNKNOWN";
    SfResult results[32];
    if (sf_multipv > 32) sf_multipv = 32;

    for (int ply = 0; ply < max_plies; ply++) {
        Move legal[218];
        int n_legal = legal_moves_now(&b, legal);
        if (n_legal == 0) {
            int side = b.side_to_move, opp = (side == WHITE) ? BLACK : WHITE;
            int king_sq = bb_lsb_index(b.piece_bb[piece_index(side, KING)]);
            bool in_check = is_square_attacked(&b, king_sq, opp);
            result_code = !in_check ? "1/2-1/2 {Stalemate}"
                        : (side == WHITE ? "0-1 {Checkmate}" : "1-0 {Checkmate}");
            break;
        }

        char pos_cmd[4096];
        line_position_cmd(&ln, pos_cmd, sizeof(pos_cmd));
        sf_set_multipv(&sf, sf_multipv);
        sf_go(&sf, pos_cmd, sf_depth, sf_multipv, results);

        bool is_ondsel_turn = (b.side_to_move == ondsel_color);
        Move m;
        int ondsel_eval_cp = 0, ondsel_depth = 0;
        long long ondsel_nodes = 0;
        bool have_ondsel_stats = false;

        if (is_ondsel_turn) {
            m = ondsel_move(&b, node_budget);
            ondsel_eval_cp = last_best_score;
            ondsel_depth = last_search_depth;
            ondsel_nodes = debug_node_count;
            have_ondsel_stats = true;
        } else {
            if (!results[0].seen) { fprintf(stderr, "Stockfish gave no move at ply %d\n", ply); break; }
            m = parse_uci_move(&b, results[0].move_uci);
            if (m == 0) { fprintf(stderr, "Could not parse Stockfish move '%s' at ply %d\n", results[0].move_uci, ply); break; }
        }

        char san[8], uci_str[8];
        move_to_san(&b, m, san);
        move_to_uci(m, uci_str);

        // JSONL record -- hand-rolled (no library), same project convention
        // as everywhere else: the schema here is simple enough (plain
        // identifiers/numbers, no special characters needing escaping) that
        // string-building it directly is safe and avoids a dependency.
        fprintf(jf, "{\"ply\":%d,\"side_to_move\":\"%s\",\"mover\":\"%s\",\"move_uci\":\"%s\",\"move_san\":\"%s\"",
                ply, b.side_to_move == WHITE ? "w" : "b", is_ondsel_turn ? "ondsel" : "stockfish",
                uci_str, san);
        fprintf(jf, ",\"sf_eval_before_cp\":%d,\"sf_multipv_before\":[",
                results[0].seen ? results[0].score_cp : 0);
        for (int i = 0; i < sf_multipv; i++) {
            if (!results[i].seen) break;
            fprintf(jf, "%s{\"uci\":\"%s\",\"cp\":%d}", i ? "," : "", results[i].move_uci, results[i].score_cp);
        }
        fprintf(jf, "]");
        if (have_ondsel_stats) {
            fprintf(jf, ",\"ondsel_eval_cp\":%d,\"ondsel_depth\":%d,\"ondsel_nodes\":%lld",
                    ondsel_eval_cp, ondsel_depth, ondsel_nodes);
        } else {
            fprintf(jf, ",\"ondsel_eval_cp\":null,\"ondsel_depth\":null,\"ondsel_nodes\":null");
        }
        fprintf(jf, "}\n");
        fflush(jf);

        strncpy(ln.uci[ln.count], uci_str, 7); ln.uci[ln.count][7] = '\0';
        strncpy(ln.san[ln.count], san, 7); ln.san[ln.count][7] = '\0';
        ln.count++;

        UndoInfo undo;
        int8_t moved_piece = b.mailbox[move_from(m)];
        bool irreversible = (moved_piece == PAWN || moved_piece == -PAWN) || move_is_capture(m);
        make_move(&b, m, &undo);
        search_record_move(b.hash, irreversible);

        if (ln.count >= MAX_PLIES_CAP - 1) { result_code = "* {Ply cap reached}"; break; }
    }
    if (!strcmp(result_code, "UNKNOWN")) result_code = "* {Max plies reached}";

    fclose(jf);

    // Minimal PGN -- just enough to read the game back; analysis.py builds
    // the fully-annotated version (with eval/depth/nodes comments) from the
    // JSONL, which has the data this quick dump doesn't bother repeating.
    fprintf(pf, "[Event \"Ondsel devloop\"]\n[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n\n",
            ondsel_color == WHITE ? "Ondsel" : "Stockfish11",
            ondsel_color == WHITE ? "Stockfish11" : "Ondsel",
            result_code);
    for (int i = 0; i < ln.count; i++) {
        if (i % 2 == 0) fprintf(pf, "%d. ", i / 2 + 1);
        fprintf(pf, "%s ", ln.san[i]);
    }
    fprintf(pf, "%s\n", result_code);
    fclose(pf);

    sf_send(&sf, "quit");
    int status;
    waitpid(sf.pid, &status, 0);

    fprintf(stderr, "Game done (%d plies): %s -> %s / %s\n", ln.count, result_code, jsonl_path, pgn_path);
    return 0;
}
