#ifndef BOOK_H
#define BOOK_H

#include "board.h"
#include "movegen.h"

// ---------------------------------------------------------------------------
// OPENING BOOK: maps a position's Zobrist hash to a known move, drawn from a
// curated set of real opening lines. When enabled, search consults this
// BEFORE spending any node budget on the current position -- the entire
// point of a book is to skip searching positions whose theory is already
// settled, and only start "thinking for real" once the game actually
// reaches something the book doesn't know. Every real position a game
// reaches is looked up by its own Zobrist hash, so this works regardless of
// move order (two different move orders reaching the same position both
// hit the same book entry) -- one of the main things a Zobrist hash is
// good for.
//
// TWO SOURCES, one lookup function (book_probe() below checks both):
//
//  1. STATIC (device-ready): book_data.c/.h, generated ahead of time on a
//     host by tools/gen_book_data.c from a moves-only opening file, and
//     checked into the source tree like any other file. It's two flat
//     `const` arrays -- a sorted list of position hashes and the matching
//     moves -- so it lives in FLASH (not RAM) and is searched with a
//     binary search, not a hash table. This is what actually ships to the
//     real device: ~10 bytes of flash per book position, zero RAM cost,
//     zero startup parsing. See tools/gen_book_data.c's own comment for
//     the full reasoning and how to regenerate it after hand-tuning the
//     book's source file.
//
//  2. DYNAMIC (host-only): book_load() below, which parses a text file
//     into an in-RAM hash table at runtime. This remains useful for fast
//     host-side iteration (try a book file, run a match, throw it away)
//     BEFORE spending the extra step of baking a chosen book into
//     book_data.c for the device -- but at a size (a full open-addressing
//     table, currently 64K slots of 16 bytes each = 1MB) that would
//     overrun the micro:bit's entire 128KB of RAM roughly eight times
//     over, so it is NEVER what should end up wired into the real device
//     firmware. main_play_test.c should only ever rely on the static book
//     being present, never call book_load().
//
//     THIS ISN'T JUST A "DON'T CALL IT" CONVENTION -- it's enforced at
//     compile time. Define ONDSEL_DEVICE_BUILD (the device Makefile does
//     this for every target that links search.c, since search.c now
//     always references book_probe()/book_enabled) and the entire dynamic
//     table, plus book_load() and everything it depends on, compiles out
//     of book.c completely -- the 1MB array simply doesn't exist in a
//     device build, rather than existing-but-unused. A host build (any
//     match harness, tools/gen_book_data.c, tools/build_opening_book.c)
//     leaves ONDSEL_DEVICE_BUILD undefined and gets the full dynamic table
//     as before. book_load()'s declaration disappears too in a device
//     build, so a device file that mistakenly calls it fails loudly (a
//     compiler warning plus a link error for the missing symbol) instead
//     of the 1MB table quietly coming back the moment someone wires it up.
// ---------------------------------------------------------------------------

void book_init(void);

#ifndef ONDSEL_DEVICE_BUILD
// Parses a "moves-only" opening file -- one PGN-style movetext line per
// opening, e.g. "1. d4 Nf6 2. c4 e6 3. Nf3 ..." -- and inserts every
// (position, move actually played next) pair it walks through into the
// DYNAMIC (host-only, RAM) lookup table. Safe to call more than once to
// merge additional files; later calls simply add more entries (a position
// already known keeps its first-seen move rather than being overwritten).
//
// Returns the number of NEW distinct positions this call actually inserted
// (many lines share a common opening prefix, so this is usually far
// smaller than the number of plies read).
//
// Not declared at all in a device build (ONDSEL_DEVICE_BUILD defined) --
// see this file's header comment on why that's deliberate.
int book_load(const char *path);
#endif

// Returns the book's move for this exact position, or 0 if the position
// isn't in the book (checks the static/flash book first, then the dynamic
// host-only table if book_load() has ever been called). Leaving book
// theory is the normal, expected outcome of every single game eventually
// -- it isn't an error condition, just the signal for search to take over
// from here.
Move book_probe(const Board *b);

// Off by default, same "prove it before it changes real play" convention
// as search.h's null_move_enabled and eval.h's eval_mobility_enabled /
// eval_endgame_heuristics_enabled. See search.c's find_best_move_impl()
// for exactly where and how this is consulted.
extern int book_enabled;

// How many (position -> move) entries are currently available across BOTH
// sources (static + dynamic combined), purely for reporting/benchmarking.
extern int book_entry_count;

#endif
