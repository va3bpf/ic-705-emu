/*
 * Main dial and power.
 *
 * Main dial: MTU2 channel 1 in phase-counting mode 1 counts the encoder;
 * dial_read_delta() returns the counts since the last call.
 *
 * Power (as the Icom firmware does it, docs/ic705/IO_MAP.md): PWRS (P7_15)
 * switches the radio's supply rails, PWRDS (P10_3) the POWER LED. "Off" is
 * the RZ/A1H's deep standby with only PWRK (P8_7, the POWER key) as wake-up
 * source; a press resets the CPU with DSFR bit 10 set.
 */
#include "hw.h"

#define PWRS        (1u << 15)
#define PWRDS       (1u << 3)
#define PWRK        (1u << 7)
#define DSFR_PWRK   (1u << 10)

static uint16_t dial_last;

void dial_init(void)
{
    MTU2_TMDR_1 = 0x04;                     /* phase counting mode 1 */
    MTU2_TSTR |= 1u << 1;                   /* CST1 */
    dial_last = MTU2_TCNT_1;
}

int dial_read_delta(void)
{
    uint16_t now = MTU2_TCNT_1;
    int delta = (int16_t)(now - dial_last);

    dial_last = now;
    return delta;
}

bool power_woken_by_key(void)
{
    return DSFR & DSFR_PWRK;
}

bool power_key_down(void)
{
    return !(GPIO_PPR(8) & PWRK);
}

void power_on(void)
{
    GPIO_PIBC(8) |= PWRK;
    GPIO_P(7) |= PWRS;
    GPIO_P(10) |= PWRDS;
    GPIO_PM(7) &= ~PWRS;
    GPIO_PM(10) &= ~PWRDS;
}

void power_off_now(void)
{
    __asm volatile("cpsid if");
    OSTM_TT(OSTM0) = 1;                     /* no more ticks */
    GICD_CTLR = 0;
    GPIO_P(10) &= ~PWRDS;                   /* POWER LED off */
    GPIO_P(7) &= ~PWRS;                     /* rails off */
    DSSSR = DSFR_PWRK;                      /* only POWER wakes us */
    DSFR = 0;
    STBCR1 = 0xC0;                          /* STBY | DEEP */
    (void)STBCR1;
    for (;;) {
        __asm volatile("dsb\n\twfi");
    }
}
