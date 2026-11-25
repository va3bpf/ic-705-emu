/*
 * IC-705 sub-CPU SoC: an STM32L051 (Cortex-M0+) running Icom's sub-CPU
 * firmware (the package's aux2 image, 64 KiB) on its own CPU cluster, beside
 * the RZ/A1H (soc.c).
 *
 * Memory map (RM0377):
 *   0x00000000  flash, aliased      0x08000000  flash, 64 KiB, the firmware
 *   0x08080000  data EEPROM, 2 KiB  0x1FF80000  system information (zeros)
 *   0x20000000  SRAM, 8 KiB         0x40000000..0x50002000  peripherals
 *
 * Peripherals, one file each: rcc.c (RCC 0x40021000, PWR 0x40007000),
 * flash.c (FLASH 0x40022000), exti.c (SYSCFG 0x40010000, EXTI 0x40010400),
 * gpio.c (GPIOA..D, H 0x50000000..), timer.c (TIM2/6/21/22), adc.c (ADC
 * 0x40012400), dma.c (DMA1 0x40020000), spi.c (SPI1 0x40013000), usart.c
 * (USART1 0x40013800). IWDG (0x40003000) reads 0 and ignores writes; other
 * addresses are logged as unimplemented.
 *
 * The sub-CPU's main loop polls its GPIO inputs all the time, so the
 * peripheral region runs without the BQL: GPIO reads only read state, every
 * other access takes the BQL.
 *
 * RZA1H_DEBUG=sub logs the sub-CPU's peripheral activity.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "system/reset.h"
#include "target/arm/cpu.h"
#include "hw/arm/rza1h_debug.h"
#include "subcpu.h"

#define FLASH_BASE      0x08000000
#define FLASH_SIZE      0x10000
#define SRAM_BASE       0x20000000
#define SRAM_SIZE       0x2000
#define DATA_EEPROM     0x08080000
#define DATA_EEPROM_SIZE 0x800
#define SYSINFO_BASE    0x1FF80000
#define PERIPH_BASE     0x40000000
#define PERIPH_SIZE     0x10002000
#define BLOCK_SIZE      0x400

const char sub_port_letter[GPIO_PORTS] = { 'A', 'B', 'C', 'D', 'H' };

void sub_log(const char *fmt, ...)
{
    va_list ap;

    if (!rza1h_debug_enabled("sub")) {
        return;
    }
    fprintf(stderr, "[sub vt=%.6f] ", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void sub_nvic(STM32L0State *s, int line, bool level)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m), line), level);
}

/* ------------------------------------------------------- peripheral map */

typedef enum {
    BLK_NONE, BLK_TIMER, BLK_IWDG, BLK_PWR, BLK_SYSCFG, BLK_EXTI, BLK_ADC, BLK_SPI,
    BLK_USART, BLK_DMA, BLK_RCC, BLK_FLASH, BLK_GPIO,
} Block;

static const struct { uint32_t base; Block block; int index; } blocks[] = {
    { 0x40000000, BLK_TIMER, 0 },  { 0x40001000, BLK_TIMER, 1 },  /* TIM2, TIM6 */
    { 0x40003000, BLK_IWDG, 0 },   { 0x40007000, BLK_PWR, 0 },
    { 0x40010000, BLK_SYSCFG, 0 }, { 0x40010400, BLK_EXTI, 0 },
    { 0x40010800, BLK_TIMER, 2 },  { 0x40011400, BLK_TIMER, 3 },  /* TIM21, TIM22 */
    { 0x40012400, BLK_ADC, 0 },    { 0x40013000, BLK_SPI, 0 },
    { 0x40013800, BLK_USART, 0 },  { 0x40020000, BLK_DMA, 0 },
    { 0x40021000, BLK_RCC, 0 },    { 0x40022000, BLK_FLASH, 0 },
    { 0x50000000, BLK_GPIO, 0 },   { 0x50000400, BLK_GPIO, 1 },
    { 0x50000800, BLK_GPIO, 2 },   { 0x50000C00, BLK_GPIO, 3 },
    { 0x50001C00, BLK_GPIO, 4 },
};

static Block find_block(hwaddr addr, int *index, hwaddr *off)
{
    uint32_t a = PERIPH_BASE + addr;

    for (size_t i = 0; i < ARRAY_SIZE(blocks); i++) {
        if (a >= blocks[i].base && a < blocks[i].base + BLOCK_SIZE) {
            *index = blocks[i].index;
            *off = a - blocks[i].base;
            return blocks[i].block;
        }
    }
    return BLK_NONE;
}

