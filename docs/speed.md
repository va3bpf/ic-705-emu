# How boot got fast

The history of the speed work, newest last. Current numbers are in the README.

Boot time to the main screen on an 8-core WSL2 host, one instance running:

| QEMU | LCD first lit | main screen |
|---|---|---|
| unpatched, `-icount shift=1` | ~150 s | >150 s |
| `icount-io.patch`, `--io-wait 0` | ~99 s | ~157 s |
| `icount-io.patch`, `--io-wait 64` | ~73 s | ~139 s |
| `icount-io.patch`, `--io-wait 128` | ~75 s | ~119 s |
| + FPGA configuration and DSP boot acknowledge | ~36 s | ~83 s |
| + event-driven timers, runnable-CPU slices | ~14 s | ~28 s |
| + BQL-free GPIO reads | ~16 s* | ~30 s* |
| + DSP version answers, sub-CPU at 16 MHz, ASID-only TLB flush, larger caches | ~10 s* | ~22 s* |
| + CPUs side by side in virtual time, 20 us quantum | ~8 s* | ~16–18 s* |
| + port-1 GPIO writes without the BQL, GIC scans only pending IRQs (default) | ~6 s* | ~14 s* |

\* measured with the spectrum scope enabled in the saved settings, which
makes boot slower: the "BQL-free" row's predecessor measures 18 s / 38 s
with the same settings. On the idle main screen guest time now runs at about
53 % of real time (25 % before the last two rows).

Why boot was slow. During boot both CPUs poll GPIO registers in tight loops
while the firmware waits for chips that are not emulated: the FPGA
configuration pins (P2_3 nSTATUS, P2_7 CONF_DONE) and the main DSP's ready
line (P7_7). The waits end only by timeout. Under `-icount` a device access
in the middle of a translation block forced `cpu_io_recompile()` on every
access, so guest time ran at about 5% of real time.

`qemu/patches/icount-io.patch` makes two changes to QEMU:

1. **I/O insns get their own TB.** The first time an insn needs
   `cpu_io_recompile()`, QEMU remembers its address and retranslates its TB
   so that the insn ends a TB. Later accesses run without a rewind.
2. **Peripheral bus wait states.** `QEMU_ICOUNT_IO_WAIT=N` (launcher option
   `--io-wait`, default 128) charges N extra instructions of virtual time per
   device access, about 256 ns at `shift=1`, in line with the RZ/A1H
   peripheral bus. Polling loops then reach their timeouts with far fewer
   iterations. The charge stays within the current icount budget, so timers
   fire on time and runs stay deterministic.

Two device models remove the boot timeouts: `qemu/machine/gpio.c` answers
the FPGA passive-serial configuration handshake (`RZA1H_IC705_FPGA=off`
disables it) and `qemu/ic705/dsp.c` acknowledges the main-DSP boot
on P7_7 (`RZA1H_IC705_DSP_READY=off`).

`qemu/patches/icount-rr.patch` also keeps `QEMU_ICOUNT_RR_WEIGHTS`
("w0,w1": share of each round per CPU), which only applies in the shared
time-slice mode (`--quantum 0`).

After boot the radio idles, but the 32 MHz sub-CPU runs a polling main
loop. Three timer models used to fire on every period whether the firmware
watched or not: the SCIF3 receive clock (`rz_a1h.c`, 50 kHz, now only while
SCIF3 is in synchronous receive-only mode), the STM32 timers (`qemu/ic705/subcpu/timer.c`)
and MTU2 (`mtu2g.c`), which now arm a QEMU timer only for events that raise
an interrupt and fill in the status flags when the firmware reads them.
Under `-icount` every timer expiry ends the vCPU's slice, so these cut both
CPUs into slices of a few hundred instructions. `icount-rr.patch` also
divides each round-robin slice among the CPUs that can run, not all of
them: with the main CPU idle, the sub-CPU used to get half the time to the
next timer, then a quarter, and so on. Guest time now runs at about 25 % of
real time on the idle main screen, up from 8 %.

GPIO reads skip the BQL: `icount-io.patch` makes the TCG MMIO path honour
`memory_region_enable_lockless_io()`, and the RZ/A1H GPIO block and the
STM32 peripheral block use it (reads of GPIO state only; writes and every
other access still take the BQL in the device). On the idle main screen
with the spectrum scope on, guest time goes from ~15 % to ~25 % of real
time.

