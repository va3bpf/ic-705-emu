/*
 * RZ/A1H Renesas SPI (RSPI) master, channels 0..4 as the IC-705 uses them
 * (rspi.c). The IC-7300 machine keeps its own RSPI2 model, qemu/machine/rspi2.c.
 *
 * Registers modelled (offsets in a channel's 0x800-byte window):
 *   +0x00 SPCR    SPRIE (7) receive interrupt, SPE (6) enable, SPTIE (5)
 *                 transmit interrupt
 *   +0x03 SPSR    SPRF (7) receive full, TEND (6), SPTEF (5) transmit empty;
 *                 bits 4..0 are error flags, cleared by writing 0
 *   +0x04 SPDR    8-, 16- or 32-bit access = 1, 2 or 4 frames of 8 bits,
 *                 the first frame in the most significant byte
 *   +0x20 SPBFCR  TXRST (7), RXRST (6), RXTRG (2..0)
 *   +0x22 SPBFDR  frames in the receive buffer (bits 5..0)
 * Everything else reads back what was written.
 *
 * A frame written to SPDR is exchanged at once with the device attached to
 * the channel (an idle line answers 0xFF) and its reply goes to the receive
 * buffer. Transmission never waits, so SPTEF and TEND always read 1. SPRF
 * and the SPRI interrupt follow the receive fill level against RXTRG; while
 * a DMA channel is reading SPDR (dmac.c) the DMA takes the bytes and SPRI
 * stays low, as the DMA request wins on the real chip. The SPTI interrupt is
 * high while SPTIE and SPE are set.
 *
 * Each write to SPCR starts a new frame for the device (the first byte after
 * it has frame_start set), and SPE going from 1 to 0 deselects it.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define RSPI_CHANNELS   5
#define RSPI_WINDOW     0x100
#define RX_BUFFER       4096    /* DMA'd Wi-Fi messages exceed the 32-frame FIFO */

#define REG_SPCR        0x00
#define REG_SPSR        0x03
#define REG_SPDR        0x04
#define REG_SPBFCR      0x20
#define REG_SPBFDR      0x22

#define SPCR_SPRIE      BIT(7)
#define SPCR_SPE        BIT(6)
#define SPCR_SPTIE      BIT(5)
#define SPSR_SPRF       BIT(7)
#define SPSR_TEND       BIT(6)
#define SPSR_SPTEF      BIT(5)
#define SPSR_ERRORS     0x1F
#define SPBFCR_RXRST    BIT(6)

OBJECT_DECLARE_SIMPLE_TYPE(RspiState, RZA1H_RSPI)

struct RspiState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t channel;
    uint8_t reg[RSPI_WINDOW];
    IC705SpiSlave slave;
    bool frame_start;
    bool dma_reading;
    uint8_t rx[RX_BUFFER];
    int rx_head, rx_count;
    qemu_irq spri, spti;
};

static RspiState *rspi[RSPI_CHANNELS];

static int rx_trigger(RspiState *s)
{
    static const int level[8] = { 1, 2, 4, 8, 16, 24, 32, 32 };

    return level[s->reg[REG_SPBFCR] & 7];
}

static void rspi_update_irq(RspiState *s)
{
    uint8_t spcr = s->reg[REG_SPCR];

    qemu_set_irq(s->spri, (spcr & SPCR_SPRIE) && !s->dma_reading &&
                 s->rx_count >= rx_trigger(s));
    qemu_set_irq(s->spti, (spcr & SPCR_SPTIE) && (spcr & SPCR_SPE));
}

static uint8_t rx_pop(RspiState *s)
{
    uint8_t byte;

    if (!s->rx_count) {
        return 0xFF;
    }
    byte = s->rx[s->rx_head];
    s->rx_head = (s->rx_head + 1) % RX_BUFFER;
    s->rx_count--;
    return byte;
}

