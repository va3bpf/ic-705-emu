/*
 * IC-705 GNSS receiver on SCIF2 (gnss.c): a Sony CXD56xx-style command
 * interface. The host sends "@CMD args\r\n" and waits for "[CMD] Done\r\n"
 * (reply table FW 0x2030C173). At power-off the firmware sends @GSTP (stop
 * positioning) and @BUP (save backup data) and waits up to 5000 ticks for
 * each [FW 0x20123714]; unanswered, every power-off took ~10 s longer.
 *
 * Every command is acknowledged 10 ms later. There is no NMEA output, so no
 * position fix. RZA1H_IC705_GPS=off leaves the receiver silent.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

static struct {
    DeviceState *scif;
    QEMUTimer *timer;
    char line[64];
    int len;
    char reply[80];
    int reply_len;
} gnss;

static void gnss_reply(void *opaque)
{
    rza1h_debug("gps", "GNSS -> host: %.*s", gnss.reply_len - 2, gnss.reply);
    for (int i = 0; i < gnss.reply_len; i++) {
        rza1h_scif_rx_byte(gnss.scif, gnss.reply[i]);
    }
}

static void gnss_rx(void *opaque, uint8_t byte)
{
    int name_len;

    if (byte == '@') {
        gnss.len = 0;
    }
    if (byte != '\n' && byte != '\r') {
        if (gnss.len < (int)sizeof(gnss.line) - 1) {
            gnss.line[gnss.len++] = byte;
        }
        return;
    }
    if (byte == '\r' || gnss.len < 2 || gnss.line[0] != '@') {
        return;
    }
    gnss.line[gnss.len] = 0;
    name_len = strcspn(gnss.line + 1, " ,");
    rza1h_debug("gps", "host -> GNSS: %s", gnss.line);
    gnss.reply_len = snprintf(gnss.reply, sizeof(gnss.reply), "[%.*s] Done\r\n",
                              name_len, gnss.line + 1);
    gnss.len = 0;
    timer_mod(gnss.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * SCALE_MS);
}

void ic705_gnss_attach(DeviceState *scif)
{
    const char *e = getenv("RZA1H_IC705_GPS");

    if (e && !strcmp(e, "off")) {
        return;
    }
    gnss.scif = scif;
    gnss.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gnss_reply, NULL);
    rza1h_scif_set_tx_hook(scif, gnss_rx, NULL);
}
