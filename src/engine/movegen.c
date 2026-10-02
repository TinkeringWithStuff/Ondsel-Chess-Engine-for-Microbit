// ============================================================================
// MOVE GENERATION AND MAKE/UNMAKE.
// ============================================================================
// This is a direct algorithmic port of an already-validated move generator
// (verified against the full standard perft test suite) -- the LOGIC here
// is unchanged from that proven version; only the plumbing is different:
// real 64-bit bitboards instead of split 32-bit halves, a single
// pop-lowest-set-bit loop instead of separate high/low-half branches,
// direct return values instead of writing into shared scratch globals, and
// a fixed undo-info array (UndoInfo, indexed by ply) instead of a
// dynamically growing stack.
// ============================================================================
#include <string.h>
#include "movegen.h"
#include "attacks.h"
#include "zobrist.h"

MoveList move_pool[MAX_PLY];

// ---------------------------------------------------------------------------
// is_square_attacked(): "could ANY of `by_side`'s pieces move to `sq` right
// now?" -- checked one piece type at a time, each using whatever attack
// information is cheapest to test.
//
// THE KEY TRICK FOR PAWNS ("attacks-square", not "attacked-by-square"):
// rather than asking "which squares does a pawn on square X attack" for
// every one of the attacking side's actual pawns, this flips the question
// around: "if a pawn attacks `sq`, which squares could it be standing on?"
// -- and that set of candidate squares is EXACTLY the attack pattern of a
// pawn of the OPPOSITE color standing on `sq` itself (a white pawn attacks
// diagonally forward, so working backward from the target square uses the
// black-pawn attack shape, and vice versa). This means no extra
// square-specific tables are needed beyond the pawn_attacks_white/black
// tables already built for normal move generation -- the same lookup
// answers both "what does this pawn attack" and, read backward, "is this
// square attacked by a pawn".
// ---------------------------------------------------------------------------
bool is_square_attacked(const Board *b, int sq, int by_side) {
    if (knight_attacks[sq] & b->piece_bb[piece_index(by_side, KNIGHT)]) return true;
    if (king_attacks[sq] & b->piece_bb[piece_index(by_side, KING)]) return true;

    if (by_side == WHITE) {
        if (pawn_attacks_black[sq] & b->piece_bb[piece_index(WHITE, PAWN)]) return true;
    } else {
        if (pawn_attacks_white[sq] & b->piece_bb[piece_index(BLACK, PAWN)]) return true;
    }

    // Sliders: bishops and queens share diagonal reach, rooks and queens
    // share straight-line reach, so each check tests BOTH piece types that
    // could be responsible for an attack along that kind of line, in one
    // combined bitboard.
    Bitboard diag = b->piece_bb[piece_index(by_side, BISHOP)] | b->piece_bb[piece_index(by_side, QUEEN)];
    if (bishop_attacks(sq, b->occ_all) & diag) return true;

    Bitboard straight = b->piece_bb[piece_index(by_side, ROOK)] | b->piece_bb[piece_index(by_side, QUEEN)];
    if (rook_attacks(sq, b->occ_all) & straight) return true;

    return false;
}

static void push_move(MoveList *list, int from, int to, int flag) {
    list->moves[list->count++] = encode_move(from, to, flag);
}

