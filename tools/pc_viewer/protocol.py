"""Parsing and game-state logic for the Ondsel self-play PC viewer.

Deliberately separate from any GUI code (see ondsel_viewer.py) so this half
-- the part that actually has to be correct -- can be unit-tested without a
display or a real micro:bit attached. See test_protocol.py.

No external chess library: the device is the one deciding legality (it's a
real chess engine, it never sends an illegal move), so the viewer only has
to APPLY a UCI move to a board it's tracking, not validate or generate
moves itself. That's a small enough job to write directly, which also means
this viewer's only real dependency is pyserial -- the one thing it actually
can't do without, since that's what talks to the device.

WIRE PROTOCOL (matches src/main_play_test.c's "PC viewer protocol" comment
exactly -- read that comment for the reasoning, this is just the mirror of
it):

    Each event is one line, of the form

        <payload>*XX\r\n

    where XX is a two-digit uppercase hex checksum: the plain 8-bit sum of
    every byte in <payload>, matching uart.h's uart_checksum8() byte for
    byte. Every line is sent PROTO_REPEATS times back to back by the device
    -- a corrupted copy is simply one whose checksum doesn't match and gets
    discarded; an intact repeat of a line already applied is recognized by
    being identical to the last ACCEPTED payload and skipped, so only
    genuinely new events get applied even though several copies of the same
    one arrive.

    <payload> is one of:
        GAME_START
        MOVE <uci> <score> [<depth> <nodes> <time_ms>]
                                -- <uci> like "e2e4" or "e7e8q" (promotion),
                                   <score> a signed integer (centipawns,
                                   from the mover's own eval). The three
                                   trailing fields are optional (present on
                                   every line the current firmware sends,
                                   but SimulatedLineSource's older demo
                                   lines and any future stripped-down sender
                                   may omit them): <depth> is the deepest
                                   iterative-deepening depth that fully
                                   completed (0 for a book move), <nodes>
                                   the node count that search took,
                                   <time_ms> the wall-clock milliseconds it
                                   actually ran.
        GAME_END <code>         -- CHECKMATE_WHITE, CHECKMATE_BLACK,
                                    STALEMATE, STOPPED, or one of the three
                                    "ended early" codes below.

    <score>/MoveRecord.score is a NEGAMAX figure: it's always from the
    perspective of whoever just moved, so positive doesn't consistently mean
    "White is doing well" -- it flips meaning every other ply (+50 after a
    White move is good for White; +50 after a Black move is good for BLACK,
    i.e. roughly -50 for White). MoveRecord.white_relative_score is the same
    number converted to a single consistent White-is-positive figure, which
    is what the GUI and PGN export both actually display -- see
    GameState.apply_payload()'s own comment for the conversion.

    REVERSE CHANNEL (PC -> device, for the "End Game" button): a single
    ASCII 'E' byte, sent several times back to back for the same reliability
    reason the forward direction repeats lines (this link drops bytes
    occasionally; the device only needs to see ONE 'E' to act, so repeats
    are harmless rather than needing de-duplication). No checksum -- see
    uart.h's own comment on this for why a single idempotent command doesn't
    need one. The device responds by ending the current self-play game
    immediately and broadcasting one of:
        GAME_END ADJUDICATED_WHITE   -- White was >=300cp ahead, called for White
        GAME_END ADJUDICATED_BLACK   -- Black was >=300cp ahead, called for Black
        GAME_END ADJUDICATED_DRAW    -- neither side was far enough ahead
    (threshold and exact wording: see main_play_test.c's adjudicate_result())
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


def line_checksum(payload: str) -> int:
    """The same plain unsigned 8-bit additive checksum as uart.h's
    uart_checksum8(): sum every byte, keep only the low 8 bits."""
    return sum(payload.encode("ascii", errors="replace")) & 0xFF


class LineDecoder:
    """Turns raw serial lines into validated, de-duplicated payload strings.

    Feed it raw lines (with or without trailing \\r\\n) as they arrive;
    get back the payload string once for each genuinely new, checksum-valid
    event, or None for a corrupted line or a repeat of the line already
    accepted.
    """

    def __init__(self) -> None:
        self.last_accepted: Optional[str] = None
        self.corrupt_count = 0
        self.accepted_count = 0
        self.duplicate_count = 0

    def feed_raw(self, raw_line: str) -> Optional[str]:
        line = raw_line.strip("\r\n")
        if len(line) < 3 or line[-3] != "*":
            self.corrupt_count += 1
            return None

        payload, checksum_hex = line[:-3], line[-2:]
        try:
            claimed = int(checksum_hex, 16)
        except ValueError:
            self.corrupt_count += 1
            return None

        if line_checksum(payload) != claimed:
            self.corrupt_count += 1
            return None

        if payload == self.last_accepted:
            self.duplicate_count += 1
            return None

        self.last_accepted = payload
        self.accepted_count += 1
        return payload


# ---------------------------------------------------------------------------
# Minimal board tracker: 64 squares, index = rank * 8 + file (a1=0, h8=63),
# same numbering the engine itself uses (see board.h's own square numbering)
# -- pieces are single letters, uppercase White / lowercase Black, matching
# board_render.h's own OLED convention on the device.
# ---------------------------------------------------------------------------
START_RANKS = ["RNBQKBNR", "PPPPPPPP", "", "", "", "", "pppppppp", "rnbqkbnr"]


class Board:
    def __init__(self) -> None:
        self.squares: list[Optional[str]] = [None] * 64
        for rank_idx, rank in enumerate(START_RANKS):
            for file_idx, ch in enumerate(rank):
                self.squares[rank_idx * 8 + file_idx] = ch
        self.white_to_move = True

    @staticmethod
    def _sq(name: str) -> int:
        file_idx = ord(name[0]) - ord("a")
        rank_idx = int(name[1]) - 1
        return rank_idx * 8 + file_idx

    def piece_at(self, file_idx: int, rank_idx: int) -> Optional[str]:
        return self.squares[rank_idx * 8 + file_idx]

    def push_uci(self, uci: str) -> None:
        """Applies a move already known to be legal (the device is the
        authority on that) -- relocates the piece and handles the three
        special cases (en passant, castling, promotion) a plain "copy the
        piece to its destination" wouldn't."""
        from_sq = self._sq(uci[0:2])
        to_sq = self._sq(uci[2:4])
        promo = uci[4] if len(uci) > 4 else None

        piece = self.squares[from_sq]
        if piece is None:
            raise ValueError(f"no piece on {uci[0:2]} to move (uci={uci!r})")
        is_white = piece.isupper()
        captured = self.squares[to_sq]

        from_file, from_rank = from_sq % 8, from_sq // 8
        to_file, to_rank = to_sq % 8, to_sq // 8

        # En passant: a pawn moving diagonally onto an EMPTY square can only
        # mean this -- the captured pawn sits on the mover's own rank, at
        # the destination's file.
        if piece.upper() == "P" and from_file != to_file and captured is None:
            captured_sq = from_rank * 8 + to_file
            self.squares[captured_sq] = None

        # Castling: a king moving two files also drags the matching rook
        # across, UCI notation only encodes the king's own from/to.
        if piece.upper() == "K" and abs(to_file - from_file) == 2:
            rook_rank = from_rank
            if to_file == 6:  # kingside
                rook_from, rook_to = rook_rank * 8 + 7, rook_rank * 8 + 5
            else:  # queenside (to_file == 2)
                rook_from, rook_to = rook_rank * 8 + 0, rook_rank * 8 + 3
            self.squares[rook_to] = self.squares[rook_from]
            self.squares[rook_from] = None

        moved_piece = piece
        if promo:
            moved_piece = promo.upper() if is_white else promo.lower()

        self.squares[to_sq] = moved_piece
        self.squares[from_sq] = None
        self.white_to_move = not self.white_to_move


