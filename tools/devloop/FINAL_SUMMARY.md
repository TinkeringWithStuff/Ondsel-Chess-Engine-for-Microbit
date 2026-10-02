# Ondsel devloop -- final summary (2026-10-02, live run)

## What happened to the overnight run

The originally scheduled overnight task fired but finished in ~2 minutes
with zero progress -- almost certainly because a scheduled task's fresh
session gets its own separate container, and this repo was never pushed
to a git remote or other persistent location, so that session had nothing
to work from. Rune caught this the next afternoon and asked me to just
run the loop live instead. Everything below happened in that live,
interactive run (afternoon of 2026-10-02), following the exact same
methodology RUNBOOK.md laid out for the unattended version.

## Cycle count

- **Cycle 0** (done the previous evening, before handoff): manual
  pipeline validation. Found a real divergence, diagnosed it, tried the
  existing mobility term, measured it wasn't enough, reverted. Proved the
  tools work end to end.
- **Cycles 1-4** (live run): four honest attempts, all REVERTED after
  failing the regression gate or an earlier position-level check:
  1. A new SPACE evaluation term (two formulations) -- didn't explain
     either of two real divergences at the position level.
  2. A knight OUTPOST term (any file, weight 25) -- clearly narrowed the
     eval gap at its motivating position, but lost its 40-game regression
     match 14-18.
  3. A revised outpost term (central files only, weight 10) -- still
     correctly signed at the position, still lost its match, 13-19 (an
     even worse margin than the first version).
  4. Null-move pruning (already implemented, never proven) tested
     directly via regression match -- lost 10-15, with far more draws
     than any other match this session, suggesting it pushes the engine
     toward passive lines rather than stronger ones at this node budget.
- **Cycle 5** (live run): **KEPT**. The other existing-but-unproven
  feature, endgame heuristics (passed pawns, king activity toward them,
  rook file bonuses, Tarrasch rule), tested directly via THREE independent
  40-game regression matches (120 games total) after the first came back
  suspiciously close. Combined: 57 wins / 44 losses / 19 draws for
  "enabled" -- won 2 of 3 individual matches outright, lost the third by
  only one game. Flipped `eval_endgame_heuristics_enabled`'s default from
  0 to 1 in `src/engine/eval.c`.
- A sixth game was played and a sixth divergence found (another White-
  space-advantage pattern, the third seen this session -- Caro-Kann-style
  Advance Variation, Nh4 attacking a developed bishop) but no new idea was
  implemented for it; see "What Rune should do next" below.

**Total: 6 cycles run, 1 kept, 4 rejected via the gate, 1 left
unactioned.**

## The one kept change, in plain English

`eval_endgame_heuristics_enabled` is now `1` by default (was `0`). This
switches on four classical evaluation terms that were already fully
written in `eval.c` but never turned on:
- A bonus for passed pawns, bigger the further advanced.
- In the endgame specifically, a bonus for having the king closer to
  escort/stop a passed pawn.
- A bonus for a rook on a semi-open or fully open file.
- A bonus for a rook positioned behind a passed pawn on the same file
  (its own, to push it, or the enemy's, to blockade it) -- the Tarrasch
  rule.

None of this was written during the devloop session itself -- it was
already in the codebase from earlier project work, just switched off
pending exactly the kind of A/B proof this session provided. The devloop
cycle's contribution was testing it properly (three independent 40-game
matches, not just one) and trusting the result once it was consistent.

## Honest assessment vs. the original goal

Rune's stated convergence criterion ("until Ondsel and Stockfish can play
20 ply without disagreeing drastically") was not reached, and realistically
isn't reachable in one session against a real engine -- this was flagged
as the likely outcome in RUNBOOK.md before the run started, and that held
up. What *did* happen: one real, multi-sample-verified strength
improvement was found and kept, and four plausible-looking ideas were
properly tested and correctly rejected before they could make the engine
worse. The rejections are not wasted effort -- a wrong idea that gets
caught by the regression gate is exactly what this methodology is for,
and three of the four rejections (both outpost variants, the space term)
showed a clear, real position-level insight that still failed to
generalize, which is useful information in itself (see the pattern noted
below).

**A pattern worth Rune's attention:** three separate divergences, across
three different openings (cycle 0's Nc3/h4 line, cycle 1's French-ish
e4-Nc6 line, and cycle 6's Caro-Kann-style Nh4-vs-bishop line), all
involved Ondsel underrating a position where White has pushed a central
pawn to the 5th rank, gaining real space. Two different square-counting
"space" formulations were tried and both failed to capture it well
(either diluted by symmetric home-rank pawns, or simply not closing the
gap enough). This looks like a real, recurring blind spot that's going to
need a cleverer formulation than flat square-counting -- possibly
something tied to piece mobility actually being restricted by the pawn
chain (not just raw square control), or a direct "cramped minor piece"
penalty. Worth a dedicated future session rather than another quick
attempt bolted onto this one.

## Final git log (since the pre-devloop baseline)

```
40bb0b8 Enable endgame heuristics by default: passed pawns, king activity, rook files
35ba0f6 Log devloop cycle 4: null-move pruning also fails the regression gate
31dd02f Log devloop cycle 3: central-file/reduced-weight outpost retry also fails gate
5197c47 Log devloop cycle 2: knight outpost term passed position test, failed regression gate
a8c404e Log devloop cycle 1 (live run): space/mobility idea tried and rejected
da56188 Log manual pipeline-validation cycle in devloop journal
7c41307 Add overnight Ondsel-vs-Stockfish devloop infrastructure
90dff85 Baseline snapshot before overnight Ondsel-vs-Stockfish improvement loop
```

Working tree is clean; `tools/devloop/test_analysis.py` still passes
(9/9).

## What Rune should do next

1. **This has not been verified on the real micro:bit yet.** Everything
   in this session was a HOST build of the engine only (no ARM cross-
   compiler is available in this sandbox). The one kept change
   (`eval_endgame_heuristics_enabled = 1`) needs to go through the real
   device build-and-flash process and get checked on actual hardware
   before it's trusted for real play -- it's a pure evaluation-function
   change (no new memory allocation, no new data structures), so it
   should build and fit the same way the existing code already does, but
   that's an assumption, not something this session confirmed.
2. **The recurring space/cramped-position blind spot** (see above) is
   the most promising lead for a future session -- it showed up
   independently three times, which is a much stronger signal than any
   single divergence, but needs a smarter implementation than what was
   tried here.
3. **Null-move pruning's draw-heavy result** is worth a second look with
   its own dedicated investigation (tuning the reduction depth/margin, or
   testing at a different node budget) rather than writing it off
   entirely from one test -- the devloop treated it as "tried once at
   current settings, rejected," not "proven impossible."
4. If you want another devloop run, the infrastructure is all still
   here and working (`tools/devloop/RUNBOOK.md`, `JOURNAL.md`,
   `play_vs_stockfish`/`replay_position`/`run_regression.sh`) -- just make
   sure it's driven from a live or persistently-reachable session next
   time, not a fresh scheduled-task container with no copy of the repo.
