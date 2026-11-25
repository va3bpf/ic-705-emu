/*
 * IC-705 board (board.c): creates this project's RZ/A1H blocks and IC-705
 * devices and wires them to the SoC models of qemu/machine/. rz_a1h.c calls
 * these functions when RZA1H_BOARD=ic705.
 *
 * Pins. gpio.c reports three kinds of change, and this file passes each to
 * the device on that pin (docs/ic705/IO_MAP.md):
 *   output latch (P)     P1_0/P1_1 EEPROM, P2_x Wi-Fi power, P3_4/5/10 touch,
 *                        P5_3 and P7_11 sub-CPU link
 *   direction (PM)       P1_0/P1_1 EEPROM (open drain: input = released)
 *   input from outside   P8_7 PWRK and P3_9 VBUS, the deep-standby wake-ups
 * Devices drive RZ input pins with ic705_set_pin().
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "target/arm/cpu-qom.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "subcpu/stm32l0.h"
#include "ic705.h"

static struct {
    DeviceState *gpio;
    qemu_irq wlan_tint;         /* WI_IRQ = P2_5 = TINT29 */
    QEMUTimer *rtc_tick;
    bool p1_latch[2];           /* EEPROM pins P1_0 SCL, P1_1 SDA */
    bool p1_input[2];
} board = { .p1_input = { true, true } };

static qemu_irq gic_in(DeviceState *gic, int id)
{
    return qdev_get_gpio_in(gic, id - RZA1H_GIC_NUM_INTERNAL);
}

void ic705_set_pin(int port, int bit, int level)
{
    if (board.gpio) {
        rza1h_gpio_set_input(board.gpio, port, bit, level);
    }
}

/* wlan.c: WI_IRQ, the pin level and its pin interrupt */
void rza1h_ic705_wlan_irq(int level)
{
    ic705_set_pin(2, 5, level);
    if (board.wlan_tint) {
        qemu_set_irq(board.wlan_tint, level);
    }
}

/* The EEPROM pins are open drain: low only while an output (PM = 0) with a
 * low latch, and the EEPROM itself may pull SDA low (eeprom.c). */
static void eeprom_pins_changed(void)
{
    bool scl = board.p1_input[0] || board.p1_latch[0];
    bool sda = board.p1_input[1] || board.p1_latch[1];
    bool eeprom_sda = ic705_eeprom_bitbang(scl, sda);

    ic705_set_pin(1, 0, scl);
    ic705_set_pin(1, 1, sda && eeprom_sda);
}

static void output_changed(int port, int bit, int level)
{
    if (port == 1 && bit <= 1) {
        board.p1_latch[bit] = level;
        eeprom_pins_changed();
    } else if (port == 2) {
        rza1h_ic705_wlan_pin(bit, level);
    } else if (port == 3 && (bit == 4 || bit == 5 || bit == 10)) {
        ic705_touch_pin(bit, level);
    } else {
        ic705_subcpu_link_pin(port, bit, level);
    }
}

static void direction_changed(int port, int bit, int input)
{
    if (port == 1 && bit <= 1) {
        board.p1_input[bit] = input;
        eeprom_pins_changed();
    }
}

/* riic.c: populate the IC-705's I2C buses. RIIC0: the settings EEPROM at
 * 0x50..0x53. RIIC1: the RX-8803 RTC at 0x32 and the BQ25713 charger at 0x6B.
 * RIIC2 is unused. */
void ic705_i2c_devices(int channel, I2CBus *bus)
{
    if (channel == 0) {
        ic705_eeprom_create(bus);
    } else if (channel == 1) {
        i2c_slave_create_simple(bus, "rx8803", 0x32);
        i2c_slave_create_simple(bus, TYPE_BQ25713, 0x6B);
    }
}

/* riic.c: whether a written byte starts a device write cycle that keeps the
 * bus busy (an EEPROM page write). Only RIIC0 has the EEPROM; the RTC and
 * the charger on RIIC1 take the next transaction at once. On the IC-7300
 * every bus keeps the write cycle. */
