/*
 * IC-705 battery charger: TI BQ25713 on RIIC1, 7-bit address 0x6B (charger.c).
 *
 * The chip has 16-bit registers at even command codes 0x00..0x3E, sent low
 * byte first; a transfer starts with the command code and continues to the
 * following bytes, so a word write is <cmd> <lo> <hi>. ManufacturerID (0x2E)
 * and DeviceID (0x2F) are read only and identify a BQ25713 (0x40, 0x88).
 *
 * The IC-705 firmware writes its charge settings at power on and reads some
 * of them back (docs/ic705/BOARD.md). Only the register file is modelled:
 * there is no battery, no ADC and no charging.
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

OBJECT_DECLARE_SIMPLE_TYPE(BQ25713State, BQ25713)

#define BQ25713_NREGS        0x40     /* command codes 0x00..0x3F */
#define BQ25713_MANUFACTURER 0x2E
#define BQ25713_DEVICE       0x2F

struct BQ25713State {
    I2CSlave parent_obj;
    uint8_t reg[BQ25713_NREGS];
    uint8_t cmd;            /* register the next data byte goes to or comes from */
    bool have_cmd;          /* the first byte of a write is the command code */
};

static bool bq25713_read_only(uint8_t cmd)
{
    return cmd == BQ25713_MANUFACTURER || cmd == BQ25713_DEVICE;
}

static int bq25713_event(I2CSlave *i2c, enum i2c_event event)
{
    BQ25713State *s = BQ25713(i2c);

    if (event == I2C_START_SEND) {
        s->have_cmd = false;
    }
    return 0;
}

static int bq25713_send(I2CSlave *i2c, uint8_t byte)
{
    BQ25713State *s = BQ25713(i2c);

    if (!s->have_cmd) {
        s->cmd = byte;
        s->have_cmd = true;
        return 0;
    }
    rza1h_debug("charger", "write %02x <- %02x", s->cmd, byte);
    if (s->cmd < BQ25713_NREGS && !bq25713_read_only(s->cmd)) {
        s->reg[s->cmd] = byte;
    }
    s->cmd++;
    return 0;
}

static uint8_t bq25713_recv(I2CSlave *i2c)
{
    BQ25713State *s = BQ25713(i2c);
    uint8_t byte = s->cmd < BQ25713_NREGS ? s->reg[s->cmd] : 0xFF;

    s->cmd++;
    return byte;
}

static void bq25713_reset(DeviceState *dev)
{
    BQ25713State *s = BQ25713(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->reg[BQ25713_MANUFACTURER] = 0x40;    /* Texas Instruments */
    s->reg[BQ25713_DEVICE] = 0x88;          /* BQ25713 */
    s->cmd = 0;
    s->have_cmd = false;
}

static void bq25713_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    sc->event = bq25713_event;
    sc->send = bq25713_send;
    sc->recv = bq25713_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), bq25713_reset);
}

static const TypeInfo bq25713_info = {
    .name          = TYPE_BQ25713,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(BQ25713State),
    .class_init    = bq25713_class_init,
};

static void bq25713_register_types(void)
{
    type_register_static(&bq25713_info);
}

type_init(bq25713_register_types)
