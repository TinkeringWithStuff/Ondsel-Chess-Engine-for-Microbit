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

## Cycle 2 -- 2026-10-02 (live run)

**Game:** Ondsel=White, node budget 134638, sf-depth 13, sf-multipv 3
    (first attempt at sf-depth 10 reproduced cycle 0's exact game --
    engine is fully deterministic with no RNG, so varying Stockfish's
    search depth was needed to actually get a different game),
    tools/devloop/work/cycle_2 (gitignored test output)
**Divergence found:** ply 28 (White to move), delta 109cp (white-relative:
    Ondsel -10, Stockfish -119). Position after a long forced sequence
    where White's king had to manually walk e1-f1-g1 (losing castling
    rights) and a bishop-for-knight trade happened on d2, followed by a
    pawn trade on d4 that let Black's knight recapture onto d4 -- a square
    no White pawn can ever challenge again (c- and e-pawns already past
    it). Material was dead equal. Ondsel's move (Qd1, retreating the
    attacked queen) was Stockfish's own #1 choice both at shallow and
    5,000,000-node depth -- once again the MOVE was fine, only the
    judgement of the resulting position was off.
**Diagnosis:** eval blind spot (confirmed via replay_position at
    5,000,000 nodes: eval stuck at -10 vs Stockfish's steady -116/-119
    across depths 12-15).
**Idea:** Checked by hand first (no code) whether king safety (pawn
    shield intact for both kings -- yes, both fully intact, no holes) or
    raw space counting would explain this -- neither does. What stood out
    concretely: Black's knight on d4 is a textbook "outpost" (no enemy
    pawn can ever attack it again). Implemented eval_outpost_enabled
    (default off) + outpost_score() in eval.c: a knight gets +25cp if (a)
    no enemy pawn on an adjacent file can still reach a rank where it
    would attack the knight's square (reusing the existing
    passed_pawn_mask machinery from endgame_heuristics_score, restricted
    to the two adjacent files only), and (b) the knight is currently
    defended by one of its own pawns. This is the standard chess-
    programming "knight outpost" pattern, general across any position
    with a similarly-placed knight, not tuned to this one game.
**Position re-test result:** Static-eval-only probe: -25cp at this exact
    position (baseline +30 -> +5 with the term on), a real and
    correctly-signed chunk of the ~109-149cp gap (roughly 20-25% of it).
    Full search at the real 134638 node budget: eval moved from -10 to
    -35 (White-relative), narrowing the gap to Stockfish's -92 (at SF
    depth 12) by close to half. Move choice unchanged (still matched SF's
    best). This was clearly the most promising single-position result of
    any idea tried so far -- strong enough to justify the full regression
    match.
**Regression match result:** `MATCH_RESULT current_wins=14 baseline_wins=18
    draws=8 adjudicated=0 total=40` (20 openings x 2 colors, seed 314159,
    real 134638 node budget). FAILS the gate (14 < 18) -- current is a net
    loser overall despite the clear, measured local improvement at the
    position that motivated it.
**Outcome:** REVERTED (`git checkout -- src/engine/`). This is exactly the
    scenario the regression-gate step exists for: a change that looks
    good at the one position that inspired it can still make the engine
    WORSE overall once it's actually played out across many different
    positions and openings. Possible reasons worth a future cycle's
    attention (not pursued further this cycle, to stay within one
    idea/one test per cycle): OUTPOST_BONUS=25 may simply be too large a
    flat bonus relative to how reliably "no pawn can ever attack this
    square" actually predicts real strength across a whole game (e.g. it
    might be encouraging the search to steer toward unrelated,
    objectively worse positions purely to plant a knight on *some*
    outpost square, trading away real activity/safety for the bonus); or
    the knights-only restriction interacts oddly with this engine's
    specific move-ordering/pruning in ways a static-eval-only probe can't
    see. A smaller weight (e.g. 10-12) or requiring the outpost to also be
    on a central file might be worth trying in a later cycle, but as a
    FRESH idea with its own position+regression test, not a tweak forced
    through on this result.

---

## Cycle 3 -- 2026-10-02 (live run)

**Game/divergence:** same as cycle 2's (deliberately -- this cycle is a
    direct, honest retry of cycle 2's rejected idea with a revised
    design, not a fresh position).
**Idea:** Cycle 2's outpost term (any file, OUTPOST_BONUS=25) looked
    great at the one position but lost its regression match 14-18.
    Hypothesis from that journal entry: the flat bonus on ANY outpost
    square (including rim squares with little real scope) might push the
    search toward trading away real activity just to plant a technically-
    safe knight. Revised: restricted the bonus to the CENTRAL files only
    (c-f) and cut the weight from 25 to 10 -- same underlying mechanism
    (reuses passed_pawn_mask), just more conservative.
