/*
 * Sub-CPU reset and clock control (RCC, 0x40021000) and power control (PWR,
 * 0x40007000).
 *
 * RCC registers keep what is written. Oscillators are ready as soon as they
 * are switched on: CR's HSI16RDY, MSIRDY, HSERDY and PLLRDY read as HSI16ON,
 * MSION, HSEON and PLLON, CRRCR's HSI48RDY as HSI48ON, CSR's LSIRDY and
 * LSERDY as LSION and LSEON, and CFGR's SWS as SW.
 *
 * The timers count at a clock derived from the stored CFGR: the system clock
 * by its stored SWS field (MSI from ICSCR.MSIRANGE, 16 MHz HSI16 or HSE, or
 * the PLL from 16 MHz), divided by HPRE, then by PPRE1/PPRE2 with the timer
 * doubling when that divider is above 1.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define RCC_CR      0x00
#define RCC_ICSCR   0x04
#define RCC_CRRCR   0x08
#define RCC_CFGR    0x0C
#define RCC_CSR     0x50

/* Copy each ON bit of @v (at @on) to its READY bit (at @ready). */
static uint32_t ready_follows(uint32_t v, int on, int ready)
{
    return deposit32(v, ready, 1, extract32(v, on, 1));
}

uint32_t sub_rcc_read(STM32L0State *s, hwaddr off)
{
    uint32_t v = off < sizeof(s->rcc) ? s->rcc[off / 4] : 0;

    switch (off) {
    case RCC_CR:
        v = ready_follows(v, 0, 2);             /* HSI16 */
        v = ready_follows(v, 8, 9);             /* MSI */
        v = ready_follows(v, 16, 17);           /* HSE */
        return ready_follows(v, 24, 25);        /* PLL */
    case RCC_CRRCR:
        return ready_follows(v, 0, 1);          /* HSI48 */
    case RCC_CFGR:
        return deposit32(v, 2, 2, extract32(v, 0, 2));     /* SWS = SW */
    case RCC_CSR:
        v = ready_follows(v, 0, 1);             /* LSI */
        return ready_follows(v, 8, 9);          /* LSE */
    default:
        return v;
    }
}

void sub_rcc_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    if (off < sizeof(s->rcc)) {
        s->rcc[off / 4] = v;
    }
}

uint32_t sub_pwr_read(STM32L0State *s, hwaddr off)
{
    return off == 0 ? s->pwr_cr : off == 4 ? s->pwr_csr : 0;
}

void sub_pwr_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    if (off == 0) {
        s->pwr_cr = v;
    }
}

void sub_rcc_reset(STM32L0State *s)
{
    memset(s->rcc, 0, sizeof(s->rcc));
    s->rcc[RCC_CR / 4] = BIT(8) | BIT(9);       /* MSI on and ready */
    s->rcc[RCC_ICSCR / 4] = 5u << 13;           /* MSI range 5, 2.097 MHz */
    s->pwr_cr = 0x1000;
    s->pwr_csr = 0;
}

static uint64_t system_clock_hz(STM32L0State *s)
{
    static const unsigned pll_mul[16] = { 3, 4, 6, 8, 12, 16, 24, 32, 48, 3, 3, 3, 3, 3, 3, 3 };
    uint32_t cfgr = s->rcc[RCC_CFGR / 4];

    switch (extract32(cfgr, 2, 2)) {            /* stored SWS */
    case 0:
        return 65536ULL << extract32(s->rcc[RCC_ICSCR / 4], 13, 3);
    case 1:                                     /* HSI16 */
    case 2:                                     /* HSE; the board's crystal is not known */
        return 16000000;
    default:
        return 16000000ULL * pll_mul[extract32(cfgr, 18, 4)] / (extract32(cfgr, 22, 2) + 1);
    }
}

uint64_t sub_timer_clock_hz(STM32L0State *s, int apb)
{
    static const unsigned ahb_div[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 8, 16, 64, 128, 256, 512 };
    static const unsigned apb_div[8] = { 1, 1, 1, 1, 2, 4, 8, 16 };
    uint32_t cfgr = s->rcc[RCC_CFGR / 4];
    uint64_t hclk = system_clock_hz(s) / ahb_div[extract32(cfgr, 4, 4)];
    unsigned div = apb_div[extract32(cfgr, apb == 1 ? 8 : 11, 3)];

    return div > 1 ? hclk / div * 2 : hclk;
}
