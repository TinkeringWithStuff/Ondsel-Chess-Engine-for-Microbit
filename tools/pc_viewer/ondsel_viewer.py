#!/usr/bin/env python3
"""Ondsel self-play viewer: watches the micro:bit's self-play mode live,
over the same USB cable used to flash it, and draws the board on the PC as
moves come in.

Usage:
    python3 ondsel_viewer.py                    # auto-detect the port
    python3 ondsel_viewer.py --port /dev/tty.usbmodem1102
    python3 ondsel_viewer.py --simulate          # no hardware needed -- plays
                                                  # a short scripted demo game
                                                  # through the exact same
                                                  # pipeline, to check the
                                                  # viewer itself works

Needs: pyserial (`pip install pyserial`). Nothing else -- see protocol.py's
own comment for why a chess library isn't required.

On the micro:bit side: start "Self-play" from the top menu. Every move
(and the game's start/end) gets broadcast over UART in a small
checksummed, repeat-for-reliability protocol -- see main_play_test.c's
"PC viewer protocol" comment and protocol.py's matching docstring for the
exact wire format.

Two buttons in the window itself:
    End Game    -- ends the current self-play game right now instead of
                   letting it play out, by sending a command back to the
                   device (same effect as pressing A on the micro:bit
                   itself during self-play). The result is decided from the
                   engine's own current evaluation: the side 300cp or more
                   ahead wins, otherwise it's called a draw. Only enabled
                   while a game is actually in progress.
    Export PGN  -- writes the game shown (finished or still in progress) to
                   a timestamped .pgn file in the current directory, with
                   each move's eval/depth/nodes/time as a PGN comment.
"""
from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
import tkinter as tk
from typing import Optional

from protocol import GameState, LineDecoder, build_pgn, line_checksum

DEFAULT_BAUD = 9600  # must match uart.h's UARTE_BAUDRATE_9600 -- see that
                      # file's own comment on why 9600 was chosen over
                      # 115200 for this link's reliability.

PIECE_GLYPH = {
    "K": "♔", "Q": "♕", "R": "♖", "B": "♗", "N": "♘", "P": "♙",
    "k": "♚", "q": "♛", "r": "♜", "b": "♝", "n": "♞", "p": "♟",
}

LIGHT_SQUARE = "#F0D9B5"
DARK_SQUARE = "#B58863"
HIGHLIGHT = "#F6F669"
SQUARE_PX = 60
BOARD_PX = SQUARE_PX * 8


# ---------------------------------------------------------------------------
# Line sources: a real serial port, or a built-in scripted demo game that
# needs no hardware -- both just push raw lines into the same queue, so the
# GUI code below doesn't know or care which one is feeding it.
# ---------------------------------------------------------------------------
class SerialLineSource:
    def __init__(self, port: str, baud: int, line_queue: "queue.Queue[str]") -> None:
        import serial  # imported here so --simulate doesn't need pyserial installed

        self.line_queue = line_queue
        self._stop = threading.Event()
        self._ser = serial.Serial(port, baud, timeout=1)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            self._ser.close()
        except Exception:
            pass

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                raw = self._ser.readline()
            except Exception:
                break
            if not raw:
                continue
            try:
                text = raw.decode("ascii", errors="replace")
            except Exception:
                continue
            self.line_queue.put(text)

    def send_end_game(self) -> None:
        """PC -> device command for the "End Game" button: see protocol.py's
        module docstring for the wire format (a bare ASCII 'E', repeated a
        few times back to back for the same reliability reason the device
        repeats ITS lines -- this link drops bytes in either direction, and
        the device only needs to see one 'E' to act, so duplicates are
        harmless). Best-effort: if the write fails (port closed, device
        unplugged), there's nothing more useful to do than silently skip it
        -- the same as a button press on a device that isn't there."""
        try:
            self._ser.write(b"E" * 5)
        except Exception:
            pass