// Generates every PSEUDO-legal move (obeys piece movement rules, but may
// still leave the mover's own king in check -- see movegen.h's comment on
// why that final legality check is left to the caller) for whichever side
// is to move, into move_pool[ply].
void generate_pseudo_moves(Board *b, int ply) {
    MoveList *list = &move_pool[ply];
    list->count = 0;
    int side = b->side_to_move;
    int opponent = side == WHITE ? BLACK : WHITE;
    Bitboard own = b->occ[side];
    Bitboard enemy = b->occ[opponent];
    Bitboard not_own = ~own; // a piece can move anywhere except onto its own side's pieces

    // --- Knights and king (leapers): look up their precomputed attack
    // pattern, mask out squares occupied by their own side, and every
    // remaining target square is a legal-shaped move (a capture if the
    // enemy is there, otherwise quiet).
    Bitboard bb = b->piece_bb[piece_index(side, KNIGHT)];
    while (bb) {
        int from = bb_pop_lsb(&bb);
        Bitboard targets = knight_attacks[from] & not_own;
        while (targets) {
            int to = bb_pop_lsb(&targets);
            push_move(list, from, to, (enemy & bb_square(to)) ? MOVE_CAPTURE : MOVE_QUIET);
        }
    }
    bb = b->piece_bb[piece_index(side, KING)];
    while (bb) {
        int from = bb_pop_lsb(&bb);
        Bitboard targets = king_attacks[from] & not_own;
        while (targets) {
            int to = bb_pop_lsb(&targets);
            push_move(list, from, to, (enemy & bb_square(to)) ? MOVE_CAPTURE : MOVE_QUIET);
        }
    }

    // --- Bishops, rooks, queens (sliders): same idea, but their attack
    // set has to be computed on the fly against the current occupancy
    // (attacks.c's ray-walk) rather than looked up, since -- unlike
    // leapers -- their reach depends on what's blocking them right now.
    bb = b->piece_bb[piece_index(side, BISHOP)];
    while (bb) {
        int from = bb_pop_lsb(&bb);
        Bitboard targets = bishop_attacks(from, b->occ_all) & not_own;
        while (targets) {
            int to = bb_pop_lsb(&targets);
            push_move(list, from, to, (enemy & bb_square(to)) ? MOVE_CAPTURE : MOVE_QUIET);
        }
    }
    bb = b->piece_bb[piece_index(side, ROOK)];
    while (bb) {
        int from = bb_pop_lsb(&bb);
        Bitboard targets = rook_attacks(from, b->occ_all) & not_own;
        while (targets) {
            int to = bb_pop_lsb(&targets);
            push_move(list, from, to, (enemy & bb_square(to)) ? MOVE_CAPTURE : MOVE_QUIET);
        }
    }
    bb = b->piece_bb[piece_index(side, QUEEN)];
    while (bb) {
        int from = bb_pop_lsb(&bb);
        Bitboard targets = queen_attacks(from, b->occ_all) & not_own;
        while (targets) {
            int to = bb_pop_lsb(&targets);
            push_move(list, from, to, (enemy & bb_square(to)) ? MOVE_CAPTURE : MOVE_QUIET);
        }
    }

    // --- Pawns: the one piece type that needs bespoke handling instead of
    // "look up an attack table, mask, done" -- pawns move differently
    // depending on whether they're pushing (straight ahead, no capture
    // allowed, sometimes two squares from their start rank) or capturing
    // (diagonally only), can promote on reaching the far rank, and have
    // the one-off en passant rule. Handled as separate White/Black cases
    // since pushes and captures both go in opposite directions for the
    // two colors.
    bb = b->piece_bb[piece_index(side, PAWN)];
    while (bb) {
        int from = bb_pop_lsb(&bb);
        int rank = from / 8;

        if (side == WHITE) {
            int to1 = from + 8;
            if (to1 <= 63 && b->mailbox[to1] == EMPTY) {
                if (to1 >= 56) {
                    // Reached the 8th rank: this push promotes. All four
                    // promotion choices are always offered as separate
                    // moves; move ordering/search decides which (if any)
                    // is worth exploring.
                    push_move(list, from, to1, MOVE_PROMO_QUEEN);
                    push_move(list, from, to1, MOVE_PROMO_ROOK);
                    push_move(list, from, to1, MOVE_PROMO_BISHOP);
                    push_move(list, from, to1, MOVE_PROMO_KNIGHT);
                } else {
                    push_move(list, from, to1, MOVE_QUIET);
                    // A pawn still on its own starting rank (rank index 1
                    // = rank 2 for White) may push two squares at once,
                    // but only if BOTH squares ahead of it are empty (the
                    // one-square check above already confirmed the first;
                    // this confirms the second).
                    if (rank == 1 && b->mailbox[from + 16] == EMPTY) {
                        push_move(list, from, from + 16, MOVE_DOUBLE_PAWN_PUSH);
                    }
                }
            }
            Bitboard att = pawn_attacks_white[from] & enemy; // pawns can only capture, never push, diagonally
            while (att) {
                int to = bb_pop_lsb(&att);
                if (to >= 56) {
                    push_move(list, from, to, MOVE_PROMO_QUEEN_CAPTURE);
                    push_move(list, from, to, MOVE_PROMO_ROOK_CAPTURE);
                    push_move(list, from, to, MOVE_PROMO_BISHOP_CAPTURE);
                    push_move(list, from, to, MOVE_PROMO_KNIGHT_CAPTURE);
                } else {
                    push_move(list, from, to, MOVE_CAPTURE);
                }
            }
            // En passant: only legal if the board currently remembers an
            // en passant square (set by the immediately preceding double
            // pawn push -- see board.h's ep_square comment) AND this pawn
            // actually attacks it.
            if (b->ep_square >= 0 && (pawn_attacks_white[from] & bb_square(b->ep_square))) {
                push_move(list, from, b->ep_square, MOVE_EP_CAPTURE);
            }
        } else {
            // Black pawns: the exact mirror image -- "forward" is -8
            // instead of +8, the start rank is rank index 6 instead of 1,
            // and promotion happens on reaching rank index 0 instead of 7.
            int to1 = from - 8;
            if (to1 >= 0 && b->mailbox[to1] == EMPTY) {
                if (to1 <= 7) {
                    push_move(list, from, to1, MOVE_PROMO_QUEEN);
                    push_move(list, from, to1, MOVE_PROMO_ROOK);
                    push_move(list, from, to1, MOVE_PROMO_BISHOP);
                    push_move(list, from, to1, MOVE_PROMO_KNIGHT);
                } else {
                    push_move(list, from, to1, MOVE_QUIET);
                    if (rank == 6 && b->mailbox[from - 16] == EMPTY) {
                        push_move(list, from, from - 16, MOVE_DOUBLE_PAWN_PUSH);
                    }
                }
            }
            Bitboard att = pawn_attacks_black[from] & enemy;
            while (att) {
                int to = bb_pop_lsb(&att);
                if (to <= 7) {
                    push_move(list, from, to, MOVE_PROMO_QUEEN_CAPTURE);
                    push_move(list, from, to, MOVE_PROMO_ROOK_CAPTURE);
                    push_move(list, from, to, MOVE_PROMO_BISHOP_CAPTURE);
                    push_move(list, from, to, MOVE_PROMO_KNIGHT_CAPTURE);
                } else {
                    push_move(list, from, to, MOVE_CAPTURE);
                }
            }
            if (b->ep_square >= 0 && (pawn_attacks_black[from] & bb_square(b->ep_square))) {
                push_move(list, from, b->ep_square, MOVE_EP_CAPTURE);
            }
        }
    }

    // --- Castling: needs its own dedicated logic since it's the one move
    // in chess governed by rules other than "where can this piece
    // normally go" -- it requires an unmoved king and rook (tracked via
    // the castle_* rights flags, not by checking piece history), every
    // square between them empty, AND the king not currently in check, not
    // passing through check, and not landing in check (the three
    // is_square_attacked() checks below, one per square the king crosses,
    // INCLUDING its start and end squares).
    if (side == WHITE) {
        if (b->castle_wk && b->mailbox[5] == EMPTY && b->mailbox[6] == EMPTY &&
            !is_square_attacked(b, 4, opponent) && !is_square_attacked(b, 5, opponent) && !is_square_attacked(b, 6, opponent)) {
            push_move(list, 4, 6, MOVE_KING_CASTLE);
        }
        if (b->castle_wq && b->mailbox[3] == EMPTY && b->mailbox[2] == EMPTY && b->mailbox[1] == EMPTY &&
            !is_square_attacked(b, 4, opponent) && !is_square_attacked(b, 3, opponent) && !is_square_attacked(b, 2, opponent)) {
            push_move(list, 4, 2, MOVE_QUEEN_CASTLE);
        }
    } else {
        if (b->castle_bk && b->mailbox[61] == EMPTY && b->mailbox[62] == EMPTY &&
            !is_square_attacked(b, 60, opponent) && !is_square_attacked(b, 61, opponent) && !is_square_attacked(b, 62, opponent)) {
            push_move(list, 60, 62, MOVE_KING_CASTLE);
        }
        if (b->castle_bq && b->mailbox[59] == EMPTY && b->mailbox[58] == EMPTY && b->mailbox[57] == EMPTY &&
            !is_square_attacked(b, 60, opponent) && !is_square_attacked(b, 59, opponent) && !is_square_attacked(b, 58, opponent)) {
            push_move(list, 60, 58, MOVE_QUEEN_CASTLE);
        }
    }
}

