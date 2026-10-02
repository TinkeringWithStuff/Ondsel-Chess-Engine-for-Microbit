"""Headless tests for protocol.py -- no serial port, no display, no external
packages needed. Run with: python3 test_protocol.py
"""
from protocol import Board, GameState, LineDecoder, build_pgn, line_checksum, move_to_san, moves_to_san_list


def make_line(payload: str) -> str:
    """Builds exactly what the device sends: "<payload>*XX\\r\\n" -- used
    here to fabricate realistic test input without a real micro:bit."""
    return f"{payload}*{line_checksum(payload):02X}\r\n"


def corrupt(line: str, index: int, replacement: str) -> str:
    return line[:index] + replacement + line[index + 1:]


def test_checksum_matches_device_algorithm():
    payload = "GAME_START"
    expected = sum(payload.encode("ascii")) & 0xFF
    assert line_checksum(payload) == expected


def test_decoder_accepts_valid_line():
    d = LineDecoder()
    result = d.feed_raw(make_line("MOVE e2e4 23"))
    assert result == "MOVE e2e4 23"
    assert d.accepted_count == 1
    assert d.corrupt_count == 0


def test_decoder_rejects_corrupted_checksum():
    d = LineDecoder()
    line = make_line("MOVE e2e4 23")
    bad = corrupt(line, 5, "x")  # mangle a payload byte, checksum now stale
    assert d.feed_raw(bad) is None
    assert d.corrupt_count == 1


def test_decoder_deduplicates_repeats():
    d = LineDecoder()
    line = make_line("MOVE e2e4 23")
    assert d.feed_raw(line) == "MOVE e2e4 23"
    assert d.feed_raw(line) is None  # repeat #2, same content -- suppressed
    assert d.feed_raw(line) is None  # repeat #3
    assert d.accepted_count == 1
    assert d.duplicate_count == 2


def test_decoder_recovers_when_first_copy_is_corrupt():
    # Exactly the "repeat and vote" scenario uart.h's own comment describes:
    # copy 1 is damaged, copy 2 (identical, intact) gets through.
    d = LineDecoder()
    line = make_line("MOVE d2d4 10")
    bad_copy = corrupt(line, 2, "Z")
    assert d.feed_raw(bad_copy) is None
    assert d.feed_raw(line) == "MOVE d2d4 10"


def test_full_short_game_via_gamestate():
    gs = GameState()
    d = LineDecoder()

    scripted_events = [
        "GAME_START",
        "MOVE e2e4 12",
        "MOVE e7e5 -8",
        "MOVE g1f3 15",
        "MOVE b8c6 -10",
        "MOVE f1c4 20",
        "MOVE f8c5 -18",
        "GAME_END STOPPED",
    ]
    for payload in scripted_events:
        line = make_line(payload)
        # simulate PROTO_REPEATS=3 copies of every line, as the firmware sends
        for _ in range(3):
            decoded = d.feed_raw(line)
            if decoded is not None:
                gs.apply_payload(decoded)

    assert [m.uci for m in gs.moves] == ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5"]
    assert gs.result_text == "Stopped early"
    assert gs.last_error is None
    assert d.accepted_count == len(scripted_events)
    assert d.duplicate_count == 2 * len(scripted_events)

    # Spot-check final board state: after 1.e4 e5 2.Nf3 Nc6 3.Bc4 Bc5,
    # e4/e5/f3/c6/c4/c5 should hold the moved pieces and their origin
    # squares should be empty.
    b = gs.board
    assert b.piece_at(4, 3) == "P"   # e4
    assert b.piece_at(4, 4) == "p"   # e5
    assert b.piece_at(5, 2) == "N"   # f3
    assert b.piece_at(2, 5) == "n"   # c6
    assert b.piece_at(2, 3) == "B"   # c4
    assert b.piece_at(2, 4) == "b"   # c5
    assert b.piece_at(4, 1) is None  # e2 now empty
    assert b.piece_at(6, 0) is None  # g1 now empty