@dataclass
class MoveRecord:
    uci: str
    score: int  # the MOVER's own-perspective eval -- see module docstring's
                # negamax note. Positive always means "good for whoever just
                # played this move", which flips meaning every other ply.
    white_relative_score: int  # the same eval, converted to White's
                                # perspective (positive always means good for
                                # White, regardless of whose move it was) --
                                # computed once here so nothing downstream
                                # (the GUI, PGN export) has to re-derive it
                                # from move parity.
    depth: Optional[int] = None
    nodes: Optional[int] = None
    time_ms: Optional[int] = None


@dataclass
class GameState:
    """The live game the viewer is following, updated purely by feeding it
    decoded protocol events -- no serial/GUI knowledge here at all."""

    board: Board = field(default_factory=Board)
    moves: list[MoveRecord] = field(default_factory=list)
    in_progress: bool = False
    result_text: Optional[str] = None
    last_error: Optional[str] = None

    result_code: Optional[str] = None  # the raw GAME_END code (e.g.
                                        # "CHECKMATE_WHITE"), kept alongside
                                        # result_text's human-readable
                                        # version since PGN export needs the
                                        # code, not the prose, to pick a
                                        # "[Result ...]" tag.

    RESULT_TEXT = {
        "CHECKMATE_WHITE": "Checkmate -- White wins",
        "CHECKMATE_BLACK": "Checkmate -- Black wins",
        "STALEMATE": "Stalemate -- draw",
        "STOPPED": "Stopped early",
        "ADJUDICATED_WHITE": "Ended early -- White ahead, called for White",
        "ADJUDICATED_BLACK": "Ended early -- Black ahead, called for Black",
        "ADJUDICATED_DRAW": "Ended early -- called a draw",
    }

    # PGN "[Result ...]" tag for each GAME_END code. "*" (unknown/ongoing)
    # for anything not listed here, including STOPPED -- a plain early stop
    # genuinely has no result, unlike an adjudicated one.
    PGN_RESULT = {
        "CHECKMATE_WHITE": "1-0",
        "CHECKMATE_BLACK": "0-1",
        "STALEMATE": "1/2-1/2",
        "ADJUDICATED_WHITE": "1-0",
        "ADJUDICATED_BLACK": "0-1",
        "ADJUDICATED_DRAW": "1/2-1/2",
    }

    def apply_payload(self, payload: str) -> None:
        if payload == "GAME_START":
            self.board = Board()
            self.moves = []
            self.in_progress = True
            self.result_text = None
            self.result_code = None
            self.last_error = None
            return

        if payload.startswith("MOVE "):
            parts = payload.split(" ")
            # 3 fields: older/minimal senders (SimulatedLineSource's demo
            # game, in particular) that only ever sent uci+score. 6 fields:
            # the current firmware, which adds depth/nodes/time_ms. Anything
            # else is malformed.
            if len(parts) not in (3, 6):
                self.last_error = f"malformed MOVE line: {payload!r}"
                return
            uci, score_str = parts[1], parts[2]
            try:
                score = int(score_str)
            except ValueError:
                self.last_error = f"malformed score in: {payload!r}"
                return

            depth = nodes = time_ms = None
            if len(parts) == 6:
                try:
                    depth = int(parts[3])
                    nodes = int(parts[4])
                    time_ms = int(parts[5])
                except ValueError:
                    self.last_error = f"malformed stats in: {payload!r}"
                    return

            # White-relative conversion: moves[] is empty-so-far length i
            # (0-based) BEFORE this append, and ply 0/2/4/... (even) is
            # always White's, same parity the GUI's move-list numbering
            # already relies on -- so "is this move's index even" tells us
            # who just moved without needing to track whose turn it was
            # separately.
            is_white_move = len(self.moves) % 2 == 0
            white_relative_score = score if is_white_move else -score

            try:
                self.board.push_uci(uci)
                self.moves.append(MoveRecord(
                    uci=uci, score=score, white_relative_score=white_relative_score,
                    depth=depth, nodes=nodes, time_ms=time_ms,
                ))
            except (ValueError, IndexError) as exc:
                self.last_error = f"could not apply move {uci!r}: {exc}"
            return

        if payload.startswith("GAME_END "):
            code = payload[len("GAME_END "):]
            self.in_progress = False
            self.result_code = code
            self.result_text = self.RESULT_TEXT.get(code, f"Game over ({code})")
            return

        self.last_error = f"unrecognized line: {payload!r}"


