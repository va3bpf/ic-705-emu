/*
 * Sub-CPU timers TIM2 (0x40000000), TIM6 (0x40001000), TIM21 (0x40010800)
 * and TIM22 (0x40011400), up-counting.
 *
 *   +0x00 CR1 (CEN 0, OPM 3)  +0x04 CR2 (MMS 6:4)  +0x08 SMCR (SMS 2:0)
 *   +0x0C DIER  +0x10 SR (UIF 0, CCxIF 1..4; write 0 to clear)
 *   +0x14 EGR (UG 0: counter to 0)  +0x18/+0x1C CCMR  +0x20 CCER
 *   +0x24 CNT  +0x28 PSC  +0x2C ARR  +0x34..+0x40 CCR1..4
 *
 * CNT counts the APB timer clock / (PSC + 1) from 0 to ARR while CEN is set
 * and no slave mode is selected. At each wrap UIF is set, and every CCxIF
 * whose CCRx lies within 0..ARR (one compare match per period). With MMS =
 * update the wrap is the timer's trigger output, which can start the ADC;
 * in one-pulse mode the wrap clears CEN. Flags with their DIER bit set raise
 * the timer's interrupt.
 *
 * The counter is worked out from the virtual clock. A QEMU timer runs to the
 * next wrap only when the wrap has an effect without anyone reading the
 * timer: an enabled interrupt, the ADC trigger or one-pulse mode. A
 * free-running PWM timer would otherwise expire tens of thousands of times a
 * second and cut -icount into tiny slices; for those, reading SR fills in
 * the flags of a wrap that happened meanwhile.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define CR1_CEN     BIT(0)
#define CR1_OPM     BIT(3)
#define SR_UIF      BIT(0)
#define MMS_UPDATE  2

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int64_t tick_ns(SubTimer *t)
{
    uint64_t hz = MAX(sub_timer_clock_hz(t->soc, t->apb), 1);

    return MAX(1, (int64_t)(NANOSECONDS_PER_SECOND * (t->psc + 1) / hz));
}

static bool counting(SubTimer *t)
{
    return (t->cr1 & CR1_CEN) && !(t->smcr & 7);
}

static uint32_t count_now(SubTimer *t)
{
    if (!counting(t)) {
        return t->base_count;
    }
    return (t->base_count + (now_ns() - t->base_ns) / tick_ns(t)) % (t->arr + 1);
}

static void rebase(SubTimer *t)
{
    t->base_count = count_now(t);
    t->base_ns = now_ns();
}

static bool wrap_matters(SubTimer *t)
{
    return (t->dier & 0x1F) || extract32(t->cr2, 4, 3) == MMS_UPDATE || (t->cr1 & CR1_OPM);
}

static bool wrapping(SubTimer *t)
{
    return counting(t) && t->arr != 0;
}

static void set_wrap_flags(SubTimer *t)
{
    t->sr |= SR_UIF;
    for (int i = 0; i < 4; i++) {
        if (t->ccr[i] <= t->arr) {
            t->sr |= BIT(i + 1);
        }
    }
}

static void timer_update_irq(SubTimer *t)
{
    sub_nvic(t->soc, t->nvic, t->sr & t->dier & 0x1F);
}

static void schedule(SubTimer *t)
{
    if (!wrapping(t) || !wrap_matters(t)) {
        timer_del(t->update);
        if (wrapping(t)) {
            rebase(t);
        }
        return;
    }
    rebase(t);
    timer_mod(t->update, t->base_ns + (int64_t)(t->arr + 1 - t->base_count) * tick_ns(t));
}

/* For a timer without a QEMU timer: flags of a wrap since the base. */
static void catch_up(SubTimer *t)
{
    int64_t ticks;

    if (!wrapping(t) || wrap_matters(t)) {
        return;
    }
    ticks = (now_ns() - t->base_ns) / tick_ns(t);
    if ((int64_t)t->base_count + ticks > (int64_t)t->arr) {
        set_wrap_flags(t);
        rebase(t);
    }
}

