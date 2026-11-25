/*
 * IC-705 main DSP (Analog Devices ADSP-21478 SHARC) stand-in on RSPI3
 * (dsp.c). The DSP's signal processing is not modelled; this answers what
 * the firmware checks at boot so it carries on normally. RZA1H_IC705_DSP_READY
 * =off leaves the link silent.
 *
 * The link is the SHARC's SPI slave: 4-byte big-endian words, one
 * chip-select frame each. The DSP either receives command words or, while
 * its FLAG2 output (ARM P7_6) is high, sends a status frame that the ARM
 * clocks out with FFFFFFFF words; the frame ends with the filler 10000000.
 *
 * Boot [FW 0x2011D3E0]: the ARM sends 0x02000000 | b << 7 | n << 4; n = 1
 * puts the SHARC into SPI boot (P7_7 low), the ARM streams the program from
 * the RSPI4 flash, n = 2 runs it and the ARM waits for P7_7 high (DSP up).
 *
 * Versions [FW: SHARC 0x127326, ARM 0x201195B0 / 0x2011955C]: the SHARC
 * reads three 8-character ASCII tags from the flash: 0x40000 DSP program,
 * 0x180000 FPGA, 0x80000 DSP data. The ARM asks for each half with
 * E000000k (bit 0 = half, bits 2..1 = tag); the reply is F, the 4th
 * character's digit, then the first three characters: "3060" -> F0333036,
 * shown as "3.06" "00". A tag with a 0x00 or 0xFF byte reads as cleared.
 */
#include "qemu/osdep.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define STATUS_QUEUE 16

static struct {
    uint32_t rx_word;           /* command word being received */
    int byte;                   /* byte of the current word, 0..3 */
    uint32_t tx_word;           /* word being shifted out */
    uint32_t status[STATUS_QUEUE];
    int nstatus;
    bool sending;               /* status frame in progress (FLAG2 high) */
    int sent;
} dsp;

static uint32_t version_word(int k)
{
    static const uint32_t tag_at[4] = { 0x40000, 0x180000, 0x80000, 0x40000 };
    const uint8_t *flash = ic705_spi_flash_data();
    uint8_t tag[8] = { 0 };
    const uint8_t *half;

    if (flash) {
        memcpy(tag, flash + tag_at[(k >> 1) & 3], 8);
        for (int i = 0; i < 8; i++) {
            if (tag[i] == 0x00 || tag[i] == 0xFF) {
                memset(tag, 0, 8);
                break;
            }
        }
    }
    half = tag + 4 * (k & 1);
    return 0xF0000000u | ((uint32_t)(half[3] - '0') & 0xF) << 24 |
           (uint32_t)half[0] << 16 | (uint32_t)half[1] << 8 | half[2];
}

static void command(uint32_t w)
{
    if ((w & 0xFFFFFF0Fu) == 0x02000000u) {         /* boot control */
        int n = (w >> 4) & 3;

        if (n == 1 || n == 2) {
            ic705_set_pin(7, 7, n == 2);
            rza1h_debug("dsp", "boot word %08x: P7_7 %d", w, n == 2);
        }
    } else if ((w & 0xFFFFFFF8u) == 0xE0000000u) {  /* version request */
        if (dsp.nstatus < STATUS_QUEUE) {
            dsp.status[dsp.nstatus++] = version_word(w & 7);
            rza1h_debug("dsp", "version request %08x: %08x", w, dsp.status[dsp.nstatus - 1]);
        }
    } else if (w == 0x80000000u && dsp.nstatus) {   /* status request */
        dsp.sending = true;
        dsp.sent = 0;
        ic705_set_pin(7, 6, 1);
    }
}

static uint8_t dsp_transfer(void *opaque, uint8_t mosi, bool frame_start)
{
    uint8_t miso;

    if (frame_start) {
        dsp.byte = 0;
    }
    if (dsp.byte == 0) {
        dsp.tx_word = !dsp.sending ? 0xFFFFFFFFu :
                      dsp.sent < dsp.nstatus ? dsp.status[dsp.sent] : 0x10000000u;
    }
    miso = dsp.tx_word >> (24 - 8 * dsp.byte);
    dsp.rx_word = dsp.rx_word << 8 | mosi;
    if (++dsp.byte < 4) {
        return miso;
    }
    dsp.byte = 0;
    if (!dsp.sending) {
        command(dsp.rx_word);
    } else if (dsp.sent++ >= dsp.nstatus) {         /* filler sent: back to receive */
        dsp.sending = false;
        dsp.nstatus = 0;
        ic705_set_pin(7, 6, 0);
    }
    return miso;
}

void ic705_dsp_attach(int channel)
{
    static const IC705SpiSlave slave = { .transfer = dsp_transfer };
    const char *e = getenv("RZA1H_IC705_DSP_READY");

    if (!(e && !strcmp(e, "off"))) {
        rza1h_rspi_attach(channel, &slave);
    }
}
