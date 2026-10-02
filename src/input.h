// Combines the joystick:bit's four digital buttons (C/D/E/F) and the
// micro:bit's own onboard Button A/B into one directional input API, per
// the mapping Rune asked for: C=left, D=up, E=right, F=down, A=select,
// B=cancel, and A+B held together=INPUT_INFO (shows the last move/nodes/
// eval screen during play). The rocker (SAADC) is deliberately not used
// anywhere in this checkpoint -- digital buttons only.
#pragma once
#include <stdint.h>
#include "joystick.h"
#include "buttons.h"

static inline void input_init(void) {
    joystick_init(); // C/D/E/F pull-ups + the joystick:bit's own unexplained init steps
    buttons_ab_init();
}

static inline int input_left(void)   { return joystick_button_c(); }
static inline int input_up(void)     { return joystick_button_d(); }
static inline int input_right(void)  { return joystick_button_e(); }
static inline int input_down(void)   { return joystick_button_f(); }
static inline int input_select(void) { return button_a(); }
static inline int input_cancel(void) { return button_b(); }

typedef enum {
    INPUT_NONE = 0,
    INPUT_LEFT,
    INPUT_UP,
    INPUT_RIGHT,
    INPUT_DOWN,
    INPUT_SELECT,
    INPUT_CANCEL,
    INPUT_INFO, // A+B held together -- see input_poll_edge()
} InputEvent;

// Edge-triggered: fires once per press, not once per loop iteration while
// held. Priority order (left before up before...) only matters if someone
// manages to press two buttons in the exact same poll, which doesn't
// meaningfully happen with fingers -- EXCEPT A+B together, which is
// deliberate (the "show info" combo) and is checked first so it can never
// be swallowed by the plain SELECT/CANCEL edges below it.
static inline InputEvent input_poll_edge(void) {
    static int prev_left = 0, prev_up = 0, prev_right = 0, prev_down = 0;
    static int prev_select = 0, prev_cancel = 0, prev_combo = 0;

    int left = input_left(), up = input_up(), right = input_right(), down = input_down();
    int select = input_select(), cancel = input_cancel();
    int combo = select && cancel;

    InputEvent ev = INPUT_NONE;
    if (combo && !prev_combo) ev = INPUT_INFO;
    else if (left && !prev_left) ev = INPUT_LEFT;
    else if (up && !prev_up) ev = INPUT_UP;
    else if (right && !prev_right) ev = INPUT_RIGHT;
    else if (down && !prev_down) ev = INPUT_DOWN;
    else if (select && !prev_select && !cancel) ev = INPUT_SELECT; // not part of a combo
    else if (cancel && !prev_cancel && !select) ev = INPUT_CANCEL; // not part of a combo

    prev_left = left; prev_up = up; prev_right = right; prev_down = down;
    prev_select = select; prev_cancel = cancel; prev_combo = combo;
    return ev;
}

// How long input_wait_press() waits, after seeing a lone A or B edge, to
// see whether the OTHER one lands too (see below). ~4000 * ~31us/iteration
// (2000 NOPs at 64MHz) is roughly 125ms -- long enough for two fingers
// pressed "at the same time" to both register (human synchrony is rarely
// tighter than 50-100ms), short enough that a genuine single A or B press
// never feels laggy.
#define INFO_COMBO_GRACE_POLLS 4000

// Blocks until a new button press is detected. The whole game/menu flow is
// turn-based and event-driven, so this is the only "wait" primitive most of
// the program needs -- no periodic redraw/animation loop to manage. Plain
// and immediate: whichever of A/B is pressed first returns its own
// SELECT/CANCEL edge right away, with no added delay. That's correct
// everywhere INPUT_INFO is never checked -- menus, number pickers, move
// confirmation, dismiss prompts -- which is everywhere except the one
// gameplay wait below, so this stays fast for the vast majority of presses
// in the program.
static inline InputEvent input_wait_press(void) {
    InputEvent ev;
    while ((ev = input_poll_edge()) == INPUT_NONE) {
        for (volatile uint32_t i = 0; i < 20000; i++) {
            __asm__ volatile("nop");
        }
    }
    return ev;
}

// Same as input_wait_press(), but also recognizes the A+B "show info" combo
// -- use only at call sites that actually check for INPUT_INFO (today,
// just the player's-turn wait in play_vs_engine()). Two fingers essentially
// never land in the exact same ~microsecond poll, so whichever of A/B is
// pressed first fires its own SELECT/CANCEL edge immediately, before the
// second finger has landed -- input_poll_edge()'s combo check never gets a
// chance to see both held at once. So when a lone SELECT/CANCEL edge comes
// back here, this waits a short grace window, still polling normally
// (keeping input_poll_edge()'s internal edge-tracking state consistent),
// for the combo to complete; if it does, INPUT_INFO is returned instead.
// Otherwise the original single press stands after the grace window
// elapses. This added latency is deliberately NOT paid by plain
// input_wait_press() above, since applying it there made every ordinary A
// or B press throughout the whole program -- not just gameplay -- feel
// laggy for no benefit (menus, pickers, etc. never look for INPUT_INFO).
static inline InputEvent input_wait_press_allow_combo(void) {
    InputEvent ev = input_wait_press();

    if (ev == INPUT_SELECT || ev == INPUT_CANCEL) {
        for (uint32_t g = 0; g < INFO_COMBO_GRACE_POLLS; g++) {
            InputEvent ev2 = input_poll_edge();
            if (ev2 == INPUT_INFO) {
                return INPUT_INFO;
            }
            for (volatile uint32_t i = 0; i < 2000; i++) {
                __asm__ volatile("nop");
            }
        }
    }

    return ev;
}
