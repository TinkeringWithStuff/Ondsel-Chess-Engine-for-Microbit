#!/usr/bin/env bash
# Builds src/engine/*.c as it existed at a given git ref, then renames every
# GLOBAL (external-linkage) symbol in the resulting object files with a
# prefix via `objcopy --redefine-syms`. The renamed .o files can then be
# linked alongside the CURRENT engine build in one binary, so a single
# process can run both engine versions head-to-head without any
# subprocess/UCI overhead -- see match_regression.c.
#
# This is the same technique used by the project's earlier
# match_orig_vs_rewrite.c / orig_extern.h (see RUNBOOK.md), generalized so
# it works for an arbitrary git ref instead of a fixed "orig" tree, and
# made fully automatic (the symbol list is discovered via `nm`, not
# hand-maintained).
#
# Usage: build_baseline.sh <git-ref> <prefix> <output-dir>
# Produces <output-dir>/<prefix>_combined.o -- one object file containing
# every renamed engine symbol, ready to link into a match driver.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <git-ref> <prefix> <output-dir>" >&2
    exit 1
fi

REF="$1"
PREFIX="$2"
OUTDIR="$3"

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# Confirm the ref actually resolves before doing any work.
git rev-parse --verify "$REF^{commit}" >/dev/null

SRC_CHECKOUT="$OUTDIR/src_checkout"
OBJ_DIR="$OUTDIR/obj"
rm -rf "$SRC_CHECKOUT" "$OBJ_DIR"
mkdir -p "$SRC_CHECKOUT" "$OBJ_DIR"

# Export src/engine as it existed at $REF (not the working tree) into an
# isolated directory, so this never touches or depends on uncommitted
# changes in the current working tree.
git archive "$REF" -- src/engine | tar -x -C "$SRC_CHECKOUT"

ENGINE_SRCS=(board.c movegen.c attacks.c zobrist.c eval.c see.c search.c book.c book_data.c)

OBJS=()
for src in "${ENGINE_SRCS[@]}"; do
    srcpath="$SRC_CHECKOUT/src/engine/$src"
    if [ ! -f "$srcpath" ]; then
        echo "build_baseline.sh: $src not found at ref $REF (engine file added/renamed since?) -- aborting" >&2
        exit 1
    fi
    objpath="$OBJ_DIR/${src%.c}.o"
    gcc -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L \
        -I"$SRC_CHECKOUT/src" -I"$SRC_CHECKOUT/src/engine" \
        -c "$srcpath" -o "$objpath"
    OBJS+=("$objpath")
done

# Discover every global (external-linkage) defined symbol across all the
# object files and rename each one with the prefix. Using nm's "T/D/B/R"
# (and lowercase for weak) defined-external categories only -- this
# naturally excludes local/static symbols (nm -g already restricts to
# external symbols) and undefined references (we only rename symbols this
# object file itself DEFINES, never ones it merely calls/references, since
# objcopy --redefine-syms only needs to touch the definitions -- an object
# file's undefined references to another renamed object file's symbols are
# resolved at link time against that file's new name automatically once
# BOTH files go through the same rename, because redefine-syms renames
# every occurrence of the name, both definitions and references, inside
# the one object file it's applied to; so this script applies the SAME
# rename list to every object file).
SYMFILE="$OUTDIR/symbols.txt"
> "$SYMFILE"
for obj in "${OBJS[@]}"; do
    nm --defined-only -g "$obj" | awk '{print $3}'
done | sort -u > "$OUTDIR/all_syms_raw.txt"

# Never rename C library / compiler-internal symbols that might show up as
# defined-external in some toolchains (there shouldn't be any from this
# project's own .c files, but skip anything not looking like our own
# identifiers just in case).
grep -E '^[A-Za-z_][A-Za-z0-9_]*$' "$OUTDIR/all_syms_raw.txt" > "$OUTDIR/all_syms.txt" || true

while read -r sym; do
    [ -z "$sym" ] && continue
    echo "$sym ${PREFIX}_${sym}"
done < "$OUTDIR/all_syms.txt" > "$SYMFILE"

RENAMED_OBJS=()
for obj in "${OBJS[@]}"; do
    renamed="$OBJ_DIR/renamed_$(basename "$obj")"
    objcopy --redefine-syms="$SYMFILE" "$obj" "$renamed"
    RENAMED_OBJS+=("$renamed")
done

# Combine into a single relocatable object for convenience (so the match
# driver's link line only needs one extra file per baseline).
ld -r -o "$OUTDIR/${PREFIX}_combined.o" "${RENAMED_OBJS[@]}"

echo "Built $OUTDIR/${PREFIX}_combined.o from ref '$REF' ($(wc -l < "$SYMFILE") symbols renamed with prefix '${PREFIX}_')" >&2
