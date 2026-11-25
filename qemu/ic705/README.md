# qemu/ic705

Device models written by this project, one file per device: IC-705 board
peripherals and the RZ/A1H blocks it rewrote. The machine in `qemu/machine/`
(ic7300-homebrew's RZ/A1H models) creates and wires them.

| File | Device | Part of | Connection |
|---|---|---|---|
| `board.c` | board wiring: creates the devices, routes GPIO changes to them, RTC 1 Hz IRQ0, Wi-Fi IRQ line | board | |
| `power.c` | POWER key and deep standby: PWRK / VBUS wake-up, DSFR | board | P8_7, P3_9 |
| `touch.c` | TSC2046 touch controller, machine property `touch` | board | P3_4/5/6/10/11/12 |
| `panel_input.c` | front panel from the QEMU window: touch, dial, keys, POWER | board | QEMU input |
| `subcpu_link.c` | link to the sub-CPU: SCIF3 <-> SPI1 and four handshake lines | board | SCIF3, P5_3/P5_4/P7_9/P7_11 |
| `gnss.c` | GNSS receiver: acknowledges `@` commands | board | SCIF2 |
| `charger.c` | TI BQ25713 battery charger (register file) | board | RIIC1, 0x6B |
| `eeprom.c` | 24M02 settings EEPROM, saved to `RZA1H_IC705_EEPROM` | board | RIIC0, 0x50..0x53; bit-banged reads on P1_0/P1_1 |
| `spi_flash.c` | ISSI IS25LP128 16 MiB SPI NOR, DSP/FPGA image (`RZA1H_RSPI4_FLASH`) | board | RSPI4 |
| `dsp.c` | main DSP (ADSP-21478) stand-in: boot acknowledge, version answers | board | RSPI3; FLAG2 = P7_6, ready = P7_7 |
| `dv_dsp.c` | DV DSP (C5517) stand-in: version packet | board | RSPI0; GPIO15 = P3_8 |
| `wlan.c` | TI SimpleLink Wi-Fi NWP: init, version, stop | board | RSPI1; P2_0/1/4 power, P2_5 IRQ |
| `adc.c` | 10-bit ADC: key ladders and sense voltages (QOM `vals`, `RZA1H_ADC_VALS`) | RZ/A1H | 0xE8005800 |
| `dmac_ch0_7.c` | the IC-705's transfers on DMAC channels 1..7 (RSPI, GPIO); registers stay in `qemu/machine/dmac.c` | RZ/A1H | DMAINT1..7 |
| `dmac_ch8_15.c` | DMA controller channels 8..15 | RZ/A1H | 0xE8200400, DMAINT8..15 |
| `mtu2_timer.c` | MTU2 timer channels 0..4, phase counting for the main dial | RZ/A1H | 0xFCFF0000, TGI/TCIV interrupts |
| `rspi.c` | RSPI SPI master channels 0..4; one SPI device per channel | RZ/A1H | 0xE800C800 + 0x800 n, SPRI/SPTI |

`subcpu/` is the sub-CPU, an STM32L051 running Icom's sub-CPU firmware:
`soc.c` (CPU cluster, memory map, firmware, peripheral dispatch, QOM
properties) and one file per peripheral: `rcc.c` (RCC, PWR), `flash.c`,
`exti.c` (SYSCFG, EXTI), `gpio.c`, `timer.c` (TIM2/6/21/22), `adc.c`,
`dma.c`, `spi.c`, `usart.c`. `stm32l0.h` is its interface for the board,
`subcpu.h` the state its files share.

`ic705.h` holds the type names and the functions the machine calls.
`meson.build` lists the files; `build-qemu.sh` links this directory to
`hw/arm/ic705` in the QEMU tree. Board behaviour: docs/ic705/BOARD.md; pins:
docs/ic705/IO_MAP.md.
