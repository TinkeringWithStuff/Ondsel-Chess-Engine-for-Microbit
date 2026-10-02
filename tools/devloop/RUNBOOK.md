# Ondsel devloop RUNBOOK

**Read this whole file before doing anything.** You are a fresh Claude
session with NO memory of the conversation that set this up. Everything
you need to know is in this file, in git history, and in JOURNAL.md. Do
not assume any other context exists.

## What this is

Ondsel is a chess engine for the BBC micro:bit v2, owned by the user
(Rune). Overnight, while Rune sleeps, this devloop autonomously tries to
make Ondsel's search+eval genuinely stronger, using Stockfish as a trusted
reference. Rune's own words, verbatim, describing the method (this is the
spec -- follow it):

> Ondsel plays stockfish - probably in your test harness. Ondsel loses,
> obviously. You examine the pgn. You find the first drastic discrepancy
> between Ondsel and Stockfish's eval - a delta of, say, 100 centipawns.
> We trust Stockfish and therefore considers this moment the moment where
> Ondsel went wrong. You look at the position where Ondsel went wrong. You
> look at Stockfish's eval and top moves (within some delta...). You think
> deeply about how we could make Ondsel find the correct move. You come up
> with some idea for a heuristic or other improvement to Ondsel. You
> implement it. You put the same position into the harness again. You see
> if your idea worked or not. If it helped Ondsel choose a better move,
> you have this new version of Ondsel play a 20 game match against the
> older version. If your new heuristic/feature/idea hasn't made the new
> Ondsel worse (i.e. it now loses against the older version) you rinse and
> repeat... All the while you are vigilant to work from abstract and not
> "cheat" by somehow hardcoding "in this exact position play X".

Rune chose a ~3-4 hour overnight run, with no interruption notifications --
just leave a clear journal for the morning.

## THE ANTI-HARDCODING RULE (read twice)

Every change must be a **general heuristic or search/eval improvement**
that would help Ondsel in any position sharing the relevant pattern --
never a special case for the one FEN/position you're looking at. If you
ever catch yourself writing `if (this exact position) return this exact
move`, or anything that only works because of exact square/piece
coincidences in the one diverging position, STOP and discard it. A good
test: would this change plausibly help or hurt in OTHER unrelated
positions that share the same pattern (e.g. "a rook behind a passed pawn",
"a king still in the center after move 15")? If you can't articulate why
in one sentence, it's probably not general enough.

## One cycle, precisely

1. **Play a game.** `tools/devloop/play_vs_stockfish` (already built, or
   rebuild per "Building" below) plays one full Ondsel-vs-Stockfish game
   and writes a JSONL diagnostic trail + a PGN. Alternate which color
   Ondsel plays between cycles for variety. Example:
   ```
   ./tools/devloop/play_vs_stockfish --ondsel-color white \
       --node-budget 134638 --sf-depth 10 --sf-multipv 3 --max-plies 160 \
       --out-prefix tools/devloop/work/cycle_N
   ```
   `--node-budget 134638` matches the real device's per-move node budget --
   keep using this number so results stay representative of the actual
   hardware, unless you are deliberately doing the bigger-budget diagnostic
   in step 3.

2. **Find the first divergence.** Use `tools/devloop/analysis.py`'s
   `find_divergences()` on the JSONL (see its docstring -- it's already
   tested, `test_analysis.py` passes). Load the records, pick
   `ondsel_color`, call `find_divergences(records, ondsel_color,
   threshold_cp=100)`, take the FIRST one (lowest ply). That's "the moment
   Ondsel went wrong," per Rune's spec. A quick way to do this from the
   shell:
   ```
   python3 -c "
   from analysis import load_jsonl, find_divergences
   recs = load_jsonl('tools/devloop/work/cycle_N.jsonl')
   divs = find_divergences(recs, ondsel_color='w', threshold_cp=100)
   d = divs[0]
   print('ply', d.ply, 'delta', d.eval_agreement_delta_cp, 'sf_best', d.sf_best_move_uci)
   print(' '.join(d.moves_before_uci))
   "
   ```
   If there's NO divergence >= 100cp in the whole game, that game is clean
   -- log it in JOURNAL.md as a clean game and go back to step 1 with a new
   game (different color or node-budget seed via Stockfish variability --
   note play_vs_stockfish itself has no RNG, so to get a different game you
   may need to vary `--sf-depth` slightly or rely on cumulative engine
   changes from earlier cycles).

