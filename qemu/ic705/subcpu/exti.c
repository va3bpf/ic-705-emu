/*
 * Sub-CPU external interrupts: SYSCFG (0x40010000) and EXTI (0x40010400).
 *
 * SYSCFG keeps what is written; its EXTICR1..4 (+0x08..+0x14) pick the GPIO
 * port of each EXTI line (A 0, B 1, C 2, D 3, H 5), and CFGR3 (+0x20) shows
 * the reference voltage ready (bits 30 and 26).
 *
 * EXTI: IMR (+0x00), EMR, RTSR, FTSR, SWIER (+0x10, sets pending bits that
 * are unmasked) and PR (+0x14, write 1 to clear). A GPIO edge on a line's
 * selected port, in a direction enabled in RTSR / FTSR, sets the line
 * pending. Pending unmasked lines drive EXTI0_1, EXTI2_3 and EXTI4_15.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define SYSCFG_CFGR3 0x20

static const int exti_port_code[GPIO_PORTS] = { 0, 1, 2, 3, 5 };

uint32_t sub_syscfg_read(STM32L0State *s, hwaddr off)
{
    uint32_t v = off < sizeof(s->syscfg) ? s->syscfg[off / 4] : 0;

    return off == SYSCFG_CFGR3 ? v | BIT(30) | BIT(26) : v;
}

void sub_syscfg_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    if (off < sizeof(s->syscfg)) {
        s->syscfg[off / 4] = v;
    }
    sub_log("SYSCFG+%02x = %08x", (unsigned)off, v);
}

void sub_exti_update(STM32L0State *s)
{
    uint32_t active = s->exti.pr & s->exti.imr;

    sub_nvic(s, NVIC_EXTI0_1, active & 0x0003);
    sub_nvic(s, NVIC_EXTI2_3, active & 0x000C);
    sub_nvic(s, NVIC_EXTI4_15, active & 0xFFF0);
}

/* gpio.c: input level of @port @bit changed */
void sub_exti_edge(STM32L0State *s, int port, int bit, bool rising)
{
    uint32_t selected = extract32(s->syscfg[(0x08 + 4 * (bit / 4)) / 4], 4 * (bit % 4), 4);
    uint32_t enabled = rising ? s->exti.rtsr : s->exti.ftsr;

    if (selected == exti_port_code[port] && (enabled & BIT(bit))) {
        s->exti.pr |= BIT(bit);
        sub_log("EXTI%d %s (P%c%d)", bit, rising ? "rise" : "fall", sub_port_letter[port], bit);
    }
}

uint32_t sub_exti_read(STM32L0State *s, hwaddr off)
{
    switch (off) {
    case 0x00:
        return s->exti.imr;
    case 0x04:
        return s->exti.emr;
    case 0x08:
        return s->exti.rtsr;
    case 0x0C:
        return s->exti.ftsr;
    case 0x14:
        return s->exti.pr;
    default:
        return 0;
    }
}

void sub_exti_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    if (off != 0x14) {
        sub_log("EXTI+%02x = %08x", (unsigned)off, v);
    }
    switch (off) {
    case 0x00:
        s->exti.imr = v;
        break;
    case 0x04:
        s->exti.emr = v;
        break;
    case 0x08:
        s->exti.rtsr = v;
        break;
    case 0x0C:
        s->exti.ftsr = v;
        break;
    case 0x10:
        s->exti.pr |= v & s->exti.imr;
        break;
    case 0x14:
        s->exti.pr &= ~v;
        break;
    }
    sub_exti_update(s);
}

void sub_exti_reset(STM32L0State *s)
{
    memset(s->syscfg, 0, sizeof(s->syscfg));
    s->exti.imr = 0x3F840000;
    s->exti.emr = s->exti.rtsr = s->exti.ftsr = s->exti.pr = 0;
}
