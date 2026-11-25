/*
 * RZ/A1H multi-function timer pulse unit 2 (MTU2), channels 0..4, as used by
 * the IC-705 firmware (mtu2_timer.c). The IC-7300 machine keeps its own
 * event-level model, qemu/machine/mtu2.c.
 *
 * Each channel has a 16-bit up-counter TCNT clocked by P0phi (32 MHz) through
 * the TCR.TPSC prescaler while its TSTR.CST bit is set; TGRA..TGRD compare
 * registers (two on channels 1 and 2); a status register TSR and an
 * interrupt enable register TIER. Modelled:
 *
 *  - compare match: TCNT reaching TGRx sets TSR.TGFx;
 *  - counter clear on the compare match chosen by TCR.CCLR: TCNT then
 *    counts 0..TGR-1, and reaching TGR (which also matches TGR = 0) clears
 *    it. Above that point it runs on to 0xFFFF and wraps first;
 *  - overflow: passing 0xFFFF sets TSR.TCFV;
 *  - interrupts: a TSR flag with its TIER bit drives that event's GIC line,
 *    level, until software clears the flag by writing 0 to it;
 *  - phase counting (channels 1 and 2, TMDR.MD = 4..7): TCNT stops counting
 *    the clock and follows the encoder. The IC-705 main dial is channel 1 in
 *    phase-counting mode 1; the QOM property "dial" moves it by a signed
 *    number of counts, with TSR.TCFD showing the direction.
 *
 * The counter is not stepped: each channel keeps the count at a base time
 * and works out the count, and which events it passed, from the virtual
 * clock when the firmware looks (a TSR or TCNT read, or any write). A QEMU
 * timer is armed only for the next event whose interrupt is enabled; the
 * firmware runs PWM on channels 3 and 4 with several compare points per
 * period, and a timer per point would cut -icount into tiny slices.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define MTU2_P0_HZ      32000000ULL
#define MTU2_CHANNELS   5
#define MTU2_EVENTS     5               /* A, B, C, D compare match; V overflow */
#define EV_OVERFLOW     4
#define REG_TSTR        0x280
#define WRAP            0x10000u        /* TCNT range */

#define TSR_TCFV        BIT(4)
#define TSR_TCFU        BIT(5)
#define TSR_TCFD        BIT(7)          /* count direction: 1 = up */
#define TSR_FIXED       0xC0            /* bits 7:6 read 1 after reset */

/* Register offsets and interrupt IDs per channel (hardware manual). */
static const struct Mtu2Layout {
    uint16_t tcr, tmdr, tier, tsr, tcnt;
    uint16_t tgr[4];
    int ntgr;
    int cst;                            /* bit in TSTR */
    int gic_tgia, gic_tciv;             /* TGIxA (B, C, D follow) and TCIxV */
} layout[MTU2_CHANNELS] = {
    { 0x300, 0x301, 0x304, 0x305, 0x306, { 0x308, 0x30A, 0x30C, 0x30E }, 4, 0, 139, 143 },
    { 0x380, 0x381, 0x384, 0x385, 0x386, { 0x388, 0x38A },               2, 1, 146, 148 },
    { 0x000, 0x001, 0x004, 0x005, 0x006, { 0x008, 0x00A },               2, 2, 150, 152 },
    { 0x200, 0x202, 0x208, 0x22C, 0x210, { 0x218, 0x21A, 0x224, 0x226 }, 4, 6, 154, 158 },
    { 0x201, 0x203, 0x209, 0x22D, 0x212, { 0x21C, 0x21E, 0x228, 0x22A }, 4, 7, 159, 163 },
};

OBJECT_DECLARE_SIMPLE_TYPE(Mtu2State, RZA1H_MTU2G)

typedef struct Mtu2Channel {
    Mtu2State *mtu;
    int n;
    const struct Mtu2Layout *l;
    uint16_t base_count;                /* TCNT at base_ns */
    int64_t base_ns;
    int64_t event_ns;                   /* when the armed timer fires */
    QEMUTimer *timer;
    qemu_irq irq[MTU2_EVENTS];
} Mtu2Channel;

