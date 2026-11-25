/*
 * IC-705 settings EEPROM: a 2 Mbit (256 KiB) 24M02-class serial EEPROM.
 *
 * The firmware reaches the same chip two ways (docs/ic705/BOARD.md):
 *  - writes go through the RIIC0 peripheral. The chip answers at device
 *    addresses 0x50..0x53, whose two low bits are address bits 17:16, so one
 *    I2C slave is created per address and all of them share one array;
 *  - reads are bit-banged on the RIIC0 pins (P1_0 SCL, P1_1 SDA open drain).
 *    The machine passes the bus levels to ic705_eeprom_bitbang(), which runs
 *    an I2C slave state machine and returns the level this chip drives on SDA.
 *
 * A transfer is <device> <address 15:8> <address 7:0> then data. Writes wrap
 * inside a 256-byte page; reads continue through the whole array.
 *
 * The array starts from the file in RZA1H_IC705_EEPROM (blank = 0xFF without
 * it) and is saved back there one second of virtual time after the last
 * write, so settings survive between sessions.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define EEPROM_SIZE  0x40000        /* 18 address bits */
#define EEPROM_PAGE  256

/* ------------------------------------------------------------ the array */

static struct {
    uint8_t *data;
    char *file;
    QEMUTimer *save_timer;
} eeprom;

static void eeprom_save(void *opaque)
{
    GError *err = NULL;

    if (eeprom.file && !g_file_set_contents(eeprom.file, (const char *)eeprom.data,
                                            EEPROM_SIZE, &err)) {
        rza1h_debug("eeprom", "saving %s failed: %s", eeprom.file, err->message);
        g_error_free(err);
    }
}

static void eeprom_load(void)
{
    const char *file = getenv("RZA1H_IC705_EEPROM");
    gchar *contents = NULL;
    gsize size = 0;

    if (eeprom.data) {
        return;
    }
    eeprom.data = g_malloc(EEPROM_SIZE);
    memset(eeprom.data, 0xFF, EEPROM_SIZE);
    if (file) {
        eeprom.file = g_strdup(file);
        if (g_file_get_contents(file, &contents, &size, NULL)) {
            memcpy(eeprom.data, contents, MIN(size, EEPROM_SIZE));
            g_free(contents);
        }
    }
    eeprom.save_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, eeprom_save, NULL);
    rza1h_debug("eeprom", "%s, %zu bytes loaded", file ? file : "no file", (size_t)size);
}

static uint8_t eeprom_get(uint32_t addr)
{
    return eeprom.data[addr % EEPROM_SIZE];
}

static void eeprom_put(uint32_t addr, uint8_t byte)
{
    eeprom.data[addr % EEPROM_SIZE] = byte;
    timer_mod(eeprom.save_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
}

static uint32_t next_in_page(uint32_t addr)
{
    return (addr & ~(uint32_t)(EEPROM_PAGE - 1)) | ((addr + 1) & (EEPROM_PAGE - 1));
}

static uint32_t next_in_array(uint32_t addr)
{
    return (addr + 1) % EEPROM_SIZE;
}

/* --------------------------------------------- RIIC0: one slave per address */

OBJECT_DECLARE_SIMPLE_TYPE(IC705EepromState, IC705_EEPROM)

struct IC705EepromState {
    I2CSlave parent_obj;
    uint8_t a17_16;         /* from the device address 0x50 + n */
    int rx_count;           /* bytes received since START (address bytes first) */
    uint32_t addr;
};

static int eeprom_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    IC705EepromState *s = IC705_EEPROM(i2c);

    if (event == I2C_START_SEND) {
        s->rx_count = 0;
    }
    return 0;
}

static int eeprom_i2c_send(I2CSlave *i2c, uint8_t byte)
{
    IC705EepromState *s = IC705_EEPROM(i2c);

    switch (s->rx_count++) {
    case 0:
        s->addr = s->a17_16 << 16 | byte << 8;
        break;
    case 1:
        s->addr |= byte;
        break;
    default:
        rza1h_debug("eeprom", "i2c write %05x <- %02x", s->addr, byte);
        eeprom_put(s->addr, byte);
        s->addr = next_in_page(s->addr);
        break;
    }
    return 0;
}

static uint8_t eeprom_i2c_recv(I2CSlave *i2c)
{
    IC705EepromState *s = IC705_EEPROM(i2c);
    uint8_t byte = eeprom_get(s->addr);

    s->addr = next_in_array(s->addr);
    return byte;
}

static const Property eeprom_i2c_props[] = {
    DEFINE_PROP_UINT8("a17-16", IC705EepromState, a17_16, 0),
};

static void eeprom_i2c_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    sc->event = eeprom_i2c_event;
    sc->send = eeprom_i2c_send;
    sc->recv = eeprom_i2c_recv;
    device_class_set_props(DEVICE_CLASS(oc), eeprom_i2c_props);
}

static const TypeInfo eeprom_i2c_info = {
    .name          = TYPE_IC705_EEPROM,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(IC705EepromState),
    .class_init    = eeprom_i2c_class_init,
};

static void eeprom_register_types(void)
{
    type_register_static(&eeprom_i2c_info);
}

type_init(eeprom_register_types)

