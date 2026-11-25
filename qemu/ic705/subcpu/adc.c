/*
 * Sub-CPU 12-bit ADC (0x40012400). On the IC-705 its channels read the
 * front-panel key ladders (0, 1, 14, 15) and sense voltages reported to the
 * main CPU (docs/ic705/BOARD.md).
 *
 *   +0x00 ISR  ADRDY 0, EOSMP 1, EOC 2, EOS 3; write 1 to clear
 *   +0x04 IER  +0x0C CFGR1 (DMAEN 0, SCANDIR 2, RES 4:3, EXTSEL 8:6,
 *   EXTEN 11:10, CONT 13)  +0x10 CFGR2  +0x14 SMPR  +0x28 CHSELR
 *   +0x308 CCR  stored
 *   +0x08 CR   ADEN 0 (sets ADRDY), ADDIS 1, ADSTART 2, ADCAL 31 (done at once)
 *   +0x40 DR   reading clears EOC
 *
 * A conversion takes no time. A sequence converts every channel selected in
 * CHSELR, upwards or (SCANDIR) downwards, each result at the configured
 * resolution into DR, or to the DMA channel (ch1/ch2, request 0) when DMAEN
 * is set. It starts on ADSTART with EXTEN = 0, or on a timer trigger chosen
 * by EXTSEL with EXTEN != 0 (TIM6 TRGO 0, TIM21 CC2 2, TIM2 TRGO 3, TIM22
 * TRGO 5). A single software sequence clears ADSTART when done.
 *
 * Channel levels: the key ladders idle at 0xFFF (no key pressed), channel 4
 * at 0 (any level there tells the main CPU external power is present and
 * keeps it out of deep standby), the rest at mid-scale 0x800.
 * RZA1H_SUB_ADC="v0,v1,..." overrides them at start-up, QOM "adc" or
 * stm32l0_set_adc() at run time.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define ISR_ADRDY   BIT(0)
#define ISR_EOSMP   BIT(1)
#define ISR_EOC     BIT(2)
#define ISR_EOS     BIT(3)
#define CR_ADEN     BIT(0)
#define CR_ADDIS    BIT(1)
#define CR_ADSTART  BIT(2)
#define CR_ADCAL    BIT(31)
#define CFGR1_DMAEN BIT(0)
#define CFGR1_SCANDIR BIT(2)
#define CFGR1_CONT  BIT(13)

static void adc_update_irq(STM32L0State *s)
{
    sub_nvic(s, NVIC_ADC, s->adc.isr & s->adc.ier & 0x9F);
}

static unsigned exten(STM32L0State *s)
{
    return extract32(s->adc.cfgr1, 10, 2);
}

static void convert_sequence(STM32L0State *s)
{
    bool down = s->adc.cfgr1 & CFGR1_SCANDIR;
    unsigned drop = 2 * extract32(s->adc.cfgr1, 3, 2);     /* RES: 12, 10, 8, 6 bits */

    for (int i = 0; i < ADC_CHANNELS; i++) {
        int ch = down ? ADC_CHANNELS - 1 - i : i;

        if (!(s->adc.chselr & BIT(ch))) {
            continue;
        }
        s->adc.dr = s->adc.level[ch] >> drop;
        s->adc.isr |= ISR_EOSMP | ISR_EOC;
        if (s->adc.cfgr1 & CFGR1_DMAEN) {
            int dma = sub_dma_channel(s, 0, 1, 0);

            if (dma >= 0) {
                sub_dma_to_memory(s, dma, s->adc.dr);
                s->adc.isr &= ~ISR_EOC;
            }
        }
    }
    s->adc.isr |= ISR_EOS;
    if (!(s->adc.cfgr1 & CFGR1_CONT) && !exten(s)) {
        s->adc.cr &= ~CR_ADSTART;
    }
    adc_update_irq(s);
}

/* timer.c: timer @timer (2, 6, 21, 22) produced its trigger output */
void sub_adc_trigger(STM32L0State *s, int timer)
{
    static const int extsel_timer[8] = { 6, -1, 21, 2, -1, 22, -1, -1 };

    if ((s->adc.cr & (CR_ADEN | CR_ADSTART)) != (CR_ADEN | CR_ADSTART) || !exten(s)) {
        return;
    }
    if (extsel_timer[extract32(s->adc.cfgr1, 6, 3)] == timer) {
        convert_sequence(s);
    }
}

uint32_t sub_adc_read(STM32L0State *s, hwaddr off)
{
    switch (off) {
    case 0x00:
        return s->adc.isr;
    case 0x04:
        return s->adc.ier;
    case 0x08:
        return s->adc.cr;
    case 0x0C:
        return s->adc.cfgr1;
    case 0x10:
        return s->adc.cfgr2;
    case 0x14:
        return s->adc.smpr;
    case 0x28:
        return s->adc.chselr;
    case 0x40:
        s->adc.isr &= ~ISR_EOC;
        adc_update_irq(s);
        return s->adc.dr;
    case 0x308:
        return s->adc.ccr;
    default:
        return 0;
    }
}

void sub_adc_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    switch (off) {
    case 0x00:
        s->adc.isr &= ~v;
        break;
    case 0x04:
        s->adc.ier = v;
        break;
    case 0x08:
        if (v & CR_ADCAL) {                     /* calibration: done, factor in DR */
            v &= ~CR_ADCAL;
            s->adc.dr = 0x40;
        }
        if (v & CR_ADEN) {
            s->adc.isr |= ISR_ADRDY;
        }
        if (v & CR_ADDIS) {
            v &= ~(CR_ADEN | CR_ADDIS);
        }
        s->adc.cr = v;
        if ((v & CR_ADSTART) && !exten(s)) {
            convert_sequence(s);
        }
        break;
    case 0x0C:
        s->adc.cfgr1 = v;
        break;
    case 0x10:
        s->adc.cfgr2 = v;
        break;
    case 0x14:
        s->adc.smpr = v;
        break;
    case 0x28:
        s->adc.chselr = v;
        break;
    case 0x308:
        s->adc.ccr = v;
        break;
    }
    adc_update_irq(s);
}

void sub_adc_reset(STM32L0State *s)
{
    s->adc.isr = s->adc.ier = s->adc.cr = s->adc.cfgr1 = s->adc.cfgr2 = 0;
    s->adc.smpr = s->adc.chselr = s->adc.dr = s->adc.ccr = 0;
}

void sub_adc_init(STM32L0State *s)
{
    const char *preset = getenv("RZA1H_SUB_ADC");
    g_auto(GStrv) values = g_strsplit(preset ? preset : "", ",", ADC_CHANNELS);

    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        bool ladder = ch == 0 || ch == 1 || ch == 14 || ch == 15;

        s->adc.level[ch] = ladder ? 0xFFF : ch == 4 ? 0 : 0x800;
    }
    for (int ch = 0; preset && *preset && values[ch] && ch < ADC_CHANNELS; ch++) {
        s->adc.level[ch] = strtoul(values[ch], NULL, 0) & 0xFFF;
    }
}

void stm32l0_set_adc(STM32L0State *s, int ch, uint16_t value)
{
    if (ch >= 0 && ch < ADC_CHANNELS) {
        s->adc.level[ch] = value & 0xFFF;
    }
}
