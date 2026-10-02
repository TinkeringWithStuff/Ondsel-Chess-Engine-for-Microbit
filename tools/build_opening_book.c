// Host-only tool: explores a hypothetical opening tree and decides, move by
// move, whether Ondsel actually understands each position well enough for
// it to belong in Ondsel's opening book.
//
// THE IDEA (agreed with the user before writing a line of this): a book
// move is only safe to hard-code if Ondsel would have found something
// nearly as good ON ITS OWN, because the whole point of a book is to save
// search time in well-known positions -- but the instant the opponent
// deviates from the book, Ondsel is back to relying purely on its own
// judgement. A book built from moves Ondsel doesn't understand just delays
// the moment Ondsel gets lost, it doesn't prevent it.
//
// So at every position where it is ONDSEL's turn to move, this tool:
//   1. Lets Ondsel pick its own move, for real, via find_best_move() --
//      the exact same search the real engine uses, not a shortcut.
//   2. Asks Stockfish (linked in as an external, independent reference
//      engine, NOT the thing being graded) how good that position and
//      that move actually are.
//   3. Computes TWO different numbers, because they answer two different
//      questions and neither one alone is trustworthy (this distinction is
//      the result of real back-and-forth with the user -- see the two
//      comments right above their definitions below):
//        - MOVE QUALITY (the primary accept/reject gate): how much worse
//          Ondsel's actual move is than Stockfish's own best move, measured
//          by Stockfish's OWN judgement of the resulting positions. This is
//          "objectively, did Ondsel just blunder", independent of whatever
//          Ondsel's own eval function happens to think.
//        - EVAL AGREEMENT (a secondary, non-gating diagnostic): how far
//          Ondsel's own reported eval of the position differs from
//          Stockfish's eval of that SAME position. This is a hint about
//          whether Ondsel's eval function is well-calibrated here, which
//          move quality alone can't tell you (a lucky, badly-miscalibrated
//          eval can still stumble onto a genuinely good move by search
//          alone, and a well-calibrated eval can still miss a subtlety a
//          deeper search would have caught).
//   4. Accepts the move (and keeps exploring under it) only if move quality
//      stays within --delta centipawns of Stockfish's best. A rejected
//      move is dropped, and nothing further is explored beneath it -- an
//      opening book has no business recommending a move a step beyond one
//      Ondsel didn't actually understand.
//
// At the OPPONENT's positions, there is no "Ondsel move" to judge -- the
// book has to be ready for whatever the opponent actually plays. So the
// tree branches there instead: Stockfish's own MultiPV gives the
// opponent's top --branch replies, and every one of them is explored
// separately (this was the user's explicit choice of the three tree-shape
// options discussed).
//
// One run only ever plays ONE side (--color white|black), producing a book
// for Ondsel-as-that-color only -- also the user's explicit choice. Run it
// twice (once per color) to build both halves of a full book.
//
// OUTPUT:
//   - A "moves-only" opening-lines text file (default book_lines_<color>.txt),
//     one accepted line per row, in plain SAN with no move numbers -- this
//     is already exactly the input format tools/gen_book_data.c expects,
//     so its output feeds directly into the existing device-book pipeline.
//   - A per-decision CSV report (default book_report_<color>.csv) with one
//     row for every position where Ondsel was asked to move, showing both
//     numbers above and whether that node was accepted.
//
// This tool links against the REAL engine (board.c, movegen.c, attacks.c,
// zobrist.c, eval.c, search.c, book.c, book_data.c) for the same reason
// gen_book_data.c does: every position, move and eval it reports must be
// byte-identical to what the real device would produce. book_enabled is
// left at its default (off) throughout -- consulting the book while
// BUILDING the book would be circular.
//
// Talking to Stockfish: Stockfish runs as a real subprocess, communicated
// with over a pair of pipes (fork/exec, not popen() -- popen() only gives
// one direction). See the "STOCKFISH SUBPROCESS" section below.
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
// Small chess-notation helpers -- same approach (and largely the same code)
// as book.c / gen_book_data.c / the match harnesses' own copies. Each host
// tool in this project carries its own copy of this logic rather than
// sharing a header for it; see book.c's comment on why.
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

