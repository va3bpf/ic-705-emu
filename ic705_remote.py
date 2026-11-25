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
"""

import argparse
import hashlib
from pathlib import Path
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


def window(rfb, radio, scale, title):
    import math
    import tkinter as tk

    if sys.platform == "win32":
        # 1:1 means one LCD pixel per screen pixel: without this Windows display
        # scaling would stretch (and blur) the window.
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    root.title(title)
    root.configure(bg="#1b1b1b")
    keys, knobs, leds = controls(radio)
    state = {"step": step_for(scale), "ox": 0, "oy": 0, "frame": None, "fit": False,
             "active": None, "angle": {k[0]: 0.0 for k in knobs}, "leds": None,
             "status": ""}
    s0 = STEPS[state["step"]][0] / STEPS[state["step"]][1]
    canvas = tk.Canvas(root, width=int(PANEL_W * s0), height=int(PANEL_H * s0),
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
                           text="knobs: drag or wheel · click = push · Ctrl+wheel: zoom",
                           tags=("status",))
        canvas.create_image(*P(LCD_X, LCD_Y), anchor="nw", tags=("lcd",))
        state["leds"] = None                                  # repaint LEDs
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
        if rfb.closed:
            root.destroy()
            return
        if rfb.dirty:
            state["frame"] = rfb.frame()
            show()
        state["tick"] = state.get("tick", 0) + 1
        if state["tick"] % 16 == 0:                             # LEDs: ~4 reads a second
            radio.poll_leds()
        paint_leds()
        root.after(15, redraw)

    def set_step(i):
        state["step"] = min(max(i, 0), len(STEPS) - 1)
        s = scale_now()
        state["fit"] = False
        root.geometry(f"{int(PANEL_W * s)}x{int(PANEL_H * s)}")
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

    def lcd_xy(event):
        x, y = to_panel(event.x, event.y)
        return int(x - LCD_X), int(y - LCD_Y)

    def press(event):
        h = hit(event)
        state["active"] = h
        if not h:
            return
        if h[0] == "lcd":
            buttons["mask"] |= 1
            rfb.pointer(h[1], h[2], buttons["mask"])
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
            rfb.pointer(*lcd_xy(event), buttons["mask"])
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
            rfb.pointer(*lcd_xy(event), buttons["mask"])
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
            rfb.pointer(h[1], h[2], buttons["mask"] | bit)
            rfb.pointer(h[1], h[2], buttons["mask"])

    def key(event, down):
        if event.state & 0x4:                                   # Ctrl+key: window, not radio
            if down and event.keysym in ("plus", "equal", "KP_Add"):
                set_step(state["step"] + 1)
            elif down and event.keysym in ("minus", "KP_Subtract"):
                set_step(state["step"] - 1)
            elif down and event.keysym in ("0", "KP_0"):
                set_step(STEPS.index((1, 1)))
            return
        if event.keysym_num and not event.keysym.startswith("Control"):
            rfb.key(event.keysym_num, down)

    canvas.bind("<ButtonPress-1>", press)
    canvas.bind("<ButtonRelease-1>", release)
    canvas.bind("<B1-Motion>", motion)
    canvas.bind("<MouseWheel>", wheel)
    canvas.bind("<Configure>", resized)
    root.bind("<KeyPress>", lambda e: key(e, True))
    root.bind("<KeyRelease>", lambda e: key(e, False))
    canvas.focus_set()
    redraw()
    root.mainloop()


def upload(host, path):
    """Copy a local file to ~/.cache/ic705-upload/ on the host over ssh; return its
    remote path. The name carries the content hash, so a re-run skips the copy."""
    data = path.read_bytes()
    name = shlex.quote(f"{hashlib.sha256(data).hexdigest()[:16]}-{path.name}")
    script = (f"mkdir -p ~/.cache/ic705-upload && cd ~/.cache/ic705-upload && "
              f"if [ -f {name} ]; then cat >/dev/null; "
              f"else cat > .part && mv .part {name}; fi")
    print(f"uploading {path} ({len(data)} bytes) to {host}", flush=True)
    result = subprocess.run(["ssh", host, script], input=data)
    if result.returncode:
        raise RuntimeError(f"upload failed (ssh exit {result.returncode})")
    return f"~/.cache/ic705-upload/{name}"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True,
                        help="ssh destination of the emulator host, e.g. user@raspberrypi.local")
    parser.add_argument("--dir", default="~/ic705-emu", help="emulator directory on the host")
    parser.add_argument("--firmware", default="firmware/705E140.dat",
                        help="firmware package, relative to --dir")
    parser.add_argument("--load", type=Path, metavar="FILE",
                        help="local .dat package or .bin flash image: upload it to the "
                             "host and run it instead of --firmware")
    parser.add_argument("--subcpu", action="store_true",
                        help="with --load FILE.bin: also run Icom's sub-CPU firmware from "
                             "--firmware (panel keys, TX/RX LEDs)")
    parser.add_argument("--display", type=int, default=11, help="remote VNC display number")
    parser.add_argument("--port", type=int, default=5911, help="local forwarded port")
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

    qport = args.port + 100
    firmware = args.firmware
    if args.load:
        try:
            firmware = upload(args.host, args.load)
        except (OSError, RuntimeError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
    remote = (f"cd {args.dir} && exec python3 ic705.py {firmware} "
              f"--vnc {args.display}")
    if args.subcpu and args.load and args.load.suffix.lower() == ".bin":
        remote += f" --subcpu {args.firmware}"   # Icom's sub-CPU beside your main firmware
    ssh = subprocess.Popen([
        "ssh", "-tt", "-o", "ExitOnForwardFailure=yes",
        "-L", f"{args.port}:127.0.0.1:{5900 + args.display}",
        "-L", f"{qport}:127.0.0.1:{4400 + args.display}", args.host, remote,
    ])
    rfb = None
    try:
        rfb = connect(args.port, ssh)
        radio = Radio(Qmp(qport))
        threading.Thread(target=rfb.pump, daemon=True).start()
        if args.snapshot:
            time.sleep(args.wait)
            if args.tap:
                x, y = (int(v) for v in args.tap.split(","))
                rfb.pointer(x, y, 1)
                time.sleep(0.5)
                rfb.pointer(x, y, 0)
                time.sleep(3)
            keys, knobs, _ = controls(radio)
            if args.press:
                radio.tap(next(k[2] for k in keys if k[0] == args.press))
                time.sleep(3)
            if args.turn:
                name, steps = args.turn.rsplit(",", 1)
                next(k[4] for k in knobs if k[0] == name)(int(steps))
                time.sleep(4)
            w, h, rgb = rfb.frame()
            write_png(args.snapshot, w, h, rgb)
            print(args.snapshot)
        else:
            window(rfb, radio, args.scale, f"IC-705 on {args.host}")
    except (RuntimeError, ConnectionError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        if rfb is not None:
            rfb.sock.close()
        if ssh.poll() is None:
            ssh.terminate()
            try:
                ssh.wait(timeout=5)
            except subprocess.TimeoutExpired:
                ssh.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
