"""Run the IC-705 emulator on a remote Linux host (e.g. a Raspberry Pi) and show
it as a radio front panel in a local window.

The script starts `ic705.py --vnc` on the remote host over ssh and forwards its
LCD (VNC) and control (QMP) ports through the same ssh connection. The window
draws the 480x272 LCD in a front panel laid out like the IC-705's: keys (held
while the mouse button is held), the TWIN PBT, AF/RF-SQL and MULTI knobs (drag
or wheel to turn, click to push), the main dial, POWER and the POWER, TX/RX and
CHARGING LEDs. Every control drives the input the firmware reads (sub-CPU key
ladders, the port-8 encoders, MTU2 phase counting, PWRK), and the LEDs show the
sub-CPU's LED outputs. The LCD is the touch panel; the keyboard keys of the GTK
window still work. Only the Python standard library and the system's `ssh`
client are needed.

    python ic705_remote.py --host user@raspberrypi.local
    python ic705_remote.py --host user@raspberrypi.local --scale 2

--load sends a file from this computer to the host first and runs that instead
of --firmware: an Icom .dat package, or a bare-metal flash image (.bin, booted
at 0x18000000 on the IC-705 board, e.g. examples/power/power.bin). With
--subcpu a .bin also gets Icom's sub-CPU firmware from --firmware beside it
(panel keys, TX/RX LEDs).

    python ic705_remote.py --host user@raspberrypi.local --load firmware/705E140.dat
    python ic705_remote.py --host user@raspberrypi.local --load examples/hello/hello.bin

Without arguments the window opens with nothing running: File > Host... sets
the emulator host (remembered): 'local' runs the emulator on this computer (in
WSL on Windows), user@host on another machine over ssh. File > Open
firmware... starts one.
The window's File menu opens another firmware (Ctrl+O), reloads the current one
after a rebuild (F5) or goes back to the Icom firmware; the Run menu saves LCD
screenshots and chooses whether a .bin gets the sub-CPU. Recent files and that
choice are kept in ~/.ic705_remote.json.
"""

import argparse
import hashlib
from pathlib import Path
import queue
import shlex
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib


class Rfb:
    """Minimal RFB 3.8 client: no authentication, raw encoding, 32-bit pixels."""

    def __init__(self, sock):
        self.sock = sock
        self.send_lock = threading.Lock()
        self.f = sock.makefile("rb")
        if not self.read(12).startswith(b"RFB "):
            raise ConnectionError("not a VNC server")
        sock.sendall(b"RFB 003.008\n")
        types = self.read(self.read(1)[0])
        if 1 not in types:
            raise ConnectionError("VNC server requires a password")
        sock.sendall(b"\x01")
        if struct.unpack(">I", self.read(4))[0] != 0:
            raise ConnectionError("VNC security handshake failed")
        sock.sendall(b"\x01")                                    # shared
        self.width, self.height = struct.unpack(">HH", self.read(4))
        self.read(16)                                            # server pixel format
        self.read(struct.unpack(">I", self.read(4))[0])          # desktop name
        # 32 bpp, depth 24, little-endian, true colour, R<<16 G<<8 B
        pf = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        self.send(b"\x00\x00\x00\x00" + pf)
        self.send(struct.pack(">BxHii", 2, 2, 0, -223))           # raw, DesktopSize
        self.rgb = bytearray(self.width * self.height * 3)
        self.dirty = False
        self.closed = False
        self.lock = threading.Lock()

    def read(self, n):
        data = self.f.read(n)
        if len(data) != n:
            raise ConnectionError("VNC connection closed")
        return data

    def send(self, data):
        with self.send_lock:
            self.sock.sendall(data)

    def request(self, incremental=True):
        self.send(struct.pack(">BBHHHH", 3, int(incremental), 0, 0, self.width, self.height))

    def key(self, keysym, down):
        self.send(struct.pack(">BBxxI", 4, int(down), keysym))

    def pointer(self, x, y, mask):
        x = min(max(x, 0), self.width - 1)
        y = min(max(y, 0), self.height - 1)
        self.send(struct.pack(">BBHH", 5, mask, x, y))

    def pump(self):
        """Read server messages until the connection closes (run in a thread)."""
        try:
            self._pump()
        except (ConnectionError, OSError, ValueError):
            self.closed = True

    def _pump(self):
        self.request(False)
        while True:
            kind = self.read(1)[0]
            if kind == 0:                                        # FramebufferUpdate
                self.read(1)
                for _ in range(struct.unpack(">H", self.read(2))[0]):
                    x, y, w, h, enc = struct.unpack(">HHHHi", self.read(12))
                    if enc == -223:                              # DesktopSize
                        with self.lock:
                            self.width, self.height = w, h
                            self.rgb = bytearray(w * h * 3)
                        continue
                    if enc != 0:
                        raise ConnectionError(f"unexpected encoding {enc}")
                    data = self.read(w * h * 4)
                    with self.lock:
                        for row in range(h):
                            src = data[row * w * 4:(row + 1) * w * 4]
                            start = ((y + row) * self.width + x) * 3
                            line = bytearray(w * 3)
                            line[0::3] = src[2::4]
                            line[1::3] = src[1::4]
                            line[2::3] = src[0::4]
                            self.rgb[start:start + w * 3] = line
                        self.dirty = True
                self.request(True)
            elif kind == 1:                                      # SetColourMapEntries
                self.read(3)
                self.read(6 * struct.unpack(">H", self.read(2))[0])
            elif kind == 2:                                      # Bell
                pass
            elif kind == 3:                                      # ServerCutText
                self.read(3)
                self.read(struct.unpack(">I", self.read(4))[0])
            else:
                raise ConnectionError(f"unexpected VNC message {kind}")

    def frame(self):
        with self.lock:
            self.dirty = False
            return self.width, self.height, bytes(self.rgb)