struct Mtu2State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint8_t reg[RZA1H_MTU2_SIZE];
    Mtu2Channel ch[MTU2_CHANNELS];
};

static uint16_t get16(Mtu2State *s, hwaddr off)
{
    return lduw_le_p(&s->reg[off]);
}

static void set16(Mtu2State *s, hwaddr off, uint16_t v)
{
    stw_le_p(&s->reg[off], v);
}

/* ------------------------------------------------------- channel state */

static bool phase_counting(Mtu2Channel *c)
{
    uint8_t md = c->mtu->reg[c->l->tmdr] & 0xF;

    return (c->n == 1 || c->n == 2) && md >= 4 && md <= 7;
}

/* TCNT counts the clock: started in TSTR and not phase counting. */
static bool counting(Mtu2Channel *c)
{
    return (c->mtu->reg[REG_TSTR] & BIT(c->l->cst)) && !phase_counting(c);
}

static int64_t tick_ns(Mtu2Channel *c)
{
    static const unsigned prescale[8] = { 1, 4, 16, 64, 256, 1024, 1, 1 };
    int64_t ns = NANOSECONDS_PER_SECOND * prescale[c->mtu->reg[c->l->tcr] & 7] / MTU2_P0_HZ;

    return ns ? ns : 1;
}

/* Count at which TCNT clears (TCR.CCLR selects TGRA..D), else WRAP. */
static uint32_t clear_point(Mtu2Channel *c)
{
    static const int cclr_tgr[8] = { -1, 0, 1, -1, -1, 2, 3, -1 };
    int i = cclr_tgr[c->mtu->reg[c->l->tcr] >> 5];
    uint16_t tgr = i >= 0 && i < c->l->ntgr ? get16(c->mtu, c->l->tgr[i]) : 0;

    return tgr ? tgr : WRAP;
}

/* TCNT after @n ticks from @count. */
static uint16_t count_after(uint32_t count, uint64_t n, uint32_t top)
{
    if (count >= top) {                 /* above the clear point: wrap at WRAP first */
        if (n < WRAP - count) {
            return count + n;
        }
        n -= WRAP - count;
        count = 0;
    }
    return (count + n) % top;
}

/* Ticks from @count until TCNT next reaches @value, 0 if it never does.
 * Reaching @top clears the counter, which counts as reaching both @top and 0. */
static uint32_t ticks_until(uint32_t count, uint32_t value, uint32_t top)
{
    if (count < top) {
        if (value > top) {
            return 0;
        }
        if (value == top || value == 0) {
            return top - count;
        }
        return value > count ? value - count : top - count + value;
    }
    if (value > count) {
        return value - count;
    }
    if (value == 0) {
        return WRAP - count;
    }
    return value <= top ? WRAP - count + value : 0;
}

/* Ticks from @count until TCNT passes 0xFFFF, 0 if it never does. */
static uint32_t ticks_until_overflow(uint32_t count, uint32_t top)
{
    return top == WRAP || count >= top ? WRAP - count : 0;
}

static uint16_t count_now(Mtu2Channel *c, int64_t now)
{
    if (!counting(c)) {
        return c->base_count;
    }
    return count_after(c->base_count, (now - c->base_ns) / tick_ns(c), clear_point(c));
}

/* Set the flags of every event passed up to @now and move the base there. */
static void catch_up(Mtu2Channel *c, int64_t now)
{
    Mtu2State *s = c->mtu;
    uint32_t top, d;
    int64_t n;

    if (!counting(c) || (n = (now - c->base_ns) / tick_ns(c)) <= 0) {
        return;
    }
    top = clear_point(c);
    for (int i = 0; i < c->l->ntgr; i++) {
        d = ticks_until(c->base_count, get16(s, c->l->tgr[i]), top);
        if (d && d <= n) {
            s->reg[c->l->tsr] |= BIT(i);
        }
    }
    d = ticks_until_overflow(c->base_count, top);
    if (d && d <= n) {
        s->reg[c->l->tsr] |= TSR_TCFV;
    }
    c->base_count = count_after(c->base_count, n, top);
    c->base_ns += n * tick_ns(c);
}

