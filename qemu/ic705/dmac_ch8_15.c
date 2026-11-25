/*
 * RZ/A1H DMA controller, channels 8..15 (dmac_ch8_15.c).
 *
 * The DMAC has two windows of eight channels; channels 0..7 at 0xE8200000
 * are dmac.c. This device is the second window at 0xE8200400: eight channel
 * register blocks of 0x40 bytes, then the shared DCTRL_8_15 block at +0x300.
 * Per channel (offsets inside the block):
 *
 *   0x00 N0SA   source address        0x24 CHSTAT  status (read only)
 *   0x04 N0DA   destination address   0x28 CHCTRL  control (write only)
 *   0x08 N0TB   byte count            0x2C CHCFG   configuration
 *
 * A SETEN write copies N0TB bytes from N0SA to N0DA in units of the
 * destination data size (CHCFG.DDS), each address fixed when CHCFG.SAD/DAD
 * is set, then completes after one microsecond per unit: CHSTAT EN clears,
 * END and TC set, and the channel's DMAINT line goes high unless CHCFG.DEM
 * masks it. The IC-705 firmware uses channel 10 for a buffer moved out to
 * the GPIO block under MTU2 pacing and waits for the completion interrupt;
 * no request sources or link descriptors are modelled. Other registers read
 * back what was written.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define DMAC_CHANNELS   8
#define DMAC_FIRST      8               /* channel number of block 0 */
#define DMAC_BLOCK      0x40
#define DMAC_WINDOW     0x400

#define REG_N0SA        0x00
#define REG_N0DA        0x04
#define REG_N0TB        0x08
#define REG_CHSTAT      0x24
#define REG_CHCTRL      0x28
#define REG_CHCFG       0x2C

#define CHSTAT_EN       BIT(0)
#define CHSTAT_END      BIT(5)
#define CHSTAT_TC       BIT(6)

#define CHCTRL_SETEN    BIT(0)
#define CHCTRL_CLREN    BIT(1)
#define CHCTRL_SWRST    BIT(3)
#define CHCTRL_CLREND   BIT(5)
#define CHCTRL_CLRTC    BIT(6)

#define CHCFG_DDS(cfg)  (((cfg) >> 16) & 0xF)   /* 0 = 8 bits .. 7 = 1024 bits */
#define CHCFG_SAD       BIT(20)                  /* source address fixed */
#define CHCFG_DAD       BIT(21)                  /* destination address fixed */
#define CHCFG_DEM       BIT(24)                  /* completion interrupt masked */

#define NS_PER_UNIT     1000

OBJECT_DECLARE_SIMPLE_TYPE(DmacCh8State, RZA1H_DMAC_HI)

typedef struct DmacChannel {
    DmacCh8State *dmac;
    int number;                         /* 8..15 */
    uint32_t status;                    /* CHSTAT */
    QEMUTimer *done;
    qemu_irq irq;
} DmacChannel;

struct DmacCh8State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t reg[DMAC_WINDOW / 4];      /* plain registers, word-indexed */
    DmacChannel ch[DMAC_CHANNELS];
};

static uint32_t chreg(DmacChannel *c, hwaddr off)
{
    return c->dmac->reg[((c->number - DMAC_FIRST) * DMAC_BLOCK + off) / 4];
}

static void channel_irq(DmacChannel *c)
{
    bool masked = chreg(c, REG_CHCFG) & CHCFG_DEM;

    qemu_set_irq(c->irq, (c->status & CHSTAT_END) && !masked);
}

static void channel_done(void *opaque)
{
    DmacChannel *c = opaque;

    c->status = (c->status & ~CHSTAT_EN) | CHSTAT_END | CHSTAT_TC;
    rza1h_debug("dmac", "ch%d done", c->number);
    channel_irq(c);
}

/* Transfer unit in bytes: CHCFG.DDS, 8 to 1024 bits. */
uint32_t ic705_dma_unit(uint32_t cfg)
{
    return 1u << MIN(CHCFG_DDS(cfg), 7u);
}

/* Move @count bytes as a DMAC channel configured with @cfg would: in units of
 * the destination size, each address fixed when CHCFG.SAD / DAD is set.
 * Also used by dmac_ch0_7.c. */
void ic705_dma_copy(uint32_t src, uint32_t dst, uint32_t count, uint32_t cfg)
{
    uint32_t unit = ic705_dma_unit(cfg);
    uint8_t chunk[128];

    for (uint32_t done = 0; done < count; done += unit) {
        uint32_t len = MIN(unit, count - done);

        address_space_read(&address_space_memory, src + (cfg & CHCFG_SAD ? 0 : done),
                           MEMTXATTRS_UNSPECIFIED, chunk, len);
        address_space_write(&address_space_memory, dst + (cfg & CHCFG_DAD ? 0 : done),
                            MEMTXATTRS_UNSPECIFIED, chunk, len);
    }
}