# ---------------------------------------------------------------------------
# PGN export: turns a finished (or in-progress) GameState into standard PGN
# text, with each move annotated with a comment carrying the stats this
# viewer already tracks (eval, depth, nodes, time) -- so a saved game keeps
# that data instead of reducing it to a bare move list.
#
# Needs real SAN (Standard Algebraic Notation, "Nf3" not "g1f3") rather than
# just echoing the wire format's UCI moves, since UCI isn't valid PGN
# movetext. Converting UCI -> SAN from a tracked board is a smaller problem
# than it sounds: the device is the only source of moves and it never sends
# an illegal one, so this doesn't need to GENERATE or validate legal moves
# in general -- it only needs to describe the one move already being made,
# which means pseudo-legal "could another same-type piece also reach this
# square" disambiguation is enough; it never has to rule out a candidate
# because it would leave its own king in check. (One real gap from that
# shortcut: a pseudo-legal-but-pinned piece could in theory make SAN
# disambiguate more than strict SAN would. That's a cosmetic risk -- an
# unnecessary disambiguating letter a real chess GUI wouldn't print -- not a
# wrong move, and rare enough in practice not to be worth hand-rolling full
# check detection for every candidate to close.)
# ---------------------------------------------------------------------------

def _slide_can_reach(squares: list, from_sq: int, to_sq: int, deltas: list) -> bool:
    """True if a slider (bishop/rook/queen) sitting on from_sq has a clear
    path (no piece in the way, friendly or not -- the square actually
    arrived at is handled by the caller already knowing it's a legal
    destination) to to_sq along one of `deltas`."""
    from_file, from_rank = from_sq % 8, from_sq // 8
    to_file, to_rank = to_sq % 8, to_sq // 8
    for df, dr in deltas:
        f, r = from_file + df, from_rank + dr
        while 0 <= f < 8 and 0 <= r < 8:
            if (f, r) == (to_file, to_rank):
                return True
            if squares[r * 8 + f] is not None:
                break  # blocked along this direction before reaching to_sq
            f, r = f + df, r + dr
    return False


