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

(Entries start below once the overnight run begins.)