static uint32_t periph_read_bql(STM32L0State *s, hwaddr addr)
{
    int i;
    hwaddr off;

    switch (find_block(addr, &i, &off)) {
    case BLK_TIMER:  return sub_timer_read(&s->tim[i], off);
    case BLK_GPIO:   return sub_gpio_read(s, i, off);
    case BLK_RCC:    return sub_rcc_read(s, off);
    case BLK_PWR:    return sub_pwr_read(s, off);
    case BLK_FLASH:  return sub_flash_read(s, off);
    case BLK_SYSCFG: return sub_syscfg_read(s, off);
    case BLK_EXTI:   return sub_exti_read(s, off);
    case BLK_ADC:    return sub_adc_read(s, off);
    case BLK_SPI:    return sub_spi_read(s, off);
    case BLK_USART:  return sub_usart_read(s, off);
    case BLK_DMA:    return sub_dma_read(s, off);
    case BLK_IWDG:   return 0;
    case BLK_NONE:
    default:
        qemu_log_mask(LOG_UNIMP, "stm32l0: read %08" HWADDR_PRIx "\n", PERIPH_BASE + addr);
        return 0;
    }
}

static void periph_write_bql(STM32L0State *s, hwaddr addr, uint32_t v)
{
    int i;
    hwaddr off;

    switch (find_block(addr, &i, &off)) {
    case BLK_TIMER:  sub_timer_write(&s->tim[i], off, v); break;
    case BLK_GPIO:   sub_gpio_write(s, i, off, v); break;
    case BLK_RCC:    sub_rcc_write(s, off, v); break;
    case BLK_PWR:    sub_pwr_write(s, off, v); break;
    case BLK_FLASH:  sub_flash_write(s, off, v); break;
    case BLK_SYSCFG: sub_syscfg_write(s, off, v); break;
    case BLK_EXTI:   sub_exti_write(s, off, v); break;
    case BLK_ADC:    sub_adc_write(s, off, v); break;
    case BLK_SPI:    sub_spi_write(s, off, v); break;
    case BLK_USART:  sub_usart_write(s, off, v); break;
    case BLK_DMA:    sub_dma_write(s, off, v); break;
    case BLK_IWDG:   break;
    case BLK_NONE:
    default:
        qemu_log_mask(LOG_UNIMP, "stm32l0: write %08" HWADDR_PRIx " = %08x\n",
                      PERIPH_BASE + addr, v);
        break;
    }
}

static uint64_t periph_read(void *opaque, hwaddr addr, unsigned size)
{
    int i;
    hwaddr off;
    bool take_bql = find_block(addr, &i, &off) != BLK_GPIO && !bql_locked();
    uint32_t v;

    if (take_bql) {
        bql_lock();
    }
    v = periph_read_bql(opaque, addr);
    if (take_bql) {
        bql_unlock();
    }
    return v;
}

static void periph_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    bool take_bql = !bql_locked();

    if (take_bql) {
        bql_lock();
    }
    periph_write_bql(opaque, addr, v);
    if (take_bql) {
        bql_unlock();
    }
}

static const MemoryRegionOps periph_ops = {
    .read = periph_read,
    .write = periph_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------- QOM properties */

static void parse_items(const char *text, void (*item)(STM32L0State *, const char *, Error **),
                        STM32L0State *s, Error **errp)
{
    g_auto(GStrv) items = g_strsplit(text ? text : "", ",", -1);

    for (int i = 0; items[i] && *items[i]; i++) {
        item(s, items[i], errp);
        if (*errp) {
            return;
        }
    }
}

/* "adc": "ch=value" items, 12-bit, e.g. "0=0x0c0" presses a key */
static void adc_item(STM32L0State *s, const char *item, Error **errp)
{
    char *end;
    long ch = strtol(item, &end, 0);
    unsigned long value;

    if (*end != '=') {
        error_setg(errp, "adc: expected ch=value");
        return;
    }
    value = strtoul(end + 1, NULL, 0);
    if (ch >= 0 && ch < ADC_CHANNELS) {
        s->adc.level[ch] = value & 0xFFF;
        sub_log("adc ch%ld = %#lx", ch, value & 0xFFF);
    }
}

static void prop_set_adc(Object *obj, const char *text, Error **errp)
{
    parse_items(text, adc_item, STM32L0(obj), errp);
}

static char *prop_get_adc(Object *obj, Error **errp)
{
    STM32L0State *s = STM32L0(obj);
    g_autoptr(GString) out = g_string_new(NULL);

    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        g_string_append_printf(out, "%s%d=%#x", ch ? "," : "", ch, s->adc.level[ch]);
    }
    return g_string_free(g_steal_pointer(&out), false);
}