def connect(port, ssh, timeout=90):
    """Wait until the forwarded port answers with an RFB banner."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ssh.poll() is not None:
            raise RuntimeError(f"ssh exited with status {ssh.returncode}")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=2)
            sock.settimeout(None)
            return Rfb(sock)
        except (OSError, ConnectionError):
            time.sleep(0.5)
    raise RuntimeError("the emulator's VNC display did not come up")


def write_png(path, width, height, rgb):
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


# Zoom steps as (zoom, subsample) pairs: Tk scales images only by integer
# zoom and subsample, so each step is a small fraction.
STEPS = [(1, 2), (3, 4), (1, 1), (5, 4), (3, 2), (2, 1), (5, 2), (3, 1), (4, 1)]


def step_for(scale):
    """Index of the largest step not above scale (at least the smallest)."""
    best = 0
    for i, (n, d) in enumerate(STEPS):
        if n / d <= scale + 1e-9:
            best = i
    return best


class Qmp:
    """QMP control connection; commands run one at a time in a worker thread."""

    def __init__(self, port):
        import json
        import queue
        self.json = json
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.sock.makefile("rw", encoding="utf-8")
        self.f.readline()                                       # greeting
        self._call("qmp_capabilities")
        self.jobs = queue.Queue()
        threading.Thread(target=self._worker, daemon=True).start()

    def _call(self, command, **arguments):
        self.f.write(self.json.dumps({"execute": command, "arguments": arguments}) + "\n")
        self.f.flush()
        for line in self.f:
            reply = self.json.loads(line)
            if "return" in reply or "error" in reply:
                return reply
        raise ConnectionError("QMP connection closed")

    def _worker(self):
        while True:
            fn = self.jobs.get()
            try:
                fn()
            except (OSError, ConnectionError, ValueError):
                return

    def do(self, fn):
        self.jobs.put(fn)

    def set(self, path, prop, value):
        self._call("qom-set", path=path, property=prop, value=value)

    def get(self, path, prop):
        return self._call("qom-get", path=path, property=prop).get("return")


# Front-panel inputs as the firmware reads them (docs/ic705/BOARD.md,
# "Front-panel inputs").
SUB_LEVEL = {1: 0x0C0, 2: 0x400, 3: 0x800, 4: 0xC00}           # 12-bit, sub-CPU ADC
SUB_OPEN = 0xFFF
ARM_OPEN = 0x3FF
GRAY = [(0, 0), (1, 0), (1, 1), (0, 1)]
# name: (pin a, pin b, active low, Gray transitions per step)
ENCODERS = {"MULTI": (10, 11, True, 4), "AF/RF-SQL": (8, 9, False, 2),
            "TWIN PBT": (0, 1, False, 2)}
# Front-unit LEDs, each switched by a transistor from one net (service manual,
# front unit block diagram; docs/ic705/IO_MAP.md): POWER <- PWRDS = RZ P10_3,
# CHARGE <- BATTDS = RZ P10_5, TX <- TDS = sub PA12, RX <- RDS = sub PA11.
# Keys are "P<n>" for RZ ports (/machine/gpio odr), letters for sub-CPU ports.
# name: (port, bit)
LEDS = {"POWER": ("P10", 3), "RX": ("A", 11), "TX": ("A", 12), "CHARGING": ("P10", 5)}


class Radio:
    """Drives the emulated front-panel inputs over QMP and polls the LEDs."""

    def __init__(self, qmp):
        self.q = qmp
        self.phase = {name: 0 for name in ENCODERS}
        self.leds = {name: False for name in LEDS}
        self.polling = False

    def poll_leds(self):
        """Queue an LED read unless one is still pending (call it periodically)."""
        if self.polling:
            return
        self.polling = True

        def run():
            try:
                ports = {}
                for path in ("/machine/gpio", "/machine/subcpu"):
                    odr = self.q.get(path, "odr")   # "P0=0000 P1=..." / "A=0800 B=0014 ..."
                    if isinstance(odr, str):
                        ports.update(p.split("=") for p in odr.split())
                for name, (port, bit) in LEDS.items():
                    self.leds[name] = bool(int(ports.get(port, "0"), 16) >> bit & 1)
            finally:
                self.polling = False
        self.q.do(run)

    def sub_key(self, ch, level, down):
        v = SUB_LEVEL[level] if down else SUB_OPEN
        self.q.do(lambda: self.q.set("/machine/subcpu", "adc", f"{ch}={v:#x}"))

    def sub_pin(self, pin, down):
        """Active-low key on a sub-CPU input pin (XFC = XFCK = PC15)."""
        v = f"{pin}=0" if down else f"{pin}=z"
        self.q.do(lambda: self.q.set("/machine/subcpu", "pins", v))

    def arm_key(self, ch, value, down):
        v = value if down else ARM_OPEN
        self.q.do(lambda: self.q.set("/machine/adc", "vals", f"{ch}={v}"))

    def power(self, down):
        self.q.do(lambda: self.q.set("/machine/gpio", "pins", "8_7=0" if down else "8_7=1"))

    def dial(self, counts):
        """Main dial; large moves go in small chunks, like a hand turning it."""
        def run():
            left = counts
            while left:
                n = max(-10, min(10, left))
                self.q.set("/machine/mtu2", "dial", n)
                left -= n
                if left:
                    time.sleep(0.01)
        self.q.do(run)

    def encoder(self, name, steps):
        a, b, low, per = ENCODERS[name]

        def run():
            d = 1 if steps > 0 else -1
            for _ in range(abs(steps) * per):
                self.phase[name] = (self.phase[name] + d) % 4
                x, y = GRAY[self.phase[name]]
                if low:
                    x, y = 1 - x, 1 - y
                self.q.set("/machine/gpio", "pins", f"8_{a}={x},8_{b}={y}")
                time.sleep(0.03)        # sampled from the ARM's ADC tick; faster
                                        # turning triggers the firmware's speed-up
        self.q.do(run)

    def tap(self, press):
        """Short press of a key given as press(down)."""
        press(True)
        self.q.do(lambda: time.sleep(0.15))
        press(False)


# Panel layout in LCD pixels (the LCD is 480x272), after the IC-705's front:
# TWIN PBT, POWER, VOX/CALL and AF/RF-SQL on the left; the LCD with its key row
# below; MULTI and the RIT/XFC/MPAD/SCAN column, the speaker, the TX/RX and
# CHARGING LEDs and the main dial on the right.
PANEL_W, PANEL_H = 1010, 448
LCD_X, LCD_Y = 184, 66


def controls(radio):
    """Keys, knobs and LEDs on the panel.

    key: (name, (x0, y0, x1, y1), press(down) or None, small label above).
    A key without an action (input not identified) would be drawn dimmed.
    """
    def sub(ch, level):
        return lambda down: radio.sub_key(ch, level, down)

    def arm6(value):
        return lambda down: radio.arm_key(6, value, down)
    row_y = (LCD_Y + 290, LCD_Y + 314)
    bottom = ["MENU", "FUNCTION", "M.SCOPE", "QUICK", "EXIT"]
    bottom_act = [sub(1, 1), sub(14, 1), sub(14, 2), sub(14, 3), sub(14, 4)]
    keys = [
        ("POWER", (62, 168, 142, 192), radio.power, ""),
        ("VOX", (24, 228, 82, 252), sub(1, 4), "BK-IN"),
        ("CALL", (96, 228, 154, 252), sub(1, 3), "DR"),
    ]
    for i, (name, act) in enumerate(zip(bottom, bottom_act)):
        x = LCD_X + 6 + i * 80
        keys.append((name, (x, row_y[0], x + 70, row_y[1]), act, ""))
    keys += [
        ("AUTO TUNE", (LCD_X + 412, row_y[0], LCD_X + 474, row_y[1]), sub(15, 2), "RX→CS"),
        ("SPEECH", (LCD_X + 488, row_y[0], LCD_X + 550, row_y[1]), sub(15, 1), "LOCK"),
        ("RIT/ΔTX", (706, 150, 776, 174), sub(0, 3), ""),
        ("XFC", (706, 190, 776, 214), lambda down: radio.sub_pin("C15", down), ""),
        ("MPAD", (706, 230, 776, 254), sub(15, 4), ""),
        ("SCAN", (706, 270, 776, 294), sub(15, 3), ""),
        # microphone keys (ARM ADC ch6 ladder), below the AF/RF-SQL knob
        ("UP", (20, 404, 64, 426), arm6(354), "MIC"),
        ("DN", (70, 404, 114, 426), arm6(553), ""),
        ("V/M", (120, 404, 164, 426), arm6(90), ""),
    ]
    knobs = [
        # name, centre, radius, degrees per step, turn(steps), push(down) or None
        ("TWIN PBT", (90, 96), 40, 15, lambda n: radio.encoder("TWIN PBT", n), sub(0, 1)),
        ("AF/RF/SQL", (90, 334), 44, 15, lambda n: radio.encoder("AF/RF-SQL", n), sub(1, 2)),
        ("MULTI", (741, 88), 36, 15, lambda n: radio.encoder("MULTI", n), sub(0, 2)),
        ("MAIN DIAL", (900, 334), 90, 2, radio.dial, None),
    ]
    leds = [
        # name, (x0, y0, x1, y1), lit colour(s), label
        ("POWER", (36, 175, 50, 185), "#36e04a", ""),
        ("TX/RX", (826, 226, 846, 233), ("#ff3030", "#36e04a"), "TX/RX"),
        ("CHARGING", (950, 226, 970, 233), "#ff9a1a", "CHARGING"),
    ]
    return keys, knobs, leds


# Colours of the loader window (a light desktop style; the radio panel keeps
# its own look).
UI = {
    "window": "#f7f7f8", "field": "#ffffff", "border": "#d0d0d0", "card_border": "#dcdcdc",
    "rule": "#e6e6e6", "hover": "#ececec", "selected": "#eef5fc", "disabled": "#f2f2f2",
    "disabled_border": "#e0e0e0", "text": "#1b1b1b", "head": "#3a3a3a",
    "muted": "#6b6b6b", "faint": "#a8a8a8",
    "accent": "#0067c0", "accent_hover": "#1975c5", "danger": "#c42b1c",
    "danger_hover": "#d13438", "ok": "#2ea043", "busy": "#d29922", "error": "#cf222e",
    "idle": "#a0a0a0", "log_bg": "#1e1e1e", "log_fg": "#d4d4d4",
}


def window(session, scale):
    import math
    import tkinter as tk
    from tkinter import filedialog, ttk

    if sys.platform == "win32":
        # 1:1 means one LCD pixel per screen pixel: without this Windows display
        # scaling would stretch (and blur) the window.
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()                      # the loader; the panel is a window it owns
    emu = tk.Toplevel(root)
    emu.withdraw()
    emu.transient(root)
    emu.configure(bg="#1b1b1b")
    radio = LiveRadio(session)
    keys, knobs, leds = controls(radio)
    state = {"step": step_for(scale), "ox": 0, "oy": 0, "frame": None, "fit": False,
             "active": None, "angle": {k[0]: 0.0 for k in knobs}, "leds": None,
             "status": ""}
    s0 = STEPS[state["step"]][0] / STEPS[state["step"]][1]
    canvas = tk.Canvas(emu, width=int(PANEL_W * s0), height=int(PANEL_H * s0),
                       highlightthickness=0, bg="#1b1b1b")
    canvas.pack(fill="both", expand=True)
    images = {}
    buttons = {"mask": 0}

    def scale_now():
        n, d = STEPS[state["step"]]
        return n / d

    def to_panel(x, y):
        s = scale_now()
        return (x - state["ox"]) / s, (y - state["oy"]) / s

    def build():
        """Draw the radio body and every control at the current zoom."""
        s = scale_now()
        state["ox"] = max(0, (canvas.winfo_width() - PANEL_W * s) / 2)
        state["oy"] = max(0, (canvas.winfo_height() - PANEL_H * s) / 2)
        ox, oy = state["ox"], state["oy"]

        def P(x, y):
            return ox + x * s, oy + y * s

        def font(px, bold=False):
            # negative size = pixels, so labels scale with the panel, not with the
            # system's DPI setting
            return ("Segoe UI", -max(6, int(px * s)), "bold" if bold else "normal")

        def rounded(x0, y0, x1, y1, r, **kw):
            pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1,
                   x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
            xy = []
            for i in range(0, len(pts), 2):
                xy += P(pts[i], pts[i + 1])
            return canvas.create_polygon(xy, smooth=True, **kw)

        lw = max(1, int(1.5 * s))
        canvas.delete("all")
        # body: dark grey shell, a lighter top lip
        rounded(6, 6, PANEL_W - 6, PANEL_H - 6, 26, fill="#1c1d1f", outline="#3a3b3e",
                width=max(1, int(2 * s)))
        rounded(10, 10, PANEL_W - 10, 30, 14, fill="#26272a", outline="")
        canvas.create_text(*P(LCD_X + 258, 46), text="HF/VHF/UHF ALL MODE TRANSCEIVER",
                           fill="#9a9a9a", font=font(9), anchor="e")
        canvas.create_text(*P(LCD_X + 270, 46), text="IC-705", fill="#e6e6e6",
                           font=font(15, True), anchor="w")
        # LCD bezel
        rounded(LCD_X - 12, LCD_Y - 8, LCD_X + 492, LCD_Y + 282, 8, fill="#08090a",
                outline="#2d2e31", width=lw)
        canvas.create_rectangle(*P(LCD_X - 1, LCD_Y - 1), *P(LCD_X + 481, LCD_Y + 273),
                                fill="#000", outline="#000")
        # speaker grille
        for k in range(15):
            y = 44 + k * 10
            rounded(806, y, 986, y + 5, 2, fill="#0b0b0c", outline="")
        # keys
        for name, (x0, y0, x1, y1), action, small in keys:
            live = action is not None
            rounded(x0, y0, x1, y1, 6, fill="#3a3c40" if live else "#2a2b2e",
                    outline="#5c5f64" if live else "#3c3e42", width=lw,
                    tags=("key", "key:" + name, "keybg:" + name))
            canvas.create_text(*P((x0 + x1) / 2, (y0 + y1) / 2 + 1), text=name,
                               fill="#f0f0f0" if live else "#77797d",
                               font=font(10 if len(name) > 7 else 11, True),
                               tags=("key:" + name, "keytx:" + name))
            if small:
                canvas.create_text(*P((x0 + x1) / 2, y0 - 7), text=small, fill="#a8a8a8",
                                   font=font(9))
        # LEDs
        for name, (x0, y0, x1, y1), _, label in leds:
            rounded(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 3, fill="#0d0d0e", outline="")
            canvas.create_rectangle(*P(x0, y0), *P(x1, y1), fill="#26332a", outline="",
                                    tags=("led:" + name,))
            if label:
                canvas.create_text(*P((x0 + x1) / 2, y0 - 9), text=label, fill="#a8a8a8",
                                   font=font(9))
        # knobs
        for name, (cx, cy), r, _, _, push in knobs:
            big = r > 60
            canvas.create_oval(*P(cx - r - 6, cy - r - 6), *P(cx + r + 6, cy + r + 6),
                               fill="#0e0e0f", outline="#2c2d30", width=lw)
            canvas.create_oval(*P(cx - r, cy - r), *P(cx + r, cy + r), fill="#2b2c2f",
                               outline="#6b6d72", width=max(1, int(2 * s)))
            n = 60 if big else 28
            for k in range(n):                                # knurled edge
                a = 2 * math.pi * k / n
                canvas.create_line(*P(cx + (r - (7 if big else 5)) * math.sin(a),
                                      cy - (r - (7 if big else 5)) * math.cos(a)),
                                   *P(cx + r * math.sin(a), cy - r * math.cos(a)),
                                   fill="#4a4c50")
            inner = r - (14 if big else 7)
            canvas.create_oval(*P(cx - inner, cy - inner), *P(cx + inner, cy + inner),
                               fill="#232427", outline="#3c3e42")
            if big:                                           # finger dimple
                canvas.create_oval(*P(cx + r * 0.35 - 13, cy - r * 0.35 - 13),
                                   *P(cx + r * 0.35 + 13, cy - r * 0.35 + 13),
                                   fill="#1a1b1d", outline="#45474b", tags=("dimple",))
            else:
                canvas.create_line(0, 0, 0, 0, fill="#e8e8e8", width=max(2, int(3 * s)),
                                   capstyle="round", tags=("mark:" + name,))
            if not big:                                       # the main dial is unlabelled
                label = "● MULTI  CLR" if name == "MULTI" else name
                canvas.create_text(*P(cx, cy - r - 15), text=label, fill="#c8c8c8",
                                   font=font(11, True))
            turn_mark(name)
        canvas.create_text(*P(PANEL_W - 22, 20), anchor="e", fill="#8a8a8a", font=font(10),
                           text="", tags=("status",))
        canvas.create_image(*P(LCD_X, LCD_Y), anchor="nw", tags=("lcd",))
        state["leds"] = None                                  # repaint LEDs
        state["status"] = None                                # and the status line
        show()

    def turn_mark(name):
        s = scale_now()
        for kname, (cx, cy), r, *_ in knobs:
            if kname == name:
                a = math.radians(state["angle"][name])
                x0 = state["ox"] + (cx + 0.35 * r * math.sin(a)) * s
                y0 = state["oy"] + (cy - 0.35 * r * math.cos(a)) * s
                x1 = state["ox"] + (cx + 0.8 * r * math.sin(a)) * s
                y1 = state["oy"] + (cy - 0.8 * r * math.cos(a)) * s
                if r <= 60:
                    canvas.coords("mark:" + name, x0, y0, x1, y1)
                else:
                    dx, dy = 0.55 * r * math.sin(a + 0.8), -0.55 * r * math.cos(a + 0.8)
                    canvas.coords("dimple", state["ox"] + (cx + dx - 12) * s,
                                  state["oy"] + (cy + dy - 12) * s,
                                  state["ox"] + (cx + dx + 12) * s,
                                  state["oy"] + (cy + dy + 12) * s)

    def show():
        if state["frame"] is None:
            return
        w, h, rgb = state["frame"]
        n, d = STEPS[state["step"]]
        image = tk.PhotoImage(data=b"P6 %d %d 255\n" % (w, h) + rgb, format="PPM")
        if n > 1:
            image = image.zoom(n)
        if d > 1:
            image = image.subsample(d)
        images["current"] = image                               # keep a reference
        canvas.itemconfigure("lcd", image=image)

    def paint_leds():
        lit = dict(radio.leds)
        now = (lit["POWER"], lit["TX"], lit["RX"], lit["CHARGING"])
        if now == state["leds"]:
            return
        state["leds"] = now
        dark = "#26332a"
        for name, _, colour, _ in leds:
            if name == "TX/RX":
                fill = colour[0] if lit["TX"] else colour[1] if lit["RX"] else dark
            else:
                fill = colour if lit[name] else dark
            canvas.itemconfigure("led:" + name, fill=fill)

    def redraw():
        if "drain_log" in state:
            state["drain_log"]()
        rfb = session.rfb
        if rfb is not None and rfb.closed and not session.lock.locked():
            session.status = f"Stopped · {session.name}"
        if rfb is None or rfb.closed:                          # starting or stopped
            show_status(session.status, "#d0a040")
            root.after(100, redraw)
            return
        if rfb.dirty:
            state["frame"] = session.rfb.frame()
            show()
        state["tick"] = state.get("tick", 0) + 1
        if state["tick"] % 16 == 0:                             # LEDs: ~4 reads a second
            radio.poll_leds()
        paint_leds()
        show_status(session.status)
        root.after(15, redraw)

    def show_status(text, colour=None):
        """The loader's status line, its buttons, and the panel window's title."""
        rfb = session.rfb
        if rfb is not None and not rfb.closed:
            dot, running = UI["ok"], True
        elif session.lock.locked():
            dot, running = UI["busy"], True
        elif ":" in text and session.firmware is not None and "stopped" not in text.lower():
            dot, running = UI["error"], False
        else:
            dot, running = UI["idle"], False
        hidden = emu.state() == "withdrawn"
        now = (text, dot, running, hidden)
        if state["status"] == now:
            return
        state["status"] = now
        status_dot.itemconfigure("dot", fill=dot)
        status_text.configure(text=text)
        start_button.configure(text="■  Stop" if running else "▶  Start",
                               style="Danger.TButton" if running else "Accent.TButton")
        for button in (restart_button, shot_button):
            button.state(["!disabled"] if running else ["disabled"])
        if running and hidden:
            panel_button.grid()
        else:
            panel_button.grid_remove()
        place = session.destination if session.remote else "this computer"
        emu.title(f"IC-705 - {session.name} ({place})")

    # ---------------------------------------------------------- loader window
    root.title("IC-705 loader")
    root.configure(bg=UI["window"])
    root.resizable(False, False)
    k = root.winfo_fpixels("1i") / 96                   # Windows display scaling
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", font=("Segoe UI", 10), background=UI["window"],
                    foreground=UI["text"])
    style.configure("TFrame", background=UI["window"])
    style.configure("TLabel", background=UI["window"], foreground=UI["text"])
    style.configure("Head.TLabel", font=("Segoe UI Semibold", 10), foreground=UI["head"])
    style.configure("Field.TLabel", font=("Segoe UI", 9), foreground=UI["muted"])
    style.configure("Hint.TLabel", font=("Segoe UI", 8), foreground=UI["muted"])
    style.configure("TEntry", fieldbackground=UI["field"], bordercolor=UI["border"],
                    lightcolor=UI["field"], darkcolor=UI["field"], padding=(8, 5))
    style.map("TEntry", bordercolor=[("focus", UI["accent"])],
              lightcolor=[("focus", UI["accent"])])
    style.configure("Hint.TEntry", foreground=UI["muted"])
    style.configure("TButton", background=UI["field"], bordercolor=UI["border"],
                    lightcolor=UI["field"], darkcolor=UI["field"], padding=(12, 5),
                    focuscolor=UI["field"])
    style.map("TButton", background=[("disabled", UI["disabled"]), ("active", UI["hover"])],
              foreground=[("disabled", UI["faint"])],
              bordercolor=[("disabled", UI["disabled_border"])])
    style.configure("Big.TButton", padding=(12, 9))
    for name, colour, hover in (("Accent", UI["accent"], UI["accent_hover"]),
                                ("Danger", UI["danger"], UI["danger_hover"])):
        style.configure(f"{name}.TButton", background=colour, foreground="white",
                        bordercolor=colour, lightcolor=colour, darkcolor=colour,
                        focuscolor=colour, padding=(16, 9), font=("Segoe UI Semibold", 11))
        style.map(f"{name}.TButton", background=[("active", hover)])
    style.configure("Link.TButton", background=UI["window"], bordercolor=UI["window"],
                    lightcolor=UI["window"], darkcolor=UI["window"],
                    foreground=UI["accent"], padding=(4, 0), focuscolor=UI["window"])
    style.map("Link.TButton", background=[("active", UI["window"])],
              foreground=[("active", UI["accent_hover"])])
    style.configure("Fold.TButton", background=UI["window"], bordercolor=UI["window"],
                    lightcolor=UI["window"], darkcolor=UI["window"], foreground=UI["head"],
                    padding=(0, 0), focuscolor=UI["window"], font=("Segoe UI Semibold", 10))
    style.map("Fold.TButton", background=[("active", UI["window"])],
              foreground=[("active", UI["accent"])])
    style.configure("TCheckbutton", background=UI["window"], indicatorbackground=UI["field"],
                    indicatorforeground=UI["accent"], focuscolor=UI["window"],
                    indicatorsize=int(13 * k), indicatormargin=(0, 0, int(6 * k), 0))
    style.map("TCheckbutton", background=[("active", UI["window"])])

    cfg = session.config
    fields = {
        "firmware": tk.StringVar(value=cfg.get("firmware", "")),
        "mode": tk.StringVar(value=cfg.get("mode", "local")),
        "host": tk.StringVar(value=cfg.get("host", "")),
        "user": tk.StringVar(value=cfg.get("user", "")),
        "password": tk.StringVar(value=session.password),
        "subcpu": tk.BooleanVar(value=cfg.get("subcpu", False)),
        "log_open": tk.BooleanVar(value=cfg.get("log_open", False)),
    }

    def field(parent, var, hint, show=None, width=10):
        """An entry with a grey hint while it is empty and unfocused."""
        box = ttk.Entry(parent, width=width)
        box.hint_on = False

        def put_hint():
            if not var.get():
                box.hint_on = True
                box.configure(style="Hint.TEntry", show="")
                box.delete(0, "end")
                box.insert(0, hint)

        def take_hint(event=None):
            if box.hint_on:
                box.hint_on = False
                box.delete(0, "end")
                box.configure(style="TEntry", show=show or "")

        def typed(event=None):
            if not box.hint_on:
                var.set(box.get())

        def leave(event=None):
            typed()
            put_hint()

        def reload_from_var(*_):
            if box.hint_on and var.get():
                take_hint()
            if not box.hint_on and box.get() != var.get():
                box.delete(0, "end")
                box.insert(0, var.get())

        if var.get():
            box.insert(0, var.get())
            box.configure(show=show or "")
        else:
            put_hint()
        box.bind("<FocusIn>", take_hint)
        box.bind("<FocusOut>", leave)
        box.bind("<KeyRelease>", typed)
        var.trace_add("write", reload_from_var)
        return box

    body = ttk.Frame(root, padding=(24, 20, 24, 18))
    body.pack(fill="both", expand=True)
    body.columnconfigure(0, weight=1)

    # firmware
    ttk.Label(body, text="Firmware", style="Head.TLabel").grid(row=0, column=0, sticky="w")
    line = ttk.Frame(body)
    line.grid(row=1, column=0, sticky="ew", pady=(4, 0))
    line.columnconfigure(0, weight=1)
    field(line, fields["firmware"], ".bin or .dat file, or a path on the host",
          width=38).grid(row=0, column=0, sticky="ew")

    def browse(event=None):
        path = filedialog.askopenfilename(
            parent=root, title="Firmware",
            filetypes=[("Firmware", "*.bin *.dat"), ("Flash image", "*.bin"),
                       ("Icom package", "*.dat"), ("All files", "*")])
        if path:
            fields["firmware"].set(path)

    ttk.Button(line, text="Browse…", command=browse).grid(row=0, column=1, padx=(6, 0))
    ttk.Checkbutton(body, text="Also run Icom's sub-CPU firmware (with a .bin)",
                    variable=fields["subcpu"]).grid(row=2, column=0, sticky="w", pady=(8, 0))

    # where the emulator runs
    ttk.Label(body, text="Emulator", style="Head.TLabel").grid(row=3, column=0, sticky="w",
                                                              pady=(18, 0))
    cards_row = ttk.Frame(body)
    cards_row.grid(row=4, column=0, sticky="ew", pady=(6, 0))
    cards_row.columnconfigure((0, 1), weight=1, uniform="card")
    cards = {}
    for col, (value, title, detail) in enumerate((
            ("local", "Local", "on this computer (WSL)"),
            ("remote", "Remote", "over ssh, e.g. a Raspberry Pi"))):
        box = tk.Frame(cards_row, highlightthickness=2, cursor="hand2")
        box.grid(row=0, column=col, sticky="ew", padx=(0, 4) if col == 0 else (4, 0))
        t = tk.Label(box, text=title, font=("Segoe UI Semibold", 10), anchor="w")
        d = tk.Label(box, text=detail, font=("Segoe UI", 8), anchor="w")
        t.pack(fill="x", padx=10, pady=(7, 0))
        d.pack(fill="x", padx=10, pady=(0, 8))
        for widget in (box, t, d):
            widget.bind("<Button-1>", lambda e, v=value: fields["mode"].set(v))
        cards[value] = (box, t, d)

    def paint_cards():
        for value, (box, t, d) in cards.items():
            on = fields["mode"].get() == value
            bg = UI["selected"] if on else UI["field"]
            edge = UI["accent"] if on else UI["card_border"]
            box.configure(bg=bg, highlightbackground=edge, highlightcolor=edge)
            t.configure(bg=bg, fg=UI["accent"] if on else UI["text"])
            d.configure(bg=bg, fg=UI["muted"])

    remote_box = ttk.Frame(body)
    remote_box.grid(row=5, column=0, sticky="ew", pady=(10, 0))
    remote_box.columnconfigure(0, weight=2, uniform="host")
    remote_box.columnconfigure(1, weight=1, uniform="host")
    for col, (title, key_, hint) in enumerate((("Host", "host", "raspberrypi.local"),
                                               ("User", "user", "pi"))):
        ttk.Label(remote_box, text=title, style="Field.TLabel").grid(
            row=0, column=col, sticky="w", padx=(0, 8) if col == 0 else 0)
        field(remote_box, fields[key_], hint).grid(
            row=1, column=col, sticky="ew", padx=(0, 8) if col == 0 else 0)
    ttk.Label(remote_box, text="Password", style="Field.TLabel").grid(
        row=2, column=0, sticky="w", pady=(8, 0))
    field(remote_box, fields["password"], "empty: use your ssh key", show="•").grid(
        row=3, column=0, columnspan=2, sticky="ew")

    # state and actions
    tk.Frame(body, height=1, bg=UI["rule"]).grid(row=6, column=0, sticky="ew", pady=(18, 12))
    status_row = ttk.Frame(body)
    status_row.grid(row=7, column=0, sticky="ew")
    status_row.columnconfigure(1, weight=1)
    d = int(10 * k)
    status_dot = tk.Canvas(status_row, width=d, height=d, bg=UI["window"],
                           highlightthickness=0)
    status_dot.create_oval(1, 1, d - 1, d - 1, fill=UI["idle"], outline="", tags=("dot",))
    status_dot.grid(row=0, column=0, padx=(0, 8))
    status_text = ttk.Label(status_row, text="", wraplength=int(260 * k))
    status_text.grid(row=0, column=1, sticky="w")

    def show_panel(event=None):
        """Show the panel window (first time: to the right of the loader)."""
        if not state.get("placed"):
            root.update_idletasks()
            emu.geometry(f"+{root.winfo_rootx() + root.winfo_width() + 16}"
                         f"+{root.winfo_rooty()}")
            state["placed"] = True
        emu.deiconify()
        emu.lift()
        canvas.focus_set()
        state["status"] = None

    def hide_panel():
        """Closing the panel window only hides it; the emulator keeps running."""
        emu.withdraw()
        state["status"] = None

    panel_button = ttk.Button(status_row, text="Show panel", style="Link.TButton",
                              command=show_panel)
    panel_button.grid(row=0, column=2, sticky="e")

    def start_or_stop(event=None):
        if state["status"] and state["status"][2]:              # running: stop
            session.stop_async()
            hide_panel()                                        # nothing left to show
            return "break"
        if not fields["firmware"].get().strip():
            browse()
        if fields["firmware"].get().strip():
            restart()
        return "break"

    def restart(event=None):
        if fields["firmware"].get().strip():
            state["frame"] = None
            canvas.itemconfigure("lcd", image="")
            session.restart()
            mark_log(f"{session.name} started")
            show_panel()
        return "break"

    def save_screenshot(event=None):
        if state["frame"] is None:
            return
        path = filedialog.asksaveasfilename(parent=root, title="Save LCD screenshot",
                                            defaultextension=".png",
                                            filetypes=[("PNG", "*.png")])
        if path:
            write_png(path, *state["frame"])

    actions = ttk.Frame(body)
    actions.grid(row=8, column=0, sticky="ew", pady=(12, 0))
    actions.columnconfigure(0, weight=2, uniform="act")
    actions.columnconfigure((1, 2), weight=1, uniform="act")
    start_button = ttk.Button(actions, text="▶  Start", style="Accent.TButton",
                              command=start_or_stop)
    start_button.grid(row=0, column=0, sticky="ew")
    restart_button = ttk.Button(actions, text="Restart", style="Big.TButton",
                                command=restart)
    restart_button.grid(row=0, column=1, sticky="ew", padx=(8, 0))
    shot_button = ttk.Button(actions, text="Screenshot", style="Big.TButton",
                             command=save_screenshot)
    shot_button.grid(row=0, column=2, sticky="ew", padx=(8, 0))
    ttk.Label(body, text="Settings are saved as you type and used at the next start. "
                         "Changes take effect on Restart.",
              style="Hint.TLabel", wraplength=int(350 * k)).grid(row=9, column=0, sticky="w",
                                                        pady=(10, 0))

    # serial log (SCIF0/SCIF1), folded away by default
    tk.Frame(body, height=1, bg=UI["rule"]).grid(row=10, column=0, sticky="ew", pady=(14, 6))
    log_head = ttk.Frame(body)
    log_head.grid(row=11, column=0, sticky="ew")
    log_head.columnconfigure(1, weight=1)
    log_toggle = ttk.Button(log_head, style="Fold.TButton",
                            command=lambda: fields["log_open"].set(not fields["log_open"].get()))
    log_toggle.grid(row=0, column=0, sticky="w")
    ttk.Label(log_head, text="SCIF0 / SCIF1", style="Hint.TLabel").grid(row=0, column=1,
                                                                       sticky="w", padx=(6, 0))
    log_box = tk.Frame(body, bg=UI["log_bg"])
    log_box.grid(row=12, column=0, sticky="ew", pady=(6, 0))
    log_box.columnconfigure(0, weight=1)
    log_text = tk.Text(log_box, height=14, width=1, wrap="char", bd=0, padx=8, pady=6,
                       bg=UI["log_bg"], fg=UI["log_fg"], insertbackground=UI["log_fg"],
                       selectbackground="#264f78", font=("Consolas", 9), state="disabled")
    log_text.grid(row=0, column=0, sticky="nsew")
    log_text.tag_configure("mark", foreground="#6a9955")
    log_text.tag_configure("byte", foreground="#808080")
    log_scroll = ttk.Scrollbar(log_box, orient="vertical", command=log_text.yview)
    log_scroll.grid(row=0, column=1, sticky="ns")
    log_text.configure(yscrollcommand=log_scroll.set)

    def clear_log():
        log_text.configure(state="normal")
        log_text.delete("1.0", "end")
        log_text.configure(state="disabled")

    log_clear = ttk.Button(log_head, text="Clear", style="Link.TButton", command=clear_log)
    log_clear.grid(row=0, column=2, sticky="e")

    def log_write(text, tag=None):
        at_end = log_text.yview()[1] > 0.999
        log_text.configure(state="normal")
        log_text.insert("end", text, tag)
        lines = int(log_text.index("end-1c").split(".")[0])
        if lines > 5000:                                    # keep the last 5000 lines
            log_text.delete("1.0", f"{lines - 5000}.0")
        log_text.configure(state="disabled")
        if at_end:
            log_text.see("end")

    def drain_log():
        """Show what the serial reader received: text as is, other bytes as <xx>."""
        chunks = []
        try:
            while True:
                chunks.append(session.log.get_nowait())
        except queue.Empty:
            pass
        if not chunks:
            return
        data = b"".join(chunks).replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        text, odd = [], []
        for byte in data:
            if 32 <= byte < 127 or byte in (9, 10):
                text.append(chr(byte))
            else:
                if text:
                    odd.append(("".join(text), None))
                    text = []
                odd.append((f"<{byte:02x}>", "byte"))
        if text:
            odd.append(("".join(text), None))
        for part, tag in odd:
            log_write(part, tag)

    state["drain_log"] = drain_log

    def mark_log(text):
        if log_text.index("end-1c") != "1.0" and log_text.get("end-2c") != "\n":
            log_write("\n")
        log_write(f"── {text} ──\n", "mark")

    state["mark_log"] = mark_log

    def fields_changed(*_):
        if fields["mode"].get() == "remote":
            remote_box.grid()
        else:
            remote_box.grid_remove()
        if fields["log_open"].get():
            log_box.grid()
            log_clear.grid()
            log_toggle.configure(text="▾  Serial log")
        else:
            log_box.grid_remove()
            log_clear.grid_remove()
            log_toggle.configure(text="▸  Serial log")
        paint_cards()
        state["status"] = None
        session.update_config({k: v.get() for k, v in fields.items()})

    for var in fields.values():
        var.trace_add("write", fields_changed)
    fields_changed()
    emu.protocol("WM_DELETE_WINDOW", hide_panel)
    for window_ in (root, emu):
        window_.bind("<F5>", restart)
    if session.lock.locked() or session.rfb is not None:  # started from the saved setup
        root.after(100, show_panel)

    def set_step(i):
        state["step"] = min(max(i, 0), len(STEPS) - 1)
        s = scale_now()
        state["fit"] = False
        emu.geometry(f"{int(PANEL_W * s)}x{int(PANEL_H * s)}")
        root.after(50, build)

    def resized(event):
        if event.widget is not canvas:
            return
        if state["fit"]:
            state["step"] = step_for(min(event.width / PANEL_W, event.height / PANEL_H))
        state["fit"] = True
        build()

    def hit(event):
        x, y = to_panel(event.x, event.y)
        if LCD_X <= x < LCD_X + 480 and LCD_Y <= y < LCD_Y + 272:
            return ("lcd", int(x - LCD_X), int(y - LCD_Y))
        for name, (x0, y0, x1, y1), action, _ in keys:
            if x0 <= x <= x1 and y0 <= y <= y1:
                return ("key", name, action)
        for knob in knobs:
            (cx, cy), r = knob[1], knob[2]
            if (x - cx) ** 2 + (y - cy) ** 2 <= (r + 4) ** 2:
                return ("knob", knob, math.degrees(math.atan2(x - cx, cy - y)))
        return None

    def pointer(x, y, mask):
        if session.rfb and not session.rfb.closed:
            session.rfb.pointer(x, y, mask)

    def lcd_xy(event):
        x, y = to_panel(event.x, event.y)
        return int(x - LCD_X), int(y - LCD_Y)

    def press(event):
        canvas.focus_set()                                      # keys go to the radio again
        h = hit(event)
        state["active"] = h
        if not h:
            return
        if h[0] == "lcd" and session.rfb:
            buttons["mask"] |= 1
            pointer(h[1], h[2], buttons["mask"])
        elif h[0] == "key":
            if h[2] is None:
                canvas.itemconfigure("status", text=f"{h[1]}: its key input is not "
                                     "identified yet", fill="#d0a040")
                state["active"] = None
                return
            h[2](True)
            canvas.itemconfigure("keybg:" + h[1], fill="#6f7277")
            canvas.itemconfigure("keytx:" + h[1], fill="#ffffff")
        else:
            state["drag"] = {"last": h[2], "acc": 0.0, "moved": False}

    def motion(event):
        h = state["active"]
        if not h:
            return
        if h[0] == "lcd":
            pointer(*lcd_xy(event), buttons["mask"])
        elif h[0] == "knob":
            name, (cx, cy), r, per, turn, push = h[1]
            x, y = to_panel(event.x, event.y)
            a = math.degrees(math.atan2(x - cx, cy - y))
            drag = state["drag"]
            delta = (a - drag["last"] + 180) % 360 - 180
            drag["last"] = a
            drag["acc"] += delta
            if abs(drag["acc"]) > 4:
                drag["moved"] = True
            steps = int(drag["acc"] / per)
            if steps:
                drag["acc"] -= steps * per
                state["angle"][name] += steps * per
                turn_mark(name)
                turn(steps)

    def release(event):
        h = state["active"]
        state["active"] = None
        if not h:
            return
        if h[0] == "lcd":
            buttons["mask"] &= ~1
            pointer(*lcd_xy(event), buttons["mask"])
        elif h[0] == "key":
            h[2](False)
            canvas.itemconfigure("keybg:" + h[1], fill="#3a3c40")
            canvas.itemconfigure("keytx:" + h[1], fill="#f0f0f0")
        elif h[0] == "knob" and not state["drag"]["moved"] and h[1][5]:
            radio.tap(h[1][5])                                 # click = push

    def wheel(event):
        if event.state & 0x4:                                   # Ctrl+wheel: zoom
            set_step(state["step"] + (1 if event.delta > 0 else -1))
            return
        d = 1 if event.delta > 0 else -1
        h = hit(event)
        if h and h[0] == "knob":
            name, _, _, per, turn, _ = h[1]
            n = d * (5 if name == "MAIN DIAL" else 1)
            state["angle"][name] += n * per
            turn_mark(name)
            turn(n)
        elif h and h[0] == "lcd":                               # wheel on the LCD: main dial
            bit = 8 if d > 0 else 16
            pointer(h[1], h[2], buttons["mask"] | bit)
            pointer(h[1], h[2], buttons["mask"])

    def key(event, down):
        if event.state & 0x4:                                   # Ctrl+key: window, not radio
            if down and event.keysym.lower() == "o":
                browse()
            elif down and event.keysym.lower() == "s":
                save_screenshot()
            elif down and event.keysym in ("plus", "equal", "KP_Add"):
                set_step(state["step"] + 1)
            elif down and event.keysym in ("minus", "KP_Subtract"):
                set_step(state["step"] - 1)
            elif down and event.keysym in ("0", "KP_0"):
                set_step(STEPS.index((1, 1)))
            return
        if event.keysym == "F5":
            return
        if event.keysym_num and not event.keysym.startswith("Control") and session.rfb:
            session.rfb.key(event.keysym_num, down)

    canvas.bind("<ButtonPress-1>", press)
    canvas.bind("<ButtonRelease-1>", release)
    canvas.bind("<B1-Motion>", motion)
    canvas.bind("<MouseWheel>", wheel)
    canvas.bind("<Configure>", resized)
    emu.bind("<KeyPress>", lambda e: key(e, True))
    emu.bind("<KeyRelease>", lambda e: key(e, False))
    canvas.focus_set()
    redraw()
    root.mainloop()


class LiveRadio:
    """The current session's Radio, looked up at each use (the window outlives sessions)."""

    def __init__(self, session):
        self._session = session

    def __getattr__(self, name):
        if name == "leds":
            radio = self._session.radio
            return radio.leds if radio is not None else {n: False for n in LEDS}

        def call(*args, **kwargs):
            # controls() keeps bound methods such as radio.power, so look the
            # session's Radio up when the key is pressed, not when it was bound
            radio = self._session.radio
            if radio is not None:
                return getattr(radio, name)(*args, **kwargs)
        return call


