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

## Note: the scheduled overnight run never actually started

The scheduled task fired at 2026-10-02 05:14 UTC as planned but finished
two minutes later with zero commits and no journal entries -- almost
certainly because a fresh scheduled-task session gets its own separate
container, and this repo was never pushed to a git remote or other
persistent location, so the fresh session had nothing to work from. Rune
caught this in the morning and asked me to just run the loop live instead,
driving it myself in an interactive session rather than via a detached
scheduled task. Cycles below are from that live run (afternoon of
2026-10-02, not actually overnight).

## Cycle 1 -- 2026-10-02 (live run)

**Game:** Ondsel=Black, node budget 134638, sf-depth 10, sf-multipv 3,
    tools/devloop/work/cycle_1 (gitignored test output)
**Divergence found:** ply 9 (Black to move), delta -119cp (white-relative:
    Ondsel +25, Stockfish +144). Position after 1.e4 Nc6 2.d4 d5 3.e5 e6
    4.Nf3 Bb4 5.c3 (bishop attacked, must move). Ondsel played Ba5 --
    which, like cycle 0, turned out to be a perfectly reasonable move
    choice (SF's own #3 at depth 12, and matched SF's #1 choice exactly
    once SF searched deeper at depth 14/5M nodes). The mismatch was again
    purely in how good Ondsel thought the resulting POSITION was, not
    which move to play.
**Diagnosis:** eval blind spot, not search. replay_position at 5,000,000
    nodes: Ondsel's move choice became SF's exact top choice, but its own
    eval only moved from +25 to -30 (White-relative), nowhere near SF's
    consistent +144/+145/+155-ish range across depths. Confirms (same as
    cycle 0) that extra search depth does not close this gap -- it's
    static evaluation, not pruning/ordering.
**Idea:** Both divergences so far share a pattern: White has a central
    space advantage (advanced pawns cramping Black) that Ondsel's
    material+PST+mobility model under-credits. Checked the existing
    mobility term on this new position directly (static-eval-only probe,
    no search): only +4cp -- confirms cycle 0's finding that mobility's
    effect is too small here too, on a second, unrelated position. Wrote
    a NEW general term instead: SPACE (eval_space_enabled, default off,
    SPACE_WEIGHT 3) -- counts squares on the central files (c-f) within
    each side's own camp that are safely controlled (occupied/attacked by
    that side's own pawn and not attacked by the enemy's), rewarding real
    territorial gain from advanced central pawns. Modeled on the same idea
    classical engines (including Stockfish's own pre-NNUE eval) use.
**Position re-test result:** First version (camp = own half + 1 rank)
    measured via the static-eval-only probe: only -3cp on EITHER position,
    and in the WRONG direction on cycle 1 (made Ondsel's eval MORE
    negative, i.e. further from SF's +144, not closer) -- root cause
    found by hand-tracing the bitboards: counting a side's untouched
    HOME-RANK central pawns as "controlled" made the term nearly
    symmetric between both sides (both still have several untouched
    central pawns at home), diluting the real signal from genuine pawn
    advancement. Refined to exclude each side's own back two ranks
    (ranks 1-2 for White) so only genuine advancement into the center
    counts. Re-measured: cycle 0 improved slightly (-6cp, right direction,
    but the real gap there is ~150-180cp -- nowhere near enough). Cycle 1:
    exactly +0cp -- by hand-tracing again, Black's own d5/e6 pawns turned
    out to generate almost exactly as many "safely controlled central
    squares" as White's c3/d4/e5 structure once home-rank pawns were
    excluded from both sides equally, so the term washes out completely
    on this position. Conclusion: this specific divergence (and likely
    cycle 0's too) isn't really about raw pawn-occupied "space" in the
    square-counting sense -- it's more about PIECE ACTIVITY/TEMPO (a
    retreating bishop, a knight that's moved 4 times) and possibly
    long-term king safety, neither of which a pawn-square-counting metric
    captures. Rejected -- did not pass step 5 on either position it was
    designed for.
**Regression match result:** not reached -- idea didn't pass step 5.
**Outcome:** REVERTED (`git checkout -- src/engine/`). Two independent,
    reasonably-tried space/mobility formulations have now failed to
    explain either of the two real divergences found so far. Both
    divergences involve a piece (knight in cycle 0, bishop in cycle 1)
    that had to retreat/re-route after being attacked, costing tempo, and
    a king whose pawn shelter is being probed (h-pawn storm in cycle 0).
    Next cycle should look at something that measures TEMPO or
    DEVELOPMENT directly (e.g. a penalty for minor pieces still on their
    original home square once the other side has castled/developed more),
    rather than another pawn-structure-only term.

---

(Entries continue below.)
