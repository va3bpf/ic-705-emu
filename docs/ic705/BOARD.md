# IC-705 board behaviour

What the stock firmware (E 1.40) expects from the board, and how the emulator
answers it. Pin assignments are in [IO_MAP.md](IO_MAP.md). Tags: **[FW]**
seen in the firmware, **[VM]** observed in the emulator, **[SM]** service
manual.

## Boot and power

- **Reset.** The RZ/A1H boots from SPI flash, executing in place at
  `0x18000000` (boot mode 3). The flash image is the decoded package: main
  body at 0, resources at `0xC00000`. A loader copied to `0x20004000` unpacks
  the main image to `0x20005000` [FW].
- **Cold start.** With no deep-standby cancel factor set, the firmware goes
  straight back to deep standby and waits in a WFI loop at `0x2007848C` [VM].
  The launcher polls the PC for that address before pressing POWER.
- **Power on.** A low on PWRK (P8_7) while in deep standby is cancel source
  10: the CPU restarts with DSFR bit 10 set, and the firmware takes the full
  bring-up path [FW] [VM]. It raises PWRS (P7_15) and PWRDS (P10_3), wakes
  the sub-CPU over SCTRG and sends it the power-on frames (IO_MAP.md).
- **Settings say "on".** An EEPROM saved while the radio was on makes the
  firmware power itself up after reset, without a key press [VM]. The
  launcher then skips its POWER press.
- **Power off.** Holding POWER about 1.3 s shows "POWER OFF.", sends the
  sub-CPU its off frames, stops the GNSS receiver (`@GSTP`, `@BUP`) and the
  Wi-Fi processor (`sl_Stop`), drops PWRS and PWRDS and enters deep standby
  [FW] [VM]. In the emulator this takes about 7 s from the press, 3 s of it
  the hold.
- **Deep standby in the model.** STBCR1 = `0xC0` (STBY | DEEP) plus a halted
  CPU counts as deep standby. A falling PWRK resets the CPU on its own thread
  and sets DSFR bit 10; VBUS (P3_9) uses the factor in `RZA1H_DSWAKE_BIT`.
  RAM and peripherals keep their state across the reset.

## Adjust (service) mode

At power on the firmware enters adjust mode (version window for M/S/DSP/FPGA)
when ARM ADC channel 6 reads 721..802 and two front-panel status bits are
set [FW]. A default of 0x300 (768) on every channel fell into that window, so
the launcher idles the ladders at 0x3FF.

## Front-panel inputs

| Control | Input the firmware reads | Model |
|---|---|---|
| MENU, FUNCTION, M.SCOPE, QUICK, EXIT, AUTO TUNE, SPEECH, VOX, CALL, RIT/ΔTX, MPAD, SCAN | resistor ladders on sub-CPU ADC channels 0, 1, 14, 15; levels 0x0C0, 0x400, 0x800, 0xC00, open 0xFFF (12-bit) | `qom-set /machine/subcpu adc "0=0x0c0"` |
| XFC | sub-CPU PC15 (XFCK), low = pressed | `qom-set /machine/subcpu pins "C15=0"`, `"C15=z"` releases |
| TWIN PBT, AF/RF-SQL, MULTI | quadrature encoders on P8_0/P8_1, P8_8/P8_9, P8_10/P8_11 (MULTI active low), sampled from the ADC tick | Gray steps through `qom-set /machine/gpio pins` |
| Main dial | MTU2 channel 1 in phase-counting mode; 10 Hz per count in the default step | `qom-set /machine/mtu2 dial N` |
| MIC UP, DN, V/M | ARM ADC channel 6 ladder (10-bit): 160–236, 56–124, 312–397, 509–597 = levels 0–3, released ≥ 836 | `qom-set /machine/adc vals "6=90"` |
| ARM ADC channels 4, 5 | two more ladders (412–880 = levels 1–4, released ≥ 920) | idle 0x3FF |
| Battery | ARM ADC channel 7 (BTVIN, 0.359 × battery voltage, 10-bit of 3.3 V): 0x300 = 6.9 V shows one red bar (low), 0x337 = 7.4 V two bars, 0x391 = 8.2 V full | the launcher sets 0x391; `qom-set /machine/adc vals "7=0x337"` |
| POWER | PWRK, P8_7, low = pressed | `qom-set /machine/gpio pins "8_7=0"` |

Short key presses need about 4 s of wall time between them in the emulator
to be seen separately [VM].

## Touch panel

A TSC2046-class touch ADC bit-banged on port 3 (IO_MAP.md), commands 0xD0 (X)
and 0x90 (Y), 12-bit results, pen-down on /PENIRQ [FW]. With a blank EEPROM's
default calibration, raw 500..3500 maps to about X 443..52 and Y 259..12
pixels [VM]. The model takes raw coordinates from the `touch` machine
property; the GTK window and the remote panel convert mouse pixels.

## Settings EEPROM

A 2 Mbit 24M02-class EEPROM on RIIC0, device addresses 0x50..0x53, two
address bytes, stored as 64-byte records with a primary and a backup copy
[FW]. The firmware writes through the RIIC0 peripheral but reads by
bit-banging the same pins (P1_0 SCL, P1_1 SDA open drain). Both paths share
one image, saved to `RZA1H_IC705_EEPROM` between sessions [VM]. A blank
EEPROM starts in Japanese.

## Other devices

| Device | Bus | What the firmware needs | Model |
|---|---|---|---|
| Battery charger, BQ2571x class | RIIC1 0x6B | 16-bit registers; writes ChargeOption0 at power on | register file |
| RTC, RX-8803 | RIIC1 0x32, interrupt IRQ0 | polled in the standby loops | IRQ0 once per second; the running clock stays 0:00 |
| Wi-Fi, TI SimpleLink NWP | RSPI1, P2_0/P2_1/P2_4 power pins, P2_5 interrupt | init-complete, device version, `sl_Stop` stop-complete | answers the control traffic; no networking. `RZA1H_IC705_WLAN=off` |
| GNSS, Sony CXD56xx style | SCIF2 | `@CMD args` answered by `[CMD] Done` | every command acknowledged after 10 ms; no NMEA. `RZA1H_IC705_GPS=off` |
| Main DSP (SHARC) | RSPI3 | version handshake at boot | version answered. `RZA1H_IC705_DSP_READY=off` |
| DV DSP (C5517) | RSPI0 | version handshake at boot | version answered. `RZA1H_IC705_DV_READY=off` |
| DSP/FPGA image flash | RSPI4, SPI NOR | DSP program read at boot | backed by the package's DSP image |
| FPGA | | configuration handshake | done at once |
| Sub-CPU | SCIF3 + handshake lines | its own firmware (aux2) | STM32L0 model running the decoded image; see IO_MAP.md |

A full-screen RGB565 boot logo would sit at flash `0xFC0000` with the tag
"ICOM INC 2019.12" at `0x18FFFFF0`. The package does not carry it, so the
firmware skips it [FW].

## Not modelled

RF, audio samples, the DSPs' signal processing, the FPGA's logic, Bluetooth,
the SD card, networking and a GNSS fix.
