/*
 * Sub-CPU GPIO ports A, B, C, D and H (0x50000000 + 0x400 n, H at 0x50001C00).
 *
 *   +0x00 MODER  +0x04 OTYPER  +0x08 OSPEEDR  +0x0C PUPDR  +0x20/+0x24 AFR
 *   +0x10 IDR    computed      +0x14 ODR      +0x18 BSRR   +0x28 BRR
 *
 * A pin reads its output latch in output mode. Otherwise it reads the level
 * the board drives on it, or, if the board leaves it alone, 1 with a pull-up
 * and 0 without. Every change of a pin's input level is an edge for EXTI.
 * Output latch changes go out on the "p?-out" lines.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "subcpu.h"

#define MODE_OUTPUT 1
#define PULL_UP     1

static uint32_t input_levels(SubGpio *g)
{
    uint32_t idr = 0;

    for (int bit = 0; bit < 16; bit++) {
        bool level;

        if (extract32(g->moder, 2 * bit, 2) == MODE_OUTPUT) {
            level = extract32(g->odr, bit, 1);
        } else if (g->driven & BIT(bit)) {
            level = extract32(g->level, bit, 1);
        } else {
            level = extract32(g->pupdr, 2 * bit, 2) == PULL_UP;
        }
        idr |= (uint32_t)level << bit;
    }
    return idr;
}

/* Report every pin whose level changed since the last check to EXTI. */
static void check_edges(STM32L0State *s, int port)
{
    SubGpio *g = &s->gpio[port];
    uint32_t idr = input_levels(g);

    for (uint32_t changed = idr ^ g->idr_seen; changed; changed &= changed - 1) {
        int bit = ctz32(changed);

        sub_exti_edge(s, port, bit, idr & BIT(bit));
    }
    g->idr_seen = idr;
    sub_exti_update(s);
}

static void set_odr(STM32L0State *s, int port, uint32_t odr)
{
    SubGpio *g = &s->gpio[port];
    uint32_t changed = (g->odr ^ odr) & 0xFFFF;

    g->odr = odr & 0xFFFF;
    for (; changed; changed &= changed - 1) {
        int bit = ctz32(changed);

        qemu_set_irq(g->out[bit], extract32(odr, bit, 1));
        sub_log("P%c%d out %d", sub_port_letter[port], bit, extract32(odr, bit, 1));
    }
    check_edges(s, port);
}

/* The board drives @port @bit to @level, or releases it (@level < 0). */
void sub_gpio_drive(STM32L0State *s, int port, int bit, int level)
{
    SubGpio *g = &s->gpio[port];

    if (level < 0) {
        g->driven &= ~BIT(bit);
    } else {
        g->driven |= BIT(bit);
        g->level = deposit32(g->level, bit, 1, level != 0);
    }
    check_edges(s, port);
}

int sub_gpio_port(char letter)
{
    for (int port = 0; port < GPIO_PORTS; port++) {
        if (sub_port_letter[port] == g_ascii_toupper(letter)) {
            return port;
        }
    }
    return -1;
}

uint32_t sub_gpio_read(STM32L0State *s, int port, hwaddr off)
{
    SubGpio *g = &s->gpio[port];

    switch (off) {
    case 0x00:
        return g->moder;
    case 0x04:
        return g->otyper;
    case 0x08:
        return g->ospeedr;
    case 0x0C:
        return g->pupdr;
    case 0x10:
        return input_levels(g);
    case 0x14:
        return g->odr;
    case 0x20:
    case 0x24:
        return g->afr[(off - 0x20) / 4];
    default:
        return 0;
    }
}

void sub_gpio_write(STM32L0State *s, int port, hwaddr off, uint32_t v)
{
    SubGpio *g = &s->gpio[port];

    switch (off) {
    case 0x00:
        g->moder = v;
        check_edges(s, port);
        break;
    case 0x04:
        g->otyper = v;
        break;
    case 0x08:
        g->ospeedr = v;
        break;
    case 0x0C:
        g->pupdr = v;
        check_edges(s, port);
        break;
    case 0x14:
        set_odr(s, port, v);
        break;
    case 0x18:                                  /* BSRR: set low half, reset high half */
        set_odr(s, port, (g->odr | (v & 0xFFFF)) & ~(v >> 16));
        break;
    case 0x20:
    case 0x24:
        g->afr[(off - 0x20) / 4] = v;
        break;
    case 0x28:                                  /* BRR */
        set_odr(s, port, g->odr & ~(v & 0xFFFF));
        break;
    }
}

void sub_gpio_reset(STM32L0State *s)
{
    for (int port = 0; port < GPIO_PORTS; port++) {
        SubGpio *g = &s->gpio[port];

        g->moder = port == 0 ? 0xEBFFFCFF : 0xFFFFFFFF;    /* PA13/PA14 SWD */
        g->pupdr = port == 0 ? 0x24000000 : 0;
        g->otyper = g->ospeedr = g->odr = 0;
        g->afr[0] = g->afr[1] = 0;
        g->idr_seen = input_levels(g);
    }
}

/* ------------------------------------------------- board-facing interface */

void stm32l0_set_pin(STM32L0State *s, char letter, int bit, int level)
{
    int port = sub_gpio_port(letter);

    if (port >= 0) {
        sub_gpio_drive(s, port, bit, level != 0);
    }
}

uint32_t stm32l0_get_odr(STM32L0State *s, char letter)
{
    int port = sub_gpio_port(letter);

    return port >= 0 ? s->gpio[port].odr : 0;
}
