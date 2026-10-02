"""Headless tests for analysis.py. Run with: python3 test_analysis.py"""
from analysis import PlyRecord, find_divergences, to_ondsel_relative, to_white_relative


def test_to_white_relative_flips_for_black():
    assert to_white_relative(50, "w") == 50
    assert to_white_relative(50, "b") == -50


def test_to_ondsel_relative_flips_when_ondsel_is_black():
    assert to_ondsel_relative(50, "w") == 50
    assert to_ondsel_relative(50, "b") == -50


def mk(ply, side, mover, ondsel_eval=None, sf_eval=0, sf_best_cp=None, sf_best_uci="e2e4", depth=None, nodes=None):
    return PlyRecord(
        ply=ply, side_to_move=side, mover=mover, move_uci="a2a3", move_san="a3",
        sf_eval_before_cp=sf_eval,
        sf_multipv_before=[{"uci": sf_best_uci, "cp": sf_best_cp if sf_best_cp is not None else sf_eval}],
        ondsel_eval_cp=ondsel_eval, ondsel_depth=depth, ondsel_nodes=nodes,
    )


def test_no_divergence_when_evals_agree():
    records = [
        mk(0, "w", "ondsel", ondsel_eval=50, sf_eval=55, depth=6, nodes=1000),
        mk(1, "b", "stockfish", sf_eval=-50),
    ]
    assert find_divergences(records, ondsel_color="w", threshold_cp=100) == []


def test_finds_divergence_above_threshold_white_perspective():
    # White to move; Ondsel thinks +200, Stockfish thinks -50 -- a 250cp gap.
    records = [
        mk(0, "w", "ondsel", ondsel_eval=200, sf_eval=-50, sf_best_cp=-50, depth=6, nodes=1000),
        mk(1, "b", "stockfish", sf_eval=40),  # position after the move, from Black's perspective
    ]
    divs = find_divergences(records, ondsel_color="w", threshold_cp=100)
    assert len(divs) == 1
    d = divs[0]
    assert d.ply == 0
    assert d.ondsel_eval_white_rel == 200
    assert d.sf_eval_before_white_rel == -50
    assert d.eval_agreement_delta_cp == 250


def test_finds_divergence_when_ondsel_plays_black():
    # Black to move; Ondsel (playing Black) reports its OWN-perspective eval
    # of +300 (thinks it's winning), Stockfish's own-perspective eval here
    # is -10 (thinks Black is roughly equal/slightly worse) -- for white-
    # relative: ondsel_white = -300, sf_white = +10, delta = -310.
    records = [
        mk(0, "b", "ondsel", ondsel_eval=300, sf_eval=-10, sf_best_cp=-10, depth=5, nodes=900),
        mk(1, "w", "stockfish", sf_eval=15),
    ]
    divs = find_divergences(records, ondsel_color="b", threshold_cp=100)
    assert len(divs) == 1
    assert divs[0].eval_agreement_delta_cp == -310


def test_skips_stockfish_moved_plies_entirely():
    records = [
        mk(0, "w", "stockfish", sf_eval=500),  # huge "eval" but mover isn't Ondsel -- not a candidate
        mk(1, "b", "ondsel", ondsel_eval=0, sf_eval=0, depth=6, nodes=1000),
    ]
    assert find_divergences(records, ondsel_color="b", threshold_cp=50) == []


def test_move_quality_loss_uses_next_record_and_is_none_on_last_ply():
    # Ply 0: White (Ondsel) to move. Stockfish's best line here is worth
    # +100 for White. Ondsel plays something else; the resulting position
    # (ply 1's "before", from Black's perspective) is -150 for Black, i.e.
    # +150 for White -- Ondsel's actual outcome is BETTER than Stockfish's
    # own best-move benchmark here (can happen: SF's "best" is itself an
    # estimate at finite depth), so loss should come out negative.
    records = [
        mk(0, "w", "ondsel", ondsel_eval=90, sf_eval=95, sf_best_cp=100, depth=6, nodes=1000),
        mk(1, "b", "stockfish", sf_eval=-150),
    ]
    divs = find_divergences(records, ondsel_color="w", threshold_cp=1000)  # threshold irrelevant here, force-include via next test
    # (threshold_cp=1000 means this record won't be picked up by the
    # eval-agreement gate since |90-95|=5 < 1000 -- test the quality-loss
    # math directly via a threshold of 0 instead.)
    divs = find_divergences(records, ondsel_color="w", threshold_cp=0)
    assert len(divs) == 1
    assert divs[0].move_quality_loss_for_ondsel_cp == 100 - 150  # best_ondsel(100) - actual_ondsel(150) = -50

    # Last ply: no next record to borrow from -- must be None, not a guess.
    records_last_only = [mk(0, "w", "ondsel", ondsel_eval=500, sf_eval=0, sf_best_cp=0, depth=6, nodes=1000)]
    divs2 = find_divergences(records_last_only, ondsel_color="w", threshold_cp=0)
    assert len(divs2) == 1
    assert divs2[0].move_quality_loss_for_ondsel_cp is None


def test_divergences_returned_in_ply_order():
    records = [
        mk(0, "w", "ondsel", ondsel_eval=0, sf_eval=0, depth=6, nodes=1000),
        mk(1, "b", "stockfish", sf_eval=0),
        mk(2, "w", "ondsel", ondsel_eval=500, sf_eval=0, depth=6, nodes=1000),
        mk(3, "b", "stockfish", sf_eval=0),
        mk(4, "w", "ondsel", ondsel_eval=600, sf_eval=0, depth=6, nodes=1000),
    ]
    divs = find_divergences(records, ondsel_color="w", threshold_cp=100)
    assert [d.ply for d in divs] == [2, 4]


def test_moves_before_uci_is_exact_prefix():
    records = [
        mk(0, "w", "ondsel", ondsel_eval=0, sf_eval=0, depth=6, nodes=1000),
        mk(1, "b", "stockfish", sf_eval=0),
        mk(2, "w", "ondsel", ondsel_eval=500, sf_eval=0, depth=6, nodes=1000),
    ]
    divs = find_divergences(records, ondsel_color="w", threshold_cp=100)
    assert len(divs) == 1
    assert divs[0].moves_before_uci == ["a2a3", "a2a3"]  # the two prior plies' moves


if __name__ == "__main__":
    import sys
    tests = [obj for name, obj in list(globals().items()) if name.startswith("test_")]
    failures = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except Exception as exc:
            failures += 1
            print(f"FAIL {t.__name__}: {exc}")
    print(f"\n{len(tests) - failures}/{len(tests)} passed")
    sys.exit(1 if failures else 0)