// Removes whatever piece is on `sq` from BOTH the mailbox and its
// bitboard, given the piece's signed value (so the caller only has to look
// it up once). A small shared helper since "take a piece off the board"
// happens for several different reasons in make_move()/unmake_move()
// below: a normal capture, an en passant capture (a different square than
// the move's own destination), and temporarily during castling's rook
// relocation.
static void remove_piece_at(Board *b, int sq, int piece_value) {
    b->mailbox[sq] = EMPTY;
    int color = piece_value > 0 ? WHITE : BLACK;
    int ptype = piece_value > 0 ? piece_value : -piece_value;
    b->piece_bb[piece_index(color, ptype)] &= ~bb_square(sq);
}

// Turns off whichever castling right(s) are voided by something happening
// on `sq` -- called with BOTH the move's `from` and `to` square, because
// castling rights can be lost two different ways: the king or rook itself
// moving away (`from`), or an enemy piece capturing a rook that was still
// sitting on its home square (`to`) without that rook ever having moved.
static void update_castling_rights_for_square(Board *b, int sq) {
    if (sq == 4) { b->castle_wk = false; b->castle_wq = false; }      // white king's home square
    else if (sq == 0) { b->castle_wq = false; }                        // white queenside rook's home
    else if (sq == 7) { b->castle_wk = false; }                        // white kingside rook's home
    else if (sq == 60) { b->castle_bk = false; b->castle_bq = false; } // black king's home square
    else if (sq == 56) { b->castle_bq = false; }                       // black queenside rook's home
    else if (sq == 63) { b->castle_bk = false; }                       // black kingside rook's home
}

