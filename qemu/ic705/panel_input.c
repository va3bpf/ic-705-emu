/*
 * IC-705 front panel from the QEMU window (panel_input.c), for
 * `-display gtk` / VNC. The remote panel (ic705_panel.py) drives the same
 * inputs over QMP instead.
 *
 *   mouse on the LCD       touch panel, pressed while the left button is
 *   wheel, Left/Right      main dial (5 counts per wheel notch, 1 per key)
 *   P                      POWER (PWRK), pressed while held
 *   M F S Q X/Esc K A R L U D 1 2 3
 *                          front-panel keys on the sub-CPU's key ladders,
 *                          pressed while held (needs the sub-CPU)
 *
 * Mouse pixels become raw touch values with the firmware's default
 * calibration (blank EEPROM), measured in the emulator: raw 500 / 3500 is
 * X 443 / 52 px and Y 259 / 12 px, both axes reversed.
 */
#include "qemu/osdep.h"
#include "qom/object.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"
#include "subcpu/stm32l0.h"
#include "ic705.h"

#define LCD_W   480
#define LCD_H   272

/* key -> sub-CPU ADC channel and ladder level (docs/ic705/BOARD.md) */
static const struct { unsigned key; uint8_t channel, level; } panel_keys[] = {
    { KEY_M, 1, 1 },  { KEY_F, 14, 1 }, { KEY_S, 14, 2 }, { KEY_Q, 14, 3 },
    { KEY_X, 14, 4 }, { KEY_ESC, 14, 4 }, { KEY_K, 0, 2 }, { KEY_A, 1, 2 },
    { KEY_R, 0, 3 },  { KEY_L, 15, 1 }, { KEY_U, 15, 3 }, { KEY_D, 15, 4 },
    { KEY_1, 0, 1 },  { KEY_2, 1, 3 },  { KEY_3, 1, 4 },
};
static const uint16_t ladder_level[5] = { 0xFFF, 0x0C0, 0x400, 0x800, 0xC00 };

static struct {
    int x, y;                   /* mouse position in LCD pixels */
    bool button;
} mouse;

static int clamp_raw(int v)
{
    return MIN(MAX(v, 0), 4095);
}

static void update_touch(void)
{
    if (mouse.button) {
        ic705_touch_set(true, clamp_raw(500 + (int)((443 - mouse.x) / 0.130333)),
                        clamp_raw(500 + (int)((259 - mouse.y) / 0.082167)));
    } else {
        ic705_touch_set(false, 0, 0);
    }
}

static void turn_dial(int counts)
{
    Object *mtu2 = object_resolve_path("/machine/mtu2", NULL);

    if (mtu2) {
        object_property_set_int(mtu2, "dial", counts, NULL);
    }
}

static void key_event(unsigned key, bool down)
{
    STM32L0State *sub = ic705_subcpu();

    if (key == KEY_LEFT || key == KEY_RIGHT) {
        if (down) {
            turn_dial(key == KEY_RIGHT ? 1 : -1);
        }
        return;
    }
    if (key == KEY_P) {                             /* PWRK, low while pressed */
        ic705_set_pin(8, 7, !down);
        ic705_power_pin(8, 7, !down);
        return;
    }
    for (size_t i = 0; sub && i < ARRAY_SIZE(panel_keys); i++) {
        if (panel_keys[i].key == key) {
            stm32l0_set_adc(sub, panel_keys[i].channel,
                            ladder_level[down ? panel_keys[i].level : 0]);
            return;
        }
    }
}

static void panel_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            mouse.x = qemu_input_scale_axis(evt->abs.value, INPUT_EVENT_ABS_MIN,
                                            INPUT_EVENT_ABS_MAX, 0, LCD_W - 1);
        } else {
            mouse.y = qemu_input_scale_axis(evt->abs.value, INPUT_EVENT_ABS_MIN,
                                            INPUT_EVENT_ABS_MAX, 0, LCD_H - 1);
        }
        if (mouse.button) {
            update_touch();
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            mouse.button = evt->btn.down;
            update_touch();
        } else if (evt->btn.down && (evt->btn.button == INPUT_BUTTON_WHEEL_UP ||
                                     evt->btn.button == INPUT_BUTTON_WHEEL_DOWN)) {
            turn_dial(evt->btn.button == INPUT_BUTTON_WHEEL_UP ? 5 : -5);
        }
        break;
    case INPUT_EVENT_KIND_KEY:
        key_event(evt->key.key, evt->key.down);
        break;
    default:
        break;
    }
}

static const QemuInputHandler panel_handler = {
    .name  = "IC-705 front panel",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_KEY,
    .event = panel_event,
};

void ic705_panel_input_init(DeviceState *dev)
{
    qemu_input_handler_activate(qemu_input_handler_register(dev, &panel_handler));
}
