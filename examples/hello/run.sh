#!/bin/sh
# Run hello.bin in the IC-705 emulator's RZ/A1H machine. GTK window by default;
# `run.sh shot.png` runs headless for 2 s and saves the LCD instead.
set -e
cd "$(dirname "$0")"
QEMU=${QEMU:-$HOME/ic705-qemu/build/qemu-system-arm}
if [ -z "$1" ]; then
    exec "$QEMU" -M rz-a1h -kernel hello.bin -display gtk -serial none -monitor none
fi
SOCK=$(mktemp -u)
"$QEMU" -M rz-a1h -kernel hello.bin -display none -serial none -monitor none \
    -qmp unix:$SOCK,server=on,wait=off &
sleep 2
python3 - "$SOCK" "$(realpath -m "$1")" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile("rw")
f.readline()
for cmd in ({"execute": "qmp_capabilities"},
            {"execute": "screendump", "arguments": {"filename": sys.argv[2], "format": "png"}},
            {"execute": "quit"}):
    f.write(json.dumps(cmd) + "\n"); f.flush()
    while "return" not in (r := json.loads(f.readline())) and "error" not in r: pass
    if "error" in r: sys.exit(r["error"]["desc"])
PY
wait
echo "saved $1"
