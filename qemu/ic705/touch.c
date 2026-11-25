/*
 * IC-705 touch panel controller: a TSC2046 / ADS7843-class resistive touch
 * ADC, bit-banged by the firmware on port 3 (touch.c):
 *
 *   P3_10 /CS   out        P3_6  DOUT     in
 *   P3_4  DCLK  out        P3_11 BUSY     in
 *   P3_5  DIN   out        P3_12 /PENIRQ  in, low while the pen is down
 *
 * A conversion: /CS low, 8 command bits on DIN sampled at rising DCLK edges
 * (start bit, channel A2..A0, mode, ...). BUSY goes high after the 8th
 * falling edge and low after the 9th, when the 12-bit result starts on DOUT,
 * MSB first, one bit per falling edge. The firmware reads X with 0xD0
 * (channel 5) and Y with 0x90 (channel 1); other channels and a lifted pen
 * read 0.
 *
 * The pen is set through the machine property "touch": "X,Y" in raw 12-bit
 * panel units, or "" for no touch. See docs/ic705/BOARD.md for the
 * firmware's default calibration.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

static struct {
    bool selected;              /* /CS low */
    bool dclk, din;
    int clocks;                 /* rising DCLK edges since /CS went low */
    uint8_t command;
    uint16_t result;            /* bits not yet shifted out are at 11 and up */
    bool pen;
    uint16_t x, y;
} tsc;

static void drive_dout(void)
{
    ic705_set_pin(3, 6, tsc.result >> 11 & 1);
}

static void dclk_falling(void)
{
    switch (tsc.clocks) {
    case 8: {                   /* command complete: convert */
        int channel = tsc.command >> 4 & 7;

        tsc.result = !tsc.pen ? 0 : channel == 5 ? tsc.x : channel == 1 ? tsc.y : 0;
        ic705_set_pin(3, 11, 1);
        rza1h_debug("touch", "command %02x: %03x", tsc.command, tsc.result);
        break;
    }
    case 9:                     /* result MSB */
        ic705_set_pin(3, 11, 0);
        drive_dout();
        break;
    default:
        if (tsc.clocks > 9) {
            tsc.result <<= 1;
            drive_dout();
        }
        break;
    }
}

/* board.c: the firmware changed one of its outputs on port 3 */
void ic705_touch_pin(int bit, int level)
{
    switch (bit) {
    case 10:                    /* /CS: any edge restarts the exchange */
        tsc.selected = !level;
        tsc.clocks = 0;
        tsc.command = 0;
        ic705_set_pin(3, 11, 0);
        break;
    case 5:
        tsc.din = level;
        break;
    case 4:
        if (level == tsc.dclk) {
            break;
        }
        tsc.dclk = level;
        if (!tsc.selected) {
            break;
        }
        if (level) {
            if (++tsc.clocks <= 8) {
                tsc.command = tsc.command << 1 | tsc.din;
            }
        } else {
            dclk_falling();
        }
        break;
    }
}

void ic705_touch_set(bool down, unsigned x, unsigned y)
{
    tsc.pen = down;
    tsc.x = x & 0xFFF;
    tsc.y = y & 0xFFF;
    ic705_set_pin(3, 12, !down);
    rza1h_debug("touch", down ? "pen at %u,%u" : "pen up", tsc.x, tsc.y);
}

static void touch_prop_set(Object *obj, const char *value, Error **errp)
{
    unsigned x, y;

    if (value && sscanf(value, "%u,%u", &x, &y) == 2) {
        ic705_touch_set(true, x, y);
    } else {
        ic705_touch_set(false, 0, 0);
    }
}

static char *touch_prop_get(Object *obj, Error **errp)
{
    return tsc.pen ? g_strdup_printf("%u,%u", tsc.x, tsc.y) : g_strdup("");
}

void ic705_touch_init(Object *machine)
{
    tsc.selected = false;
    ic705_set_pin(3, 12, 1);                        /* no touch */
    object_property_add_str(machine, "touch", touch_prop_get, touch_prop_set);
}