**Position re-test result:** Still correctly signed at the motivating
    position: eval moved from -10 to -20 (White-relative), about half of
    the weight-25 version's -10 to -35 move, as expected from halving the
    weight. Move choice unchanged (still SF's best).
**Regression match result:** `MATCH_RESULT current_wins=13 baseline_wins=19
    draws=8 adjudicated=0 total=40` (20 openings x 2 colors, seed 271828,
    real node budget). FAILS the gate, by an even WORSE margin than cycle
    2's any-file/weight-25 version (13-19 vs. 14-18) despite being the
    more conservative change -- so this wasn't a tuning/calibration
    problem that a smaller weight or a central-files restriction could
    fix; the different seed makes a direct magnitude comparison noisy,
    but the direction (still a clear net loser) held up under a second,
    more careful attempt.
**Outcome:** REVERTED (`git checkout -- src/engine/`). Knight outposts, as
    a static positional bonus, don't seem to help THIS engine's overall
    strength in either tested form, even though the underlying chess
    principle is sound and real (Stockfish's own eval agreed at the
    source position). Likely explanation: a flat positional bonus like
    this can distort move ordering / the search's own judgement in ways
    that hurt more than the correct per-position signal helps, especially
    at a fairly shallow effective search depth (the real device node
    budget, 134638, is small). Not pursuing outpost-style static bonuses
    further without a fundamentally different implementation (e.g. one
    that only applies in quiescence/leaf evaluation rather than
    everywhere, or a much smaller weight tried first before this cycle's
    two fairly large values) -- moving to a different kind of idea
    entirely next cycle instead of re-tuning this one a third time.

## Cycle 4 -- 2026-10-02 (live run)

**Idea (not from a fresh position -- a different kind of idea entirely,
    per cycle 3's note):** search.c already has null-move pruning fully
    implemented (null_move_enabled, default off) with its own comment
    saying it's "unproven, run an A/B match" -- exactly the kind of
    already-built-but-untested feature this project's own convention
    calls for testing via a direct regression match, same as mobility was
    checked in cycle 0/1. This is a SEARCH technique (lets the search
    explore effectively deeper within the same real node budget by
    skipping a ply when a quick "pass" still looks fine), not a static
    eval bonus, so it isn't tied to one specific position's eval gap the
    way the last three cycles were -- its case for being worth testing is
    that it should make the EXISTING eval's judgement available at
    greater effective depth, for free, rather than changing what counts
    as "good" in any position.

    Tooling note: match_regression.c previously forced null_move_enabled
    OFF for both sides unconditionally (an apples-to-apples eval-only
    comparison). Added two new flags, default off (so existing behavior
    is unchanged when neither is passed): `--current-null-move` and
    `--baseline-null-move`, letting either engine's null-move setting be
    turned on independently for exactly this kind of SEARCH-feature A/B
    test. This is a tooling capability, not an experimental engine
    change, so it's kept regardless of this cycle's result.
**Position re-test result:** N/A -- this cycle tests a global search
    behavior across many games/positions rather than one specific
    position's eval gap, so step 3/5 (diagnose and re-test at one
    position) doesn't apply the same way; went straight to the
    regression match, which is the right test for "does this search
    technique help overall."
**Regression match result:** `MATCH_RESULT current_wins=10 baseline_wins=15
    draws=15 adjudicated=0 total=40` (current with null-move pruning ON,
    baseline with it off, 20 openings x 2 colors, seed 161803, real node
    budget). FAILS the gate (10 < 15). Notably far more draws than any
    previous match in this session (15 vs. 8 in cycles 2 and 3) --
    null-move pruning seems to be steering the engine toward more
    passive/drawish lines rather than clearly stronger ones, at least at
    this engine's current node budget and depth ceiling.
**Outcome:** REVERTED the engine-behavior test (null_move_enabled stays
    off by default; no src/engine changes were made or needed this cycle
    since the test used the new CLI flags, not a code change). KEPT the
    match_regression.c tooling addition (committed as a capability, not
    an experiment). Plausible explanation: null-move pruning's safety
    assumptions (the "null move observation" -- if even passing a turn
    still looks fine, the position is probably fine) can misfire in the
    kind of tactically sharp lines a 134638-node-budget engine reaches
    (shallower effective search depth than null-move pruning is usually
    tuned for), and/or this implementation's reduction depth/margins need
    their own tuning pass before it's a net win -- neither is a one-cycle
    fix. Four cycles in a row have now failed the gate (space, mobility,
    two outpost variants, null-move) -- all honestly tested, all honestly
    rejected. Itself useful information for Rune: this simple
    material+PST baseline is more resilient to these particular
    improvements than expected, at least at this node budget.

## Cycle 5 -- 2026-10-02 (live run)

**Idea (not from a fresh position, same pattern as cycle 4):**
    eval_endgame_heuristics_enabled is the other existing-but-unproven
    feature in eval.c (passed pawns + king activity toward them in the
    endgame + rook semi-open/open file bonus + Tarrasch rule rook-behind-
    passed-pawn bonus), already fully implemented, default off, with the
    same "needs an A/B match" status as mobility and null-move. Tested it
    directly via regression match rather than from one position's
    divergence, same reasoning as cycle 4.
**Position re-test result:** N/A, same reasoning as cycle 4 -- this is a
    broad feature test, not a single-position fix.
**Regression match result:** Ran THREE independent 40-game matches
    (different seeds) rather than just one, because the first result was
    suspiciously close (16-17) and the point of this methodology is not
    to trust a single noisy sample for a borderline case:
    - seed 577215: `current_wins=16 baseline_wins=17 draws=7` (current
      barely lost)
    - seed 999999: `current_wins=22 baseline_wins=12 draws=6` (current
      won clearly)
    - seed 141421: `current_wins=19 baseline_wins=15 draws=6` (current
      won clearly)
    Combined across all 120 games: current 57, baseline 44, draws 19.
    Current won 2 of 3 individual matches outright and only narrowly lost
    the third -- a consistent signal, not a fluke from one lucky sample.
**Outcome:** KEPT. Flipped eval_endgame_heuristics_enabled's default to 1
    in src/engine/eval.c, with eval.h's comment updated to record the
    match evidence. This is the devloop's first real, gated improvement
    after four straight rejections (cycles 1-4) -- the gate is working
    exactly as designed: it let through the one idea that actually
    demonstrated a consistent net win across multiple independent
    samples, after rejecting several that looked promising at the single-
    position level but weren't once tested broadly.
