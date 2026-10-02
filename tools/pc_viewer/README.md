# Ondsel self-play viewer

Watches the micro:bit's Self-play mode live, over the same USB cable used
to flash it, and draws the board on your PC as moves come in.

## Setup (once)

```
pip install pyserial
```

That's the only dependency -- no chess library needed (see `protocol.py`'s
docstring for why: the device is the one deciding legality, the viewer just
applies the moves it's told about).

`tkinter` (the GUI toolkit) ships with most Python installs already. If
`python3 ondsel_viewer.py` complains it's missing:
- macOS (python.org installer): reinstall Python and make sure to tick
  the "tcl/tk" option, or `brew install python-tk`.
- Linux: `sudo apt install python3-tk` (or your distro's equivalent).
- Windows: it's bundled with the standard python.org installer already.

## Try it without hardware first

```
python3 ondsel_viewer.py --simulate
```

Plays a short scripted demo game through the exact same code path a real
micro:bit would use, one move per second. If this shows a board updating
correctly, the viewer itself is working and any issue afterwards is on the
device/cable/port side, not here.

## Running it for real

1. Plug the micro:bit in over USB (same cable as flashing).
2. Build and flash the `play_test` target as usual (`make play_test`,
   flash it the way you already do).
3. Find the port:
   - **macOS**: `ls /dev/tty.usbmodem*` while it's plugged in.
   - **Windows**: check Device Manager under "Ports (COM & LPT)" -- it'll
     be something like `COM5`.
   - **Linux**: usually `/dev/ttyACM0`.
4. Run:
   ```
   python3 ondsel_viewer.py --port /dev/tty.usbmodemXXXX
   ```
   (or just `python3 ondsel_viewer.py` -- it tries to auto-detect the port
   first, and only asks you to specify one if that fails.)
5. On the micro:bit: from the top menu, choose **Self-play**. Moves should
   start appearing on the PC board as they're played.

The side panel shows whose move it is, the last move and its eval, a
running move list, and a small `lines ok / dup / bad` counter -- that's
diagnostic: `dup` and a few `bad` are normal and expected (see "Why lines
repeat" below), climbing `bad` with a board that never updates would mean
something's actually wrong with the link.

## Why lines repeat (and why that's fine)

The micro:bit's USB-serial link has a known quirk (documented in
`src/uart.h`): it can silently drop a single byte here and there. Rather
than fight that on the wire, every event is sent 3 times with a checksum
on each copy. The viewer verifies the checksum on every line, throws away
any copy that got corrupted, and recognizes an intact repeat of a line it
already applied and ignores it too -- so out of 3 copies, exactly one
genuine update gets through as long as at least one of the 3 arrives
intact. `test_protocol.py` (see below) has a test for exactly this
"first copy corrupted, second gets through" scenario.

## Running the tests

```
python3 test_protocol.py
```

No pyserial or tkinter needed for this -- it only exercises `protocol.py`
(checksum verification, de-duplication, and move application including
castling, en passant and promotion), which is the part that actually needs
to be correct. The GUI layer in `ondsel_viewer.py` is intentionally kept
thin on top of it.

## Files

- `protocol.py` -- wire format parsing, checksum verification, and the
  board/game-state tracker. No GUI or serial code at all.
- `test_protocol.py` -- headless tests for the above.
- `ondsel_viewer.py` -- the Tkinter GUI and serial/simulated line sources.
