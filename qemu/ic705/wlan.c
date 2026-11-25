/*
 * IC-705 Wi-Fi module stand-in (wlan.c): a TI SimpleLink network processor (CC31xx
 * class) on RSPI1 [service manual, MAIN unit 6/8: WI_SCK/SS/MOSI/MISO =
 * P4_4..7, WI_PWS = P2_0, WI_NHIB = P2_1, WI_NRESET = P2_4, WI_IRQ = P2_5;
 * FW: SimpleLink host driver, sync words at 0x2033D98C, "Abort NWP event"].
 * Without it the host driver never sees the init-complete interrupt, and at
 * power-off the firmware waits ~40 s of guest time for its Wi-Fi stop command.
 *
 * SimpleLink SPI transport: the host writes 21 43 34 12 + opcode(16 LE) +
 * length(16 LE) + payload (padded to 4); to read, the NWP raises IRQ, the
 * host writes 65 87 78 56 and clocks the message out: sync BA DC CD AB,
 * opcode, length (incl. a 4-byte response header), the header (TxPoolCnt
 * etc.), payload. A response's opcode is the command's with bit 15 cleared.
 * Modelled: init complete (0x0008) after power-up, the device version for
 * sl_DevGet (0x8466), a zeroed status-0 response to every other command, and
 * the stop-complete event (0x0073) after a stop command (0x8473).
 * RZA1H_IC705_WLAN=off disables it.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define WL_MSGS 16
#define WL_MSG_MAX 96
static struct {
    bool enabled, on;
    int pins;                       /* bit 0 PWS, 1 NHIB, 4 NRESET */
    QEMUTimer *boot;
    QEMUTimer *kick;                /* raises IRQ once a queued reply is "ready" */
    QEMUTimer *stopped;             /* stop-complete event, some time after the stop reply */
    int64_t ready_at;               /* replies take the NWP ~1 ms of guest time */
    uint8_t msg[WL_MSGS][WL_MSG_MAX];
    int len[WL_MSGS], head, count;
    bool tx;                        /* clocking out msg[head] */
    int txpos;
    uint32_t win;                   /* last four host bytes, little-endian */
    int state;                      /* 0 idle, 1 header, 2 payload */
    uint8_t hdr[4];
    int got, need;
    uint16_t op;
    uint8_t pay[256];               /* command payload */
    uint8_t role;                   /* set by sl_WlanSetMode, reported at init complete */
} wl;

/*
 * IRQ while a message waits and the NWP has had time to produce it. Raising it
 * in the middle of the host's command write (the reply "instantly") lands in
 * the host driver before it waits for the reply, and the reply is lost.
 */
static void wl_irq(void)
{
    bool pending = wl.on && wl.count > 0 && !wl.tx;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (pending && now < wl.ready_at) {
        timer_mod(wl.kick, wl.ready_at);
        pending = false;
    }
    rza1h_ic705_wlan_irq(pending);
}

static void wl_kick(void *opaque)
{
    wl_irq();
}

static void wl_queue(uint16_t op, const uint8_t *payload, int n);

static void wl_stop_done(void *opaque)
{
    /* _BasicResponse_t: status 0, sender. The host matches an async response to the
     * waiting object by this byte; sl_Stop registers its object with SL_MAX_SOCKETS (16)
     * [FW 0x20252274: 0x2025554C(obj, 23, 16)]. */
    uint8_t ok[4] = { 0, 0, 16, 0 };

    if (wl.on) {
        /* SL_OPCODE_DEVICE_STOP_ASYNC_RESPONSE: 0x0073 in this host driver (action table
         * 0x2033D99C, entry 23), 0x0873 in older SimpleLink releases */
        wl_queue(0x0073, ok, sizeof(ok));
    }
}

