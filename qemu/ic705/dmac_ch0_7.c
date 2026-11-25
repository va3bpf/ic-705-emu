/*
 * IC-705 transfers on RZ/A1H DMAC channels 1..7 (dmac_ch0_7.c). The channel
 * registers stay in qemu/machine/dmac.c, which models the IC-7300's flows
 * (SSIF audio, SDHI, RSPI2); when it starts a channel on the IC-705 it asks
 * ic705_dmac_start() first. Two kinds of transfer are the IC-705's:
 *
 *  - to or from an RSPI data register: RSPI0 (DV DSP) and RSPI1 (Wi-Fi). A
 *    transmit moves its bytes at once. A receive from SPDR is moved when the
 *    channel completes, after the transmit side (usually another channel
 *    started right after it) has clocked the bytes in; meanwhile the RSPI
 *    keeps its receive interrupt quiet (rza1h_rspi_dma_rx).
 *  - to or from the GPIO block, e.g. the touch-controller command waveform
 *    played into PSR3 on channel 2 [FW 0x2008CECC, completion 0x2008CF34].
 *
 * Completion follows one microsecond per unit (CHCFG.DDS) later, plus one
 * (GPIO) or two (RSPI) microseconds; dmac.c then sets END and TC and raises
 * DMAINT unless CHCFG.DEM masks it.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define CHANNELS    8
#define RSPI0_SPDR  0xE800C804
#define RSPI_STRIDE 0x800
#define GPIO_START  0xFCFE3000
#define GPIO_END    0xFCFE4000

static struct {
    QEMUTimer *done;
    bool receive;               /* move the data at completion */
    int rspi;                   /* RSPI channel being read, or -1 */
    uint32_t src, dst, count, cfg;
} chan[CHANNELS];

/* RSPI channel whose SPDR is at @addr (RSPI0 and RSPI1 only), or -1. */
static int rspi_at(uint32_t addr)
{
    if (addr == RSPI0_SPDR) {
        return 0;
    }
    return addr == RSPI0_SPDR + RSPI_STRIDE ? 1 : -1;
}

static bool in_gpio(uint32_t addr)
{
    return addr >= GPIO_START && addr < GPIO_END;
}

static void channel_done(void *opaque)
{
    int ch = (intptr_t)opaque;

    if (chan[ch].receive) {
        chan[ch].receive = false;
        ic705_dma_copy(chan[ch].src, chan[ch].dst, chan[ch].count, chan[ch].cfg);
        rza1h_rspi_dma_rx(chan[ch].rspi, false);
    }
    rza1h_debug("dmac", "ch%d done", ch);
    rza1h_dmac_done(ch);
}

bool ic705_dmac_start(int ch, uint32_t src, uint32_t dst, uint32_t count, uint32_t cfg)
{
    int rx = rspi_at(src), tx = rspi_at(dst);
    bool gpio = in_gpio(src) || in_gpio(dst);
    uint32_t units;

    if (ch < 1 || ch >= CHANNELS || (rx < 0 && tx < 0 && !gpio)) {
        return false;
    }
    if (!chan[ch].done) {
        chan[ch].done = timer_new_ns(QEMU_CLOCK_VIRTUAL, channel_done, (void *)(intptr_t)ch);
    }
    chan[ch] = (typeof(chan[ch])) {
        .done = chan[ch].done, .receive = rx >= 0, .rspi = rx,
        .src = src, .dst = dst, .count = count, .cfg = cfg,
    };
    rza1h_debug("dmac", "ch%d %08x -> %08x, %u bytes%s", ch, src, dst, count,
                rx >= 0 ? " (receive)" : "");
    if (rx >= 0) {
        rza1h_rspi_dma_rx(rx, true);
    } else {
        ic705_dma_copy(src, dst, count, cfg);
    }
    units = count / ic705_dma_unit(cfg) + (gpio && rx < 0 && tx < 0 ? 1 : 2);
    timer_mod(chan[ch].done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)units * 1000);
    return true;
}
