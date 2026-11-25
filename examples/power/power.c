/* Bare-metal power on/off demo for the emulated IC-705 main CPU (RZ/A1H), no sub-CPU.
 *
 *   cold start          -> deep standby (LCD dark)
 *   press POWER (PWRK)  -> deep-standby cancel resets the CPU with DSFR bit 10 set:
 *                          power on, show Hello, World! and an uptime counter
 *   hold POWER 1 s      -> "POWER OFF...", LCD dark, back to deep standby
 *
 * PWRK is the front-panel POWER key on P8_7 (pressed = low). Deep standby is what
 * the real firmware uses for "off" too: arm cancel source 10 in DSSSR, set STBCR1
 * STBY|DEEP, WFI.
 *
 * Power on also does what the Icom firmware does with two RZ outputs
 * (docs/ic705/IO_MAP.md): PWRS (P7_15) switches the radio's other supply rails, and
 * PWRDS (P10_3) drives Q81, the front-panel POWER LED. Both go low before deep
 * standby. The CPUs themselves run from H3R3V, which is always on. */
#include <stdint.h>
#include "font.h"

#define REG8(a)  (*(volatile uint8_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))

#define STBCR1   REG8(0xFCFE0020)       /* bits 7:6 = STBY, DEEP */
#define DSSSR    REG16(0xFCFF1802)      /* deep-standby cancel source select */
#define DSFR     REG16(0xFCFF1808)      /* deep-standby cancel factor (after wake) */
#define DSFR_PWRK (1u << 10)

#define PPR8     REG16(0xFCFE3220)      /* port 8 pin levels */
#define PIBC8    REG16(0xFCFE7020)      /* port 8 input buffer enable */
#define PWRK_DOWN() (!(PPR8 & (1u << 7)))

#define OSTM0_CNT REG32(0xFCFEC004)
#define OSTM0_TS  REG8(0xFCFEC014)
#define OSTM0_TT  REG8(0xFCFEC018)
#define OSTM0_CTL REG8(0xFCFEC020)
#define TICKS_PER_SEC 32000000u         /* P0 clock, 32 MHz */

#define P7   REG16(0xFCFE301C)
#define P10  REG16(0xFCFE3028)
#define PM7  REG16(0xFCFE331C)
#define PM10 REG16(0xFCFE3328)
#define PWRS  (1u << 15)                /* P7_15: supply rails on */
#define PWRDS (1u << 3)                 /* P10_3: POWER LED */

#define GR0(off) REG32(0xFCFF7600u + (off))   /* VDC5 ch0 graphics plane 0 */

#define LCD_W 480
#define LCD_H 272
#define FB ((volatile uint16_t *)0x20100000)
#define RGB(r, g, b) (uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | (b) >> 3)

#define BG     RGB(0, 32, 64)
#define PANEL  RGB(0, 64, 128)
#define YELLOW RGB(255, 220, 0)
#define WHITE  RGB(230, 230, 230)
#define GREY   RGB(150, 170, 190)

static void fill(int x, int y, int w, int h, uint16_t color)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            FB[j * LCD_W + i] = color;
}

static int text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n])
        n++;
    return n * 8 * scale;
}

static void text(const char *s, int x, int y, int scale, uint16_t fg, uint16_t bg)
{
    for (; *s; s++, x += 8 * scale) {
        const unsigned char *g = font8x16[(*s >= 32 && *s < 127 ? *s : '?') - 32];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                fill(x + col * scale, y + row * scale, scale, scale,
                     g[row] >> (7 - col) & 1 ? fg : bg);
    }
}

static void centered(const char *s, int y, int scale, uint16_t fg, uint16_t bg)
{
    text(s, (LCD_W - text_width(s, scale)) / 2, y, scale, fg, bg);
}

