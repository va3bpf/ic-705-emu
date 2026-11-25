/*
 * IC-705 link between the main CPU and the sub-CPU (subcpu_link.c).
 *
 * Wiring (docs/ic705/IO_MAP.md):
 *   RZ SCIF3 SCK/TxD/RxD  <->  sub PB3/PB5/PB4, SPI1 slave (SCSCK/SCTXD/SCRXD)
 *   RZ P7_11 SCTRG  ->  sub PC13 (EXTI13): low = transfer request, wakes STOP
 *   RZ P7_9  SCACK  <-  sub PB7: low = request acknowledged
 *   RZ P5_3  SCTXD  ->  sub PB5: between transfers, low = "send me your data"
 *   RZ P5_4  SCRXD  <-  sub PB4: between transfers, low = "I have data"
 *
 * SCIF3 in clock-synchronous mode is the SPI master: each byte the RZ sends
 * is exchanged with the sub's SPI1 at once, and the reply is received when
 * SCR.RE is set. In receive-only mode (RE set, TE clear) the SCIF keeps
 * clocking by itself; the link then pulls the sub's pending bytes, one per
 * 20 us while fewer than four wait in the SCIF. That clock runs only in that
 * mode: a free-running 50 kHz timer would cut -icount into tiny slices. In
 * asynchronous mode SCIF3 is a UART to the sub's USART1 (its boot loader).
 *
 * Under -icount the two CPUs take turns over the same stretch of virtual
 * time; a handshake edge ends the running CPU's turn so the other one sees
 * it at this moment, not a turn later.
 *
 * Without the sub-CPU firmware (no RZA1H_SUBCPU_FW) only the acknowledge is
 * modelled: SCACK follows SCTRG.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "subcpu/stm32l0.h"
#include "ic705.h"

#define SMR_SYNC    0x80        /* SCIF SMR C/A: clock-synchronous */
#define SCR_TE      0x20
#define SCR_RE      0x10
#define RX_CLOCK_NS 20000

static struct {
    STM32L0State *sub;
    DeviceState *scif;
    QEMUTimer *rx_clock;
} sub_link;

static void kick_other_cpu(void)
{
    if (current_cpu) {
        cpu_exit(current_cpu);
    }
}

static bool receive_only(int *rx_waiting)
{
    uint16_t smr, scr;

    rza1h_scif_get_mode(sub_link.scif, &smr, &scr, rx_waiting);
    return (smr & SMR_SYNC) && (scr & SCR_RE) && !(scr & SCR_TE);
}

static void scif_tx(void *opaque, uint8_t byte)
{
    uint16_t smr, scr;
    int rx_waiting;

    rza1h_scif_get_mode(sub_link.scif, &smr, &scr, &rx_waiting);
    if (!(smr & SMR_SYNC)) {
        stm32l0_usart_rx(sub_link.sub, byte);
        return;
    }
    byte = stm32l0_spi_slave_xfer(sub_link.sub, byte);
    if (scr & SCR_RE) {
        rza1h_scif_rx_byte(sub_link.scif, byte);
    }
}

static void rx_clock_tick(void *opaque)
{
    int rx_waiting;

    if (!receive_only(&rx_waiting)) {
        return;
    }
    if (rx_waiting < 4 && stm32l0_spi_slave_tx_pending(sub_link.sub)) {
        rza1h_scif_rx_byte(sub_link.scif, stm32l0_spi_slave_xfer(sub_link.sub, 0xFF));
    }
    timer_mod(sub_link.rx_clock, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RX_CLOCK_NS);
}

static void scif_mode_changed(void *opaque)
{
    int rx_waiting;

    if (!receive_only(&rx_waiting)) {
        timer_del(sub_link.rx_clock);
    } else if (!timer_pending(sub_link.rx_clock)) {
        timer_mod(sub_link.rx_clock, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RX_CLOCK_NS);
    }
}

/* sub PB4 -> RZ P5_4, sub PB7 -> RZ P7_9 */
static void sub_output(void *opaque, int line, int level)
{
    static int last[2] = { -1, -1 };
    int which = line == 4 ? 0 : 1;

    if (line == 4) {
        rza1h_debug("sub", "sub PB4 -> RZ P5_4 = %d", level);
        ic705_set_pin(5, 4, level);
    } else {
        ic705_set_pin(7, 9, level);
    }
    if (level != last[which]) {
        last[which] = level;
        kick_other_cpu();
    }
}

/* board.c: an RZ output changed; true if it is one of the link's lines */
bool ic705_subcpu_link_pin(int port, int bit, int level)
{
    bool sctrg = port == 7 && bit == 11, sctxd = port == 5 && bit == 3;

    if (!sctrg && !sctxd) {
        return false;
    }
    if (!sub_link.sub) {
        if (sctrg) {
            ic705_set_pin(7, 9, level);             /* no sub firmware: acknowledge */
        }
        return true;
    }
    rza1h_debug("sub", "RZ P%d_%d -> %d", port, bit, level);
    stm32l0_set_pin(sub_link.sub, sctrg ? 'C' : 'B', sctrg ? 13 : 5, level);
    kick_other_cpu();
    return true;
}

STM32L0State *ic705_subcpu(void)
{
    return sub_link.sub;
}

void ic705_subcpu_link_init(STM32L0State *sub, DeviceState *scif3)
{
    sub_link.sub = sub;
    sub_link.scif = scif3;
    sub_link.rx_clock = timer_new_ns(QEMU_CLOCK_VIRTUAL, rx_clock_tick, NULL);
    stm32l0_set_usart_tx_hook(sub, rza1h_scif_rx_byte, scif3);
    rza1h_scif_set_tx_hook(scif3, scif_tx, NULL);
    rza1h_scif_set_mode_hook(scif3, scif_mode_changed, NULL);
    qdev_connect_gpio_out_named(DEVICE(sub), "pb-out", 4, qemu_allocate_irq(sub_output, NULL, 4));
    qdev_connect_gpio_out_named(DEVICE(sub), "pb-out", 7, qemu_allocate_irq(sub_output, NULL, 7));
    stm32l0_set_pin(sub, 'B', 5, 1);                /* RZ request lines idle high */
    stm32l0_set_pin(sub, 'C', 13, 1);
}