def test_gamestate_reports_error_without_crashing_on_bad_move():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE h1h1 0")  # no-op "move", from==to: not a real piece move but shouldn't crash
    # h1 starts empty of interest but let's use a square with no piece instead:
    gs2 = GameState()
    gs2.apply_payload("GAME_START")
    gs2.apply_payload("MOVE e4e5 0")  # nothing sits on e4 in the start position
    assert gs2.last_error is not None
    assert len(gs2.moves) == 0  # board untouched by the rejected move


def test_gamestate_handles_castling_uci():
    gs = GameState()
    gs.apply_payload("GAME_START")
    for uci, score in [
        ("e2e4", 0), ("e7e5", 0),
        ("g1f3", 0), ("b8c6", 0),
        ("f1c4", 0), ("f8c5", 0),
        ("e1g1", 0),  # White castles kingside -- UCI king-move notation
    ]:
        gs.apply_payload(f"MOVE {uci} {score}")
    assert gs.last_error is None
    b = gs.board
    assert b.piece_at(6, 0) == "K"   # g1
    assert b.piece_at(5, 0) == "R"   # f1 (rook came from h1)
    assert b.piece_at(4, 0) is None  # e1 empty
    assert b.piece_at(7, 0) is None  # h1 empty


def test_gamestate_handles_en_passant():
    gs = GameState()
    gs.apply_payload("GAME_START")
    for uci in ["e2e4", "a7a6", "e4e5", "d7d5", "e5d6"]:  # e5xd6 en passant
        gs.apply_payload(f"MOVE {uci} 0")
    assert gs.last_error is None
    b = gs.board
    assert b.piece_at(3, 5) == "P"   # d6, the capturing pawn
    assert b.piece_at(3, 4) is None  # d5, the captured pawn is gone
    assert b.piece_at(4, 4) is None  # e5, mover's origin now empty


def test_gamestate_parses_optional_stats_fields():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE e2e4 15 6 48213 310")
    assert gs.last_error is None
    m = gs.moves[-1]
    assert m.uci == "e2e4"
    assert m.score == 15
    assert m.depth == 6
    assert m.nodes == 48213
    assert m.time_ms == 310


def test_gamestate_accepts_move_without_stats_fields():
    # SimulatedLineSource's older wire format (and any future minimal
    # sender) -- uci+score only, no depth/nodes/time_ms -- must still work.
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE e2e4 15")
    assert gs.last_error is None
    m = gs.moves[-1]
    assert m.uci == "e2e4"
    assert m.score == 15
    assert m.depth is None
    assert m.nodes is None
    assert m.time_ms is None


def test_gamestate_rejects_move_with_wrong_field_count():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE e2e4 15 6 48213")  # 4 fields after "MOVE" -- neither 3 nor 6
    assert gs.last_error is not None
    assert len(gs.moves) == 0


def test_gamestate_handles_promotion():
    gs = GameState()
    gs.apply_payload("GAME_START")
    for uci in ["a2a4", "b7b5", "a4b5", "a7a6", "b5b6", "a6a5", "b6b7", "a5a4", "b7b8q"]:
        gs.apply_payload(f"MOVE {uci} 0")
    assert gs.last_error is None, gs.last_error
    b = gs.board
    assert b.piece_at(1, 7) == "Q"  # b8 now a White queen


def test_white_relative_score_flips_sign_on_black_moves():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE e2e4 50")   # White's move: own-perspective == White-relative
    gs.apply_payload("MOVE e7e5 50")   # Black's move: +50 for Black == -50 for White
    assert gs.moves[0].score == 50 and gs.moves[0].white_relative_score == 50
    assert gs.moves[1].score == 50 and gs.moves[1].white_relative_score == -50


def test_adjudicated_result_codes_get_readable_text_and_pgn_result():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE e2e4 400")
    gs.apply_payload("GAME_END ADJUDICATED_WHITE")
    assert "White" in gs.result_text
    assert GameState.PGN_RESULT["ADJUDICATED_WHITE"] == "1-0"
    assert GameState.PGN_RESULT["ADJUDICATED_BLACK"] == "0-1"
    assert GameState.PGN_RESULT["ADJUDICATED_DRAW"] == "1/2-1/2"


def test_san_matches_standard_opening_sequence():
    gs = GameState()
    gs.apply_payload("GAME_START")
    for uci in ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5"]:
        gs.apply_payload(f"MOVE {uci} 0")
    assert moves_to_san_list(gs.moves, None) == ["e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5"]