static void lcd_on(void)
{
    GR0(0x0c) = (uint32_t)FB;                   /* FLM2: frame buffer base */
    GR0(0x10) = (LCD_W * 2) << 16;              /* FLM3: line stride */
    GR0(0x18) = (LCD_H - 1) << 16;              /* FLM5: lines - 1 */
    GR0(0x1c) = 0u << 28 | (LCD_W - 1) << 16;   /* FLM6: RGB565, width - 1 */
    GR0(0x24) = 0;                              /* AB2: vertical start */
    GR0(0x28) = 0;                              /* AB3: horizontal start */
    GR0(0x20) = 2;                              /* AB1: show this plane */
    GR0(0x04) = 1;                              /* FLM_RD: read the frame buffer */
    GR0(0x00) = 0x111;                          /* UPDATE at next vsync */
}

static void lcd_off(void)
{
    GR0(0x04) = 0;
    GR0(0x20) = 0;
    GR0(0x00) = 0x111;
}

static uint32_t ticks(void)
{
    return OSTM0_CNT;
}

static void delay(uint32_t t)
{
    uint32_t t0 = ticks();
    while (ticks() - t0 < t)
        ;
}

static void power_rails(int on)
{
    if (on) {
        P7 |= PWRS;
        P10 |= PWRDS;
        PM7 &= ~PWRS;                   /* outputs */
        PM10 &= ~PWRDS;
    } else {
        P10 &= ~PWRDS;
        P7 &= ~PWRS;
    }
}

static void __attribute__((noreturn)) deep_standby(void)
{
    DSSSR = DSFR_PWRK;              /* only PWRK may wake us */
    DSFR = 0;
    STBCR1 = 0xC0;                  /* STBY | DEEP */
    (void)STBCR1;                   /* make sure the write landed before WFI */
    __asm__ volatile("cpsid if");
    for (;;)
        __asm__ volatile("dsb\n\twfi");
}

static void utoa(uint32_t v, char *out)
{
    char tmp[11];
    int n = 0;
    do
        tmp[n++] = '0' + v % 10;
    while (v /= 10);
    while (n)
        *out++ = tmp[--n];
    *out = 0;
}

static void show_uptime(uint32_t secs)
{
    char buf[32] = "Uptime ", num[11];
    int i = 7;
    utoa(secs, num);
    for (char *p = num; *p; p++)
        buf[i++] = *p;
    buf[i++] = ' ';
    buf[i++] = 's';
    buf[i] = 0;
    fill(20, 170, LCD_W - 40, 32, PANEL);
    centered(buf, 170, 2, WHITE, PANEL);
}

static void __attribute__((noreturn)) power_off(void)
{
    fill(0, 0, LCD_W, LCD_H, BG);
    centered("POWER OFF...", (LCD_H - 48) / 2, 3, WHITE, BG);
    delay(TICKS_PER_SEC);
    while (PWRK_DOWN())             /* let go first, or the next press is lost */
        ;
    lcd_off();
    power_rails(0);                 /* POWER LED off, rails off */
    deep_standby();
}

int main(void)
{
    if (!(DSFR & DSFR_PWRK))        /* cold start, or woken by anything but POWER */
        deep_standby();

    PIBC8 |= 1u << 7;               /* PWRK input buffer */
    OSTM0_TT = 1;
    OSTM0_CTL = 0x02;               /* free-running, no interrupt */
    OSTM0_TS = 1;
    power_rails(1);                 /* rails on, POWER LED on */

    fill(0, 0, LCD_W, LCD_H, BG);
    fill(20, 20, LCD_W - 40, LCD_H - 40, PANEL);
    centered("Hello, World!", 52, 4, YELLOW, PANEL);
    centered("IC-705 bare-metal power demo", 130, 1, GREY, PANEL);
    centered("Hold POWER 1 s to switch off", 220, 1, GREY, PANEL);
    show_uptime(0);
    lcd_on();

    while (PWRK_DOWN())             /* still held from the press that woke us */
        ;

    uint32_t start = ticks(), shown = 0, held_since = 0;
    int held = 0;
    for (;;) {
        uint32_t now = ticks();
        uint32_t secs = (now - start) / TICKS_PER_SEC;
        if (secs != shown)
            show_uptime(shown = secs);
        if (PWRK_DOWN()) {
            if (!held) {
                held = 1;
                held_since = now;
            } else if (now - held_since >= TICKS_PER_SEC) {
                power_off();
            }
        } else {
            held = 0;
        }
    }
}