static void update_irq(Mtu2Channel *c)
{
    uint8_t active = c->mtu->reg[c->l->tsr] & c->mtu->reg[c->l->tier];

    for (int e = 0; e < MTU2_EVENTS; e++) {
        qemu_set_irq(c->irq[e], active >> e & 1);
    }
}

/* Arm the timer for the next event whose interrupt is enabled. */
static void schedule(Mtu2Channel *c)
{
    Mtu2State *s = c->mtu;
    uint8_t tier = s->reg[c->l->tier];
    int64_t now, ticks;
    uint32_t top, count, d, next = 0;

    if (!counting(c) || !(tier & 0x1F)) {
        timer_del(c->timer);
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    top = clear_point(c);
    count = count_now(c, now);
    for (int i = 0; i < c->l->ntgr; i++) {
        if (tier & BIT(i)) {
            d = ticks_until(count, get16(s, c->l->tgr[i]), top);
            next = d && (!next || d < next) ? d : next;
        }
    }
    if (tier & BIT(EV_OVERFLOW)) {
        d = ticks_until_overflow(count, top);
        next = d && (!next || d < next) ? d : next;
    }
    if (!next) {
        timer_del(c->timer);
        return;
    }
    ticks = (now - c->base_ns) / tick_ns(c) + next;
    c->event_ns = c->base_ns + ticks * tick_ns(c);
    timer_mod(c->timer, c->event_ns);
}

static void channel_event(void *opaque)
{
    Mtu2Channel *c = opaque;

    catch_up(c, c->event_ns);
    rza1h_debug("mtu2", "ch%d event, TSR %02x TIER %02x", c->n,
                c->mtu->reg[c->l->tsr], c->mtu->reg[c->l->tier]);
    update_irq(c);
    schedule(c);
}

/* ------------------------------------------------------------- registers */

static Mtu2Channel *tcnt_channel(Mtu2State *s, hwaddr off)
{
    for (int i = 0; i < MTU2_CHANNELS; i++) {
        if (off == s->ch[i].l->tcnt || off == s->ch[i].l->tcnt + 1u) {
            return &s->ch[i];
        }
    }
    return NULL;
}

static Mtu2Channel *tsr_channel(Mtu2State *s, hwaddr off)
{
    for (int i = 0; i < MTU2_CHANNELS; i++) {
        if (off == s->ch[i].l->tsr) {
            return &s->ch[i];
        }
    }
    return NULL;
}

static uint64_t mtu2_read(void *opaque, hwaddr off, unsigned size)
{
    Mtu2State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    Mtu2Channel *c;

    if ((c = tsr_channel(s, off))) {
        catch_up(c, now);
    }
    if ((c = tcnt_channel(s, off))) {
        uint16_t count = count_now(c, now);

        if (size >= 2) {
            return count;
        }
        return off == c->l->tcnt ? count & 0xFF : count >> 8;
    }
    switch (size) {
    case 1:
        return s->reg[off];
    case 2:
        return get16(s, off);
    default:
        return get16(s, off) | (uint32_t)get16(s, off + 2) << 16;
    }
}

static void mtu2_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    Mtu2State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    Mtu2Channel *c;

    /* Settle every channel at the old configuration before changing it. */
    for (int i = 0; i < MTU2_CHANNELS; i++) {
        catch_up(&s->ch[i], now);
        s->ch[i].base_count = count_now(&s->ch[i], now);
        s->ch[i].base_ns = now;
    }
    if ((c = tcnt_channel(s, off))) {
        if (size >= 2) {
            c->base_count = value;
        } else if (off == c->l->tcnt) {
            c->base_count = (c->base_count & 0xFF00) | (value & 0xFF);
        } else {
            c->base_count = (c->base_count & 0x00FF) | (value & 0xFF) << 8;
        }
    } else if ((c = tsr_channel(s, off)) && size == 1) {
        s->reg[off] &= value | TSR_FIXED;       /* writing 0 clears a flag */
    } else if (size == 1) {
        s->reg[off] = value;
    } else if (size == 2) {
        set16(s, off, value);
    } else {
        set16(s, off, value);
        set16(s, off + 2, value >> 16);
    }
    for (int i = 0; i < MTU2_CHANNELS; i++) {
        update_irq(&s->ch[i]);
        schedule(&s->ch[i]);
    }
}

