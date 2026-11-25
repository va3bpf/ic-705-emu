/*
 * Device models written by this project (IC-705 board and RZ/A1H blocks),
 * one file per device.
 * See qemu/ic705/README.md.
 */
#ifndef HW_ARM_IC705_IC705_H
#define HW_ARM_IC705_IC705_H

#include "hw/i2c/i2c.h"

/* charger.c: TI BQ25713 battery charger, I2C slave at 0x6B on RIIC1 */
#define TYPE_BQ25713 "ic705-bq25713"

/* eeprom.c: 24M02 settings EEPROM, 0x50..0x53 on RIIC0 */
#define TYPE_IC705_EEPROM "ic705-24m02"
void ic705_eeprom_create(I2CBus *riic0);
/* Bit-banged access on the RIIC0 pins: bus levels in (true = high), the level
 * the EEPROM drives on SDA out (true = released). */
bool ic705_eeprom_bitbang(bool scl, bool sda);

/* dmac_ch8_15.c: RZ/A1H DMAC channels 8..15, mapped at 0xE8200400 */
#define TYPE_RZA1H_DMAC_HI "rza1h-dmac-ch8-15"
uint32_t ic705_dma_unit(uint32_t cfg);
void ic705_dma_copy(uint32_t src, uint32_t dst, uint32_t count, uint32_t cfg);

/* dmac_ch0_7.c: the IC-705's transfers on DMAC channels 1..7; dmac.c asks
 * first and, on true, leaves the channel to it. dmac.c: rza1h_dmac_done()
 * ends a channel (END + TC, DMAINT unless DEM). */
bool ic705_dmac_start(int ch, uint32_t src, uint32_t dst, uint32_t count, uint32_t cfg);
void rza1h_dmac_done(int ch);

/* mtu2_timer.c: RZ/A1H MTU2 channels 0..4 at 0xFCFF0000; IRQ output
 * channel * 5 + event (A, B, C, D compare match, overflow). QOM "dial" moves
 * the phase-counting channel (main dial). */
#define TYPE_RZA1H_MTU2G "rza1h-mtu2-timer"
int rza1h_mtu2_irq_id(int channel, int event);    /* GIC ID, -1 if absent */

/* rspi.c: RZ/A1H RSPI channels 0..4 at 0xE800C800 + 0x800 * n; IRQ outputs
 * SPRI, SPTI. One SPI device per channel: transfer() exchanges a byte
 * (frame_start = first byte after an SPCR write), deselect() is called when
 * SPE goes from 1 to 0. Either may be NULL. */
#define TYPE_RZA1H_RSPI "rza1h-rspi"
typedef struct IC705SpiSlave {
    uint8_t (*transfer)(void *opaque, uint8_t mosi, bool frame_start);
    void (*deselect)(void *opaque);
    void *opaque;
} IC705SpiSlave;
void rza1h_rspi_attach(int channel, const IC705SpiSlave *slave);
/* a DMA channel starts (true) or stops (false) reading SPDR of RSPI @ch */
void rza1h_rspi_dma_rx(int ch, bool on);

/* SPI devices of the IC-705 board */
void ic705_spi_flash_attach(int channel, const char *file);   /* spi_flash.c */
const uint8_t *ic705_spi_flash_data(void);                    /* NULL if absent */
void ic705_dsp_attach(int channel);                           /* dsp.c */
void ic705_dv_dsp_attach(int channel);                        /* dv_dsp.c */
void ic705_wlan_attach(int channel);                          /* wlan.c */

/* board.c: the IC-705 board, called by rz_a1h.c */
typedef struct MachineState MachineState;
typedef struct ArchCPU ARMCPU;
typedef struct STM32L0State STM32L0State;
void ic705_board_init(MachineState *machine, ARMCPU *cpu, DeviceState *gic,
                      DeviceState *gpio, DeviceState *scif3);
void ic705_create_mtu2(DeviceState *gic);
void ic705_create_dmac_ch8_15(DeviceState *gic);
void ic705_create_rspi(DeviceState *gic);
void ic705_set_pin(int port, int bit, int level);  /* drive an RZ input pin */
void ic705_i2c_devices(int channel, I2CBus *bus);  /* riic.c: populate a bus */
bool ic705_i2c_write_cycle(int channel);

/* adc.c: RZ/A1H ADC as the IC-705 uses it (QOM "vals", RZA1H_ADC_VALS) */
#define TYPE_IC705_ADC "ic705-adc"

/* power.c: deep standby and its PWRK / VBUS wake-ups */
void ic705_power_init(ARMCPU *cpu);
void ic705_power_pin(int port, int bit, int level);
void ic705_power_reset(void);                   /* DSFR after a machine reset */

/* touch.c: TSC2046 touch controller on port 3, machine property "touch" */
void ic705_touch_init(Object *machine);
void ic705_touch_pin(int bit, int level);
void ic705_touch_set(bool down, unsigned x, unsigned y);

/* subcpu_link.c: SCIF3 and the handshake lines to the sub-CPU */
void ic705_subcpu_link_init(STM32L0State *sub, DeviceState *scif3);
bool ic705_subcpu_link_pin(int port, int bit, int level);
STM32L0State *ic705_subcpu(void);               /* NULL without sub firmware */

/* gnss.c: GNSS receiver on SCIF2 */
void ic705_gnss_attach(DeviceState *scif);

/* panel_input.c: front panel from the QEMU window */
void ic705_panel_input_init(DeviceState *dev);

#endif