// Plays `m` on the board: updates mailbox, bitboards, occupancy, castling
// rights, en passant state, and the Zobrist hash together, so the Board is
// left in a fully consistent state for whatever comes next (another
// make_move, an is_square_attacked() legality check, evaluate(), etc).
// Snapshots everything into `undo` needed to reverse this exact call.
void make_move(Board *b, Move m, UndoInfo *undo) {
    int from = move_from(m), to = move_to(m), flag = move_flag(m);
    int moving = b->mailbox[from];
    int captured = b->mailbox[to];
    int moving_side = b->side_to_move;
    int opponent_side = moving_side == WHITE ? BLACK : WHITE;
    int moving_type = moving > 0 ? moving : -moving;

    // Snapshot everything unmake_move() will need to restore, BEFORE any
    // of it changes below.
    undo->hash_before = b->hash;
    undo->captured_piece = (int8_t)captured;
    undo->ep_square = b->ep_square;
    undo->castle_wk = b->castle_wk; undo->castle_wq = b->castle_wq;
    undo->castle_bk = b->castle_bk; undo->castle_bq = b->castle_bq;

    // Build the new hash by XORing in each change as it happens (see
    // zobrist.h: XOR is its own inverse, so "this piece is leaving this
    // square" is exactly the same operation as "this piece is arriving
    // there" -- each is just XOR-ing that piece/square's random value).
    uint64_t h = b->hash;
    h ^= zobrist_piece[piece_index(moving_side, moving_type)][from]; // mover leaves its origin square

    if (flag == MOVE_EP_CAPTURE) {
        // En passant's victim pawn is NOT on the destination square `to`
        // -- it's the pawn immediately behind it (one rank toward the
        // capturing side), still sitting where its double push left it.
        int cap_sq = moving_side == WHITE ? to - 8 : to + 8;
        int8_t ep_captured = b->mailbox[cap_sq];
        h ^= zobrist_piece[piece_index(opponent_side, PAWN)][cap_sq];
        remove_piece_at(b, cap_sq, ep_captured);
    } else if (captured != EMPTY) {
        int captured_type = captured > 0 ? captured : -captured;
        h ^= zobrist_piece[piece_index(opponent_side, captured_type)][to];
        remove_piece_at(b, to, captured);
    }

    remove_piece_at(b, from, moving);
    int placed = moving;
    if (flag >= MOVE_PROMO_KNIGHT) {
        // Promotions replace the pawn with the chosen piece type as it
        // lands, rather than placing a pawn on the back rank and
        // separately upgrading it.
        int promo_type = move_promotion_piece_type(m);
        placed = moving_side == WHITE ? promo_type : -promo_type;
    }
    board_place(b, to, placed);
    int placed_type = placed > 0 ? placed : -placed;
    h ^= zobrist_piece[piece_index(moving_side, placed_type)][to]; // mover (or its promoted form) arrives

    // Castling also relocates the rook, as a second piece movement bundled
    // into the same move.
    if (flag == MOVE_KING_CASTLE) {
        if (moving_side == WHITE) {
            remove_piece_at(b, 7, b->mailbox[7]); board_place(b, 5, ROOK);
            h ^= zobrist_piece[piece_index(WHITE, ROOK)][7]; h ^= zobrist_piece[piece_index(WHITE, ROOK)][5];
        } else {
            remove_piece_at(b, 63, b->mailbox[63]); board_place(b, 61, -ROOK);
            h ^= zobrist_piece[piece_index(BLACK, ROOK)][63]; h ^= zobrist_piece[piece_index(BLACK, ROOK)][61];
        }
    } else if (flag == MOVE_QUEEN_CASTLE) {
        if (moving_side == WHITE) {
            remove_piece_at(b, 0, b->mailbox[0]); board_place(b, 3, ROOK);
            h ^= zobrist_piece[piece_index(WHITE, ROOK)][0]; h ^= zobrist_piece[piece_index(WHITE, ROOK)][3];
        } else {
            remove_piece_at(b, 56, b->mailbox[56]); board_place(b, 59, -ROOK);
            h ^= zobrist_piece[piece_index(BLACK, ROOK)][56]; h ^= zobrist_piece[piece_index(BLACK, ROOK)][59];
        }
    }

    // A double pawn push creates a fresh en passant opportunity on the
    // square it passed through; every other move type clears it (en
    // passant is only ever legal on the very next move).
    int new_ep = -1;
    if (flag == MOVE_DOUBLE_PAWN_PUSH) new_ep = moving_side == WHITE ? from + 8 : from - 8;

    update_castling_rights_for_square(b, from);
    update_castling_rights_for_square(b, to);

    // XORing both the pre- and post-move castling-rights hash contribution
    // unconditionally is correct even when rights didn't actually change:
    // XORing the same value in twice cancels back out to a no-op, so this
    // never needs an if-changed check -- it's simpler AND correct to just
    // always do both.
    int old_rights = (undo->castle_wk << 3) | (undo->castle_wq << 2) | (undo->castle_bk << 1) | undo->castle_bq;
    int new_rights = (b->castle_wk << 3) | (b->castle_wq << 2) | (b->castle_bk << 1) | b->castle_bq;
    h ^= zobrist_castle[old_rights];
    h ^= zobrist_castle[new_rights];

    // Same "unconditionally XOR both old and new" pattern for en passant:
    // clear the old file's contribution (if there was one) and set the
    // new file's contribution (if there is one).
    if (undo->ep_square >= 0) h ^= zobrist_ep_file[undo->ep_square % 8];
    if (new_ep >= 0) h ^= zobrist_ep_file[new_ep % 8];
    h ^= zobrist_side; // whoever's turn it is always flips

    b->ep_square = new_ep;
    b->side_to_move = opponent_side;
    b->hash = h;
    board_recompute_occupancy(b);
}

