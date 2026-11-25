/*
 * Touch panel: a TSC2046-class touch ADC bit-banged on port 3.
 *   P3_10 /CS, P3_4 DCLK, P3_5 DIN (outputs); P3_6 DOUT, P3_12 /PENIRQ (inputs)
 * A reading: 8 command bits on DIN, clocked in on rising DCLK edges, then the
 * 12-bit result on DOUT, MSB first after the 9th falling edge. 0xD0 reads X,
 * 0x90 reads Y. Raw values become pixels with the firmware's default
 * calibration (docs/ic705/BOARD.md): raw 500/3500 = X 443/52, Y 259/12.
 */
#include "lvgl.h"
#include "hw.h"

#define CS      (1u << 10)
#define DCLK    (1u << 4)
#define DIN     (1u << 5)
#define DOUT    (1u << 6)
#define PENIRQ  (1u << 12)

static void set(uint16_t bits, bool level)
{
    GPIO_P(3) = level ? GPIO_P(3) | bits : GPIO_P(3) & ~bits;
}

static unsigned read_channel(uint8_t command)
{
    unsigned value = 0;

    set(CS, false);
    for (int i = 7; i >= 0; i--) {
        set(DIN, command >> i & 1);
        set(DCLK, true);
        set(DCLK, false);
    }
    for (int i = 0; i < 12; i++) {          /* falling edges 9..20 */
        set(DCLK, true);
        set(DCLK, false);
        value = value << 1 | ((GPIO_PPR(3) & DOUT) != 0);
    }
    set(CS, true);
    return value;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (GPIO_PPR(3) & PENIRQ) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    unsigned x = read_channel(0xD0), y = read_channel(0x90);

    data->point.x = clamp(443 - ((int)x - 500) * 391 / 3000, 0, LCD_W - 1);
    data->point.y = clamp(259 - ((int)y - 500) * 247 / 3000, 0, LCD_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
}

void touch_init(void)
{
    lv_indev_t *indev;

    GPIO_P(3) = (GPIO_P(3) | CS) & ~(DCLK | DIN);   /* idle: deselected, clock low */
    GPIO_PM(3) &= ~(CS | DCLK | DIN);
    GPIO_PIBC(3) |= DOUT | PENIRQ;
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_touch);
}