class SimulatedLineSource:
    """Plays a short, fixed demo game (Italian Game opening, then stopped)
    through the real wire format -- including PROTO_REPEATS=3 copies of
    every line -- so the viewer can be exercised with no micro:bit attached.
    Useful for checking the viewer itself works before trusting it to show
    you something real."""

    # (uci, score, depth, nodes, time_ms) -- the 3 trailing fields are what
    # the current firmware's broadcast_move() actually sends; made up here
    # (not real search output) just to exercise the viewer's stats display
    # end to end without a real micro:bit attached.
    DEMO_MOVES = [
        ("e2e4", 15, 6, 48213, 310), ("e7e5", -12, 6, 51022, 325),
        ("g1f3", 25, 7, 112044, 480), ("b8c6", -20, 7, 108771, 470),
        ("f1c4", 30, 7, 134509, 510), ("f8c5", -28, 7, 129830, 495),
        ("e1g1", 35, 8, 310442, 920), ("g8f6", -25, 8, 298120, 905),
        ("d2d3", 10, 6, 44108, 290), ("d7d6", -8, 6, 45900, 295),
    ]

    def __init__(self, line_queue: "queue.Queue[str]", delay_seconds: float = 1.0) -> None:
        self.line_queue = line_queue
        self.delay_seconds = delay_seconds
        self._stop = threading.Event()
        self._ended_early = threading.Event()  # set by send_end_game()
        self._emitted_count = 0  # how many DEMO_MOVES have gone out so far
        self._last_score = 0     # that last move's own-perspective score
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def _emit(self, payload: str) -> None:
        line = f"{payload}*{line_checksum(payload):02X}\r\n"
        for _ in range(3):  # PROTO_REPEATS
            if self._stop.is_set():
                return
            self.line_queue.put(line)

    def _should_end(self) -> bool:
        return self._stop.is_set() or self._ended_early.is_set()

    def _run(self) -> None:
        self._emit("GAME_START")
        for i, (uci, score, depth, nodes, time_ms) in enumerate(self.DEMO_MOVES):
            if self._should_end():
                return
            time.sleep(self.delay_seconds)
            if self._should_end():  # re-check: send_end_game() may have
                return              # landed while this move was "in flight"
            self._emitted_count = i + 1
            self._last_score = score
            self._emit(f"MOVE {uci} {score} {depth} {nodes} {time_ms}")
        if self._should_end():
            return
        time.sleep(self.delay_seconds)
        self._emit("GAME_END STOPPED")

    def send_end_game(self) -> None:
        """No real device behind --simulate to ask, so this adjudicates
        locally from whatever of the scripted demo game has played so far,
        using the IDENTICAL >=300cp-decides-it rule as the firmware's own
        adjudicate_result() (see main_play_test.c) -- close enough to
        exercise the viewer's End Game button end to end without hardware."""
        self._ended_early.set()
        if self._emitted_count == 0:
            code = "STOPPED"
        else:
            is_white_move = (self._emitted_count - 1) % 2 == 0
            white_relative = self._last_score if is_white_move else -self._last_score
            if white_relative >= 300:
                code = "ADJUDICATED_WHITE"
            elif white_relative <= -300:
                code = "ADJUDICATED_BLACK"
            else:
                code = "ADJUDICATED_DRAW"
        self._emit(f"GAME_END {code}")


