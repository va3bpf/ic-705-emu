"""Load a local IC-705 firmware package into the RZ/A1H QEMU machine."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent
STATE = (
    Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state") / "ic705"
    if sys.platform == "linux"
    else ROOT / "state"
)
QEMU = Path.home() / "ic705-qemu/build/qemu-system-arm"
IMAGES = ("flash.bin", "aux2.application.bin", "dsp.image.bin")


def prepare(source):
    package_data = source.read_bytes()
    digest = hashlib.sha256(package_data).hexdigest()
    target = STATE / digest
    if all((target / name).is_file() for name in IMAGES):
        return target
    sys.path.insert(0, str(ROOT / "decoder"))
    from ic705_decode import decode_file

    STATE.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=STATE) as staging:
        decoded = Path(staging) / "decoded"
        package = Path(staging) / source.name
        package.write_bytes(package_data)
        decode_file(package, decoded)
        body = (decoded / "main_body.clear.bin").read_bytes()
        if len(body) != 0x9C0000 or not (decoded / IMAGES[1]).is_file():
            raise ValueError("The package must contain the IC-705 main and sub-CPU images")
        flash = bytearray(b"\xff" * 0x1000000)
        flash[:0x600000] = body[:0x600000]
        flash[0xC00000:0xFC0000] = body[0x600000:]
        (decoded / IMAGES[0]).write_bytes(flash)
        try:
            decoded.rename(target)
        except OSError:
            if not all((target / name).is_file() for name in IMAGES):
                raise
    return target


def qmp(stream, command, **arguments):
    stream.write(json.dumps({"execute": command, "arguments": arguments}) + "\n")
    stream.flush()
    for line in stream:
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(reply["error"]["desc"])
        if "return" in reply:
            return reply["return"]
    raise RuntimeError("QEMU closed the monitor connection")


def run(qemu, images, screenshot, seconds, shift, io_wait, quantum_us, vnc=None):
    # Main CPU and sub-CPU run side by side over the same virtual time, each at
    # its own speed: the Cortex-A9 at 2^shift ns per instruction, the 16 MHz
    # STM32L0 at 32 times that. quantum_us bounds how far one runs ahead of the
    # other (100 us breaks the sub-CPU SPI handshake; 0 = old shared time slices).
    env = dict(
        os.environ,
        QEMU_ICOUNT_IO_WAIT=f"{io_wait},0",
        QEMU_ICOUNT_CPU_SCALE="1,32",
        QEMU_ICOUNT_RR_PARALLEL="1" if quantum_us > 0 else "0",
        QEMU_ICOUNT_RR_QUANTUM_NS=str(int(quantum_us * 1000)),
        RZA1H_BOARD="ic705",
        RZA1H_PINS="8_7=1,8_0=0,8_1=0,8_8=0,8_9=0,8_10=1,8_11=1",
        RZA1H_DSWAKE_BIT="10",
        # ARM ADC AN0..AN7 (10-bit, 3.3 V); AN4..AN6 key ladders released; AN7 = BTVIN,
        # the battery through 1M/560k: 0x391 = 8.2 V, a charged BP-272 (docs/ic705/IO_MAP.md)
        RZA1H_ADC_VALS="0x300,0x300,0x300,0x300,0x3ff,0x3ff,0x3ff,0x391",
        RZA1H_SUBCPU_FW=str(images / IMAGES[1]),
        RZA1H_RSPI4_FLASH=str(images / IMAGES[2]),
        RZA1H_IC705_EEPROM=str(images / "eeprom.bin"),
    )
    with tempfile.TemporaryDirectory(prefix="ic705-") as runtime:
        monitor = str(Path(runtime) / "qmp")
        command = [
            str(qemu), "-M", "rz-a1h", "-kernel", str(images / IMAGES[0]),
            # data files (VNC keymaps) of a QEMU run from its build tree
            "-L", str(Path(qemu).parent.parent / "pc-bios"),
            "-display",
            "none" if screenshot
            else f"vnc=127.0.0.1:{vnc}" if vnc is not None
            else "gtk,zoom-to-fit=on",
            "-serial", "none", "-monitor", "none", "-icount", f"shift={shift}",
            "-qmp", f"unix:{monitor},server=on,wait=off",
        ]
        if vnc is not None and not screenshot:
            # control port for ic705_remote.py's front panel (keys, knobs, dial, POWER)
            command += ["-qmp", f"tcp:127.0.0.1:{4400 + vnc},server=on,wait=off"]
        with subprocess.Popen(command, env=env) as process:
            try:
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10)
                    for _ in range(100):
                        if process.poll() is not None:
                            raise RuntimeError(f"QEMU exited with status {process.returncode}")
                        try:
                            connection.connect(monitor)
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            time.sleep(0.1)
                    else:
                        raise RuntimeError("QEMU monitor did not start")
                    with connection.makefile("rw", encoding="utf-8") as stream:
                        json.loads(stream.readline())
                        qmp(stream, "qmp_capabilities")
                        print("Waiting for the firmware to enter power standby.", flush=True)
                        # A fresh EEPROM reaches deep standby within seconds. An EEPROM saved
                        # while the radio was on makes the firmware power itself up instead;
                        # pressing POWER then would switch it off, so skip the press.
                        deadline = time.monotonic() + 30
                        standby = False
                        while time.monotonic() < deadline:
                            if process.poll() is not None:
                                raise RuntimeError(f"QEMU exited with status {process.returncode}")
                            registers = qmp(
                                stream, "human-monitor-command", **{"command-line": "info registers"}
                            )
                            if re.search(r"\bR15=2007848[cC]\b", registers):
                                standby = True
                                break
                            time.sleep(0.5)
                        if standby:
                            gpio = {"path": "/machine/gpio", "property": "pins"}
                            qmp(stream, "qom-set", **gpio, value="8_7=0")  # POWER = PWRK
                            time.sleep(3)
                            qmp(stream, "qom-set", **gpio, value="8_7=1")
                            print("POWER pressed. Waiting for firmware startup.", flush=True)
                        else:
                            print("Firmware is powering on from the saved EEPROM state; "
                                  "POWER not pressed. Waiting for firmware startup.", flush=True)
                        if screenshot:
                            try:
                                process.wait(timeout=seconds)
                            except subprocess.TimeoutExpired:
                                qmp(stream, "screendump", filename=str(screenshot), format="png")
                                qmp(stream, "quit")
                                print(screenshot)
                            else:
                                raise RuntimeError("QEMU stopped before the screenshot")
                        return process.wait()
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()


def run_image(qemu, image, screenshot, seconds, vnc=None, sub_images=None,
              shift=1, io_wait=128, quantum_us=20):
    """Boot a bare-metal flat image (e.g. examples/hello) at the flash base, on
    the IC-705 board: LCD, PWRK deep-standby wake, touch, port-8 knobs and main
    dial reach the image. With sub_images (a prepared Icom package) Icom's own
    sub-CPU firmware runs beside it, for the panel keys and LEDs; both CPUs then
    share virtual time under -icount, as for the Icom firmware."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("RZA1H_")}
    env.update(RZA1H_BOARD="ic705",
               RZA1H_PINS="8_7=1,8_0=0,8_1=0,8_8=0,8_9=0,8_10=1,8_11=1",
               RZA1H_ADC_VALS="0x3ff,0x3ff,0x3ff,0x3ff,0x3ff,0x3ff,0x3ff,0x391")
    icount = []
    if sub_images is not None:
        env.update(QEMU_ICOUNT_IO_WAIT=f"{io_wait},0",
                   QEMU_ICOUNT_CPU_SCALE="1,32",
                   QEMU_ICOUNT_RR_PARALLEL="1" if quantum_us > 0 else "0",
                   QEMU_ICOUNT_RR_QUANTUM_NS=str(int(quantum_us * 1000)),
                   RZA1H_SUBCPU_FW=str(sub_images / IMAGES[1]))
        icount = ["-icount", f"shift={shift}"]
    with tempfile.TemporaryDirectory(prefix="ic705-") as runtime:
        monitor = str(Path(runtime) / "qmp")
        command = [
            str(qemu), "-M", "rz-a1h", "-kernel", str(image),
            "-L", str(Path(qemu).parent.parent / "pc-bios"),
            "-display",
            "none" if screenshot
            else f"vnc=127.0.0.1:{vnc}" if vnc is not None
            else "gtk,zoom-to-fit=on",
            "-serial", "none", "-monitor", "none", *icount,
            "-qmp", f"unix:{monitor},server=on,wait=off",
        ]
        if vnc is not None and not screenshot:
            command += ["-qmp", f"tcp:127.0.0.1:{4400 + vnc},server=on,wait=off"]
        with subprocess.Popen(command, env=env) as process:
            try:
                if not screenshot:
                    return process.wait()
                time.sleep(1)
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.connect(monitor)
                    with connection.makefile("rw", encoding="utf-8") as stream:
                        json.loads(stream.readline())
                        qmp(stream, "qmp_capabilities")
                        time.sleep(seconds)
                        qmp(stream, "screendump", filename=str(screenshot), format="png")
                        print(screenshot)
                return 0
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path,
                        help="local Icom .dat file, or a bare-metal flash image (.bin) "
                             "booted at 0x18000000 on the IC-705 board")
    parser.add_argument("--subcpu", type=Path, metavar="DAT",
                        help="with a .bin: also run the sub-CPU firmware from this Icom "
                             "package (panel keys, LEDs); skipped with a note if missing")
    parser.add_argument("--prepare", action="store_true", help="only decode and assemble flash")
    parser.add_argument("--qemu", type=Path, default=QEMU, help="path to the patched QEMU binary")
    parser.add_argument("--screenshot", type=Path, help="run headlessly and save a PNG")
    parser.add_argument("--seconds", type=float,
                        help="wait after POWER before capture (default 300; 2 for a .bin)")
    parser.add_argument("--clock-shift", type=int, choices=range(0, 11), default=1,
                        help="QEMU instruction clock shift")
    parser.add_argument("--io-wait", type=int, default=128,
                        help="extra instructions of virtual time per main-CPU device access "
                             "(peripheral bus wait states; 0 = plain QEMU icount)")
    parser.add_argument("--quantum", type=float, default=20,
                        help="microseconds of virtual time one CPU may run ahead of the "
                             "other (50 works, 100 breaks the sub-CPU link; 0 = shared "
                             "time slices, the old and slower mode)")
    parser.add_argument("--vnc", type=int, metavar="N",
                        help="no local window: serve the LCD as VNC display N on 127.0.0.1 "
                             "(port 5900+N) and a QMP control port on 4400+N, for "
                             "ic705_remote.py over an SSH tunnel")
    args = parser.parse_args()
    # SIGTERM and SIGHUP exit through the normal path, so QEMU is stopped
    # instead of being left running
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
    if hasattr(signal, "SIGHUP"):           # the ssh session of ic705_remote.py ended
        signal.signal(signal.SIGHUP, lambda signum, frame: sys.exit(128 + signum))
    if args.seconds is not None and args.seconds <= 0:
        parser.error("--seconds must be positive")
    try:
        if not args.prepare and sys.platform != "linux":
            raise ValueError("Run the emulator under Linux or Windows WSL2.")
        if not args.prepare and not args.qemu.is_file():
            raise ValueError("Patched QEMU is missing. Run bash build-qemu.sh first.")
        if args.firmware.suffix.lower() == ".bin":
            if args.prepare:
                parser.error("--prepare is for Icom .dat packages")
            screenshot = args.screenshot.resolve() if args.screenshot else None
            sub_images = None
            if args.subcpu and args.subcpu.is_file():
                sub_images = prepare(args.subcpu)
            elif args.subcpu:
                print(f"ic705: {args.subcpu} not found; running without the sub-CPU",
                      file=sys.stderr)
            return run_image(args.qemu.resolve(), args.firmware.resolve(), screenshot,
                             args.seconds or 2, args.vnc, sub_images, args.clock_shift,
                             args.io_wait, args.quantum)
        images = prepare(args.firmware)
        if args.prepare:
            print(images)
            return 0
        screenshot = args.screenshot.resolve() if args.screenshot else None
        import fcntl

        with (images / "eeprom.lock").open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError("This firmware profile is already running.") from None
            return run(args.qemu.resolve(), images, screenshot, args.seconds or 300, args.clock_shift,
                       args.io_wait, args.quantum, args.vnc)
    except (OSError, ImportError, ValueError, RuntimeError) as error:
        parser.exit(1, f"ic705: {error}\n")
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
