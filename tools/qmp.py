#!/usr/bin/env python3
"""Control the running test machine over QMP.

Usage:
  tools/qmp.py screenshot out.png
  tools/qmp.py status
  tools/qmp.py quit
  tools/qmp.py raw '{"execute": "query-status"}'
  tools/qmp.py hmp 'info registers'     run a human monitor command
  tools/qmp.py change-cd image          insert a disk image as the guest's CD
  tools/qmp.py eject-cd                 remove the guest's CD (host side)
  tools/qmp.py cd-state                 empty, or locked/unlocked and the image
  tools/qmp.py guest-eject NAME         eject the mounted disc NAME from the
                                        guest's Finder (needed before a new
                                        disc can replace a mounted one)
  tools/qmp.py wait-shutdown            print why QEMU stopped (used by run.sh)

Guest UI (coordinates are guest screen pixels, x right, y down):
  tools/qmp.py pos                      print the cursor position
  tools/qmp.py move X Y                 move the cursor to X,Y
  tools/qmp.py click X Y                move there and click
  tools/qmp.py dclick X Y               move there and double-click
  tools/qmp.py drag X1 Y1 X2 Y2         press at X1,Y1, move to X2,Y2, release
  tools/qmp.py key cmd-e [MS]           press a key combination, held MS
                                        milliseconds (default 100)
                                        (modifiers: cmd, shift, opt, ctrl);
                                        Force Quit (cmd-opt-esc) needs 600
  tools/qmp.py type 'some text'         type text on a US keyboard layout

Set QMP_SOCK to use a socket other than run/qmp.sock.

The guest mouse is a relative USB mouse and Mac OS accelerates it, so
`move` reads where the cursor is from the guest's low memory (RawMouse at
0x82C), moves by the remaining distance, and repeats until it is exact.
"""

import json
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SOCK = os.environ.get("QMP_SOCK") or os.path.join(HERE, "..", "run", "qmp.sock")

# Low-memory globals, as Point (v, h) pairs of 16-bit big-endian values.
MTEMP = 0x828
RAWMOUSE = 0x82C


class QMP:
    def __init__(self, path=SOCK):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.f = self.sock.makefile("rw")
        self._recv()  # greeting
        self.cmd("qmp_capabilities")

    def _recv(self):
        """Read QMP messages until one that is not an async event."""
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QMP socket closed")
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        return self.send(msg)

    def send(self, msg):
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        reply = self._recv()
        if "error" in reply:
            raise SystemExit(f"QMP error: {reply['error']}")
        return reply.get("return")

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def close(self):
        self.sock.close()

    # Guest UI -----------------------------------------------------------

    def pos(self):
        """Return the guest cursor position as (x, y).

        The read goes through the CPU's current address translation, which
        is only the Mac OS one while Mac OS code is running, so a read is
        accepted only when MTemp and RawMouse agree and are on screen.
        """
        for _ in range(50):
            out = self.hmp(f"x /4hx {MTEMP:#x}")
            words = [int(w, 16) for w in out.split(":", 1)[1].split()]
            mv, mh, rv, rh = words
            if (mv, mh) == (rv, rh) and mv < 4096 and mh < 4096:
                return rh, rv
            time.sleep(0.01)
        raise SystemExit("could not read the cursor position")

    def rel(self, dx, dy):
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        if events:
            self.cmd("input-send-event", events=events)

    def move(self, x, y):
        for _ in range(60):
            cx, cy = self.pos()
            dx, dy = x - cx, y - cy
            if dx == 0 and dy == 0:
                return
            self.rel(step(dx), step(dy))
            time.sleep(0.03)
        cx, cy = self.pos()
        raise SystemExit(f"cursor stopped at {cx},{cy}, wanted {x},{y}")

    def button(self, down):
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": down, "button": "left"}}])

    # Mac OS's USB mouse driver misses a press shorter than a few of its
    # polls, so presses are held and spaced out; two presses still fit well
    # inside the default double-click time.
    PRESS = 0.12

    def click(self, x, y, count=1):
        self.move(x, y)
        time.sleep(0.1)
        for _ in range(count):
            self.button(True)
            time.sleep(self.PRESS)
            self.button(False)
            time.sleep(self.PRESS)

    def drag(self, x1, y1, x2, y2):
        self.move(x1, y1)
        time.sleep(0.1)
        self.button(True)
        time.sleep(0.2)
        self.move(x2, y2)
        time.sleep(0.2)
        self.button(False)

    def keys(self, qcodes, hold=None):
        args = {"keys": [{"type": "qcode", "data": q} for q in qcodes]}
        if hold:
            args["hold-time"] = hold
        self.cmd("send-key", **args)
        time.sleep(0.05)

    def type_text(self, text):
        for c in text:
            self.keys(char_codes(c))

    def screen_size(self):
        path = os.path.join(HERE, "..", "run", ".size.png")
        self.cmd("screendump", filename=os.path.abspath(path), format="png")
        with open(path, "rb") as f:
            head = f.read(24)
        os.remove(path)
        return (int.from_bytes(head[16:20], "big"),
                int.from_bytes(head[20:24], "big"))

    def cd_state(self):
        """Return (inserted image or None, locked by the guest)."""
        for b in self.cmd("query-block"):
            if b["device"] == "ide1-cd0":
                return (b.get("inserted", {}).get("file"),
                        bool(b.get("locked")))
        raise SystemExit("no CD drive ide1-cd0")

    def guest_eject(self, name, timeout=15):
        """Eject the mounted disc called name from inside the guest.

        Clicks the desktop near the bottom left corner to bring the Finder
        forward, makes the desktop active (Cmd-Shift-Up), selects the disc
        by typing its name, and puts it away with Cmd-E.
        """
        _, height = self.screen_size()
        self.click(20, height - 40)
        self.keys(combo("cmd-shift-up"))
        time.sleep(0.3)
        self.type_text(name)
        time.sleep(0.5)
        self.keys(combo("cmd-e"))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            inserted, locked = self.cd_state()
            if not inserted or not locked:
                return
            time.sleep(0.25)
        raise SystemExit(f'the guest did not eject "{name}"; '
                         "eject it in the guest, then push again")