bool ic705_i2c_write_cycle(int channel)
{
    return !rza1h_board_ic705() || channel == 0;
}

/* The RX-8803 RTC's 1 Hz interrupt on IRQ0: the firmware polls IRQRR bit 0
 * and then reads the time over RIIC1 [FW 0x200E4588, 0x200E3B98]. */
static void rtc_second(void *opaque)
{
    rza1h_gpio_ext_irq(board.gpio, 0);
    timer_mod(board.rtc_tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
}

void ic705_board_init(MachineState *machine, ARMCPU *cpu, DeviceState *gic,
                      DeviceState *gpio, DeviceState *scif3)
{
    const char *subcpu_fw = getenv("RZA1H_SUBCPU_FW");

    board.gpio = gpio;
    board.wlan_tint = gic_in(gic, RZA1H_TINT_IRQ(29));
    rza1h_gpio_out_hook = output_changed;
    rza1h_gpio_pm_hook = direction_changed;
    rza1h_gpio_pin_hook = ic705_power_pin;
    ic705_power_init(cpu);
    ic705_touch_init(OBJECT(machine));
    ic705_panel_input_init(gpio);
    board.rtc_tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, rtc_second, NULL);
    timer_mod(board.rtc_tick, NANOSECONDS_PER_SECOND);

    if (subcpu_fw) {            /* STM32L0 running Icom's own sub-CPU firmware */
        DeviceState *sub = qdev_new(TYPE_STM32L0);

        object_property_add_child(OBJECT(machine), "subcpu", OBJECT(sub));
        qdev_prop_set_string(sub, "firmware", subcpu_fw);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sub), &error_fatal);
        ic705_subcpu_link_init(STM32L0(sub), scif3);
    }
}

/* ------------------------------------- this project's RZ/A1H blocks */

void ic705_create_mtu2(DeviceState *gic)
{
    DeviceState *mtu2 = qdev_new(TYPE_RZA1H_MTU2G);

    object_property_add_child(OBJECT(qdev_get_machine()), "mtu2", OBJECT(mtu2));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mtu2), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(mtu2), 0, RZA1H_MTU2_BASE, 0);
    for (int ch = 0; ch < 5; ch++) {
        for (int ev = 0; ev < 5; ev++) {
            int id = rza1h_mtu2_irq_id(ch, ev);

            if (id >= 0) {
                sysbus_connect_irq(SYS_BUS_DEVICE(mtu2), ch * 5 + ev, gic_in(gic, id));
            }
        }
    }
}

void ic705_create_dmac_ch8_15(DeviceState *gic)
{
    DeviceState *dmac = qdev_new(TYPE_RZA1H_DMAC_HI);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dmac), 0, RZA1H_DMAC_BASE + 0x400, 0);
    for (int i = 0; i < 8; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dmac), i, gic_in(gic, RZA1H_DMAC_CH0_IRQ + 8 + i));
    }
}

void ic705_create_rspi(DeviceState *gic)
{
    for (int ch = 0; ch < 5; ch++) {
        DeviceState *rspi = qdev_new(TYPE_RZA1H_RSPI);

        qdev_prop_set_uint32(rspi, "channel", ch);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(rspi), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(rspi), 0, 0xE800C800 + ch * 0x800, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(rspi), 0, gic_in(gic, 271 + 3 * ch));  /* SPRI */
        sysbus_connect_irq(SYS_BUS_DEVICE(rspi), 1, gic_in(gic, 272 + 3 * ch));  /* SPTI */
    }
    /* DV DSP on RSPI0, Wi-Fi on RSPI1, main DSP on RSPI3, DSP/FPGA flash on RSPI4 */
    ic705_spi_flash_attach(4, getenv("RZA1H_RSPI4_FLASH"));
    ic705_dv_dsp_attach(0);
    ic705_wlan_attach(1);
    ic705_dsp_attach(3);
}
