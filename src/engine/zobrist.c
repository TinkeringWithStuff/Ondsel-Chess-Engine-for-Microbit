// ============================================================================
// ZOBRIST TABLE GENERATION AND FROM-SCRATCH HASHING.
// See zobrist.h for the conceptual explanation of what this is and why XOR
// is the operation that makes incremental hashing work.
// ============================================================================
#include "zobrist.h"

uint64_t zobrist_piece[12][64];
uint64_t zobrist_side;
uint64_t zobrist_castle[16];
uint64_t zobrist_ep_file[8];

// splitmix64 -- a small, well-known, fixed-output pseudo-random number
// generator (Sebastiano Vigna's design, public domain). It's used ONLY to
// fill the fixed lookup tables above at startup, never for anything that
// needs genuine unpredictability -- so a plain deterministic generator is
// exactly right here, and is in fact BETTER than pulling in <stdlib.h>'s
// rand(): rand()'s exact sequence isn't guaranteed to be the same across
// different C library implementations, but this project needs the SAME
// zobrist tables every single run, on every machine, so that a host test
// build and the embedded micro:bit build compute identical hashes for
// identical positions, and so a recorded match/PGN is exactly reproducible
// from its fixed random seed.
static uint64_t splitmix64_state;
static uint64_t splitmix64_next(void) {
    uint64_t z = (splitmix64_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Fills every zobrist table with fresh pseudo-random values from a FIXED
// seed, so the exact same tables come out every time this runs, anywhere.
void zobrist_init(void) {
    splitmix64_state = 0x5EED5EEDC0FFEEULL; // fixed seed -> fixed tables, always
    for (int p = 0; p < 12; p++)
        for (int sq = 0; sq < 64; sq++)
            zobrist_piece[p][sq] = splitmix64_next();
    zobrist_side = splitmix64_next();
    for (int i = 0; i < 16; i++) zobrist_castle[i] = splitmix64_next();
    for (int f = 0; f < 8; f++) zobrist_ep_file[f] = splitmix64_next();
}

// Computes a position's hash the "slow but obviously correct" way: start
// from zero and XOR in every fact about the current position, one at a
// time. This is the reference implementation everything else trusts --
// make_move()/unmake_move() (movegen.c) maintain the SAME hash
// incrementally afterward (see zobrist.h's comment on why XOR makes that
// possible), but this from-scratch version is what actually seeds a
// freshly loaded position, since there's no prior move to update
// incrementally from.
uint64_t zobrist_hash_from_scratch(const Board *b) {
    uint64_t h = 0;

    // One XOR per occupied square, for whatever piece sits there.
    for (int sq = 0; sq < 64; sq++) {
        int8_t piece = b->mailbox[sq];
        if (piece == 0) continue; // empty squares contribute nothing
        int color = piece > 0 ? WHITE : BLACK;
        int ptype = piece > 0 ? piece : -piece;
        h ^= zobrist_piece[piece_index(color, ptype)][sq];
    }

    // Side to move: White contributes nothing (an arbitrary but consistent
    // choice -- only the DIFFERENCE between the two matters, so one side
    // has to be the "baseline"); Black XORs in zobrist_side.
    if (b->side_to_move == BLACK) h ^= zobrist_side;

    // Castling rights, packed into a 4-bit number (one bit per right) and
    // used as an index into a 16-entry table -- one random value per
    // possible combination of rights, rather than four independent XORs.
    // Either scheme works; this one was chosen simply because it mirrors
    // how make_move() already needs to compute "rights before" and
    // "rights after" as small integers anyway (see movegen.c).
    int rights = (b->castle_wk << 3) | (b->castle_wq << 2) | (b->castle_bk << 1) | b->castle_bq;
    h ^= zobrist_castle[rights];

    // En passant: only the FILE matters for hashing purposes (which rank
    // the en passant square is on is fully determined by whose turn it
    // is, so it carries no extra information), and only if a capture is
    // actually possible right now.
    if (b->ep_square >= 0) h ^= zobrist_ep_file[b->ep_square % 8];

    return h;
}