def step(d):
    """A relative move that undershoots: Mac OS accelerates larger moves."""
    if d == 0:
        return 0
    s = int(d * 0.6)
    if s == 0:
        s = 1 if d > 0 else -1
    return max(-80, min(80, s))


MODIFIERS = {"cmd": "meta_l", "shift": "shift", "opt": "alt", "alt": "alt",
             "ctrl": "ctrl"}

NAMED = {"return": "ret", "enter": "kp_enter", "esc": "esc", "tab": "tab",
         "space": "spc", "delete": "backspace", "backspace": "backspace",
         "fwddel": "delete", "up": "up", "down": "down", "left": "left",
         "right": "right", "home": "home", "end": "end",
         "pageup": "pgup", "pagedown": "pgdn"}

# Characters on a US layout: char -> (qcode, needs shift).
PLAIN = {"-": "minus", "=": "equal", "[": "bracket_left",
         "]": "bracket_right", "\\": "backslash", ";": "semicolon",
         "'": "apostrophe", "`": "grave_accent", ",": "comma", ".": "dot",
         "/": "slash", " ": "spc", "\n": "ret", "\t": "tab"}
SHIFTED = {"_": "minus", "+": "equal", "{": "bracket_left",
           "}": "bracket_right", "|": "backslash", ":": "semicolon",
           '"': "apostrophe", "~": "grave_accent", "<": "comma",
           ">": "dot", "?": "slash", "!": "1", "@": "2", "#": "3",
           "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9",
           ")": "0"}


def combo(spec):
    """Turn 'cmd-shift-e' into QEMU qcodes."""
    parts = spec.lower().split("-")
    codes = [MODIFIERS[p] for p in parts[:-1]]
    last = parts[-1]
    codes.append(NAMED.get(last, last))
    return codes


def char_codes(c):
    if c.isalpha():
        return ["shift", c.lower()] if c.isupper() else [c]
    if c.isdigit():
        return [c]
    if c in PLAIN:
        return [PLAIN[c]]
    if c in SHIFTED:
        return ["shift", SHIFTED[c]]
    raise SystemExit(f"cannot type {c!r}")


def wait_shutdown():
    """Wait for QEMU's SHUTDOWN event and print its reason.

    Prints "closed" if QEMU goes away without one (killed or crashed).
    """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.monotonic() + 30
    while True:
        try:
            s.connect(SOCK)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() > deadline:
                raise SystemExit(f"no QMP socket at {SOCK}")
            time.sleep(0.05)
    f = s.makefile("rw")
    f.readline()  # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    for line in f:
        msg = json.loads(line)
        if msg.get("event") == "SHUTDOWN":
            print(msg.get("data", {}).get("reason", ""))
            return
    print("closed")


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    op, args = argv[1], argv[2:]
    if op == "wait-shutdown":
        wait_shutdown()
        return

    q = QMP()
    try:
        if op == "screenshot" and len(args) == 1:
            out = os.path.abspath(args[0])
            q.cmd("screendump", filename=out, format="png")
            print(out)
        elif op == "status":
            print(json.dumps(q.cmd("query-status")))
        elif op == "quit":
            q.cmd("quit")
        elif op == "raw" and len(args) == 1:
            print(json.dumps(q.send(json.loads(args[0])), indent=2))
        elif op == "hmp" and len(args) == 1:
            print(q.hmp(args[0]), end="")
        elif op == "change-cd" and len(args) == 1:
            # force: Mac OS locks the tray while a disc is mounted.
            q.cmd("blockdev-change-medium", device="ide1-cd0",
                  filename=os.path.abspath(args[0]), format="raw",
                  **{"read-only-mode": "read-only", "force": True})
        elif op == "eject-cd" and not args:
            q.cmd("eject", device="ide1-cd0", force=True)
        elif op == "pos" and not args:
            print("%d %d" % q.pos())
        elif op == "move" and len(args) == 2:
            q.move(int(args[0]), int(args[1]))
        elif op == "click" and len(args) == 2:
            q.click(int(args[0]), int(args[1]))
        elif op == "dclick" and len(args) == 2:
            q.click(int(args[0]), int(args[1]), count=2)
        elif op == "drag" and len(args) == 4:
            q.drag(*map(int, args))
        elif op == "key" and len(args) in (1, 2):
            q.keys(combo(args[0]), int(args[1]) if len(args) == 2 else None)
        elif op == "type" and len(args) == 1:
            q.type_text(args[0])
        elif op == "cd-state" and not args:
            inserted, locked = q.cd_state()
            if not inserted:
                print("empty")
            else:
                print(("locked " if locked else "unlocked ") + inserted)
        elif op == "guest-eject" and len(args) == 1:
            q.guest_eject(args[0])
        else:
            raise SystemExit(__doc__)
    finally:
        q.close()


if __name__ == "__main__":
    main(sys.argv)