def find_port() -> Optional[str]:
    """Best-effort auto-detect: looks for a serial port whose description
    or device path suggests a micro:bit's DAPLink USB-serial bridge."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None

    candidates = []
    for p in list_ports.comports():
        haystack = f"{p.device} {p.description}".lower()
        if any(tag in haystack for tag in ("microbit", "daplink", "mbed", "usbmodem")):
            candidates.append(p.device)
    if len(candidates) == 1:
        return candidates[0]
    return None


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
class ViewerApp:
    def __init__(self, root: tk.Tk, line_queue: "queue.Queue[str]", source) -> None:
        self.root = root
        self.line_queue = line_queue
        self.source = source  # SerialLineSource or SimulatedLineSource -- both
                               # expose send_end_game(), which is all this class needs
        self.decoder = LineDecoder()
        self.game = GameState()

        root.title("Ondsel self-play viewer")
        root.configure(bg="#222222")

        main = tk.Frame(root, bg="#222222")
        main.pack(padx=10, pady=10)

        self.canvas = tk.Canvas(main, width=BOARD_PX, height=BOARD_PX, highlightthickness=0)
        self.canvas.grid(row=0, column=0)

        side = tk.Frame(main, bg="#222222", width=340)
        side.grid(row=0, column=1, sticky="ns", padx=(12, 0))

        self.status_label = tk.Label(side, text="Waiting for GAME_START...", fg="white",
                                      bg="#222222", font=("Helvetica", 13, "bold"), wraplength=250, justify="left")
        self.status_label.pack(anchor="w", pady=(0, 8))

        self.last_move_label = tk.Label(side, text="", fg="#CCCCCC", bg="#222222",
                                         font=("Helvetica", 11), justify="left")
        self.last_move_label.pack(anchor="w", pady=(0, 8))

        tk.Label(side, text="Moves", fg="white", bg="#222222", font=("Helvetica", 11, "bold")).pack(anchor="w")
        list_frame = tk.Frame(side)
        list_frame.pack(fill="both", expand=True)
        scrollbar = tk.Scrollbar(list_frame)
        scrollbar.pack(side="right", fill="y")
        self.move_list = tk.Listbox(list_frame, width=44, height=20, yscrollcommand=scrollbar.set,
                                     font=("Courier", 11))
        self.move_list.pack(side="left", fill="both", expand=True)
        scrollbar.config(command=self.move_list.yview)

        self.link_label = tk.Label(side, text="", fg="#888888", bg="#222222", font=("Helvetica", 9))
        self.link_label.pack(anchor="w", pady=(8, 0))

        buttons = tk.Frame(side, bg="#222222")
        buttons.pack(anchor="w", fill="x", pady=(10, 0))

        self.end_game_button = tk.Button(buttons, text="End Game", command=self.on_end_game)
        self.end_game_button.pack(side="left")

        self.export_button = tk.Button(buttons, text="Export PGN", command=self.on_export_pgn)
        self.export_button.pack(side="left", padx=(8, 0))

        self.export_status_label = tk.Label(side, text="", fg="#888888", bg="#222222",
                                             font=("Helvetica", 9), wraplength=250, justify="left")
        self.export_status_label.pack(anchor="w", pady=(4, 0))

        self.draw_board()
        self.refresh_labels()  # sets the buttons' correct initial (disabled) state
        self.root.after(50, self.poll_queue)

    def draw_board(self, highlight_from: Optional[int] = None, highlight_to: Optional[int] = None) -> None:
        self.canvas.delete("all")
        for rank in range(8):
            for file in range(8):
                x0 = file * SQUARE_PX
                y0 = (7 - rank) * SQUARE_PX  # rank 7 (rank 8) drawn at the top
                x1, y1 = x0 + SQUARE_PX, y0 + SQUARE_PX
                sq_index = rank * 8 + file
                color = LIGHT_SQUARE if (rank + file) % 2 == 0 else DARK_SQUARE
                if sq_index in (highlight_from, highlight_to):
                    color = HIGHLIGHT
                self.canvas.create_rectangle(x0, y0, x1, y1, fill=color, outline="")

                piece = self.game.board.piece_at(file, rank)
                if piece:
                    glyph = PIECE_GLYPH[piece]
                    cx, cy = x0 + SQUARE_PX / 2, y0 + SQUARE_PX / 2
                    font = ("Arial", int(SQUARE_PX * 0.7))
                    # White glyphs filled white on the light square color
                    # (#F0D9B5) have almost no contrast -- a plain fill color
                    # swap wasn't enough. Draw a thin dark outline (the glyph
                    # repeated at small pixel offsets) underneath the real
                    # fill, same trick as outlined map/UI labels -- this
                    # keeps both colors readable on both square shades
                    # instead of relying on square color alone for contrast.
                    is_white = piece.isupper()
                    fill = "#FFFFFF" if is_white else "#000000"
                    outline = "#000000" if is_white else "#FFFFFF"
                    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                        self.canvas.create_text(cx + dx, cy + dy, text=glyph, font=font, fill=outline)
                    self.canvas.create_text(cx, cy, text=glyph, font=font, fill=fill)

    def poll_queue(self) -> None:
        moved = False
        try:
            while True:
                raw = self.line_queue.get_nowait()
                payload = self.decoder.feed_raw(raw)
                if payload is None:
                    continue
                self.game.apply_payload(payload)
                moved = True
        except queue.Empty:
            pass

        if moved:
            self.refresh_labels()
            last_move = self.game.moves[-1] if self.game.moves else None
            hi_from = hi_to = None
            if last_move:
                hi_from = _sq_index(last_move.uci[0:2])
                hi_to = _sq_index(last_move.uci[2:4])
            self.draw_board(hi_from, hi_to)

        self.root.after(50, self.poll_queue)

    def refresh_labels(self) -> None:
        if self.game.result_text:
            self.status_label.config(text=self.game.result_text)
        elif self.game.in_progress:
            to_move = "White" if self.game.board.white_to_move else "Black"
            self.status_label.config(text=f"In progress -- {to_move} to move")
        else:
            self.status_label.config(text="Waiting for GAME_START...")

        if self.game.moves:
            last = self.game.moves[-1]
            # eval shown White-relative (positive always = good for White),
            # not the engine's own raw negamax figure -- see protocol.py's
            # module docstring for why those two numbers differ.
            text = f"Last: {last.uci}   eval(White)={last.white_relative_score:+d}"
            if last.depth is not None:
                text += f"\ndepth={last.depth}  nodes={last.nodes:,}  time={last.time_ms}ms"
                if last.time_ms:
                    text += f"\n({last.nodes * 1000 // last.time_ms:,} nodes/sec)"
            self.last_move_label.config(text=text)
        else:
            self.last_move_label.config(text="")

        self.move_list.delete(0, tk.END)
        for i, m in enumerate(self.game.moves):
            move_no = i // 2 + 1
            prefix = f"{move_no}." if i % 2 == 0 else "  "
            row = f"{prefix} {m.uci:6s} {m.white_relative_score:+d}"
            if m.depth is not None:
                row += f"  d{m.depth} {m.nodes:>7,}n {m.time_ms:>5}ms"
            self.move_list.insert(tk.END, row)
        self.move_list.see(tk.END)

        stats = self.decoder
        self.link_label.config(
            text=f"lines ok:{stats.accepted_count}  dup:{stats.duplicate_count}  bad:{stats.corrupt_count}"
        )
        if self.game.last_error:
            print(f"[viewer] {self.game.last_error}", file=sys.stderr)

        # End Game only makes sense while a game is actually running; Export
        # PGN is useful any time there's at least one move to write out.
        self.end_game_button.config(state=tk.NORMAL if self.game.in_progress else tk.DISABLED)
        self.export_button.config(state=tk.NORMAL if self.game.moves else tk.DISABLED)

    def on_end_game(self) -> None:
        self.source.send_end_game()
        self.export_status_label.config(text="End Game requested -- waiting for the result...")

    def on_export_pgn(self) -> None:
        import datetime

        filename = f"ondsel_game_{datetime.datetime.now():%Y%m%d_%H%M%S}.pgn"
        try:
            pgn_text = build_pgn(self.game)
            with open(filename, "w") as f:
                f.write(pgn_text)
            self.export_status_label.config(text=f"Saved {filename}")
        except Exception as exc:
            self.export_status_label.config(text=f"Export failed: {exc}")


def _sq_index(name: str) -> int:
    return (int(name[1]) - 1) * 8 + (ord(name[0]) - ord("a"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port, e.g. /dev/tty.usbmodem1102 or COM5 (auto-detected if omitted)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"baud rate (default {DEFAULT_BAUD}, matches the firmware)")
    parser.add_argument("--simulate", action="store_true", help="play a scripted demo game instead of reading real hardware")
    args = parser.parse_args()

    line_queue: "queue.Queue[str]" = queue.Queue()

    if args.simulate:
        source = SimulatedLineSource(line_queue)
    else:
        port = args.port or find_port()
        if not port:
            print("Could not auto-detect the micro:bit's serial port.", file=sys.stderr)
            print("Pass it explicitly, e.g.:", file=sys.stderr)
            print("  macOS:   python3 ondsel_viewer.py --port /dev/tty.usbmodemXXXX", file=sys.stderr)
            print("           (run `ls /dev/tty.usbmodem*` while it's plugged in to find it)", file=sys.stderr)
            print("  Windows: python3 ondsel_viewer.py --port COM5", file=sys.stderr)
            print("  Linux:   python3 ondsel_viewer.py --port /dev/ttyACM0", file=sys.stderr)
            sys.exit(1)
        print(f"Connecting to {port} at {args.baud} baud...")
        try:
            source = SerialLineSource(port, args.baud, line_queue)
        except Exception as exc:
            print(f"Could not open {port}: {exc}", file=sys.stderr)
            sys.exit(1)

    source.start()

    root = tk.Tk()
    ViewerApp(root, line_queue, source)

    def on_close() -> None:
        source.stop()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
