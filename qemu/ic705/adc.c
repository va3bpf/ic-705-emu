/*
 * RZ/A1H 10-bit A/D converter at 0xE8005800, as the IC-705 uses it (adc.c).
 * The IC-7300 machine keeps its fixed-reading stub, qemu/machine/adc.c.
 *
 *   +0x00..+0x0E  ADDRA..ADDRH  result of channel 0..7, 10 bits left-justified
 *                               (bits 15:6)
 *   +0x60         ADCSR         ADF (15) conversion done, ADST (13) start
 *
 * The IC-705's channels carry key ladders and sense voltages
 * (docs/ic705/BOARD.md). A channel reads, in order of precedence: the value
 * set live through the QOM property "vals" ("ch=value,...", 10-bit; -1
 * drops it), the channel's entry in RZA1H_ADC_VALS ("v0,v1,...,v7"), or
 * mid-scale 0x200.
 *
 * Conversions take no time: writing ADST = 1 clears ADST and sets ADF at
 * once. ADF is cleared by writing 0 to it. Other registers read back what
 * was written.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define ADC_CHANNELS    8
#define REG_ADCSR       0x60
#define ADCSR_ADF       0x8000
#define ADCSR_ADST      0x2000
#define MID_SCALE       0x200
#define UNSET           (-1)

OBJECT_DECLARE_SIMPLE_TYPE(AdcState, IC705_ADC)

struct AdcState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint8_t reg[RZA1H_ADC_SIZE];
    int live[ADC_CHANNELS];             /* from "vals", or UNSET */
    int preset[ADC_CHANNELS];           /* from RZA1H_ADC_VALS, or UNSET */
};

static uint16_t channel_result(AdcState *s, int ch)
{
    int v = s->live[ch] != UNSET ? s->live[ch] :
            s->preset[ch] != UNSET ? s->preset[ch] : MID_SCALE;

    return (v & 0x3FF) << 6;
}

static uint64_t adc_read(void *opaque, hwaddr off, unsigned size)
{
    AdcState *s = opaque;
    uint64_t value = 0;

    if (off < 2 * ADC_CHANNELS && !(off & 1)) {
        return channel_result(s, off / 2);
    }
    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)s->reg[off + i] << (8 * i);
    }
    return value;
}

static void adc_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    AdcState *s = opaque;

    if (off == REG_ADCSR && size == 2) {
        uint16_t old = lduw_le_p(&s->reg[REG_ADCSR]);
        uint16_t adf = old & value & ADCSR_ADF;         /* writing 0 clears ADF */

        if (value & ADCSR_ADST) {                       /* convert at once */
            adf = ADCSR_ADF;
        }
        stw_le_p(&s->reg[REG_ADCSR], (value & ~(ADCSR_ADST | ADCSR_ADF)) | adf);
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        s->reg[off + i] = value >> (8 * i);
    }
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static char *adc_get_vals(Object *obj, Error **errp)
{
    AdcState *s = IC705_ADC(obj);
    GString *out = g_string_new(NULL);

    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        g_string_append_printf(out, "%s%d=%d", ch ? "," : "", ch, s->live[ch]);
    }
    return g_string_free(out, false);
}

static void adc_set_vals(Object *obj, const char *text, Error **errp)
{
    AdcState *s = IC705_ADC(obj);
    g_auto(GStrv) items = g_strsplit(text ? text : "", ",", -1);

    for (int i = 0; items[i]; i++) {
        int ch, v;

        if (sscanf(items[i], "%i=%i", &ch, &v) == 2 && ch >= 0 && ch < ADC_CHANNELS) {
            s->live[ch] = v < 0 ? UNSET : v;
            rza1h_debug("adc", "channel %d = %d", ch, v);
        }
    }
}

static void adc_reset(DeviceState *dev)
{
    AdcState *s = IC705_ADC(dev);

    memset(s->reg, 0, sizeof(s->reg));
}

static void adc_init(Object *obj)
{
    AdcState *s = IC705_ADC(obj);
    const char *preset = getenv("RZA1H_ADC_VALS");
    g_auto(GStrv) values = g_strsplit(preset ? preset : "", ",", ADC_CHANNELS);

    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        s->live[ch] = UNSET;
        s->preset[ch] = UNSET;
    }
    for (int ch = 0; values[ch] && ch < ADC_CHANNELS; ch++) {
        if (*values[ch]) {
            s->preset[ch] = strtoul(values[ch], NULL, 0) & 0x3FF;
        }
    }
    memory_region_init_io(&s->mmio, obj, &adc_ops, s, "ic705-adc", RZA1H_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    object_property_add_str(obj, "vals", adc_get_vals, adc_set_vals);
}

static void adc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), adc_reset);
}

static const TypeInfo adc_info = {
    .name          = TYPE_IC705_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AdcState),
    .instance_init = adc_init,
    .class_init    = adc_class_init,
};

static void adc_register_types(void)
{
    type_register_static(&adc_info);
}

type_init(adc_register_types)