/* "odr": "A=xxxx B=xxxx C=xxxx D=xxxx H=xxxx", the output registers */
static char *prop_get_odr(Object *obj, Error **errp)
{
    STM32L0State *s = STM32L0(obj);
    g_autoptr(GString) out = g_string_new(NULL);

    for (int port = 0; port < GPIO_PORTS; port++) {
        g_string_append_printf(out, "%s%c=%04x", port ? " " : "", sub_port_letter[port],
                               s->gpio[port].odr);
    }
    return g_string_free(g_steal_pointer(&out), false);
}

/* "pins": "<port><bit>=<0|1|z>" items, e.g. "C15=0"; z releases the pin */
static void pin_item(STM32L0State *s, const char *item, Error **errp)
{
    int port = sub_gpio_port(item[0]);
    char *end;
    long bit = strtol(item + 1, &end, 10);

    if (port < 0 || *end != '=' || bit < 0 || bit > 15) {
        error_setg(errp, "pins: expected e.g. C9=0, A3=1 or B2=z");
        return;
    }
    sub_gpio_drive(s, port, bit, g_ascii_tolower(end[1]) == 'z' ? -1 : end[1] != '0');
    sub_log("pin %c%ld = %c", sub_port_letter[port], bit, end[1]);
}

static void prop_set_pins(Object *obj, const char *text, Error **errp)
{
    parse_items(text, pin_item, STM32L0(obj), errp);
}

/* ------------------------------------------- GPIO input lines "pa".."ph" */

static void pin_in(void *opaque, int port, int bit, int level)
{
    sub_gpio_drive(opaque, port, bit, level != 0);
}

static void pin_in_a(void *opaque, int bit, int level) { pin_in(opaque, 0, bit, level); }
static void pin_in_b(void *opaque, int bit, int level) { pin_in(opaque, 1, bit, level); }
static void pin_in_c(void *opaque, int bit, int level) { pin_in(opaque, 2, bit, level); }
static void pin_in_d(void *opaque, int bit, int level) { pin_in(opaque, 3, bit, level); }
static void pin_in_h(void *opaque, int bit, int level) { pin_in(opaque, 4, bit, level); }

/* ---------------------------------------------------------------- device */

static void soc_reset(DeviceState *dev)
{
    STM32L0State *s = STM32L0(dev);

    sub_rcc_reset(s);
    sub_flash_reset(s);
    sub_exti_reset(s);
    sub_gpio_reset(s);
    for (int i = 0; i < TIMERS; i++) {
        sub_timer_reset(&s->tim[i]);
    }
    sub_adc_reset(s);
    sub_dma_reset(s);
    sub_spi_reset(s);
    sub_usart_reset(s);
}

/* The M-profile reset loads SP and PC before the firmware is in flash, so
 * take them from the vector table at 0x08000000 after every system reset. */
static void cpu_start(void *opaque)
{
    STM32L0State *s = opaque;
    ARMCPU *cpu = s->armv7m.cpu;
    const uint8_t *vectors = memory_region_get_ram_ptr(&s->flash);

    cpu_reset(CPU(cpu));
    cpu->env.regs[13] = ldl_le_p(vectors) & ~3u;
    cpu->env.regs[15] = ldl_le_p(vectors + 4) & ~1u;
    cpu->env.thumb = 1;
    sub_log("reset: SP=%08x PC=%08x", cpu->env.regs[13], cpu->env.regs[15]);
}

static void add_ram(STM32L0State *s, MemoryRegion *mr, const char *name, hwaddr base,
                    uint64_t size)
{
    memory_region_init_ram(mr, OBJECT(s), name, size, &error_fatal);
    memory_region_add_subregion(&s->space, base, mr);
}