void ic705_eeprom_create(I2CBus *bus)
{
    eeprom_load();
    for (int n = 0; n < 4; n++) {
        I2CSlave *dev = i2c_slave_new(TYPE_IC705_EEPROM, 0x50 + n);

        qdev_prop_set_uint8(DEVICE(dev), "a17-16", n);
        i2c_slave_realize_and_unref(dev, bus, &error_fatal);
    }
}

/* ------------------------------------------- bit-banged I2C on the same pins */

enum bb_phase {
    BB_IDLE,        /* until the next START */
    BB_DEVICE,      /* receiving the device address */
    BB_ADDR_HI,
    BB_ADDR_LO,
    BB_WRITE,       /* receiving data */
    BB_READ,        /* sending data */
};

enum bb_slot {
    BB_BITS,        /* data bits */
    BB_ACK_OUT,     /* we acknowledge the byte just received */
    BB_ACK_IN,      /* the master acknowledges the byte just sent */
};

static struct {
    bool scl, sda;          /* bus levels at the last call */
    bool drive_high;        /* our SDA output: true = released */
    enum bb_phase phase;
    enum bb_slot slot;
    int bits;               /* bits shifted in or out of the current byte */
    uint8_t shift;
    bool master_nack;
    uint32_t addr;
} bb = { .scl = true, .sda = true, .drive_high = true };

static void bb_load_byte(void)
{
    bb.shift = eeprom_get(bb.addr);
    rza1h_debug("eeprom", "read %05x = %02x", bb.addr, bb.shift);
    bb.addr = next_in_array(bb.addr);
    bb.bits = 0;
}

/* A whole byte has arrived: act on it, return whether to acknowledge. */
static bool bb_byte_received(uint8_t byte)
{
    switch (bb.phase) {
    case BB_DEVICE:
        if ((byte & 0xF0) != 0xA0) {
            bb.phase = BB_IDLE;             /* another chip */
            return false;
        }
        bb.addr = ((byte >> 1) & 3) << 16 | (bb.addr & 0xFFFF);
        bb.phase = byte & 1 ? BB_READ : BB_ADDR_HI;
        return true;
    case BB_ADDR_HI:
        bb.addr = (bb.addr & 0x30000) | byte << 8;
        bb.phase = BB_ADDR_LO;
        return true;
    case BB_ADDR_LO:
        bb.addr |= byte;
        bb.phase = BB_WRITE;
        return true;
    case BB_WRITE:
        eeprom_put(bb.addr, byte);
        bb.addr = next_in_page(bb.addr);
        return true;
    default:
        return false;
    }
}

static void bb_scl_rising(bool sda)
{
    if (bb.phase == BB_IDLE) {
        return;
    }
    if (bb.slot == BB_ACK_IN) {
        bb.master_nack = sda;
    } else if (bb.slot == BB_BITS && bb.phase != BB_READ && bb.bits < 8) {
        bb.shift = bb.shift << 1 | sda;
        bb.bits++;
    }
}

static void bb_send_next_bit(void)
{
    bb.drive_high = bb.shift >> (7 - bb.bits) & 1;
    bb.bits++;
}

static void bb_scl_falling(void)
{
    if (bb.phase == BB_IDLE) {
        bb.drive_high = true;
        return;
    }
    switch (bb.slot) {
    case BB_ACK_OUT:                        /* our ACK clock is over */
        bb.slot = BB_BITS;
        bb.drive_high = true;
        if (bb.phase == BB_READ) {
            bb_load_byte();
            bb_send_next_bit();
        } else {
            bb.bits = 0;
            bb.shift = 0;
        }
        break;
    case BB_ACK_IN:                         /* the master's ACK clock is over */
        bb.slot = BB_BITS;
        if (bb.master_nack) {
            bb.phase = BB_IDLE;             /* end of read; wait for STOP */
            bb.drive_high = true;
        } else {
            bb_load_byte();
            bb_send_next_bit();
        }
        break;
    case BB_BITS:
        if (bb.phase == BB_READ) {
            if (bb.bits < 8) {
                bb_send_next_bit();
            } else {
                bb.drive_high = true;       /* let the master acknowledge */
                bb.slot = BB_ACK_IN;
            }
        } else if (bb.bits == 8) {
            bool ack = bb_byte_received(bb.shift);

            bb.drive_high = !ack;
            bb.slot = ack ? BB_ACK_OUT : BB_BITS;
            bb.bits = 0;
            bb.shift = 0;
        }
        break;
    }
}

bool ic705_eeprom_bitbang(bool scl, bool sda)
{
    bool was_scl = bb.scl, was_sda = bb.sda;

    eeprom_load();
    bb.scl = scl;
    bb.sda = sda;
    if (was_scl && scl && was_sda != sda) {
        /* SDA changed while SCL is high: START (falling) or STOP (rising) */
        bb.phase = sda ? BB_IDLE : BB_DEVICE;
        bb.slot = BB_BITS;
        bb.bits = 0;
        bb.shift = 0;
        bb.drive_high = true;
    } else if (!was_scl && scl) {
        bb_scl_rising(sda);
    } else if (was_scl && !scl) {
        bb_scl_falling();
    }
    return bb.drive_high;
}