static void channel_start(DmacChannel *c)
{
    uint32_t src = chreg(c, REG_N0SA), dst = chreg(c, REG_N0DA);
    uint32_t count = chreg(c, REG_N0TB), cfg = chreg(c, REG_CHCFG);
    uint32_t unit = ic705_dma_unit(cfg);

    rza1h_debug("dmac", "ch%d start %08x -> %08x, %u bytes, cfg %08x",
                c->number, src, dst, count, cfg);
    c->status |= CHSTAT_EN;
    ic705_dma_copy(src, dst, count, cfg);
    timer_mod(c->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)MAX(DIV_ROUND_UP(count, unit), 1u) * NS_PER_UNIT);
}

static void channel_control(DmacChannel *c, uint32_t ctrl)
{
    if (ctrl & CHCTRL_SWRST) {
        c->status = 0;
        timer_del(c->done);
    }
    if (ctrl & CHCTRL_CLREN) {
        c->status &= ~CHSTAT_EN;
        timer_del(c->done);
    }
    if (ctrl & CHCTRL_CLREND) {
        c->status &= ~CHSTAT_END;
    }
    if (ctrl & CHCTRL_CLRTC) {
        c->status &= ~CHSTAT_TC;
    }
    if (ctrl & CHCTRL_SETEN) {
        channel_start(c);
    }
    channel_irq(c);
}

static DmacChannel *channel_at(DmacCh8State *s, hwaddr off, hwaddr *in_block)
{
    *in_block = 0;
    if (off >= DMAC_CHANNELS * DMAC_BLOCK) {
        return NULL;                    /* DCTRL_8_15 and the rest */
    }
    *in_block = off % DMAC_BLOCK;
    return &s->ch[off / DMAC_BLOCK];
}

static uint64_t dmac_read(void *opaque, hwaddr off, unsigned size)
{
    DmacCh8State *s = opaque;
    hwaddr in_block;
    DmacChannel *c = channel_at(s, off, &in_block);
    uint32_t word;

    if (c && in_block == REG_CHSTAT) {
        word = c->status;
    } else if (c && in_block == REG_CHCTRL) {
        word = 0;                       /* write-only */
    } else {
        word = s->reg[off / 4];
    }
    return extract32(word, (off & 3) * 8, size * 8);
}

static void dmac_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    DmacCh8State *s = opaque;
    hwaddr in_block;
    DmacChannel *c = channel_at(s, off, &in_block);

    if (c && in_block == REG_CHSTAT) {
        return;                         /* read-only */
    }
    if (c && in_block == REG_CHCTRL) {
        channel_control(c, value << ((off & 3) * 8));
        return;
    }
    s->reg[off / 4] = deposit32(s->reg[off / 4], (off & 3) * 8, size * 8, value);
    if (c && in_block == REG_CHCFG) {
        channel_irq(c);                 /* DEM may have changed */
    }
}

static const MemoryRegionOps dmac_ops = {
    .read = dmac_read,
    .write = dmac_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void dmac_reset(DeviceState *dev)
{
    DmacCh8State *s = RZA1H_DMAC_HI(dev);

    memset(s->reg, 0, sizeof(s->reg));
    for (int i = 0; i < DMAC_CHANNELS; i++) {
        s->ch[i].status = 0;
        timer_del(s->ch[i].done);
        qemu_irq_lower(s->ch[i].irq);
    }
}

static void dmac_init(Object *obj)
{
    DmacCh8State *s = RZA1H_DMAC_HI(obj);

    memory_region_init_io(&s->mmio, obj, &dmac_ops, s, "rza1h-dmac-ch8-15", DMAC_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    for (int i = 0; i < DMAC_CHANNELS; i++) {
        DmacChannel *c = &s->ch[i];

        c->dmac = s;
        c->number = DMAC_FIRST + i;
        c->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, channel_done, c);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &c->irq);  /* DMAINT8 + i */
    }
}

static void dmac_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), dmac_reset);
}

static const TypeInfo dmac_info = {
    .name          = TYPE_RZA1H_DMAC_HI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DmacCh8State),
    .instance_init = dmac_init,
    .class_init    = dmac_class_init,
};

static void dmac_register_types(void)
{
    type_register_static(&dmac_info);
}

type_init(dmac_register_types)
