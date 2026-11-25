/*
 * Sub-CPU SPI1 (0x40013000). In the IC-705 it is a slave clocked by the
 * RZ/A1H's SCIF3 (subcpu_link.c); as a master its device is not known and
 * reads 0xFF.
 *
 *   +0x00 CR1 (MSTR 2, SPE 6)  +0x04 CR2 (RXDMAEN 0, TXDMAEN 1, RXNEIE 6,
 *   TXEIE 7)  +0x08 SR (RXNE 0, TXE 1)  +0x0C DR
 *
 * Slave (SPE set, MSTR clear): a byte written to DR waits for the master's
 * next byte, which takes it (or the TX DMA channel's next item) and leaves
 * its own byte in DR or in the RX DMA channel. Master: a byte written to DR
 * (or fed by TX DMA) is exchanged at once.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define CR1_MSTR    BIT(2)
#define CR1_SPE     BIT(6)
#define CR2_RXDMAEN BIT(0)
#define CR2_TXDMAEN BIT(1)
#define CR2_RXNEIE  BIT(6)
#define CR2_TXEIE   BIT(7)

static void spi_update_irq(STM32L0State *s)
{
    sub_nvic(s, NVIC_SPI1, ((s->spi.cr2 & CR2_RXNEIE) && s->spi.rx_full) ||
                           ((s->spi.cr2 & CR2_TXEIE) && !s->spi.tx_full));
}

static bool is_slave(STM32L0State *s)
{
    return (s->spi.cr1 & CR1_SPE) && !(s->spi.cr1 & CR1_MSTR);
}

bool sub_spi_tx_dma(STM32L0State *s)
{
    return (s->spi.cr2 & CR2_TXDMAEN) &&
           (s->spi.cr1 & (CR1_MSTR | CR1_SPE)) == (CR1_MSTR | CR1_SPE);
}

static int tx_channel(STM32L0State *s)
{
    return s->spi.cr2 & CR2_TXDMAEN ? sub_dma_channel(s, 2, 2, 1) : -1;
}

/* A byte arrived from the bus: to the RX DMA channel, else into DR. */
static void received(STM32L0State *s, uint8_t byte)
{
    int ch = s->spi.cr2 & CR2_RXDMAEN ? sub_dma_channel(s, 1, 1, 1) : -1;

    if (!sub_dma_to_memory(s, ch, byte)) {
        s->spi.rx = byte;
        s->spi.rx_full = true;
    }
}

void sub_spi_master_send(STM32L0State *s, uint8_t byte)
{
    sub_log("spi1 tx %02x", byte);
    received(s, 0xFF);
    spi_update_irq(s);
}

uint32_t sub_spi_read(STM32L0State *s, hwaddr off)
{
    switch (off) {
    case 0x00:
        return s->spi.cr1;
    case 0x04:
        return s->spi.cr2;
    case 0x08:
        return (s->spi.tx_full ? 0 : BIT(1)) | (s->spi.rx_full ? BIT(0) : 0);
    case 0x0C:
        s->spi.rx_full = false;
        spi_update_irq(s);
        return s->spi.rx;
    default:
        return 0;
    }
}

void sub_spi_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    switch (off) {
    case 0x00:
    case 0x04:
        if (off == 0x00) {
            s->spi.cr1 = v;
        } else {
            s->spi.cr2 = v;
        }
        sub_log("SPI1 CR%d = %04x", off == 0x00 ? 1 : 2, v);
        sub_dma_run_transmitters(s);
        break;
    case 0x0C:
        if (is_slave(s)) {
            s->spi.tx = v;                      /* goes out on the master's next byte */
            s->spi.tx_full = true;
        } else {
            sub_spi_master_send(s, v);
        }
        break;
    }
    spi_update_irq(s);
}

void sub_spi_reset(STM32L0State *s)
{
    s->spi.cr1 = s->spi.cr2 = 0;
    s->spi.rx_full = s->spi.tx_full = false;
}

/* ------------------------------------------------- board-facing interface */

bool stm32l0_spi_slave_tx_pending(STM32L0State *s)
{
    return is_slave(s) && (s->spi.tx_full || sub_dma_pending(s, tx_channel(s)));
}

uint8_t stm32l0_spi_slave_xfer(STM32L0State *s, uint8_t mosi)
{
    uint32_t miso = 0xFF;

    if (!is_slave(s)) {
        return 0xFF;
    }
    if (!sub_dma_from_memory(s, tx_channel(s), &miso) && s->spi.tx_full) {
        miso = s->spi.tx;
        s->spi.tx_full = false;
    }
    received(s, mosi);
    sub_log("spi1 slave in %02x out %02x", mosi, (uint8_t)miso);
    spi_update_irq(s);
    return miso;
}
