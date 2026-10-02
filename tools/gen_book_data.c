// Host-only build tool: turns a "moves-only" opening file into the compact,
// device-ready opening book baked into src/engine/book_data.c/.h.
//
// WHY THIS EXISTS: book.c's book_load() builds its lookup table by parsing
// text into a RAM hash table at startup -- fine on a host, but the
// micro:bit only has 128KB of RAM total, nowhere near enough for a table
// sized for thousands of entries (see book.h's own comment on this). The
// fix real engines use for a book that never changes at runtime: do the
// parsing ONCE, on a host, ahead of time, and ship only the *result* --
// two flat, sorted arrays of (position hash, move) -- as `const` data.
// `const` data lives in FLASH, not RAM, and is looked up with a binary
// search instead of a hash table, so the on-device cost is just
// (10 bytes x number of entries) of flash and a few dozen bytes of stack
// for the search -- no RAM table, no runtime text parsing, no file I/O at
// all on the device.
//
// This tool links against the REAL engine (board.c, movegen.c, attacks.c,
// zobrist.c) rather than reimplementing move generation or hashing, so
// every hash it computes is guaranteed to be byte-identical to what the
// same position hashes to on the actual device -- there is no separate
// "book hash function" to accidentally drift out of sync with the real
// one.
//
// Usage: gen_book_data <moves-only-opening-file> [max_entries]
//   Writes src/engine/book_data.h and src/engine/book_data.c in place.
//   max_entries (optional) caps the table size -- if the input file
//   produces more distinct positions than this, only the first
//   max_entries encountered (in file order) are kept. Omit it to keep
//   everything the input file produces; the real way to control flash
//   size is trimming the INPUT file, same as the host-testing book.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "board.h"
#include "movegen.h"
#include "attacks.h"
#include "zobrist.h"

// ---------------------------------------------------------------------------
// SAN parsing -- same approach as book.c's own parser (see that file's
// comment): replay each line move by move from the start position,
// matching each SAN token against the actual legal moves at that point.
// Duplicated here rather than shared, matching this project's existing
// convention of each host tool carrying its own copy of this logic (see
// match_nullmove_book.c, book.c).
// ---------------------------------------------------------------------------
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

static int square_from_name(const char *s) { return (s[0] - 'a') + (s[1] - '1') * 8; }

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

    if (len - 2 < 0) return 0;
    int to_sq = square_from_name(&san[len - 2]);

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
    return found_count == 1 ? found : 0;
}

static int tokenize_line(const char *line, char tokens[][16], int max_tokens) {
    char buf[2048];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int count = 0;
    char *tok = strtok(buf, " \t");
    while (tok && count < max_tokens) {
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

#define MAX_LINE_PLIES 32
#define MAX_ENTRIES 200000 // generous host-side collection cap; the real cap is max_entries (flash budget)

typedef struct {
    uint64_t hash;
    Move move;
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count = 0;

// First-seen wins on a duplicate position, same policy as book.c's runtime
// table -- deterministic, depends only on input file order.
static void collect_insert(uint64_t hash, Move move) {
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].hash == hash) return; // already have this position
    }
    if (entry_count >= MAX_ENTRIES) return;
    entries[entry_count].hash = hash;
    entries[entry_count].move = move;
    entry_count++;
}

static int compare_entries(const void *a, const void *b) {
    uint64_t ha = ((const Entry *)a)->hash, hb = ((const Entry *)b)->hash;
    return (ha > hb) - (ha < hb); // ascending, required for binary search on-device
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <moves-only-opening-file> [max_entries]\n", argv[0]);
        return 1;
    }
    long max_entries = argc > 2 ? atol(argv[2]) : -1; // -1 = no cap

    init_attack_tables();
    zobrist_init();

    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("fopen"); return 1; }

    char line[2048];
    long lines_read = 0;
    while (fgets(line, sizeof(line), f)) {
        if (max_entries >= 0 && entry_count >= max_entries) break; // input-order cap: keep the earliest-seen positions
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        lines_read++;

        char tokens[MAX_LINE_PLIES][16];
        int n_tokens = tokenize_line(line, tokens, MAX_LINE_PLIES);

        Board b;
        board_reset(&b);
        for (int i = 0; i < n_tokens; i++) {
            if (max_entries >= 0 && entry_count >= max_entries) break;
            Move m = parse_san(&b, tokens[i]);
            if (m == 0) break;
            collect_insert(b.hash, m);
            UndoInfo undo;
            make_move(&b, m, &undo);
        }
    }
    fclose(f);

    qsort(entries, entry_count, sizeof(Entry), compare_entries);

    fprintf(stderr, "Read %ld opening lines from %s\n", lines_read, argv[1]);
    fprintf(stderr, "Collected %d distinct positions%s\n", entry_count,
            max_entries >= 0 ? " (capped by max_entries)" : "");
    fprintf(stderr, "Flash footprint: %d bytes (%d hashes x 8B + %d moves x 2B)\n",
            entry_count * 10, entry_count, entry_count);

    FILE *h = fopen("src/engine/book_data.h", "w");
    if (!h) { perror("fopen book_data.h"); return 1; }
    fprintf(h,
        "// GENERATED by tools/gen_book_data.c -- do not hand-edit.\n"
        "// Regenerate with: tools/gen_book_data <opening-file> [max_entries]\n"
        "//\n"
        "// The device-ready opening book: two parallel `const` (flash-resident,\n"
        "// zero RAM cost) arrays, book_static_hashes[] sorted ascending so\n"
        "// book.c's book_probe_static() can binary-search it, and\n"
        "// book_static_moves[] holding each entry's move at the same index.\n"
        "#ifndef BOOK_DATA_H\n"
        "#define BOOK_DATA_H\n\n"
        "#include <stdint.h>\n\n"
        "#define BOOK_STATIC_COUNT %d\n\n"
        "extern const uint64_t book_static_hashes[BOOK_STATIC_COUNT];\n"
        "extern const uint16_t book_static_moves[BOOK_STATIC_COUNT];\n\n"
        "#endif\n",
        entry_count);
    fclose(h);

    FILE *c = fopen("src/engine/book_data.c", "w");
    if (!c) { perror("fopen book_data.c"); return 1; }
    fprintf(c, "// GENERATED by tools/gen_book_data.c -- do not hand-edit.\n");
    fprintf(c, "// Source: %s (%d distinct positions)\n", argv[1], entry_count);
    fprintf(c, "#include \"book_data.h\"\n\n");

    fprintf(c, "const uint64_t book_static_hashes[BOOK_STATIC_COUNT] = {\n");
    for (int i = 0; i < entry_count; i++) {
        fprintf(c, "0x%016llxULL,%s", (unsigned long long)entries[i].hash, (i % 4 == 3) ? "\n" : "");
    }
    fprintf(c, "\n};\n\n");

    fprintf(c, "const uint16_t book_static_moves[BOOK_STATIC_COUNT] = {\n");
    for (int i = 0; i < entry_count; i++) {
        fprintf(c, "%u,%s", (unsigned)entries[i].move, (i % 12 == 11) ? "\n" : "");
    }
    fprintf(c, "\n};\n");
    fclose(c);

    fprintf(stderr, "Wrote src/engine/book_data.h and src/engine/book_data.c\n");
    return 0;
}
