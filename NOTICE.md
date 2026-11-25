# Sources and licences

This repository combines code from several projects with work of its own.
The table lists where each part came from.

| Path | Origin | Licence |
|---|---|---|
| `qemu/machine/` | [jeskko/ic7300-homebrew](https://github.com/jeskko/ic7300-homebrew) `qemu-machine/src` @ `84fb65b`, with [nemanjan00/ic-705](https://github.com/nemanjan00/ic-705) `qemu/ic705-port.patch` @ `6629078` applied, then extended by this project. See the table below for the share of each file that is still IC-705 port code. | MIT (`qemu/LICENSE`) for the ic7300-homebrew code. The IC-705 port has no stated licence. |
| `qemu/ic705/` | this project: device models (IC-705 board peripherals and rewritten RZ/A1H blocks), one file per device | MIT (`LICENSE`); built into QEMU, whose binary is GPL-2.0-or-later |
| `qemu/patches/hw-arm-build.patch` | this project; the Kconfig entry follows ic7300-homebrew | GPL-2.0-or-later (modifies QEMU) |
| `qemu/patches/icount-io.patch`, `icount-rr.patch`, `tcg-caches.patch`, `arm-gic-scan.patch` | this project | GPL-2.0-or-later (modifies QEMU) |
| `decoder/` | [sq6emm/ic705-ic9700-ic905-bring-your-own-firmware](https://github.com/sq6emm/ic705-ic9700-ic905-bring-your-own-firmware) @ `a91231f`, reduced to the files the launcher uses | MIT (`decoder/LICENSE`) |
| `docs/ic705/IO_MAP.md`, `docs/ic705/BOARD.md` | this project, from the IC-705 service manual, the firmware and tests | MIT (`LICENSE`) |
| `docs/rz-a1h-machine.md` | ic7300-homebrew `qemu-machine/README.md` | MIT |
| `ic705.py`, `ic705_remote.py`, `build-qemu.sh`, `README.md`, `examples/`, `docs/speed.md`, `docs/emulator.html`, `docs/images/` | this project | MIT (`LICENSE`) |
| LVGL, FreeRTOS kernel (fetched by `examples/lvgl/build.sh`, not in the repository) | [lvgl/lvgl](https://github.com/lvgl/lvgl) v9.2.2, [FreeRTOS/FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) V11.1.0 | MIT |

A QEMU binary built from this tree is covered by QEMU's licence
(GPL-2.0-or-later).

## IC-705 port code in `qemu/machine/`

Lines the IC-705 port added to ic7300-homebrew that are still present
(2026-11-24). Counted as substantive lines: blank lines, lone braces and the
like are left out; a line counts if its text is unchanged.

| File | Port lines still present | Lines in file | Share |
|---|---:|---:|---:|
| `rz_a1h.c` (braces, gotos, includes) | 19 | 489 | 4% |
| `scif.c` (braces, function signatures, one-line statements) | 16 | 1303 | 1% |
| `gpio.c` (braces, function signatures) | 14 | 588 | 2% |
| `rza1h_debug.h` (a function signature) | 1 | 85 | 1% |
| **Total** | **55** | **8882** | **1%** |

Rewritten so far, in `qemu/ic705/`: the battery charger (`charger.c`
replaces `bq2571x.c`), the settings EEPROM (`eeprom.c` replaces `m24m02.c`
and the bit-banged reader in `rz_a1h.c`), DMAC channels 8..15
(`dmac_ch8_15.c` replaces `dmac_hi.c`) and the IC-705 transfers on channels
1..7 (`dmac_ch0_7.c`), the MTU2 timer (`mtu2_timer.c` replaces `mtu2g.c`),
the RSPI SPI master with its devices (`rspi.c`, `spi_flash.c`, `dsp.c`,
`dv_dsp.c`, `wlan.c` replace `rspi_gen.c`; its external-socket device, used
only by the removed DSP tools, is gone), the ADC (`adc.c`; `qemu/machine/adc.c`
is ic7300-homebrew's original again), and the IC-705 board code of
`rz_a1h.c` (`board.c`, `power.c`, `touch.c`, `subcpu_link.c`,
`panel_input.c`, `gnss.c`), and the sub-CPU (`subcpu/`, one file per
STM32L051 peripheral, replaces `stm32l0.c`). The port's GPIO, SCIF, RIIC and board-selection
additions in `qemu/machine/` are rewritten in place; its behavioural sub-CPU
(status frames on SCIF3 without the sub-CPU firmware, `RZA1H_IC705_SUBSTAT`)
is removed.

What is left are braces, function signatures and one-line statements.

`qemu/ic705/` was written after reading the port, so it was checked the
same way: 209 of its 3423 substantive lines also occur in the port. They
are QEMU API boilerplate (creating a Cortex-M in a CPU cluster, clocks,
`MemoryRegionOps` fields), `switch (off) { case 0x00:`, register names and
reset values from the reference manuals, required function names of the
interfaces, and facts such as the panel key table and the flash ID bytes.

The other files in `qemu/machine/` contain no port code. This project's own
work there (see the git history) includes running both CPUs side by side in
virtual time, the DSP version answers, the Wi-Fi NWP and GNSS answers, PWRK
on P8_7, deep-standby wake without the sub-CPU, the RZ `odr` property and
the speed work.

**Licence status.** nemanjan00/ic-705 states no licence. This repository
therefore no longer contains its code or documents: its tools and documents
were removed, and its code in `qemu/machine/` was rewritten in `qemu/ic705/`
(see the audit above). Its IC-705 firmware analysis, which this work builds
on, is credited in README.md.

No Icom firmware, decoded images or EEPROM dumps are committed. The
screenshot `docs/images/front-panel.png` shows this project's front-panel
window running `examples/power`, a program of this project.
"Icom" and "IC-705" are trademarks of Icom Inc. This project is not affiliated
with Icom.
