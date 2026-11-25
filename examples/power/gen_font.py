"""Generate font.h: 8x16 bitmaps of ASCII 32..126 rendered from DejaVu Sans Mono Bold.
Bit 7 of each row byte is the leftmost pixel. Run once; font.h is checked in."""
import sys
from PIL import Image, ImageDraw, ImageFont

ttf = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
font = ImageFont.truetype(ttf, 13)
lines = ["/* 8x16 ASCII 32..126 from DejaVu Sans Mono Bold (Bitstream Vera license), gen_font.py */",
         "static const unsigned char font8x16[95][16] = {"]
for c in range(32, 127):
    img = Image.new("1", (8, 16), 0)
    ImageDraw.Draw(img).text((0, -1), chr(c), font=font, fill=1)
    rows = [sum(img.getpixel((x, y)) << (7 - x) for x in range(8)) for y in range(16)]
    lines.append("    {" + ", ".join(f"0x{r:02x}" for r in rows) + f"}}, /* {chr(c)!r} */")
lines.append("};")
open("font.h", "w").write("\n".join(lines) + "\n")