static void wl_queue(uint16_t op, const uint8_t *payload, int n)
{
    uint8_t *m;
    int len;

    if (wl.count >= WL_MSGS) {
        return;
    }
    m = wl.msg[(wl.head + wl.count) % WL_MSGS];
    n = MIN(n, WL_MSG_MAX - 16);
    memset(m, 0, WL_MSG_MAX);
    /* N2H sync 0xABCDDCBA, no sequence number (the host masks bits 7/15/31 and matches
     * the low bits exactly) */
    m[0] = 0xBA; m[1] = 0xDC; m[2] = 0xCD; m[3] = 0xAB;
    m[4] = op; m[5] = op >> 8;
    /* length counts the 8-byte response header [FW 0x20253510: payload = Len - 8] */
    m[6] = (n + 8); m[7] = (n + 8) >> 8;
    m[8] = 8;                                                  /* TxPoolCnt */
    if (payload) {
        memcpy(m + 16, payload, n);
    }
    len = 16 + ((n + 3) & ~3);
    wl.len[(wl.head + wl.count) % WL_MSGS] = len;
    wl.count++;
    rza1h_debug("wlan", "NWP -> host: opcode %04x, %d bytes", op, n);
    wl_irq();
}

static void wl_booted(void *opaque)
{
    /* Status, chip id, more data. Status is INIT_STA_OK 0x11111111 / INIT_AP_OK
     * 0x33333333 / INIT_P2P_OK 0x55555555 (the host maps the low nibble to the role
     * sl_Start returns; 0 is ROLE_UNKNOWN_ERR). The firmware sets its role with
     * sl_WlanSetMode + restart and checks it. */
    static const uint8_t ok[4] = { 0x11, 0x11, 0x33, 0x55 };    /* by role 0 STA .. 3 P2P */
    uint8_t s = ok[wl.role & 3];
    uint8_t init[12] = { s, s, s, s };

    if (wl.on) {
        wl_queue(0x0008, init, sizeof(init));
    }
}

/* rz_a1h.c board wiring: an ARM output on port 2 changed */
void rza1h_ic705_wlan_pin(int bit, int level)
{
    bool on;

    if (!wl.enabled || (bit != 0 && bit != 1 && bit != 4)) {
        return;
    }
    wl.pins = level ? wl.pins | 1 << bit : wl.pins & ~(1 << bit);
    on = (wl.pins & 0x13) == 0x13;
    if (on == wl.on) {
        return;
    }
    wl.on = on;
    wl.count = wl.head = 0;
    wl.tx = false;
    wl.state = 0;
    rza1h_debug("wlan", "NWP %s", on ? "powered, booting" : "off/hibernating");
    if (on) {
        if (!wl.boot) {
            wl.boot = timer_new_ns(QEMU_CLOCK_VIRTUAL, wl_booted, NULL);
            wl.kick = timer_new_ns(QEMU_CLOCK_VIRTUAL, wl_kick, NULL);
            wl.stopped = timer_new_ns(QEMU_CLOCK_VIRTUAL, wl_stop_done, NULL);
        }
        timer_mod(wl.boot, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50 * SCALE_MS);
    } else if (wl.boot) {
        timer_del(wl.boot);
        timer_del(wl.stopped);
    }
    wl_irq();
}