def test_san_handles_castling_and_capture_and_promotion():
    gs = GameState()
    gs.apply_payload("GAME_START")
    for uci in ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "e1g1"]:
        gs.apply_payload(f"MOVE {uci} 0")
    assert moves_to_san_list(gs.moves, None)[-1] == "O-O"

    gs2 = GameState()
    gs2.apply_payload("GAME_START")
    for uci in ["e2e4", "d7d5", "e4d5"]:  # exd5, a capture
        gs2.apply_payload(f"MOVE {uci} 0")
    assert moves_to_san_list(gs2.moves, None)[-1] == "exd5"

    gs3 = GameState()
    gs3.apply_payload("GAME_START")
    # b8's own knight has to move out of the way first -- a straight pawn
    # advance onto an occupied square (same file) is a blocked move, not a
    # capture, so promoting here with the knight left on b8 isn't a move
    # the device would ever actually send.
    for uci in ["a2a4", "b7b5", "a4b5", "b8c6", "b5b6", "a7a6", "b6b7", "a6a5", "b7b8q"]:
        gs3.apply_payload(f"MOVE {uci} 0")
    assert moves_to_san_list(gs3.moves, None)[-1] == "b8=Q"


def test_san_disambiguates_two_identical_pieces():
    # Direct unit test of the disambiguation logic, constructed straight
    # from a custom position rather than replayed from the start position
    # (simpler to set up deterministically): two White knights, on b1 and
    # d1, both able to reach c3 -- SAN needs the origin file to tell them
    # apart ("Nbc3", not just "Nc3").
    b = Board()
    b.squares = [None] * 64
    b.squares[Board._sq("b1")] = "N"
    b.squares[Board._sq("d1")] = "N"
    b.squares[Board._sq("e1")] = "K"
    b.squares[Board._sq("e8")] = "k"
    b.white_to_move = True
    assert move_to_san(b, "b1c3") == "Nbc3"


def test_build_pgn_includes_headers_comments_and_checkmate_result():
    gs = GameState()
    gs.apply_payload("GAME_START")
    # Fool's mate: fastest possible checkmate, convenient for a '#'-ending test.
    for uci, score in [("f2f3", -50), ("e7e5", 10), ("g2g4", -900), ("d8h4", 9000)]:
        gs.apply_payload(f"MOVE {uci} {score} 4 100 10")
    gs.apply_payload("GAME_END CHECKMATE_BLACK")

    pgn = build_pgn(gs, event="Test Event", white="Ondsel-A", black="Ondsel-B")
    assert '[Event "Test Event"]' in pgn
    assert '[White "Ondsel-A"]' in pgn
    assert '[Black "Ondsel-B"]' in pgn
    assert '[Result "0-1"]' in pgn
    assert "Qh4#" in pgn
    assert "eval(White)=-9000cp" in pgn  # Black's own +9000 flipped to White-relative
    assert "depth=4" in pgn and "nodes=100" in pgn and "time=10ms" in pgn


def test_build_pgn_unfinished_game_gets_star_result():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("MOVE e2e4 10")
    pgn = build_pgn(gs)
    assert '[Result "*"]' in pgn
    assert pgn.strip().endswith("*")


def test_checkmate_code_maps_to_readable_text():
    gs = GameState()
    gs.apply_payload("GAME_START")
    gs.apply_payload("GAME_END CHECKMATE_BLACK")
    assert gs.result_text == "Checkmate -- Black wins"


def test_board_starting_position_spot_check():
    b = Board()
    assert b.piece_at(0, 0) == "R"
    assert b.piece_at(4, 0) == "K"
    assert b.piece_at(3, 7) == "q"
    assert b.piece_at(4, 1) == "P"
    assert b.piece_at(4, 4) is None


if __name__ == "__main__":
    import sys

    tests = [obj for name, obj in list(globals().items()) if name.startswith("test_")]
    failures = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except Exception as exc:  # noqa: BLE001 - test runner, want to keep going
            failures += 1
            print(f"FAIL {t.__name__}: {exc}")
    print(f"\n{len(tests) - failures}/{len(tests)} passed")
    sys.exit(1 if failures else 0)