// Full SAN generator -- copied from match_book_vs_nobook.c's move_to_san(),
// used for the human-readable opening-lines output and the CSV report.
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

// UCI long-algebraic notation ("e2e4", "e7e8q") -- what gets sent to
// Stockfish. Simpler than SAN: no disambiguation, no check/mate suffix.
static void move_to_uci(Move m, char *out) {
    char from[3], to[3];
    square_name(move_from(m), from);
    square_name(move_to(m), to);
    int len = 0;
    out[len++] = from[0]; out[len++] = from[1];
    out[len++] = to[0]; out[len++] = to[1];
    if (move_is_promotion(m)) {
        static const char promo_letter[4] = { 'n', 'b', 'r', 'q' }; // matches move_promotion_piece_type()'s 0-3 -> KNIGHT..QUEEN mapping
        out[len++] = promo_letter[move_promotion_piece_type(m) - KNIGHT];
    }
    out[len] = '\0';
}

// Matches a UCI move string ("e7e5", "e7e8q") against this position's
// legal moves. Used to turn Stockfish's MultiPV replies back into engine
// Move values so they can actually be played on the board.
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
// STOCKFISH SUBPROCESS -- a real bidirectional pipe pair (fork + exec),
// since popen() only gives you one direction and UCI needs both: we write
// commands to Stockfish's stdin and read "info"/"bestmove" lines back from
// its stdout.
// ---------------------------------------------------------------------------
typedef struct {
    pid_t pid;
    FILE *to_engine;
    FILE *from_engine;
} Engine;

static Engine sf_start(const char *path) {
    int in_pipe[2];  // parent writes[1] -> child reads[0] (child's stdin)
    int out_pipe[2]; // child writes[1] -> parent reads[0] (child's stdout)
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        perror("pipe");
        exit(1);
    }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        // Child: become Stockfish.
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(path, "stockfish", (char *)NULL);
        perror("execl");
        _exit(127);
    }
    // Parent.
    close(in_pipe[0]);
    close(out_pipe[1]);
    Engine e;
    e.pid = pid;
    e.to_engine = fdopen(in_pipe[1], "w");
    e.from_engine = fdopen(out_pipe[0], "r");
    if (!e.to_engine || !e.from_engine) { perror("fdopen"); exit(1); }
    setvbuf(e.to_engine, NULL, _IOLBF, 0); // line-buffered: every command must actually reach the child promptly
    return e;
}

static void sf_send(Engine *e, const char *cmd) {
    fprintf(e->to_engine, "%s\n", cmd);
    fflush(e->to_engine);
}

// Reads and discards lines until one starting with `token` is seen
// (inclusive) -- e.g. waiting out "uciok" or "readyok". Critically, this is
// what makes it safe to send the NEXT command: unlike a naive
// pipe-everything-in-at-once script, we never send anything (including
// "quit") before Stockfish has actually finished responding to the
// previous one.
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

// Converts a UCI "score cp X" or "score mate M" token pair into a single
// centipawn-ish number, relative to the side to move at the queried
// position (Stockfish's own convention, which conveniently is exactly
// Ondsel's own last_best_score convention too -- see the file comment).
// Mate scores are mapped to a large magnitude that still sorts correctly
// against ordinary centipawn scores and against each other (a shorter
// mate is always further from zero than a longer one).
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

