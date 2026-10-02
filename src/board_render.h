// Draws the live board on the Kitronik :VIEW 128x64 OLED, filling the whole
// screen (8 squares x 16px wide x 8px tall each -- the display is exactly
// twice as wide as it is tall, so non-square cells are what actually fills
// it edge to edge). Pieces are drawn as plain letters (font5x5.h, the 5x5
// bitmap font ported from Kitronik's own OLED driver) -- uppercase for
// White, lowercase for Black -- the same convention chess notation itself
// uses, in place of an earlier hand-drawn glyph/outline scheme that turned
// out to be more complexity than this checkpoint needed. Dark squares keep
// the simple dot-dither pattern from the very first OLED board test; no
// side-to-move indicator is drawn here any more -- that and other status
// (last move, nodes searched, eval) live on the separate info screen
// (main_play_test.c's show_info_screen(), reached by holding A+B together),
// keeping the board screen itself uncluttered. Full-screen text (menus,
// depth picker, test/game results) uses draw_menu_screen()/
// draw_text_screen(), which replace the board entirely since those screens
// have no board to show. Everything still also goes out over UART in full.
//
// Square (file, rank) -> screen: file*SQUARE_W across, with rank 7 (rank
// "8") at the TOP of the screen and rank 0 (rank "1") at the bottom,
// matching how a human reads a board from White's side. Square index
// follows the engine's own convention (confirmed from move_to_str()
// elsewhere in this project): index = rank*8 + file, file=0..7 is a..h,
// rank=0..7 is 1..8.
#pragma once
#include <stdint.h>
#include "oled.h"
#include "engine/board.h"
#include "font5x5.h"

#define SQUARE_W (OLED_WIDTH / 8)  // 16px
#define SQUARE_H (OLED_HEIGHT / 8) // 8px

// One letter per piece TYPE (PAWN..KING, index 0 unused) -- the render loop
// lowercases it for Black. Standard chess-notation letters, so anyone who
// already reads algebraic notation reads the board for free.
static const char PIECE_LETTER[7] = { '?', 'P', 'N', 'B', 'R', 'Q', 'K' };

static void draw_square_border(int sq, int thickness) {
    int file = sq % 8, rank = sq / 8;
    int x0 = file * SQUARE_W, y0 = (7 - rank) * SQUARE_H;
    for (int t = 0; t < thickness; t++) {
        for (int i = 0; i < SQUARE_W; i++) {
            oled_set_pixel(x0 + i, y0 + t, 1);                // top
            oled_set_pixel(x0 + i, y0 + SQUARE_H - 1 - t, 1); // bottom
        }
        for (int i = 0; i < SQUARE_H; i++) {
            oled_set_pixel(x0 + t, y0 + i, 1);                // left
            oled_set_pixel(x0 + SQUARE_W - 1 - t, y0 + i, 1); // right
        }
    }
}

// cursor_sq / selected_sq: pass -1 to not draw that highlight (e.g.
// self-play, which has no cursor).
static void render_board(const Board *b, int cursor_sq, int selected_sq) {
    oled_clear();

    for (int sq = 0; sq < 64; sq++) {
        int file = sq % 8, rank = sq / 8;
        int x0 = file * SQUARE_W, y0 = (7 - rank) * SQUARE_H;

        int8_t piece = b->mailbox[sq];
        if (piece == 0) {
            // Dark squares get a dot-dither pattern (a1 is dark, standard
            // convention: (file+rank) even = dark); light squares stay
            // blank. Only drawn on EMPTY squares -- an occupied square
            // shows just its letter on a blank cell, so the dither never
            // fights the glyph for contrast.
            if (((file + rank) % 2) == 0) {
                for (int py = 0; py < SQUARE_H; py++) {
                    for (int px = 0; px < SQUARE_W; px++) {
                        if (((px + py) % 2) == 0) {
                            oled_set_pixel(x0 + px, y0 + py, 1);
                        }
                    }
                }
            }
        } else {
            int ptype = (piece > 0) ? piece : -piece;
            char letter = PIECE_LETTER[ptype];
            if (piece < 0) letter = (char)(letter - 'A' + 'a'); // Black: lowercase
            // Centered in the 16x8 cell: (16-5)/2=5, (8-5)/2=1 (rounds down).
            draw_char(x0 + 5, y0 + 1, letter);
        }
    }

    if (selected_sq >= 0) {
        draw_square_border(selected_sq, 2); // thicker: "picked up"
    }
    if (cursor_sq >= 0) {
        draw_square_border(cursor_sq, 1);
    }

    oled_flush();
}

// ---- text helpers, built on font5x5.h's draw_char/draw_text ----

// Full-screen text menu: title on the first line, one item per line below
// it, '>' marking the selected item. Used for the top menu, the Testing
// submenu, and the depth picker -- none of those have a board to show.
static void draw_menu_screen(const char *title, const char *const *items, int count, int selected) {
    oled_clear();
    draw_text(0, 0, title);
    for (int i = 0; i < count; i++) {
        int y = 9 + i * FONT_LINE_H;
        if (y + 5 >= OLED_HEIGHT) break;
        if (i == selected) draw_text(0, y, ">");
        draw_text(7, y, items[i]);
    }
    oled_flush();
}

// Full-screen plain text dump, up to 9 lines (64px / 7px line height) --
// used for perft/test-position results and other one-off status screens
// with no board to show.
static void draw_text_screen(const char *const *lines, int count) {
    oled_clear();
    for (int i = 0; i < count; i++) {
        int y = i * FONT_LINE_H;
        if (y + 5 >= OLED_HEIGHT) break;
        draw_text(0, y, lines[i]);
    }
    oled_flush();
}