static void soc_realize(DeviceState *dev, Error **errp)
{
    STM32L0State *s = STM32L0(dev);

    memory_region_init(&s->space, OBJECT(s), "stm32l0", UINT64_MAX);
    clock_set_hz(s->sysclk, 32000000);
    clock_set_hz(s->refclk, 1000000);
    qdev_connect_clock_in(DEVICE(&s->armv7m), "cpuclk", s->sysclk);
    qdev_connect_clock_in(DEVICE(&s->armv7m), "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory", OBJECT(&s->space), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }
    /* a cluster counts its CPUs when realized, so after armv7m; the RZ/A1H is 0 */
    qdev_prop_set_uint32(DEVICE(&s->cluster), "cluster-id", 1);
    qdev_realize(DEVICE(&s->cluster), NULL, &error_fatal);

    add_ram(s, &s->flash, "stm32l0.flash", FLASH_BASE, FLASH_SIZE);
    memory_region_init_alias(&s->flash_at_0, OBJECT(s), "stm32l0.flash-alias", &s->flash,
                             0, FLASH_SIZE);
    memory_region_add_subregion(&s->space, 0, &s->flash_at_0);
    add_ram(s, &s->sram, "stm32l0.sram", SRAM_BASE, SRAM_SIZE);
    add_ram(s, &s->data_eeprom, "stm32l0.eeprom", DATA_EEPROM, DATA_EEPROM_SIZE);
    add_ram(s, &s->sysinfo, "stm32l0.sysinfo", SYSINFO_BASE, 0x100);
    memory_region_init_io(&s->periph, OBJECT(s), &periph_ops, s, "stm32l0.periph",
                          PERIPH_SIZE);
    memory_region_enable_lockless_io(&s->periph);
    memory_region_add_subregion(&s->space, PERIPH_BASE, &s->periph);

    if (s->firmware) {
        g_autofree gchar *image = NULL;
        gsize size;

        if (!g_file_get_contents(s->firmware, &image, &size, NULL)) {
            error_setg(errp, "stm32l0: cannot read firmware %s", s->firmware);
            return;
        }
        memcpy(memory_region_get_ram_ptr(&s->flash), image, MIN(size, FLASH_SIZE));
    }
    sub_adc_init(s);
    sub_timer_init(s);
    sub_usart_init(s);
    qemu_register_reset(cpu_start, s);
}

static void soc_init(Object *obj)
{
    STM32L0State *s = STM32L0(obj);
    DeviceState *dev = DEVICE(obj);
    static const char *const in[GPIO_PORTS] = { "pa", "pb", "pc", "pd", "ph" };
    static const char *const out[GPIO_PORTS] = { "pa-out", "pb-out", "pc-out", "pd-out", "ph-out" };
    static const qemu_irq_handler input[GPIO_PORTS] = {
        pin_in_a, pin_in_b, pin_in_c, pin_in_d, pin_in_h,
    };

    object_initialize_child(obj, "cluster", &s->cluster, TYPE_CPU_CLUSTER);
    object_initialize_child(OBJECT(&s->cluster), "armv6m", &s->armv7m, TYPE_ARMV7M);
    qdev_prop_set_string(DEVICE(&s->armv7m), "cpu-type", ARM_CPU_TYPE_NAME("cortex-m0"));
    qdev_prop_set_uint32(DEVICE(&s->armv7m), "num-irq", 32);
    s->sysclk = qdev_init_clock_in(dev, "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(dev, "refclk", NULL, NULL, 0);

    object_property_add_str(obj, "adc", prop_get_adc, prop_set_adc);
    object_property_add_str(obj, "odr", prop_get_odr, NULL);
    object_property_add_str(obj, "pins", NULL, prop_set_pins);
    for (int port = 0; port < GPIO_PORTS; port++) {
        qdev_init_gpio_in_named(dev, input[port], in[port], 16);
        qdev_init_gpio_out_named(dev, s->gpio[port].out, out[port], 16);
    }
}

static const Property soc_props[] = {
    DEFINE_PROP_STRING("firmware", STM32L0State, firmware),
};

static void soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = soc_realize;
    device_class_set_legacy_reset(dc, soc_reset);
    device_class_set_props(dc, soc_props);
}

static const TypeInfo soc_info = {
    .name          = TYPE_STM32L0,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STM32L0State),
    .instance_init = soc_init,
    .class_init    = soc_class_init,
};

static void soc_register_types(void)
{
    type_register_static(&soc_info);
}

type_init(soc_register_types)