// Runs "go depth D" (MultiPV must already be set via sf_set_multipv()) from
// the given position and collects, per MultiPV slot, that slot's best
// (highest-depth) reported move and score. results[] must have room for at
// least `multipv` entries. Returns once "bestmove" is seen.
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
        if (!strstr(line, " pv ")) continue; // ignore info lines with no principal variation (e.g. "currmove" chatter)

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
    // The final "bestmove" is authoritative for slot 0's move (it can
    // differ from the last multipv-1 "info" line by a ply of search that
    // happened after that line printed); trust it over whatever "info"
    // left behind.
    if (bestmove[0] && multipv >= 1) {
        strncpy(results[0].move_uci, bestmove, sizeof(results[0].move_uci) - 1);
        results[0].move_uci[sizeof(results[0].move_uci) - 1] = '\0';
        results[0].seen = true;
    }
}

// ---------------------------------------------------------------------------
// The move-list-so-far, kept in TWO parallel forms as the tree is walked:
// UCI long-algebraic (what Stockfish's "position startpos moves ..." wants)
// and SAN (what the opening-lines output file and CSV report want, and
// what tools/gen_book_data.c's input format already is).
// ---------------------------------------------------------------------------
#define MAX_PLIES_CAP 40
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
// Tunable parameters -- all overridable from the command line, see main().
// ---------------------------------------------------------------------------
static int g_ondsel_color;      // WHITE or BLACK: which side this run's book is for
static int g_max_plies = 12;    // how deep (in plies from the root) to explore
static int g_branch = 3;        // top-K opponent replies to branch on
static int g_delta_cp = 50;     // move-quality accept threshold, in centipawns
static int g_sf_depth = 15;     // Stockfish's analysis depth for every query
static Engine g_sf;

// ---------------------------------------------------------------------------
// Ondsel's own search config while generating: NODE-BUDGETED by default,
// not fixed-depth. This matters because a fixed depth is measured in
// exponentially-growing work -- the same depth that's instant on this host
// can be minutes on the real M4 Cortex the device actually runs on, with no
// way to know in advance which depths are "fine" and which aren't. A node
// budget sidesteps that entirely: it directly reuses the same
// project-wide convention already established for host-vs-device match
// testing (see match_book_vs_nobook.c) -- NODE_BUDGET_DEFAULT below is
// exactly the node count this project has been using throughout as a real
// device move's worth of search effort (measured from a depth-6 host
// search from the start position; ~13.5s/move on real hardware at its
// measured ~10,000 NPS). Using the same figure here means a book built by
// this tool reflects what Ondsel can actually work out inside one real
// move's time budget on the device -- not what it could work out given
// unlimited time, which would make for a book Ondsel can recommend but not
// live up to once it's actually running on the micro:bit.
//
// --depth on the command line switches to the OLD fixed-depth behavior
// instead (find_best_move(), no time/budget component at all) -- kept
// around for the odd case a fully deterministic, budget-independent run is
// wanted, but node-budgeted is the default and the mode this tool expects
// to actually be used in.
// ---------------------------------------------------------------------------
#define NODE_BUDGET_DEFAULT 134638
#define MAX_SEARCH_DEPTH_CEILING 20 // generous ceiling; the node budget is the real limit, same as match_book_vs_nobook.c
static long long g_node_budget = NODE_BUDGET_DEFAULT;
static bool g_use_fixed_depth = false;
static int g_ondsel_depth = 8; // only used when --depth is explicitly given

static long long g_node_budget_target;
static int node_budget_reached(void) { return debug_node_count >= g_node_budget_target; }

static FILE *g_lines_out;
static FILE *g_report_out;
static long long g_ondsel_nodes_considered = 0;
static long long g_ondsel_nodes_accepted = 0;

static void write_line_san(const Line *ln) {
    for (int i = 0; i < ln->count; i++) {
        fprintf(g_lines_out, "%s%s", i ? " " : "", ln->san[i]);
    }
    fprintf(g_lines_out, "\n");
    fflush(g_lines_out);
}

// Forward declaration: explore() and the two node kinds call each other.
static void explore(Board *b, Line *ln);