static void wrapped(void *opaque)
{
    SubTimer *t = opaque;

    t->base_count = 0;
    t->base_ns = now_ns();
    set_wrap_flags(t);
    timer_update_irq(t);
    if (extract32(t->cr2, 4, 3) == MMS_UPDATE) {
        sub_adc_trigger(t->soc, t->number);
    }
    if (t->cr1 & CR1_OPM) {
        t->cr1 &= ~CR1_CEN;
    }
    schedule(t);
}

uint32_t sub_timer_read(SubTimer *t, hwaddr off)
{
    switch (off) {
    case 0x00:
        return t->cr1;
    case 0x04:
        return t->cr2;
    case 0x08:
        return t->smcr;
    case 0x0C:
        return t->dier;
    case 0x10:
        catch_up(t);
        return t->sr;
    case 0x18:
        return t->ccmr1;
    case 0x1C:
        return t->ccmr2;
    case 0x20:
        return t->ccer;
    case 0x24:
        return count_now(t);
    case 0x28:
        return t->psc;
    case 0x2C:
        return t->arr;
    case 0x34: case 0x38: case 0x3C: case 0x40:
        return t->ccr[(off - 0x34) / 4];
    default:
        return 0;
    }
}

void sub_timer_write(SubTimer *t, hwaddr off, uint32_t v)
{
    catch_up(t);
    rebase(t);
    switch (off) {
    case 0x00:
        if ((v & CR1_CEN) && !(t->cr1 & CR1_CEN)) {
            sub_log("%s start psc=%u arr=%u", t->name, t->psc, t->arr);
        }
        t->cr1 = v & 0x3FF;
        break;
    case 0x04:
        t->cr2 = v;
        break;
    case 0x08:
        t->smcr = v;
        break;
    case 0x0C:
        t->dier = v;
        break;
    case 0x10:
        t->sr &= v;
        break;
    case 0x14:
        if (v & 1) {
            t->base_count = 0;
        }
        break;
    case 0x18:
        t->ccmr1 = v;
        break;
    case 0x1C:
        t->ccmr2 = v;
        break;
    case 0x20:
        t->ccer = v;
        break;
    case 0x24:
        t->base_count = v & 0xFFFF;
        break;
    case 0x28:
        t->psc = v & 0xFFFF;
        break;
    case 0x2C:
        t->arr = v & 0xFFFF;
        break;
    case 0x34: case 0x38: case 0x3C: case 0x40:
        t->ccr[(off - 0x34) / 4] = v & 0xFFFF;
        break;
    }
    timer_update_irq(t);
    schedule(t);
}

void sub_timer_reset(SubTimer *t)
{
    t->cr1 = t->cr2 = t->smcr = t->dier = t->sr = t->ccmr1 = t->ccmr2 = t->ccer = 0;
    t->psc = 0;
    t->arr = 0xFFFF;
    memset(t->ccr, 0, sizeof(t->ccr));
    t->base_count = 0;
    timer_del(t->update);
}

void sub_timer_init(STM32L0State *s)
{
    static const struct { const char *name; int number, nvic, apb; } info[TIMERS] = {
        { "TIM2", 2, NVIC_TIM2, 1 },   { "TIM6", 6, NVIC_TIM6, 1 },
        { "TIM21", 21, NVIC_TIM21, 2 }, { "TIM22", 22, NVIC_TIM22, 2 },
    };

    for (int i = 0; i < TIMERS; i++) {
        SubTimer *t = &s->tim[i];

        t->soc = s;
        t->name = info[i].name;
        t->number = info[i].number;
        t->nvic = info[i].nvic;
        t->apb = info[i].apb;
        t->update = timer_new_ns(QEMU_CLOCK_VIRTUAL, wrapped, t);
    }
}