static void wl_command(uint16_t op, const uint8_t *p, int n)
{
    uint8_t resp[4] = { 0 };        /* _BasicResponse_t: status 0, sender 0 */

    rza1h_debug("wlan", "host -> NWP: opcode %04x, %d bytes %02x %02x %02x %02x %02x %02x %02x %02x",
                op, n, n > 0 ? p[0] : 0, n > 1 ? p[1] : 0, n > 2 ? p[2] : 0, n > 3 ? p[3] : 0,
                n > 4 ? p[4] : 0, n > 5 ? p[5] : 0, n > 6 ? p[6] : 0, n > 7 ? p[7] : 0);
    if (op == 0x8CB4 && n >= 1) {   /* SL_OPCODE_WLAN_SET_MODE: takes effect at restart */
        wl.role = p[0];
    }
    wl.ready_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS;
    if (op == 0x8466 && n >= 8) {   /* SL_OPCODE_DEVICE_DEVICEGET */
        /* _DeviceSetGet_t {Status, DeviceSetId, Option, ConfigLen} (u16 each) echoed
         * with status 0, then ConfigLen bytes of data. */
        uint8_t get[8 + 64] = { 0 };
        int id = p[2] | p[3] << 8, opt = p[4] | p[5] << 8;
        /* the host leaves ConfigLen 0 in the command (it only sizes the receive) */
        int clen = MIN(p[6] | p[7] << 8, 64);

        if (id == 1 && opt == 12) {
            clen = 20;              /* SlVersionFull as this host driver reads it */
        }
        memcpy(get + 2, p + 2, 4);
        get[6] = clen; get[7] = clen >> 8;
        if (id == 1 && opt == 12) {
            /* SL_DEVICE_GENERAL / VERSION: ChipId, FwVersion, PhyVersion, NwpVersion
             * (4 bytes each), RomVersion. The firmware wants exactly these three
             * versions [FW 0x20116878 compares 12 bytes with 0x20116B58] or it shows
             * "WLAN firmware is wrong version" at boot. */
            static const uint8_t ver[16] = {
                0x00, 0x00, 0x00, 0x04,                 /* ChipId (CC3100 class) */
                0x02, 0x00, 0x00, 0x00,                 /* FwVersion */
                0x02, 0x02, 0x00, 0x07,                 /* PhyVersion */
                0x03, 0x11, 0x00, 0x04,                 /* NwpVersion */
            };
            memcpy(get + 8, ver, sizeof(ver));
        }
        wl_queue(op & 0x7FFF, get, 8 + clen);
        return;
    }
    wl_queue(op & 0x7FFF, resp, sizeof(resp));
    if (op == 0x8473) {             /* SL_OPCODE_DEVICE_STOP_COMMAND: done 20 ms later */
        timer_mod(wl.stopped, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * SCALE_MS);
    }
}

static uint8_t wl_transfer(void *opaque, uint8_t tx, bool frame_start)
{
    uint8_t out = 0xFF;

    if (wl.tx) {
        out = wl.msg[wl.head][wl.txpos++];
        if (wl.txpos >= wl.len[wl.head]) {
            wl.tx = false;
            wl.head = (wl.head + 1) % WL_MSGS;
            wl.count--;
            wl_irq();
        }
        return out;
    }
    wl.win = wl.win >> 8 | (uint32_t)tx << 24;
    switch (wl.state) {
    case 0:
        if (wl.win == 0x12344321) {             /* H2N sync: a command follows */
            wl.state = 1;
            wl.got = 0;
        } else if (wl.win == 0x56788765 && wl.on && wl.count) {     /* CNYS: read */
            wl.tx = true;
            wl.txpos = 0;
            wl_irq();
        }
        break;
    case 1:
        wl.hdr[wl.got++] = tx;
        if (wl.got == 4) {
            wl.op = wl.hdr[0] | wl.hdr[1] << 8;
            wl.need = ((wl.hdr[2] | wl.hdr[3] << 8) + 3) & ~3;
            wl.got = 0;
            wl.state = 2;
            if (!wl.need) {
                wl_command(wl.op, NULL, 0);
                wl.state = 0;
            }
        }
        break;
    case 2:
        if (wl.got < (int)sizeof(wl.pay)) {
            wl.pay[wl.got] = tx;
        }
        if (++wl.got >= wl.need) {
            wl_command(wl.op, wl.pay, MIN(wl.need, (int)sizeof(wl.pay)));
            wl.state = 0;
            wl.win = 0;
        }
        break;
    }
    return out;
}

void ic705_wlan_attach(int channel)
{
    static const IC705SpiSlave slave = { .transfer = wl_transfer };
    const char *e = getenv("RZA1H_IC705_WLAN");

    if (e && !strcmp(e, "off")) {
        return;
    }
    wl.enabled = true;
    rza1h_rspi_attach(channel, &slave);
}