// An ONDSEL node: let Ondsel choose for real, judge it against Stockfish,
// accept/reject, and (if accepted) recurse one ply deeper.
static void explore_ondsel_node(Board *b, Line *ln) {
    if (ln->count >= g_max_plies) { write_line_san(ln); return; }

    char pos_cmd[2048];
    line_position_cmd(ln, pos_cmd, sizeof(pos_cmd));

    // 1) Stockfish's own read on the position BEFORE Ondsel moves: its top
    //    move (used as the move-quality reference) and its eval (used only
    //    for the secondary eval-agreement diagnostic).
    sf_set_multipv(&g_sf, 1);
    SfResult sf_before[1];
    sf_go(&g_sf, pos_cmd, g_sf_depth, 1, sf_before);
    int sf_eval_before = sf_before[0].score_cp;
    char sf_best_uci[8];
    strncpy(sf_best_uci, sf_before[0].move_uci, sizeof(sf_best_uci) - 1);
    sf_best_uci[sizeof(sf_best_uci) - 1] = '\0';

    // 2) Ondsel's own move and its own eval, via the real search --
    //    book/null-move/mobility/endgame heuristics are all left at
    //    whatever this build's compiled-in defaults are (book is always
    //    off here regardless -- see the file comment). Node-budgeted by
    //    default (see g_node_budget's comment); --depth switches to a
    //    fixed-depth search instead.
    Move ondsel_move;
    if (g_use_fixed_depth) {
        ondsel_move = find_best_move(b, g_ondsel_depth);
    } else {
        g_node_budget_target = g_node_budget;
        debug_node_count = 0;
        ondsel_move = find_best_move_timed(b, MAX_SEARCH_DEPTH_CEILING, node_budget_reached);
    }
    int ondsel_eval = last_best_score;
    char ondsel_uci[8];
    move_to_uci(ondsel_move, ondsel_uci);
    char ondsel_san[8];
    move_to_san(b, ondsel_move, ondsel_san);

    // 3) Stockfish's eval of the position AFTER Stockfish's own top move,
    //    and AFTER Ondsel's actual move -- both queried the same way, both
    //    negated to convert "value to whoever moves next" back into "value
    //    to whoever just moved", so they're directly comparable.
    Line after_sf = *ln;
    strncpy(after_sf.uci[after_sf.count], sf_best_uci, 7);
    after_sf.uci[after_sf.count][7] = '\0';
    after_sf.count++;
    char pos_after_sf[2048];
    line_position_cmd(&after_sf, pos_after_sf, sizeof(pos_after_sf));
    SfResult sf_after_sfbest[1];
    sf_go(&g_sf, pos_after_sf, g_sf_depth, 1, sf_after_sfbest);
    int val_after_sfbest = -sf_after_sfbest[0].score_cp;

    Line after_ondsel = *ln;
    strncpy(after_ondsel.uci[after_ondsel.count], ondsel_uci, 7);
    after_ondsel.uci[after_ondsel.count][7] = '\0';
    after_ondsel.count++;
    char pos_after_ondsel[2048];
    line_position_cmd(&after_ondsel, pos_after_ondsel, sizeof(pos_after_ondsel));
    SfResult sf_after_ondselmove[1];
    sf_go(&g_sf, pos_after_ondsel, g_sf_depth, 1, sf_after_ondselmove);
    int val_after_ondsel = -sf_after_ondselmove[0].score_cp;

    int move_quality_cploss = val_after_sfbest - val_after_ondsel;
    int eval_agreement_delta = abs(ondsel_eval - sf_eval_before);
    bool accepted = move_quality_cploss <= g_delta_cp;

    g_ondsel_nodes_considered++;
    if (accepted) g_ondsel_nodes_accepted++;

    char line_so_far_san[512] = "";
    for (int i = 0; i < ln->count; i++) {
        strcat(line_so_far_san, ln->san[i]);
        strcat(line_so_far_san, " ");
    }

    fprintf(g_report_out, "%d,\"%s\",%s,%d,%s,%d,%d,%d,%d,%d,%s\n",
            ln->count, line_so_far_san, ondsel_san, ondsel_eval, sf_best_uci,
            sf_eval_before, val_after_sfbest, val_after_ondsel,
            move_quality_cploss, eval_agreement_delta, accepted ? "accept" : "reject");
    fflush(g_report_out);

    if (!accepted) return; // reject: don't add this move, don't explore anything beneath it

    strncpy(ln->uci[ln->count], ondsel_uci, 7); ln->uci[ln->count][7] = '\0';
    strncpy(ln->san[ln->count], ondsel_san, 7); ln->san[ln->count][7] = '\0';
    ln->count++;

    write_line_san(ln); // every accepted prefix is itself a valid, safe book line

    UndoInfo undo;
    make_move(b, ondsel_move, &undo);
    explore(b, ln);
    unmake_move(b, ondsel_move, &undo);

    ln->count--;
}

