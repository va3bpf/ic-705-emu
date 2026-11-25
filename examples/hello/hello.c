/* Bare-metal "Hello, World!" for the emulated RZ/A1H: draw into an RGB565
 * frame buffer in RAM and point VDC5 graphics plane GR0 at it. */
#include <stdint.h>

#define LCD_W 480
#define LCD_H 272
#define FB    ((volatile uint16_t *)0x20100000)

#define VDC5_GR0 0xFCFF7600u          /* VDC50 base 0xFCFF7400 + 0x200 */
#define REG(off) (*(volatile uint32_t *)(VDC5_GR0 + (off)))

#define RGB(r, g, b) (uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | (b) >> 3)

/* 8x8 glyphs, bit 0 = leftmost pixel (font8x8_basic, public domain) */
static const struct { char c; uint8_t rows[8]; } font[] = {
    {'H', {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}},
    {'e', {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00}},
    {'l', {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}},
    {'o', {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}},
    {'W', {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}},
    {'r', {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}},
    {'d', {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00}},
    {'!', {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}},
};

static const uint8_t *glyph(char c)
{
    for (unsigned i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].rows;
    return 0;                        /* space and anything unknown */
}

static void fill(int x, int y, int w, int h, uint16_t color)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            FB[j * LCD_W + i] = color;
}

static void text(const char *s, int x, int y, int scale, uint16_t color)
{
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = glyph(*s);
        if (!g)
            continue;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] >> col & 1)
                    fill(x + col * scale, y + row * scale, scale, scale, color);
    }
}

int main(void)
{
    static const char msg[] = "Hello, World!";
    const int scale = 4, len = sizeof msg - 1;

    fill(0, 0, LCD_W, LCD_H, RGB(0, 32, 64));
    fill(20, 20, LCD_W - 40, LCD_H - 40, RGB(0, 64, 128));
    text(msg, (LCD_W - len * 8 * scale) / 2, (LCD_H - 8 * scale) / 2, scale, RGB(255, 220, 0));

    REG(0x0c) = 0x20100000;                  /* FLM2: frame buffer base */
    REG(0x10) = (LCD_W * 2) << 16;           /* FLM3: line stride in bytes */
    REG(0x18) = (LCD_H - 1) << 16;           /* FLM5: lines - 1 */
    REG(0x1c) = 0u << 28 | (LCD_W - 1) << 16;/* FLM6: RGB565, width - 1 */
    REG(0x24) = 0;                           /* AB2: vertical start 0 */
    REG(0x28) = 0;                           /* AB3: horizontal start 0 */
    REG(0x20) = 2;                           /* AB1: DISP_SEL = current plane */
    REG(0x04) = 1;                           /* FLM_RD: enable plane read */
    REG(0x00) = 0x111;                       /* UPDATE: apply at next vsync */
    return 0;
}
