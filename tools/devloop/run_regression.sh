#!/usr/bin/env bash
# One-shot regression gate: builds the BASELINE git ref (via
# build_baseline.sh), compiles the CURRENT working tree's src/engine,
# links both together with match_regression.c, and runs the match --
# everything this repo's devloop needs to answer "did my change make
# Ondsel worse?" in one command.
#
# Usage: run_regression.sh <baseline-ref> [--games N] [--node-budget N]
#                           [--seed N] [--opening-plies N] [--out path.pgn]
#
# Prints the match to stderr as it plays, and on the last stdout line
# prints: MATCH_RESULT current_wins=X baseline_wins=Y draws=Z adjudicated=W total=T
# Exit code is always 0 (the caller reads the MATCH_RESULT line to decide
# pass/fail -- see RUNBOOK.md's gating rule).
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <baseline-ref> [match_regression.c args...]" >&2
    exit 1
fi

BASELINE_REF="$1"
shift

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

WORK="tools/devloop/work/regression_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "--- Building baseline ($BASELINE_REF) ---" >&2
./tools/devloop/build_baseline.sh "$BASELINE_REF" old "$WORK/baseline"

echo "--- Compiling current engine ---" >&2
CFLAGS="-O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/engine -Itools/devloop"
for src in board movegen attacks zobrist eval see search book book_data; do
    gcc $CFLAGS -c "src/engine/$src.c" -o "$WORK/$src.o"
done
gcc $CFLAGS -c tools/devloop/match_regression.c -o "$WORK/match_regression.o"

echo "--- Linking ---" >&2
gcc -O2 "$WORK"/*.o "$WORK/baseline/old_combined.o" -lm -o "$WORK/match_regression"

echo "--- Running match ---" >&2
"$WORK/match_regression" --baseline-ref "$BASELINE_REF" "$@"