// An OPPONENT node: no move to judge -- branch into Stockfish's top
// --branch replies and explore each one.
static void explore_opponent_node(Board *b, Line *ln) {
    if (ln->count >= g_max_plies) return; // shouldn't normally be reached (the Ondsel node above already stops at the cap), kept as a safety net

    Move legal[218];
    int n_legal = legal_moves_now(b, legal);
    if (n_legal == 0) return; // checkmate/stalemate: nothing to branch into, line already recorded by the accepting Ondsel node above

    char pos_cmd[2048];
    line_position_cmd(ln, pos_cmd, sizeof(pos_cmd));

    int k = g_branch < n_legal ? g_branch : n_legal;
    sf_set_multipv(&g_sf, k);
    SfResult replies[32]; // g_branch is never set anywhere near this high in practice
    sf_go(&g_sf, pos_cmd, g_sf_depth, k, replies);

    for (int i = 0; i < k; i++) {
        if (!replies[i].seen) continue;
        Move m = parse_uci_move(b, replies[i].move_uci);
        if (m == 0) continue; // shouldn't happen (Stockfish only reports legal moves), but never trust an external process blindly

        char san[8];
        move_to_san(b, m, san);
        char uci[8];
        strncpy(uci, replies[i].move_uci, 7); uci[7] = '\0';

        strncpy(ln->uci[ln->count], uci, 7); ln->uci[ln->count][7] = '\0';
        strncpy(ln->san[ln->count], san, 7); ln->san[ln->count][7] = '\0';
        ln->count++;

        UndoInfo undo;
        make_move(b, m, &undo);
        explore(b, ln);
        unmake_move(b, m, &undo);

        ln->count--;
    }
}

static void explore(Board *b, Line *ln) {
    if (b->side_to_move == g_ondsel_color) explore_ondsel_node(b, ln);
    else explore_opponent_node(b, ln);
}

