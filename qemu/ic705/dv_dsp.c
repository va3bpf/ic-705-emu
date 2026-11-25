/*
 * IC-705 digital-voice DSP (TI TMS320C5517) stand-in on RSPI0 (dv_dsp.c).
 * Only the version exchange at boot is answered; RZA1H_IC705_DV_READY=off
 * leaves the link silent.
 *
 * Packets are [L][type][L-2 data bytes][FF] both ways [FW: ARM 0x201FFF08,
 * C5517 0x180F9]. The ARM asks with 03 00 00 FF; the DV DSP answers
 * 16 00 "1.11-000" "1.41-000" 01 01 01 01 FF and holds its GPIO15 (ARM P3_8)
 * high while it has bytes to send. The two version strings are the first
 * two "d.dd-ddd" UTF-16BE strings in the DV-DSP region of the RSPI4 flash
 * (its boot table is at 0x10000).
 */
#include "qemu/osdep.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

static struct {
    uint8_t in[256];
    int nin;
    uint8_t out[64];
    int nout, sent;
} dv;

static void find_versions(char version[2][8])
{
    const uint8_t *flash = ic705_spi_flash_data();
    int found = 0;

    memset(version, '0', 2 * 8);
    for (uint32_t a = 0x10000; flash && found < 2 && a + 16 <= 0x40000; a += 2) {
        const uint8_t *p = flash + a;
        char c[8];
        bool ok = true;

        for (int i = 0; i < 8 && ok; i++) {
            c[i] = p[2 * i + 1];
            ok = p[2 * i] == 0 &&
                 (i == 1 ? c[i] == '.' : i == 4 ? c[i] == '-' : g_ascii_isdigit(c[i]));
        }
        if (ok) {
            memcpy(version[found++], c, 8);
            a += 14;
        }
    }
}

static void packet(const uint8_t *p, int n)
{
    char version[2][8];
    int k = 0;

    if (n != 4 || p[0] != 3 || p[1] != 0) {         /* only the version request */
        return;
    }
    find_versions(version);
    dv.out[k++] = 0x16;
    dv.out[k++] = 0x00;
    memcpy(dv.out + k, version, 16);
    k += 16;
    for (int i = 0; i < 4; i++) {
        dv.out[k++] = 0x01;
    }
    dv.out[k++] = 0xFF;
    dv.nout = k;
    dv.sent = 0;
    ic705_set_pin(3, 8, 1);
    rza1h_debug("dv", "version request: %.8s %.8s", version[0], version[1]);
}

static uint8_t dv_transfer(void *opaque, uint8_t mosi, bool frame_start)
{
    uint8_t miso = 0xFF;

    if (dv.sent < dv.nout) {
        miso = dv.out[dv.sent++];
        if (dv.sent == dv.nout) {
            dv.nout = dv.sent = 0;
            ic705_set_pin(3, 8, 0);
        }
    }
    if (dv.nin == 0) {
        if (mosi >= 2 && mosi <= 0xA5) {            /* a length byte starts a packet */
            dv.in[dv.nin++] = mosi;
        }
    } else {
        dv.in[dv.nin++] = mosi;
        if (dv.nin == dv.in[0] + 1 || dv.nin == (int)sizeof(dv.in)) {
            packet(dv.in, dv.nin);
            dv.nin = 0;
        }
    }
    return miso;
}

void ic705_dv_dsp_attach(int channel)
{
    static const IC705SpiSlave slave = { .transfer = dv_transfer };
    const char *e = getenv("RZA1H_IC705_DV_READY");

    if (!(e && !strcmp(e, "off"))) {
        rza1h_rspi_attach(channel, &slave);
    }
}