`QEMU_ICOUNT_IO_WAIT` also takes a per-CPU list. The launcher passes
`128,0`: the sub-CPU's single-cycle GPIO is not charged the main CPU's bus
wait states.

**Two CPUs, two clock rates.** Stock QEMU counts every instruction of every
CPU as the same time and splits each stretch of virtual time between them.
The sub-CPU's polling main loop then ran at 250–500 MIPS instead of the
~15 MIPS of a 16 MHz STM32L0 (its firmware selects HSI16), and the main CPU
got only half of the time. Three changes in `icount-rr.patch`:

- `QEMU_ICOUNT_CPU_SCALE="1,32"`: a sub-CPU instruction costs 32 counter
  units (64 ns at `shift=1`).
- `QEMU_ICOUNT_RR_PARALLEL=1`: in each round-robin round both CPUs run over
  the same stretch of virtual time; the clock is rewound to the round's start
  for the second CPU and then left at the furthest one.
- `QEMU_ICOUNT_RR_QUANTUM_NS=20000` caps a round at 20 us, so a request one
  CPU raises reaches the other at most that late or early. 50 us still
  boots; 100 us breaks the sub-CPU SPI exchange and the LCD stays dark.
  `rz_a1h.c` also ends a CPU's turn when it changes a handshake line.

Launcher option `--quantum` (microseconds, default 20; 0 returns to the
shared time slices).

**TCG caches** (`tcg-caches.patch`). The RTOS writes its task number into
CONTEXTIDR on every task switch (PROCID, bits 31:8; the ASID stays 0), and
QEMU flushed the whole TLB and jump cache on every such write, about 6000
times per guest second. QEMU now flushes only when the ASID bits change, as
the architecture requires. The jump cache grows from 4096 to 65536 entries and
the initial softmmu TLB from 256 to 4096 entries: with flushes rare, the TLB
no longer gets a chance to resize.

**GIC** (`arm-gic-scan.patch`). QEMU's GICv1/v2 model looked for the
highest-priority pending interrupt by scanning all 320 IRQs on every
interrupt state change (EOI, acknowledge, every device line change). A
bitmap now marks the IRQs whose pending or level state was ever set; the scan
visits only those and drops the ones it finds idle. Reset and migration load
mark every IRQ, so the result is the same as a full scan.

**Bit-banged EEPROM** (`gpio.c`). Writes to GPIO port 1 skip the BQL: the
port only drives the `ic705_ee` state machine in `rz_a1h.c`, with no timers,
IRQs or other CPU behind it. Pin levels are updated atomically. Register
offsets are decoded from a table built once, and the hot paths skip QOM cast
checks.

**TB page protection** (`icount-io.patch`). Each new translation block
write-protects its page in every MMU mode's TLB. With the larger TLB that
scan cost a tenth of boot time; modes with no entries since their last flush
are now skipped (9.8 % -> 0.9 % of the boot-time EEPROM phase).

Tried and not kept: charging GPIO accesses less guest time than the 128-insn
bus default (a faster "GPIO clock"). It shortens the EEPROM phase in guest
time, but nearly all of that phase is the firmware's own bit-banging, so
wall-clock boot time did not change measurably.

Where the time goes now: during boot (guest seconds 0–6.5) the firmware reads
the EEPROM by bit-banging P1_0/P1_1, about 4 M GPIO accesses. On the idle
screen it services ~17 000 timer interrupts per guest second (MTU2 ch3/ch4 are
rescheduled by the firmware every ~300 us, OSTM, the sub-CPU's timers) and
runs ~95 M instructions; QEMU's per-instruction and per-access overhead is
what keeps it below real time.

Running without `-icount` (MTTCG, `-accel tcg,thread=multi -smp
1,maxcpus=2`) starts, but the firmware waits only 200 us of real time for
the sub-CPU to answer a request. Host thread wake-up and BQL hand-off in
WSL2 take longer than that, so the firmware reports a sub-CPU error.

See `docs/emulator.html` for how the emulator works.

Avoid running several instances at once: each uses one core, and they slow
each other down.