int main(int argc, char **argv) {
    const char *color_arg = NULL;
    const char *stockfish_path = "/tmp/Stockfish/src/stockfish";
    const char *fen_arg = NULL;
    const char *out_lines_path = NULL;
    const char *out_report_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--max-plies") && i + 1 < argc) g_max_plies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--branch") && i + 1 < argc) g_branch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delta") && i + 1 < argc) g_delta_cp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) { g_ondsel_depth = atoi(argv[++i]); g_use_fixed_depth = true; }
        else if (!strcmp(argv[i], "--node-budget") && i + 1 < argc) g_node_budget = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--sf-depth") && i + 1 < argc) g_sf_depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stockfish") && i + 1 < argc) stockfish_path = argv[++i];
        else if (!strcmp(argv[i], "--fen") && i + 1 < argc) fen_arg = argv[++i];
        else if (!strcmp(argv[i], "--out-lines") && i + 1 < argc) out_lines_path = argv[++i];
        else if (!strcmp(argv[i], "--out-report") && i + 1 < argc) out_report_path = argv[++i];
        else if (!color_arg) color_arg = argv[i];
        else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!color_arg || (strcmp(color_arg, "white") != 0 && strcmp(color_arg, "black") != 0)) {
        fprintf(stderr,
            "Usage: build_opening_book <white|black> [--max-plies N] [--branch K]\n"
            "                          [--delta CP] [--node-budget NB] [--depth D]\n"
            "                          [--sf-depth SFD]\n"
            "                          [--stockfish PATH] [--fen FEN]\n"
            "                          [--out-lines PATH] [--out-report PATH]\n"
            "\n"
            "  <white|black>   which color Ondsel plays for this run (required)\n"
            "  --max-plies N   how deep to explore, in plies from the root (default 12)\n"
            "  --branch K      top-K Stockfish replies to branch on at opponent nodes (default 3)\n"
            "  --delta CP      move-quality accept threshold in centipawns (default 50)\n"
            "  --node-budget NB  Ondsel's per-move node budget while generating (default 134638, a real device move's worth)\n"
            "  --depth D       overrides node-budget with a FIXED search depth instead (no default; off unless given)\n"
            "  --sf-depth SFD  Stockfish's analysis depth for every query (default 15)\n"
            "  --stockfish P   path to the Stockfish binary (default /tmp/Stockfish/src/stockfish)\n"
            "  --fen FEN       start from this FEN instead of the standard start position\n"
            "  --out-lines P   opening-lines output path (default book_lines_<color>.txt)\n"
            "  --out-report P  per-decision CSV report path (default book_report_<color>.csv)\n");
        return 1;
    }
    g_ondsel_color = !strcmp(color_arg, "white") ? WHITE : BLACK;

    char default_lines[256], default_report[256];
    snprintf(default_lines, sizeof(default_lines), "book_lines_%s.txt", color_arg);
    snprintf(default_report, sizeof(default_report), "book_report_%s.csv", color_arg);
    if (!out_lines_path) out_lines_path = default_lines;
    if (!out_report_path) out_report_path = default_report;

    g_lines_out = fopen(out_lines_path, "w");
    g_report_out = fopen(out_report_path, "w");
    if (!g_lines_out || !g_report_out) { perror("fopen"); return 1; }
    fprintf(g_report_out, "ply,line_before,ondsel_move,ondsel_eval_cp,sf_best_move,sf_eval_before_cp,sf_eval_after_sfbest_cp,sf_eval_after_ondsel_cp,move_quality_cploss,eval_agreement_delta,decision\n");

    init_attack_tables();
    zobrist_init();
    search_init();
    book_init(); // static (flash) book stays loaded but book_enabled defaults to 0, so it's never consulted below -- see file comment

    g_sf = sf_start(stockfish_path);
    sf_init(&g_sf);

    Board b;
    if (fen_arg) board_load_fen(&b, fen_arg);
    else board_reset(&b);

    Line ln = { .count = 0 };

    if (g_use_fixed_depth) {
        fprintf(stderr, "Building opening book for Ondsel-as-%s: max_plies=%d branch=%d delta=%dcp ondsel_depth=%d (fixed) sf_depth=%d\n",
                color_arg, g_max_plies, g_branch, g_delta_cp, g_ondsel_depth, g_sf_depth);
    } else {
        fprintf(stderr, "Building opening book for Ondsel-as-%s: max_plies=%d branch=%d delta=%dcp ondsel_node_budget=%lld sf_depth=%d\n",
                color_arg, g_max_plies, g_branch, g_delta_cp, g_node_budget, g_sf_depth);
    }

    explore(&b, &ln);

    fprintf(stderr, "Done. Ondsel nodes considered: %lld, accepted: %lld (%.1f%%)\n",
            g_ondsel_nodes_considered, g_ondsel_nodes_accepted,
            g_ondsel_nodes_considered ? 100.0 * g_ondsel_nodes_accepted / g_ondsel_nodes_considered : 0.0);
    fprintf(stderr, "Opening lines written to %s\n", out_lines_path);
    fprintf(stderr, "Per-decision report written to %s\n", out_report_path);

    fclose(g_lines_out);
    fclose(g_report_out);
    sf_send(&g_sf, "quit");
    waitpid(g_sf.pid, NULL, 0);
    return 0;
}