_BISHOP_DELTAS = [(1, 1), (1, -1), (-1, 1), (-1, -1)]
_ROOK_DELTAS = [(1, 0), (-1, 0), (0, 1), (0, -1)]
_QUEEN_DELTAS = _BISHOP_DELTAS + _ROOK_DELTAS


def _piece_can_reach(squares: list, piece_type: str, from_sq: int, to_sq: int) -> bool:
    """piece_type is one of 'N','B','R','Q' (pawns and kings are handled
    separately by the caller -- a pawn's SAN disambiguation rule doesn't
    need this at all, and there's only ever one king)."""
    if piece_type == "N":
        from_file, from_rank = from_sq % 8, from_sq // 8
        to_file, to_rank = to_sq % 8, to_sq // 8
        return (abs(from_file - to_file), abs(from_rank - to_rank)) in {(1, 2), (2, 1)}
    if piece_type == "B":
        return _slide_can_reach(squares, from_sq, to_sq, _BISHOP_DELTAS)
    if piece_type == "R":
        return _slide_can_reach(squares, from_sq, to_sq, _ROOK_DELTAS)
    if piece_type == "Q":
        return _slide_can_reach(squares, from_sq, to_sq, _QUEEN_DELTAS)
    return False


def _square_attacked(squares: list, sq: int, by_white: bool) -> bool:
    """Is `sq` attacked by any piece of color `by_white`? Used only to add
    '+'/'#' to SAN -- not part of move legality anywhere in this file (the
    device is the legality authority; see this section's own top comment)."""
    file0, rank0 = sq % 8, sq // 8

    # Pawn attacks: a White pawn on (f, r) attacks (f-1, r+1) and (f+1, r+1)
    # -- so a pawn attacking `sq` sits one rank BELOW it (from White's
    # perspective) at an adjacent file; mirrored for Black.
    pawn_rank_offset = -1 if by_white else 1
    for df in (-1, 1):
        f, r = file0 + df, rank0 + pawn_rank_offset
        if 0 <= f < 8 and 0 <= r < 8:
            p = squares[r * 8 + f]
            if p and p.upper() == "P" and p.isupper() == by_white:
                return True

    for df, dr in [(1, 2), (2, 1), (-1, 2), (-2, 1), (1, -2), (2, -1), (-1, -2), (-2, -1)]:
        f, r = file0 + df, rank0 + dr
        if 0 <= f < 8 and 0 <= r < 8:
            p = squares[r * 8 + f]
            if p and p.upper() == "N" and p.isupper() == by_white:
                return True

    for df in (-1, 0, 1):
        for dr in (-1, 0, 1):
            if df == 0 and dr == 0:
                continue
            f, r = file0 + df, rank0 + dr
            if 0 <= f < 8 and 0 <= r < 8:
                p = squares[r * 8 + f]
                if p and p.upper() == "K" and p.isupper() == by_white:
                    return True

    for deltas, types in ((_BISHOP_DELTAS, ("B", "Q")), (_ROOK_DELTAS, ("R", "Q"))):
        for df, dr in deltas:
            f, r = file0 + df, rank0 + dr
            while 0 <= f < 8 and 0 <= r < 8:
                p = squares[r * 8 + f]
                if p is not None:
                    if p.upper() in types and p.isupper() == by_white:
                        return True
                    break
                f, r = f + df, r + dr
    return False