static const MemoryRegionOps mtu2_ops = {
    .read = mtu2_read,
    .write = mtu2_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* --------------------------------------------------------- the main dial */

static void mtu2_set_dial(Object *obj, Visitor *v, const char *name, void *opaque,
                          Error **errp)
{
    Mtu2State *s = RZA1H_MTU2G(obj);
    Mtu2Channel *c = phase_counting(&s->ch[1]) ? &s->ch[1] :
                     phase_counting(&s->ch[2]) ? &s->ch[2] : NULL;
    int64_t counts;
    int step;

    if (!visit_type_int(v, name, &counts, errp)) {
        return;
    }
    if (!c) {
        rza1h_debug("mtu2", "dial %+" PRId64 ": no channel in phase-counting mode", counts);
        return;
    }
    step = counts < 0 ? -1 : 1;
    for (int64_t i = 0; i != counts; i += step) {
        uint16_t from = c->base_count, to = from + step;

        if (step > 0 && from == 0xFFFF) {
            s->reg[c->l->tsr] |= TSR_TCFV;
        } else if (step < 0 && from == 0) {
            s->reg[c->l->tsr] |= TSR_TCFU;
        }
        for (int t = 0; t < c->l->ntgr; t++) {
            if (to == get16(s, c->l->tgr[t])) {
                s->reg[c->l->tsr] |= BIT(t);
            }
        }
        c->base_count = to;
    }
    s->reg[c->l->tsr] = (s->reg[c->l->tsr] & ~TSR_TCFD) | (step > 0 ? TSR_TCFD : 0);
    rza1h_debug("mtu2", "dial %+" PRId64 ": ch%d TCNT %04x", counts, c->n, c->base_count);
    update_irq(c);
}

/* ---------------------------------------------------------------- device */

int rza1h_mtu2_irq_id(int channel, int event)
{
    const struct Mtu2Layout *l = &layout[channel];

    if (event == EV_OVERFLOW) {
        return l->gic_tciv;
    }
    return event < l->ntgr ? l->gic_tgia + event : -1;
}

static void mtu2_reset(DeviceState *dev)
{
    Mtu2State *s = RZA1H_MTU2G(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    memset(s->reg, 0, sizeof(s->reg));
    for (int i = 0; i < MTU2_CHANNELS; i++) {
        Mtu2Channel *c = &s->ch[i];

        for (int t = 0; t < c->l->ntgr; t++) {
            set16(s, c->l->tgr[t], 0xFFFF);
        }
        s->reg[c->l->tsr] = TSR_FIXED;
        c->base_count = 0;
        c->base_ns = now;
        timer_del(c->timer);
    }
}

static void mtu2_init(Object *obj)
{
    Mtu2State *s = RZA1H_MTU2G(obj);

    memory_region_init_io(&s->mmio, obj, &mtu2_ops, s, "rza1h-mtu2", RZA1H_MTU2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    object_property_add(obj, "dial", "int", NULL, mtu2_set_dial, NULL, NULL);
    for (int i = 0; i < MTU2_CHANNELS; i++) {
        Mtu2Channel *c = &s->ch[i];

        c->mtu = s;
        c->n = i;
        c->l = &layout[i];
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, channel_event, c);
        for (int e = 0; e < MTU2_EVENTS; e++) {
            sysbus_init_irq(SYS_BUS_DEVICE(obj), &c->irq[e]);   /* output i * 5 + e */
        }
    }
}

static void mtu2_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), mtu2_reset);
}

static const TypeInfo mtu2_info = {
    .name          = TYPE_RZA1H_MTU2G,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Mtu2State),
    .instance_init = mtu2_init,
    .class_init    = mtu2_class_init,
};

static void mtu2_register_types(void)
{
    type_register_static(&mtu2_info);
}

type_init(mtu2_register_types)
