# Ondsel devloop journal

Running log of the overnight autonomous devloop (see RUNBOOK.md for the
methodology this follows). Append one entry per cycle, in real time, in
the format below. Never edit or delete a past entry -- if something you
wrote turns out to be wrong, add a new note rather than rewriting history.

Entry template:

```
## Cycle N -- <timestamp>

**Game:** <color Ondsel played, node budget, sf-depth/multipv, file prefix>
**Divergence found:** ply <N>, delta <X>cp, Ondsel played <move>,
    Stockfish wanted <move> (<cp>cp)
**Diagnosis:** <search problem | eval blind spot | other>, based on
    <what the bigger-node-budget replay showed>
**Idea:** <one or two sentences describing the general heuristic/change,
    and WHY it should help in the abstract, not just this position>
**Position re-test result:** <did it change Ondsel's move / narrow the
    eval gap at the same position? numbers>
**Regression match result:** <MATCH_RESULT line, or "not reached -- idea
    didn't pass step 5">
**Outcome:** KEPT (commit <hash>) | REVERTED (reason)
```

---

## Cycle 0 (manual harness validation, done before handoff) -- 2026-10-02

**Game:** Ondsel=White, node budget 134638, sf-depth 10, sf-multipv 3,
    tools/devloop/work/cycle_1 (not committed -- test output, gitignored)
**Divergence found:** ply 14 (White to move), delta 161cp. Position after
    1.Nc3 d5 2.e4 d4 3.Nce2 e5 4.Nf3 Nc6 5.Ng3 a6 6.Bc4 h5 7.O-O h4 (knight
    on g3 attacked, must move). Ondsel played Ne2 (own eval +15 white-rel);
    Stockfish rated the position -146 (best Ng3-f5, -129; Ondsel's Ne2 was
    actually SF's #2 choice at -130/-178 depending on depth -- so move
    CHOICE was fine, the EVAL of the position was the mismatch).
**Diagnosis:** eval blind spot, not search. Replayed the same position at
    5,000,000 nodes (37x the real budget): Ondsel still only reached eval
    +5 (barely moved from +15), vs. Stockfish's consistent -127 to -146.
    More search depth did not close the gap at all -- confirms this is a
    STATIC evaluation gap, not a pruning/move-ordering problem.
**Idea:** eval.c already has an unused, disabled mobility term
    (eval_mobility_enabled, default 0, weight 2) -- a natural first thing
    to try since it's already implemented and this position has an
    awkwardly placed White knight and blocked bishop (textbook low-
    mobility symptoms). Enabled it for a direct test.
**Position re-test result:** Isolated the STATIC (no-search) effect first:
    evaluate() with mobility on vs off at this exact position only moved
    the white-relative score by -10cp (weight 2) or -40cp (weight 8, a 4x
    bump tested as a sanity check on the term's ceiling). The real gap is
    ~150-180cp. Mobility alone is nowhere near enough to explain or fix
    this particular divergence -- confirmed via the full search too
    (replay_position at the real 134638 budget with mobility on: eval
    moved from +15 to +13, i.e. no meaningful change).
**Regression match result:** not reached -- idea didn't pass step 5
    (position re-test showed no meaningful improvement), so no 20-game
    match was run.
**Outcome:** REVERTED (`git checkout -- src/engine/`). The actual missing
    factor at this position is more likely a COMBINATION of development
    tempo (White's knight made 4 moves by move 8: Nb1-c3-e2-g3-e2) and
    long-term king-safety risk from Black's h-pawn storm -- neither of
    which the current simple material+PST+mobility model represents, and
    neither is a one-line fix. Left as a note for the next cycle rather
    than forcing an undersized idea through just to have something to
    commit.

This cycle was run manually, before the unattended overnight loop started,
specifically to validate that the full pipeline (play game -> find
divergence -> diagnose via bigger-budget replay -> try an idea -> test at
the position -> reject cleanly) works end-to-end. It does. The overnight
session's own cycles start below.

---

(Entries start below once the overnight run begins.)