SETTINGS = Path.home() / ".ic705_remote.json"
ROOT = Path(__file__).resolve().parent


# ------------------------------------------------------------- stored password
def _dpapi(data, protect):
    """Windows DPAPI: encrypt or decrypt @data for the current user account."""
    import ctypes
    from ctypes import wintypes

    class Blob(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    buf = ctypes.create_string_buffer(data, len(data))
    src, dst = Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), Blob()
    crypt32 = ctypes.windll.crypt32
    fn = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
    if not fn(ctypes.byref(src), None, None, None, None, 0, ctypes.byref(dst)):
        raise OSError("DPAPI failed")
    try:
        return ctypes.string_at(dst.pbData, dst.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(dst.pbData)


def seal_password(text):
    """The password as stored in the settings: encrypted for this Windows user
    with DPAPI. Elsewhere it is not stored (use an ssh key there)."""
    import base64
    if not text or sys.platform != "win32":
        return ""
    return base64.b64encode(_dpapi(text.encode(), True)).decode()


def open_password(sealed):
    import base64
    if not sealed or sys.platform != "win32":
        return ""
    try:
        return _dpapi(base64.b64decode(sealed), False).decode()
    except (OSError, ValueError):
        return ""


# ------------------------------------------------------------- remote transport
class ParamikoRun:
    """The ssh session for a password login (Windows' ssh cannot take a password
    from a program): runs @remote on a pty and forwards local ports to the
    host's 127.0.0.1, like `ssh -tt -L`. Looks like a subprocess to Session."""

    def __init__(self, user, host, password, remote, forwards):
        import logging
        import paramiko
        # forwarded connections tried before the emulator listens are expected
        logging.getLogger("paramiko").setLevel(logging.CRITICAL)
        self.client = paramiko.SSHClient()
        self.client.load_system_host_keys()
        self.client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self.client.connect(host, username=user or None, password=password or None, timeout=15)
        self.transport = self.client.get_transport()
        self.listeners = []
        for local_port, remote_port in forwards:
            listener = socket.socket()
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", local_port))
            listener.listen(4)
            self.listeners.append(listener)
            threading.Thread(target=self._accept, args=(listener, remote_port),
                             daemon=True).start()
        self.channel = None
        if remote:
            self.channel = self.transport.open_session()
            self.channel.get_pty()
            self.channel.exec_command(remote)

    def _accept(self, listener, remote_port):
        while True:
            try:
                conn, peer = listener.accept()
            except OSError:
                return
            threading.Thread(target=self._pump, args=(conn, peer, remote_port),
                             daemon=True).start()

    def _pump(self, conn, peer, remote_port):
        import select
        try:
            chan = self.transport.open_channel("direct-tcpip", ("127.0.0.1", remote_port), peer)
        except Exception:
            conn.close()
            return
        try:
            while True:
                ready, _, _ = select.select([conn, chan], [], [], 1)
                if conn in ready:
                    data = conn.recv(65536)
                    if not data:
                        break
                    chan.sendall(data)
                if chan in ready:
                    data = chan.recv(65536)
                    if not data:
                        break
                    conn.sendall(data)
        except OSError:
            pass
        finally:
            chan.close()
            conn.close()

    def run(self, command, data=b""):
        """Run @command with @data on its stdin; return its exit status."""
        stdin, stdout, _ = self.client.exec_command(command)
        stdin.write(data)
        stdin.channel.shutdown_write()
        return stdout.channel.recv_exit_status()

    def poll(self):
        if not self.transport.is_active():
            return 255
        if self.channel is not None and self.channel.exit_status_ready():
            return self.channel.recv_exit_status()
        return None

    def terminate(self):
        for listener in self.listeners:
            listener.close()
        self.client.close()

    kill = terminate

    def wait(self, timeout=None):
        return self.poll()


def local_path(path):
    """@path as ic705.py on this computer sees it: a WSL path on Windows."""
    if sys.platform != "win32":
        return str(path)
    result = subprocess.run(["wsl", "wslpath", "-a", str(path).replace("\\", "/")],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"wslpath failed for {path}")
    return result.stdout.strip()


def upload_script(name):
    return (f"mkdir -p ~/.cache/ic705-upload && cd ~/.cache/ic705-upload && "
            f"if [ -f {name} ]; then cat >/dev/null; "
            f"else cat > .part && mv .part {name}; fi")


def upload(host, path, password="", user=""):
    """Copy a local file to ~/.cache/ic705-upload/ on the host; return its remote
    path. The name carries the content hash, so a re-run skips the copy."""
    data = path.read_bytes()
    name = shlex.quote(f"{hashlib.sha256(data).hexdigest()[:16]}-{path.name}")
    print(f"uploading {path} ({len(data)} bytes) to {host}", flush=True)
    if password:
        link = ParamikoRun(user, host, password, None, [])
        try:
            status = link.run(upload_script(name), data)
        finally:
            link.terminate()
    else:
        status = subprocess.run(["ssh", f"{user}@{host}" if user else host,
                                 upload_script(name)], input=data).returncode
    if status:
        raise RuntimeError(f"upload failed (exit {status})")
    return f"~/.cache/ic705-upload/{name}"


# ------------------------------------------------------------- session
class Session:
    """One run of the emulator and the connections to its LCD (VNC) and control
    (QMP) ports. The configuration (firmware, local or remote, host, user,
    password, sub-CPU) is saved on every change and used at the next start;
    restart() replaces the run in the background."""

    def __init__(self, args):
        import json
        self.args = args
        self.icom_firmware = args.firmware
        self.proc = self.rfb = self.radio = None
        self.lock = threading.Lock()
        self.log = queue.Queue()                # serial output for the loader's log pane
        self.run_id = 0
        try:
            self.config = json.loads(SETTINGS.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError):
            self.config = {}
        self.config.setdefault("mode", "local")
        self.password = open_password(self.config.get("password", ""))
        if args.host:                           # the command line wins, and is kept
            if args.host == "local":
                self.config["mode"] = "local"
            else:
                user, _, host = args.host.rpartition("@")
                self.config.update(mode="remote", host=host, user=user)
        if args.load:
            self.config["firmware"] = str(args.load.resolve())
        if args.subcpu:
            self.config["subcpu"] = True
        if args.host or args.load or args.subcpu:
            self.save()
        self.status = "Not running"

    # configuration
    def save(self):
        import json
        try:
            SETTINGS.write_text(json.dumps(self.config, indent=1), encoding="utf-8")
        except OSError:
            pass

    def update_config(self, values):
        self.password = values.pop("password", self.password)
        self.config.update(values)
        self.config["password"] = seal_password(self.password)
        self.save()

    @property
    def remote(self):
        return self.config.get("mode") == "remote"

    @property
    def destination(self):
        user, host = self.login
        return f"{user}@{host}" if user else host

    @property
    def login(self):
        """(user, host); a user@host typed into the host field wins over the user field."""
        user, host = self.config.get("user", "").strip(), self.config.get("host", "").strip()
        if "@" in host:
            user, _, host = host.rpartition("@")
        return user, host

    @property
    def firmware(self):
        """The firmware to run: a local file (Path), or a path on the emulator host."""
        text = self.config.get("firmware", "").strip().strip('"')
        if not text:
            return None
        path = Path(text).expanduser()
        return path if path.is_file() else text

    @property
    def name(self):
        fw = self.firmware
        return fw.name if isinstance(fw, Path) else (fw or "nothing")

    # running
    def start(self):
        """Start the emulator with the current configuration and connect."""
        a, fw = self.args, self.firmware
        if fw is None:
            raise RuntimeError("no firmware chosen")
        if self.remote and not self.login[1]:
            raise RuntimeError("no remote host")
        if isinstance(fw, Path):
            if self.remote:
                self.status = f"uploading {fw.name} ..."
                firmware = upload(self.login[1], fw, self.password, self.login[0])
            else:
                firmware = local_path(fw.resolve())
        else:
            firmware = fw
        options = ["--vnc", str(a.display)]
        if self.config.get("subcpu") and self.name.lower().endswith(".bin"):
            options += ["--subcpu", self.icom_firmware]   # Icom's sub-CPU beside your firmware
        command = ["python3", "ic705.py", firmware] + options
        self.status = f"starting {self.name} ..."
        if not self.remote:                     # this computer: WSL on Windows
            if sys.platform == "win32":
                command = ["wsl", "--cd", str(ROOT), "--exec"] + command
            self.proc = subprocess.Popen(command, cwd=ROOT)
            lcd_port, control_port = 5900 + a.display, 4400 + a.display
            serial_port = 4500 + a.display
        else:
            remote = f"cd {a.dir} && exec " + " ".join(command)
            forwards = [(a.port, 5900 + a.display), (a.port + 100, 4400 + a.display),
                        (a.port + 200, 4500 + a.display)]
            if self.password:
                self.proc = ParamikoRun(*self.login,
                                        self.password, remote, forwards)
            else:
                self.proc = subprocess.Popen(
                    ["ssh", "-tt", "-o", "ExitOnForwardFailure=yes"] +
                    [arg for lp, rp in forwards for arg in ("-L", f"{lp}:127.0.0.1:{rp}")] +
                    [self.destination, remote])
            lcd_port, control_port, serial_port = a.port, a.port + 100, a.port + 200
        self.run_id += 1
        threading.Thread(target=self.read_serial, args=(serial_port, self.run_id),
                         daemon=True).start()
        rfb = connect(lcd_port, self.proc)
        self.radio = Radio(Qmp(control_port))
        threading.Thread(target=rfb.pump, daemon=True).start()
        self.rfb = rfb
        place = self.destination if self.remote else "this computer"
        self.status = f"Running · {self.name} · {place}"

    def read_serial(self, port, run_id):
        """Copy the emulator's SCIF0/SCIF1 output into self.log for this run.
        QEMU (or the ssh tunnel to it) may not be listening yet, so retry."""
        while run_id == self.run_id:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=1) as link:
                    link.settimeout(0.5)
                    while run_id == self.run_id:
                        try:
                            data = link.recv(4096)
                        except socket.timeout:
                            continue
                        if not data:
                            break
                        self.log.put(data)
            except OSError:
                pass
            time.sleep(0.2)

    def stop(self):
        self.run_id += 1                        # ends this run's serial reader
        rfb, proc = self.rfb, self.proc
        self.rfb = self.radio = self.proc = None
        if rfb is not None:
            rfb.sock.close()
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    def stop_async(self):
        def work():
            with self.lock:
                self.stop()
                self.status = "Stopped"
        threading.Thread(target=work, daemon=True).start()

    def restart(self):
        """Stop the current run and start the configured firmware in the background."""
        def work():
            with self.lock:
                self.stop()
                for attempt in range(3):        # the old QEMU may still hold its ports
                    try:
                        self.start()
                        return
                    except Exception as error:  # ssh, paramiko, sockets: show it, retry
                        self.stop()
                        self.status = f"{self.name}: {error}"
                        time.sleep(2)
        self.status = f"loading {self.name} ..."
        threading.Thread(target=work, daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host",
                        help="emulator host: 'local' (this computer, WSL on Windows) or an "
                             "ssh destination such as user@raspberrypi.local; saved")
    parser.add_argument("--dir", default="~/ic705-emu", help="emulator directory on the host")
    parser.add_argument("--firmware", default="firmware/705E140.dat",
                        help="Icom package on the emulator host, for the sub-CPU option")
    parser.add_argument("--load", type=Path, metavar="FILE",
                        help="local .dat package or .bin flash image to run; saved")
    parser.add_argument("--subcpu", action="store_true",
                        help="with a .bin: also run Icom's sub-CPU firmware from --firmware")
    parser.add_argument("--display", type=int, default=11, help="emulator VNC display number")
    parser.add_argument("--port", type=int, default=5911, help="local forwarded port (remote)")
    parser.add_argument("--scale", type=float, default=1,
                        help="initial zoom (0.5 .. 4; Ctrl+wheel, Ctrl +/-/0 or dragging "
                             "the window edge change it)")
    parser.add_argument("--snapshot", help="no window: save the LCD to this PNG and exit")
    parser.add_argument("--wait", type=float, default=20,
                        help="with --snapshot: seconds to wait after connecting")
    parser.add_argument("--tap", metavar="X,Y",
                        help="with --snapshot: touch this LCD point before the capture")
    parser.add_argument("--press", metavar="KEY",
                        help="with --snapshot: tap this panel key (e.g. MENU) before the capture")
    parser.add_argument("--turn", metavar="KNOB,STEPS",
                        help="with --snapshot: turn a knob (e.g. MULTI,5 or 'MAIN DIAL,100')")
    args = parser.parse_args()

    session = Session(args)
    if not args.snapshot:
        if session.firmware is not None:        # the saved configuration starts at once
            session.restart()
        window(session, args.scale)
        session.stop()
        return 0
    try:
        session.start()
        radio = session.radio
        time.sleep(args.wait)
        if args.tap:
            x, y = (int(v) for v in args.tap.split(","))
            session.rfb.pointer(x, y, 1)
            time.sleep(0.5)
            session.rfb.pointer(x, y, 0)
            time.sleep(3)
        keys, knobs, _ = controls(radio)
        if args.press:
            radio.tap(next(k[2] for k in keys if k[0] == args.press))
            time.sleep(3)
        if args.turn:
            name, steps = args.turn.rsplit(",", 1)
            next(k[4] for k in knobs if k[0] == name)(int(steps))
            time.sleep(4)
        w, h, rgb = session.rfb.frame()
        write_png(args.snapshot, w, h, rgb)
        print(args.snapshot)
    except (RuntimeError, ConnectionError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        session.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