def _find_king(squares: list, is_white: bool) -> Optional[int]:
    target = "K" if is_white else "k"
    for sq, p in enumerate(squares):
        if p == target:
            return sq
    return None


def move_to_san(board: Board, uci: str) -> str:
    """SAN for `uci` as a move about to be played FROM `board`'s current
    position (call this BEFORE board.push_uci(uci), not after -- it needs
    to see the position the move is being made in)."""
    from_sq = Board._sq(uci[0:2])
    to_sq = Board._sq(uci[2:4])
    promo = uci[4] if len(uci) > 4 else None

    piece = board.squares[from_sq]
    if piece is None:
        raise ValueError(f"no piece on {uci[0:2]} to move (uci={uci!r})")
    is_white = piece.isupper()
    piece_type = piece.upper()
    captured = board.squares[to_sq]

    from_file, from_rank = from_sq % 8, from_sq // 8
    to_file, _to_rank = to_sq % 8, to_sq // 8
    dest_name = uci[2:4]

    if piece_type == "K" and abs(to_file - from_file) == 2:
        san = "O-O" if to_file == 6 else "O-O-O"
    elif piece_type == "P":
        is_capture = captured is not None or (from_file != to_file and captured is None)
        san = f"{uci[0]}x{dest_name}" if is_capture else dest_name
        if promo:
            san += f"={promo.upper()}"
    else:
        is_capture = captured is not None
        others = [
            sq for sq, p in enumerate(board.squares)
            if sq != from_sq and p is not None and p.upper() == piece_type and p.isupper() == is_white
            and _piece_can_reach(board.squares, piece_type, sq, to_sq)
        ]
        disambig = ""
        if others:
            same_file = any((sq % 8) == from_file for sq in others)
            same_rank = any((sq // 8) == from_rank for sq in others)
            if not same_file:
                disambig = uci[0]
            elif not same_rank:
                disambig = uci[1]
            else:
                disambig = uci[0:2]
        san = f"{piece_type}{disambig}{'x' if is_capture else ''}{dest_name}"

    # '+'/'#' -- simulate the move on a scratch copy of the squares to see
    # whether it leaves the opponent's king attacked. '#' only when the
    # device itself reported this as the mate-ending move (see the caller,
    # moves_to_san_list()) -- that's the one piece of ground truth we don't
    # have to re-derive (telling mate from "just a check" would mean
    # generating EVERY legal opponent reply, well past what this file's
    # pseudo-legal approach is set up to do).
    scratch = Board()
    scratch.squares = list(board.squares)
    scratch.white_to_move = board.white_to_move
    scratch.push_uci(uci)
    opponent_king = _find_king(scratch.squares, not is_white)
    if opponent_king is not None and _square_attacked(scratch.squares, opponent_king, is_white):
        san += "+"  # upgraded to "#" by the caller for the actual mating move

    return san


def moves_to_san_list(moves: list, result_code: Optional[str]) -> list:
    """Replays `moves` (MoveRecord objects, in order, from the start
    position) and returns their SAN strings. If result_code is a checkmate
    code, the last move's trailing '+' (from move_to_san()'s own check
    detection) is upgraded to '#'."""
    board = Board()
    out = []
    for m in moves:
        out.append(move_to_san(board, m.uci))
        board.push_uci(m.uci)
    if out and result_code in ("CHECKMATE_WHITE", "CHECKMATE_BLACK") and out[-1].endswith("+"):
        out[-1] = out[-1][:-1] + "#"
    return out


def build_pgn(
    game: "GameState",
    event: str = "Ondsel self-play",
    site: str = "micro:bit v2",
    white: str = "Ondsel",
    black: str = "Ondsel",
    round_: str = "1",
    date: Optional[str] = None,
) -> str:
    """Builds a complete PGN document for `game` (which may still be
    in_progress -- an unfinished game just gets PGN's "*" result and a
    movetext with no trailing result token after it, which any PGN reader
    accepts as "game ongoing/unknown"). Every move gets a {comment} with
    this viewer's own tracked stats, when the device sent them -- a human
    reading the file sees eval/depth/nodes/time right where the move is."""
    import datetime

    if date is None:
        date = datetime.date.today().strftime("%Y.%m.%d")

    result = GameState.PGN_RESULT.get(game.result_code, "*")

    headers = [
        f'[Event "{event}"]',
        f'[Site "{site}"]',
        f'[Date "{date}"]',
        f'[Round "{round_}"]',
        f'[White "{white}"]',
        f'[Black "{black}"]',
        f'[Result "{result}"]',
    ]
    if game.result_code:
        headers.append(f'[Termination "{game.RESULT_TEXT.get(game.result_code, game.result_code)}"]')

    sans = moves_to_san_list(game.moves, game.result_code)

    movetext_parts = []
    for i, (m, san) in enumerate(zip(game.moves, sans)):
        if i % 2 == 0:
            movetext_parts.append(f"{i // 2 + 1}.")
        movetext_parts.append(san)

        comment_bits = [f"eval(White)={m.white_relative_score:+d}cp"]
        if m.depth is not None:
            comment_bits.append(f"depth={m.depth}")
        if m.nodes is not None:
            comment_bits.append(f"nodes={m.nodes}")
        if m.time_ms is not None:
            comment_bits.append(f"time={m.time_ms}ms")
        movetext_parts.append("{" + " ".join(comment_bits) + "}")

    movetext_parts.append(result)
    movetext = " ".join(movetext_parts)

    # Standard PGN line-wrapping isn't required for correctness (readers
    # don't care), so this keeps it simple: headers, a blank line, then the
    # whole movetext as one block -- readable enough for a game this short,
    # and every PGN reader handles arbitrarily long lines fine regardless.
    return "\n".join(headers) + "\n\n" + movetext + "\n"
