/*
 * IC-705 DSP/FPGA configuration flash: a 128 Mbit (16 MiB) ISSI IS25LP128
 * SPI NOR on RSPI4 (spi_flash.c). It holds the DSP group image of the
 * firmware package (RZA1H_RSPI4_FLASH); the firmware streams the main DSP's
 * program from it at boot and its updater can rewrite it.
 *
 * A command is the bytes between a select and a deselect (SPE 1 -> 0):
 *   06 WREN, 04 WRDI          write enable latch on / off
 *   05 RDSR                   status: WEL (bit 1); never busy
 *   9F RDID                   9D 60 18
 *   03 READ, 0B FAST READ     3 address bytes (+1 dummy for 0B), then data
 *   02 PAGE PROGRAM           3 address bytes, then data ANDed into the
 *                             256-byte page, wrapping inside it
 *   20, D8 SECTOR/BLOCK ERASE 4 KiB / 64 KiB to 0xFF
 *   C7, 60 CHIP ERASE         at deselect
 * Program and erase need WEL and clear it. Changes stay in memory.
 */
#include "qemu/osdep.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define FLASH_SIZE  0x1000000
#define FLASH_PAGE  256

static struct {
    uint8_t *data;
    uint8_t cmd;
    int pos;                    /* bytes of the current command so far */
    uint32_t addr;
    bool wel;
} flash;

static void erase(uint32_t addr, uint32_t size)
{
    addr = (addr % FLASH_SIZE) & ~(size - 1);
    memset(flash.data + addr, 0xFF, size);
    rza1h_debug("flash", "erase %06x, %u KiB", addr, size / 1024);
}

static uint8_t flash_transfer(void *opaque, uint8_t mosi, bool frame_start)
{
    int pos = flash.pos++;

    if (pos == 0) {
        flash.cmd = mosi;
        flash.addr = 0;
        if (mosi == 0x06) {
            flash.wel = true;
        } else if (mosi == 0x04) {
            flash.wel = false;
        }
        return 0xFF;
    }
    switch (flash.cmd) {
    case 0x05:
        return flash.wel ? 0x02 : 0x00;
    case 0x9F: {
        static const uint8_t id[3] = { 0x9D, 0x60, 0x18 };
        return pos <= 3 ? id[pos - 1] : 0xFF;
    }
    case 0x03: case 0x0B: case 0x02: case 0x20: case 0xD8:
        if (pos <= 3) {                         /* address, MSB first */
            flash.addr = flash.addr << 8 | mosi;
            if (pos == 3 && (flash.cmd == 0x20 || flash.cmd == 0xD8) && flash.wel) {
                erase(flash.addr, flash.cmd == 0x20 ? 0x1000 : 0x10000);
                flash.wel = false;
            }
            return 0xFF;
        }
        if (flash.cmd == 0x03 || (flash.cmd == 0x0B && pos > 4)) {
            return flash.data[flash.addr++ % FLASH_SIZE];
        }
        if (flash.cmd == 0x02 && flash.wel) {
            uint32_t a = (flash.addr & ~(uint32_t)(FLASH_PAGE - 1)) |
                         ((flash.addr + pos - 4) & (FLASH_PAGE - 1));
            flash.data[a % FLASH_SIZE] &= mosi;
        }
        return 0xFF;
    default:
        return 0xFF;
    }
}

static void flash_deselect(void *opaque)
{
    if (flash.pos) {
        rza1h_debug("flash", "command %02x, address %06x, %d bytes",
                    flash.cmd, flash.addr, flash.pos);
        if (flash.cmd == 0x02) {
            flash.wel = false;
        } else if ((flash.cmd == 0xC7 || flash.cmd == 0x60) && flash.wel) {
            memset(flash.data, 0xFF, FLASH_SIZE);
            flash.wel = false;
        }
    }
    flash.pos = 0;
}

void ic705_spi_flash_attach(int channel, const char *file)
{
    static const IC705SpiSlave slave = {
        .transfer = flash_transfer,
        .deselect = flash_deselect,
    };
    gchar *contents = NULL;
    gsize size = 0;

    if (!file) {
        return;
    }
    flash.data = g_malloc(FLASH_SIZE);
    memset(flash.data, 0xFF, FLASH_SIZE);
    if (g_file_get_contents(file, &contents, &size, NULL)) {
        memcpy(flash.data, contents, MIN(size, FLASH_SIZE));
        g_free(contents);
    }
    rza1h_debug("flash", "%s, %zu bytes", file, (size_t)size);
    rza1h_rspi_attach(channel, &slave);
}

const uint8_t *ic705_spi_flash_data(void)
{
    return flash.data;
}
