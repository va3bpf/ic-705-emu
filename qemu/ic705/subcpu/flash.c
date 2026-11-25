/*
 * Sub-CPU flash memory interface (FLASH, 0x40022000).
 *
 *   +0x00 ACR     stored
 *   +0x04 PECR    lock bits PELOCK (0), PRGLOCK (1), OPTLOCK (2) set at reset;
 *                 writes can set them but not clear them
 *   +0x0C PEKEYR  the second key word, 0x02030405, clears PELOCK
 *   +0x10 PRGKEYR the second key word, 0x13141516, clears PRGLOCK
 *   +0x18 SR      READY and ENDHV always read 1; error flags write-1-to-clear
 *   +0x1C OPTR    stored
 *
 * Programming itself is not modelled: the data EEPROM is plain RAM.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define PECR_LOCKS  0x7
#define SR_READY    BIT(3)
#define SR_ENDHV    BIT(2)

uint32_t sub_flash_read(STM32L0State *s, hwaddr off)
{
    switch (off) {
    case 0x00:
        return s->flash_if.acr;
    case 0x04:
        return s->flash_if.pecr;
    case 0x18:
        return s->flash_if.sr | SR_READY | SR_ENDHV;
    case 0x1C:
        return s->flash_if.optr;
    default:
        return 0;
    }
}

void sub_flash_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    switch (off) {
    case 0x00:
        s->flash_if.acr = v;
        break;
    case 0x04:
        s->flash_if.pecr = v | (s->flash_if.pecr & PECR_LOCKS);
        break;
    case 0x0C:
        if (v == 0x02030405) {
            s->flash_if.pecr &= ~BIT(0);
        }
        break;
    case 0x10:
        if (v == 0x13141516) {
            s->flash_if.pecr &= ~BIT(1);
        }
        break;
    case 0x18:
        s->flash_if.sr &= ~(v & 0x3F02);
        break;
    }
}

void sub_flash_reset(STM32L0State *s)
{
    s->flash_if.pecr = PECR_LOCKS;
    s->flash_if.sr = 0;
}
