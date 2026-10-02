"""Analysis for the overnight Ondsel-vs-Stockfish devloop (see RUNBOOK.md).

Reads the JSON-lines diagnostic trail that play_vs_stockfish.c writes (one
record per ply) and finds the first position where Ondsel's own judgement
and Stockfish's diverge enough to be worth investigating.

PERSPECTIVE CONVENTION -- read this before touching the math below:
Every "cp" figure in the JSONL (sf_eval_before_cp, each sf_multipv_before
entry's cp, and ondsel_eval_cp) is reported from the perspective of
whichever side was actually to move at that record's position -- this is
both UCI's own native convention for "score cp" and Ondsel's own
last_best_score convention (search.h), so no conversion is needed to
compare Ondsel's figure against Stockfish's AT THE SAME ply. Converting
to a single consistent White-relative figure (positive always good for
White) is still needed to compare numbers ACROSS plies (ply N was White to
move, ply N+1 is Black), via `to_white_relative()` below -- the same
approach already used in tools/pc_viewer/protocol.py for the live viewer,
applied here to this separate offline tool.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Optional


def to_white_relative(cp: int, side_to_move: str) -> int:
    """side_to_move is 'w' or 'b' -- the side `cp` is reported FROM."""
    return cp if side_to_move == "w" else -cp


def to_ondsel_relative(white_relative_cp: int, ondsel_color: str) -> int:
    """ondsel_color is 'w' or 'b' -- which side Ondsel played this game.
    Positive always means good for Ondsel, regardless of which color that
    was."""
    return white_relative_cp if ondsel_color == "w" else -white_relative_cp


@dataclass
class PlyRecord:
    ply: int
    side_to_move: str
    mover: str  # "ondsel" or "stockfish"
    move_uci: str
    move_san: str
    sf_eval_before_cp: int
    sf_multipv_before: list  # [{"uci": str, "cp": int}, ...], best first
    ondsel_eval_cp: Optional[int]
    ondsel_depth: Optional[int]
    ondsel_nodes: Optional[int]


def load_jsonl(path: str) -> list:
    records = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            records.append(PlyRecord(**d))
    return records


@dataclass
class Divergence:
    ply: int
    move_san: str
    moves_before_uci: list  # UCI prefix to replay to reach this exact position
    ondsel_eval_white_rel: int
    sf_eval_before_white_rel: int
    eval_agreement_delta_cp: int  # ondsel_eval - sf_eval_before, white-relative (signed)
    sf_best_move_uci: str
    sf_multipv_before: list
    move_quality_loss_for_ondsel_cp: Optional[int]  # None if this was the last ply (no "after" data)
    ondsel_depth: int
    ondsel_nodes: int


def find_divergences(records: list, ondsel_color: str, threshold_cp: int = 100) -> list:
    """Walks every ply where Ondsel was the mover and Ondsel's own eval
    disagrees with Stockfish's eval of that same position (BEFORE Ondsel's
    move) by at least `threshold_cp`, in White-relative terms. Returns them
    in ply order (first disagreement first), each annotated with the
    move-quality-loss figure derived by borrowing the NEXT record's
    sf_eval_before as "the eval of the position Ondsel's move actually led
    to" (see this module's docstring and play_vs_stockfish.c's own comment
    on why that's where the "after" figure comes from)."""
    out = []
    for i, r in enumerate(records):
        if r.mover != "ondsel":
            continue
        ondsel_white = to_white_relative(r.ondsel_eval_cp, r.side_to_move)
        sf_white = to_white_relative(r.sf_eval_before_cp, r.side_to_move)
        delta = ondsel_white - sf_white
        if abs(delta) < threshold_cp:
            continue

        move_quality_loss = None
        if i + 1 < len(records):
            nxt = records[i + 1]
            best_white = to_white_relative(r.sf_multipv_before[0]["cp"], r.side_to_move)
            actual_white = to_white_relative(nxt.sf_eval_before_cp, nxt.side_to_move)
            best_ondsel = to_ondsel_relative(best_white, ondsel_color)
            actual_ondsel = to_ondsel_relative(actual_white, ondsel_color)
            move_quality_loss = best_ondsel - actual_ondsel

        out.append(Divergence(
            ply=r.ply,
            move_san=r.move_san,
            moves_before_uci=[rec.move_uci for rec in records[:i]],
            ondsel_eval_white_rel=ondsel_white,
            sf_eval_before_white_rel=sf_white,
            eval_agreement_delta_cp=delta,
            sf_best_move_uci=r.sf_multipv_before[0]["uci"],
            sf_multipv_before=r.sf_multipv_before,
            move_quality_loss_for_ondsel_cp=move_quality_loss,
            ondsel_depth=r.ondsel_depth,
            ondsel_nodes=r.ondsel_nodes,
        ))
    return out


def build_annotated_pgn(records: list, ondsel_color: str, white_name: str = "Ondsel", black_name: str = "Stockfish11",
                         result: str = "*") -> str:
    """Full PGN with each move commented with eval(White)/depth/nodes where
    available (Ondsel's moves) or just eval(White) (Stockfish's moves, from
    its own search at that ply)."""
    if ondsel_color == "w":
        white_name, black_name = white_name, black_name
    else:
        white_name, black_name = black_name, white_name

    parts = [
        '[Event "Ondsel devloop"]',
        f'[White "{white_name}"]',
        f'[Black "{black_name}"]',
        f'[Result "{result}"]',
        "",
    ]
    movetext = []
    for i, r in enumerate(records):
        if i % 2 == 0:
            movetext.append(f"{i // 2 + 1}.")
        movetext.append(r.move_san)

        white_eval = to_white_relative(r.sf_eval_before_cp, r.side_to_move)
        bits = [f"eval(White)={white_eval:+d}cp(SF)"]
        if r.ondsel_eval_cp is not None:
            ondsel_white_eval = to_white_relative(r.ondsel_eval_cp, r.side_to_move)
            bits.append(f"ondsel_eval(White)={ondsel_white_eval:+d}cp")
            bits.append(f"depth={r.ondsel_depth}")
            bits.append(f"nodes={r.ondsel_nodes}")
        movetext.append("{" + " ".join(bits) + "}")
    movetext.append(result)
    parts.append(" ".join(movetext))
    return "\n".join(parts) + "\n"
