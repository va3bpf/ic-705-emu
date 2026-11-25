# IC-705 I/O map

One table of who drives what, with the net names of the service manual
(S-15707XZ-C1, page numbers of the owner's PDF). Every row says where it comes
from:

- **[SM]** service manual schematic. Main-CPU rows pair the net, the BGA ball
  and the port label of page 85. The labels sit half a row below their pin;
  the pairing was checked against the three [FW] rows below.
- **[FW]** observed with the stock Icom firmware in the emulator.
- **[VM]** behaviour of the emulator model.
- **[INF]** inferred, not checked.

Sources: p76/p77 block diagrams, p78 front unit, p80 MAIN UNIT (1/8) power,
p85 main CPU (IC701), p86 sub-CPU (IC901, MAIN UNIT 7/8).

## Power tree

```
DCIN ──diode──┐
              ├── VCC ── IC1 TPS62110 ── H5R3V ──┬── IC2 ADP2370 ── H3R3V   always on
battery ─ Q8 ─┘   (EN=VIN)                       └── IC3 ADP2370 ── H1R18V  always on
   (Q8 and the charger, IC2/Q6/Q7 "CTRL BATT", switch the battery)

PWRS (RZ P7_15) ── IC31 +5V (5VA), IC41/IC501/IC201/IC502 DC-DCs (1.8, 1.2,
                   FPGA 1.0 V), IC351/IC411 3.3 V, IC353 2.8 V
```

- Both CPUs run from **H3R3V** [SM p80, p86]: IC901 VDD/VDDA/VDDIO2 and the
  RZ/A1H 3.3 V I/O. IC1 has EN tied to VIN, and IC2/IC3 take EN from their
  input, so the rail is on whenever a battery or external DC is present. No
  CPU holds it on, and nothing else needs to.
- "Off" is: PWRS low (the radio's other rails off), then the RZ/A1H in deep
  standby. The sub-CPU stays powered and sleeps in STOP.

## Front-panel LEDs

Each LED is switched by one transistor on the front unit, from one net
[SM p77, p78]:

| LED | Driver | Net | Source | Evidence |
|---|---|---|---|---|
| POWER | Q81 | PWRDS | RZ **P10_3** (ball AB5), high = on | [SM] [FW]: P10_3 = 1 while on, 0 in deep standby |
| Standby | Q82 | STBYDS | RZ P10_2 (Y6) | [SM]; an input (off) in deep standby [FW] |
| TX | Q101 | TDS | sub **PA12** (B8) | [SM]; sub firmware `0x80002E0` bit 1 |
| RX | Q102 | RDS | sub **PA11** (C8) | [SM]; sub firmware `0x80002E0` bit 0 |
| CHARGE | Q111 | BATTDS | RZ P10_5 (N20) | [SM]; charging is not modelled |

So the POWER, Standby and CHARGE LEDs belong to the main CPU; only TX and RX
belong to the sub-CPU. The remote panel reads the RZ latches from the
`/machine/gpio` property `odr` and the sub-CPU's from `/machine/subcpu` `odr`.

## Main CPU (RZ/A1H, IC701)

| Port | Ball | Net | Dir | Function | Evidence |
|---|---|---|---|---|---|
| P1_0, P1_1 | | (EEPROM SCL/SDA) | io | settings EEPROM; RIIC0 for writes, bit-banged reads | [FW] [VM] |
| P1_6 | A17 | TCON | | | [SM] |
| P1_7 | B16 | SCEN | | | [SM] |
| P3_4/5/6/10/11/12 | | (touch) | io | TSC2046 touch ADC: DCLK, DIN, DOUT, /CS, BUSY, /PENIRQ | [FW] [VM] |
| P3_9 | | VBUS | in | USB power detect; deep-standby cancel source | [SM] [VM] |
| P4_9 | K21 | SCTXS | | to sub PB6 (role unknown) | [SM] |
| P5_2 | A10 | SCSCK | out | SCIF3 clock → sub PB3 (SPI1 SCK) | [SM] [VM] |
| P5_3 | B10 | SCTXD | out | SCIF3 TxD → sub PB5 (SPI1 MOSI); between transfers a GPIO, low = "send me your data" | [SM] [FW] [VM] |
| P5_4 | C11 | SCRXD | in | SCIF3 RxD ← sub PB4 (SPI1 MISO); between transfers a GPIO, low = "sub has data" | [SM] [FW] [VM] |
| P7_9 | K1 | SCACK | in | ← sub PB7, low = sub acknowledges a request | [SM] [FW] [VM] |
| P7_11 | L2 | SCTRG | out | → sub PC13 (EXTI13), low = transfer request; wakes the sub from STOP | [SM] [FW] [VM] |
| P7_15 | N3 | PWRS | out | high = radio supply rails on | [SM] [FW]: 1 on, 0 off |
| P8_0/1, P8_8/9, P8_10/11 | | (encoders) | in | TWIN PBT, AF/RF-SQL, MULTI quadrature encoders | [FW] [VM] |
| P8_7 | U4 | PWRK | in | POWER key, low = pressed, 47k pull-up to H3R3VM; deep-standby cancel factor 10 | [SM] [FW] [VM] |
| P10_2 | Y6 | STBYDS | out | Standby LED (Q82) | [SM] |
| P10_3 | AB5 | PWRDS | out | POWER LED (Q81), high = on | [SM] [FW] |
| P10_5 | N20 | BATTDS | out | CHARGE LED (Q111) | [SM] |
| P10_6 | N21 | RESL | out | reset line, high while on (IC111 RESET on p80) | [SM] [FW] |
| P10_7 | M22 | SCRES | out | sub-CPU reset (IC901 NRST side) | [SM], [INF] for the target pin |
| SCIF3 | | | | clock-synchronous SPI master of the sub's SPI1 | [FW] [VM] |
| MTU2 ch1 | | (main dial) | in | phase counting | [FW] [VM] |
| ADC ch4–7 | | (MIC UP/DN, V/M, ladders) | in | see [BOARD.md](BOARD.md) | [FW] [VM] |

## Sub-CPU (STM32L051R8H6, IC901) [SM p86]

| Pin | Ball | Net | Function |
|---|---|---|---|
| PA0, PA1, PC4, PC5 | | KI0..KI3 | front-panel key ladders (ADC ch0, ch1, ch14, ch15); PC5 = KI3 read from the schematic, the others [INF] |
| PA8 | D7 | FP_CLK | front-unit serial clock |
| PA9 | C7 | FP_TXD | front-unit serial data |
| PA10 | C6 | FP_STB | front-unit serial strobe |
| PA11 | C8 | RDS | RX LED (Q102) |
| PA12 | B8 | TDS | TX LED (Q101) |
| PA13, PA14 | A8, A7 | SWDIO, SWCLK | debug |
| PA15 | A6 | BKLV | backlight level |
| PB3 | A5 | SCSCK | SPI1 SCK ← RZ P5_2 |
| PB4 | A4 | SCRXD | SPI1 MISO → RZ P5_4; GPIO "I have data" between transfers |
| PB5 | C4 | SCTXD | SPI1 MOSI ← RZ P5_3; GPIO "send" request between transfers |
| PB6 | D3 | SCTXS | ← RZ P4_9 |
| PB7 | C3 | SCACK | → RZ P7_9, low = acknowledge |
| PB8, PB9 | B3, A3 | IMS0, IMS1 | |
| PC0, PC1, PC2 | E3, E2, F2 | IMR0..IMR2 | |
| PB14, PB15 | F8, F7 | IMS6, IMS7 | |
| PC9 | D8 | PCK | |
| PC10 | B7 | PDAT | serial data, with PLSTB |
| PC11 | B6 | PLSTB | strobe; toggles in bursts right after power on [VM] |
| PC12 | C5 | RESPL | to MAIN UNIT 1/8 and the PA unit (J51 pin 33): a reset line, **not an LED** |
| PC13 | A2 | SCTRG | EXTI13 ← RZ P7_11, request and STOP wake |
| PC14 | A1 | EXTSPK | |
| PC15 | B1 | XFCK | XFC key, low = pressed |
| PH1 | D1 | BTHOP | Bluetooth module power |
| PH0 | C1 | BT_TEST | |

## ARM ↔ sub-CPU link

Wiring as above. One transfer [FW] [VM]:

1. ARM drives SCTRG (P7_11) low. The sub wakes on EXTI13, arms SPI1 DMA and
   pulls SCACK (P7_9) low.
2. ARM clocks the frame out on SCIF3 (synchronous mode, TE only).
3. ARM releases SCTRG; the sub releases SCACK and acts on the frame.

The sub announces data by pulling SCRXD (P5_4) low; the ARM fetches it with
SCTXD (P5_3) low and RE-only clocking. Frames: `fb <cmd> <cmd> fe` commands
and `fc <index> <bytes…> fe` writes into the sub's register block. Seen at
power on: `fb a4`, `fc 02 04`, `fc 0c 3f`, `fb a3`, `fb a1`, `fc 02 04`,
`fc 00 …` (24 bytes). At power off: `fc 00 00 00 00`, `fb a3`. After the
power-on block the sub raises PC11 bursts and PC12 (RESPL); after the off
frames it drops them.

## Corrections to earlier notes

- The panel used to show the POWER LED from sub PC12 and CHARGING from sub
  PC6. PC12 is RESPL and the LEDs come from PWRDS and BATTDS on the RZ.
  `examples/power` lit "POWER" only because it sent the sub the RESPL frames.
- Earlier notes called P5_3/P5_4 "RX request" and "sub has data" lines. They
  are SCIF3 TxD/RxD (SCTXD/SCRXD) that both sides use as GPIOs between
  transfers.