// Reverses exactly the most recent make_move(m, undo) call. Notably does
// NOT reverse the hash's XOR math step by step -- it doesn't need to,
// since make_move() already snapshotted the pre-move hash into
// undo->hash_before, and simply restoring that snapshot is both correct
// and far simpler than replaying the XORs backward.
void unmake_move(Board *b, Move m, const UndoInfo *undo) {
    int from = move_from(m), to = move_to(m), flag = move_flag(m);

    b->side_to_move = b->side_to_move == WHITE ? BLACK : WHITE;
    int moving_side = b->side_to_move; // the side that originally made this move

    b->castle_wk = undo->castle_wk; b->castle_wq = undo->castle_wq;
    b->castle_bk = undo->castle_bk; b->castle_bq = undo->castle_bq;
    b->ep_square = undo->ep_square;
    int captured = undo->captured_piece;

    // Undo the rook relocation first, if this was a castle -- order
    // matters here only in the sense that it must happen before we start
    // reasoning about what belongs back on the `from`/`to` squares below.
    if (flag == MOVE_KING_CASTLE) {
        if (moving_side == WHITE) { remove_piece_at(b, 5, b->mailbox[5]); board_place(b, 7, ROOK); }
        else { remove_piece_at(b, 61, b->mailbox[61]); board_place(b, 63, -ROOK); }
    } else if (flag == MOVE_QUEEN_CASTLE) {
        if (moving_side == WHITE) { remove_piece_at(b, 3, b->mailbox[3]); board_place(b, 0, ROOK); }
        else { remove_piece_at(b, 59, b->mailbox[59]); board_place(b, 56, -ROOK); }
    }

    // Move the piece back from `to` to `from`. If this move was a
    // promotion, what's currently sitting on `to` is the PROMOTED piece,
    // not the original pawn -- so what goes back to `from` must be a plain
    // pawn, not whatever it was promoted into.
    int current_at_to = b->mailbox[to];
    remove_piece_at(b, to, current_at_to);
    int original = current_at_to;
    if (flag >= MOVE_PROMO_KNIGHT) original = moving_side == WHITE ? PAWN : -PAWN;
    board_place(b, from, original);

    // Restore whatever was captured, if anything. En passant's victim goes
    // back on the square BEHIND `to`, exactly where make_move() removed it
    // from -- not on `to` itself, which is where every other kind of
    // capture's victim would be restored.
    if (flag == MOVE_EP_CAPTURE) {
        int cap_sq = moving_side == WHITE ? to - 8 : to + 8;
        board_place(b, cap_sq, moving_side == WHITE ? -PAWN : PAWN);
    } else if (captured != EMPTY) {
        board_place(b, to, captured);
    }

    b->hash = undo->hash_before; // simply restored, not reversed -- see this function's own comment above

    board_recompute_occupancy(b);
}

// See movegen.h's comment on why this exists (a correctness check against
// well-known published node counts, not something search or gameplay ever
// calls). Pure depth-first enumeration: at depth 0 there's exactly one
// leaf (the current position itself); otherwise, try every pseudo-legal
// move, keep only the ones that don't leave the mover's own king in check,
// and recurse.
long long perft(Board *b, int depth, int ply) {
    if (depth == 0) return 1;
    int side = b->side_to_move;
    int opponent = side == WHITE ? BLACK : WHITE;
    generate_pseudo_moves(b, ply);
    MoveList *list = &move_pool[ply];
    long long count = 0;
    for (int i = 0; i < list->count; i++) {
        Move m = list->moves[i];
        UndoInfo undo;
        make_move(b, m, &undo);
        int king_sq = bb_lsb_index(b->piece_bb[piece_index(side, KING)]);
        if (!is_square_attacked(b, king_sq, opponent)) {
            count += perft(b, depth - 1, ply + 1);
        }
        unmake_move(b, m, &undo);
    }
    return count;
}