static void exchange(RspiState *s, uint8_t mosi)
{
    uint8_t miso = 0xFF;

    if (s->slave.transfer) {
        miso = s->slave.transfer(s->slave.opaque, mosi, s->frame_start);
    }
    s->frame_start = false;
    if (s->rx_count < RX_BUFFER) {
        s->rx[(s->rx_head + s->rx_count++) % RX_BUFFER] = miso;
    }
    rza1h_debug("rspi", "rspi%u %02x -> %02x", s->channel, mosi, miso);
}

static uint64_t rspi_read(void *opaque, hwaddr off, unsigned size)
{
    RspiState *s = opaque;
    uint64_t value = 0;

    switch (off) {
    case REG_SPSR:
        return (s->rx_count >= rx_trigger(s) ? SPSR_SPRF : 0) | SPSR_TEND | SPSR_SPTEF |
               (s->reg[REG_SPSR] & SPSR_ERRORS);
    case REG_SPDR:
        for (unsigned i = 0; i < size; i++) {
            value = value << 8 | rx_pop(s);
        }
        rspi_update_irq(s);
        return value;
    case REG_SPBFDR:
        return MIN(s->rx_count, 32);
    default:
        for (unsigned i = 0; i < size; i++) {
            value |= (uint64_t)s->reg[off + i] << (8 * i);
        }
        return value;
    }
}

static void rspi_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    RspiState *s = opaque;

    switch (off) {
    case REG_SPDR:
        for (int i = size - 1; i >= 0; i--) {
            exchange(s, value >> (8 * i));
        }
        break;
    case REG_SPSR:
        s->reg[REG_SPSR] &= value;              /* error flags: write 0 to clear */
        break;
    case REG_SPCR:
        s->frame_start = true;
        if ((s->reg[REG_SPCR] & SPCR_SPE) && !(value & SPCR_SPE) && s->slave.deselect) {
            s->slave.deselect(s->slave.opaque);
        }
        s->reg[REG_SPCR] = value;
        break;
    case REG_SPBFCR:
        if (value & SPBFCR_RXRST) {
            s->rx_head = s->rx_count = 0;
        }
        s->reg[REG_SPBFCR] = value & 0x3F;      /* the reset bits do not stay set */
        break;
    default:
        for (unsigned i = 0; i < size; i++) {
            s->reg[off + i] = value >> (8 * i);
        }
        break;
    }
    rspi_update_irq(s);
}

static const MemoryRegionOps rspi_ops = {
    .read = rspi_read,
    .write = rspi_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void rza1h_rspi_attach(int channel, const IC705SpiSlave *slave)
{
    if (channel >= 0 && channel < RSPI_CHANNELS && rspi[channel]) {
        rspi[channel]->slave = *slave;
    }
}

/* dmac.c: a DMA channel starts (true) or stops (false) reading SPDR of @ch. */
void rza1h_rspi_dma_rx(int ch, bool on)
{
    if (ch >= 0 && ch < RSPI_CHANNELS && rspi[ch]) {
        rspi[ch]->dma_reading = on;
        rspi_update_irq(rspi[ch]);
    }
}

static void rspi_reset(DeviceState *dev)
{
    RspiState *s = RZA1H_RSPI(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->rx_head = s->rx_count = 0;
    s->frame_start = false;
    rspi_update_irq(s);
}

static void rspi_realize(DeviceState *dev, Error **errp)
{
    RspiState *s = RZA1H_RSPI(dev);

    if (s->channel < RSPI_CHANNELS) {
        rspi[s->channel] = s;
    }
}

static void rspi_init(Object *obj)
{
    RspiState *s = RZA1H_RSPI(obj);

    memory_region_init_io(&s->mmio, obj, &rspi_ops, s, "rza1h-rspi", RSPI_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->spri);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->spti);
}

static const Property rspi_props[] = {
    DEFINE_PROP_UINT32("channel", RspiState, channel, 0),
};

static void rspi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rspi_reset);
    device_class_set_props(dc, rspi_props);
    dc->realize = rspi_realize;
}

static const TypeInfo rspi_info = {
    .name          = TYPE_RZA1H_RSPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RspiState),
    .instance_init = rspi_init,
    .class_init    = rspi_class_init,
};

static void rspi_register_types(void)
{
    type_register_static(&rspi_info);
}

type_init(rspi_register_types)