3. **Diagnose: search depth or eval blind spot?** Replay the EXACT position
   (`d.moves_before_uci` from step 2) with a much bigger node budget, using
   `tools/devloop/replay_position`:
   ```
   ./tools/devloop/replay_position --moves "<d.moves_before_uci joined by spaces>" \
       --node-budget 5000000 --sf-depth 14 --sf-multipv 3
   ```
   - If `ONDSEL_MATCHES_SF_BEST=yes` at the big budget but not at the real
     134638 budget: this is a **search problem** (pruning too aggressively,
     bad move ordering, not extending somewhere important), not a missing
     eval term. Consider move-ordering/extension ideas, not eval weights.
   - If it's STILL "no" even at a huge budget: this is an **eval blind
     spot** -- Ondsel's static evaluation doesn't recognize something
     Stockfish's does (the classical Stockfish-11 eval is hand-crafted
     too, so look at material/positional factors: king safety, passed
     pawns, piece activity, pawn structure, rook on open file, etc.)

4. **Devise ONE general idea.** Based on the diagnosis, think about what
   Ondsel is actually missing in the ABSTRACT (not "move the bishop to
   g7"). Look at `src/engine/eval.c` and `src/engine/search.c` for the
   existing structure and conventions -- in particular, note the project's
   established pattern: **every new heuristic is gated behind a toggle
   flag that defaults OFF** (e.g. `eval_mobility_enabled`,
   `eval_endgame_heuristics_enabled`, `null_move_enabled`,
   `repetition_avoidance_enabled` in search.h/eval.h) so an A/B comparison
   is always possible and a regression can always be isolated to one flag.
   Follow this convention for whatever you add: implement it behind a new
   flag, default 1 ONLY once you've already proven it helps in this same
   cycle (steps 5-6) -- while still experimenting within one cycle it's
   fine to just directly enable it and see, but if you decide to KEEP it
   permanently (step 6 passes), leave a clear toggle in place the way the
   existing code does, defaulting to the now-proven-good value.

5. **Re-test the same position.** Rebuild (see "Building" below) and rerun
   the EXACT same `replay_position` command from step 3 (at the REAL
   134638 budget this time, not the diagnostic big one). Did Ondsel's move
   change to agree with (or get closer in eval to) Stockfish's? If not,
   the idea didn't work -- `git checkout -- src/engine/` to discard it
   cleanly, log the attempt and why it didn't help in JOURNAL.md, and go
   back to step 4 with a different idea (or back to step 2 if you're stuck
   on this position after 2-3 honest attempts -- not every divergence has
   an easy general fix; it's fine to move on).

6. **Regression-gate against the last known-good commit.** If step 5
   looked promising, run a real match against the current HEAD (the last
   git commit, which is always the last version Rune/this loop trusted):
   ```
   ./tools/devloop/run_regression.sh HEAD --games 20 --node-budget 134638 \
       --opening-plies 4 --seed <pick a fresh seed each time> \
       --out tools/devloop/work/regression_cycle_N.pgn
   ```
   This plays 20 opening positions x 2 colors = 40 games, current (your
   uncommitted change) vs. HEAD (baseline), and prints a line like:
   ```
   MATCH_RESULT current_wins=X baseline_wins=Y draws=Z adjudicated=W total=40
   ```
   **Gate:** keep the change only if `current_wins >= baseline_wins`
   (ties are fine -- the position-level improvement from step 5 is real
   signal even if the 40-game match is noisy at this sample size; don't
   demand statistical significance you don't have the budget to prove, but
   DO reject anything where current is a clear net loser). If it passes:
   `git add -A && git commit` with a clear message describing the idea,
   the position that motivated it, and the match result. If it fails:
   `git checkout -- src/engine/` to cleanly discard, log why in
   JOURNAL.md, and go back to step 4 with a different idea for the same
   position (or step 2 for a fresh one).

7. **Repeat** from step 1 until you run out of time budget (see "Time
   budget" below) or reach a natural stopping point. Every single
   iteration must end as either a clean commit or a clean revert -- NEVER
   leave uncommitted, half-finished changes sitting in the working tree
   between cycles. `git status` should be clean before starting the next
   cycle's step 1.

Rune's convergence criterion ("until Ondsel and Stockfish can play 20 ply
without disagreeing drastically") is almost certainly not reachable in one
overnight run against a real engine -- treat it as the North Star, not a
pass/fail bar for tonight. Making real, validated, non-regressing progress
on even 2-4 positions in one night is a success; say so plainly in the
final summary rather than pretending the full criterion was met.

## Building

Host-only (no ARM cross-compiler is available in this sandbox -- this
entire devloop works against the HOST build of the engine only; the real
device firmware build/flash is NOT touched or verified by any of this, and
must be checked separately by Rune on real hardware before he trusts any
change on the actual micro:bit):

```
gcc -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/engine \
    -o tools/devloop/play_vs_stockfish tools/devloop/play_vs_stockfish.c \
    src/engine/board.c src/engine/movegen.c src/engine/attacks.c \
    src/engine/zobrist.c src/engine/eval.c src/engine/see.c \
    src/engine/search.c src/engine/book.c src/engine/book_data.c -lm

gcc -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/engine \
    -o tools/devloop/replay_position tools/devloop/replay_position.c \
    src/engine/board.c src/engine/movegen.c src/engine/attacks.c \
    src/engine/zobrist.c src/engine/eval.c src/engine/see.c \
    src/engine/search.c src/engine/book.c src/engine/book_data.c -lm
```

Rebuild both of the above after EVERY engine source change before
re-testing (step 5) -- stale binaries will silently test the OLD code.

`tools/devloop/run_regression.sh <baseline-ref> [args...]` (step 6) builds
its own baseline + current + match binary each time it's run; you don't
need to build `match_regression` separately. See `build_baseline.sh`'s and
`match_regression.c`'s own header comments if you need to understand the
`objcopy --redefine-syms` dual-engine-linking mechanism -- you shouldn't
need to touch either file for a normal cycle.

`tools/devloop/stockfish11` is a prebuilt Stockfish 11 binary, already
committed to this repo (see the `.gitignore` note next to it for why) --
you should NOT need to rebuild it. If it's somehow missing: `git clone
--depth 1 --branch sf_11 https://github.com/official-stockfish/Stockfish.git
/tmp/sf11src && cd /tmp/sf11src/src && make -j2 build
ARCH=x86-64-modern`, then copy the resulting `stockfish` binary to
`tools/devloop/stockfish11` and `chmod +x` it. (NNUE net downloads are
blocked in this sandbox's network egress -- Stockfish 11 predates NNUE and
needs no net file, which is why this specific version was chosen. Do not
try to upgrade to a newer Stockfish that requires NNUE.)

## Time budget

Aim for roughly 3-4 hours of total work from when you start this session.
Check the time whenever convenient (e.g. before starting a new cycle) and
stop starting NEW cycles once you're past ~3.5 hours in, finishing whatever
cycle is in progress (always leave the repo in a clean committed-or-
reverted state, never mid-cycle) rather than starting a new one that might
not complete. Then move to "Finishing up" below.

If you hit a wall you can't resolve (a build break you can't fix, a tool
misbehaving, genuinely no more ideas for the current position after
reasonable effort) well before the time budget is up, it's fine to stop
early -- log clearly why in JOURNAL.md and FINAL_SUMMARY.md rather than
spinning wheels.

## Journal discipline

After EVERY cycle (whether it ended in a commit or a revert), append an
entry to `tools/devloop/JOURNAL.md` -- see that file's own template at the
top. Do this in real time, not retrospectively at the end -- if something
goes wrong partway through the night, the journal up to that point should
still be a complete, honest record.

## Finishing up

When you stop (time budget reached, or no more productive ideas), write
`tools/devloop/FINAL_SUMMARY.md`: how many cycles were run, how many
resulted in a kept commit vs. a revert, a one-paragraph plain-English
description of each KEPT change and why it helped, the final git log
(`git log --oneline` since the baseline commit), and an honest assessment
of how far this got toward Rune's convergence criterion. End with a clear
"what Rune should do next" section: anything time-constrained that was
promising but unresolved, and the one-line reminder that NONE of this has
been verified on real device hardware yet (host-only engine, see
"Building" above).

Make sure `git status` is clean (everything committed) before stopping.
