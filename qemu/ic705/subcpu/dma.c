/*
 * Sub-CPU DMA controller DMA1 (0x40020000), channels 1..7 (index 0..6 here).
 *
 *   +0x00 ISR    per channel 4 bits: GIF, TCIF, HTIF, TEIF
 *   +0x04 IFCR   write 1 to clear ISR bits
 *   +0x08 + 20c  CCR (EN 0, TCIE 1, HTIE 2, TEIE 3, DIR 4, CIRC 5, MINC 7,
 *                MSIZE 11:10), then CNDTR, CPAR, CMAR
 *   +0xA8 CSELR  request selection, 4 bits per channel
 *
 * Peripherals move items through sub_dma_to_memory() (peripheral to memory,
 * DIR = 0) and sub_dma_from_memory() (DIR = 1); the peripheral address is
 * implied. Each item advances the memory address when MINC is set and counts
 * CNDTR down: HTIF at half the length, TCIF at the end, where a circular
 * channel starts over. A channel interrupts when one of TCIF/HTIF/TEIF and
 * its enable are both set, on DMA1_CH1, DMA1_CH2_3 or DMA1_CH4_7.
 *
 * Request map used (RM0377 "DMA1 requests"): ADC ch1/ch2 request 0, SPI1 RX
 * ch2 / TX ch3 request 1, USART1 TX ch2/ch4 and RX ch3/ch5 request 3.
 */
#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "subcpu.h"

#define CCR_EN      BIT(0)
#define CCR_DIR     BIT(4)              /* 1 = memory to peripheral */
#define CCR_CIRC    BIT(5)
#define CCR_MINC    BIT(7)
#define ISR_GIF     BIT(0)
#define ISR_TCIF    BIT(1)
#define ISR_HTIF    BIT(2)

static void dma_update_irq(STM32L0State *s)
{
    uint32_t active = 0;

    for (int c = 0; c < DMA_CHANNELS; c++) {
        if (extract32(s->dma.isr, 4 * c, 4) & s->dma.ccr[c] & 0xE) {
            active |= BIT(c);
        }
    }
    sub_nvic(s, NVIC_DMA1_CH1, active & 0x01);
    sub_nvic(s, NVIC_DMA1_CH2_3, active & 0x06);
    sub_nvic(s, NVIC_DMA1_CH4_7, active & 0x78);
}

static unsigned item_size(STM32L0State *s, int c)
{
    return 1u << extract32(s->dma.ccr[c], 10, 2);           /* MSIZE */
}

static hwaddr memory_address(STM32L0State *s, int c)
{
    uint32_t step = s->dma.ccr[c] & CCR_MINC ? item_size(s, c) : 0;

    return s->dma.cmar[c] + s->dma.item[c] * step;
}

static void item_done(STM32L0State *s, int c)
{
    s->dma.item[c]++;
    s->dma.cndtr[c]--;
    if (s->dma.cndtr[c] == s->dma.length[c] / 2) {
        s->dma.isr |= (ISR_HTIF | ISR_GIF) << (4 * c);
    }
    if (s->dma.cndtr[c] == 0) {
        s->dma.isr |= (ISR_TCIF | ISR_GIF) << (4 * c);
        if (s->dma.ccr[c] & CCR_CIRC) {
            s->dma.cndtr[c] = s->dma.length[c];
            s->dma.item[c] = 0;
        }
    }
    dma_update_irq(s);
}

static bool ready(STM32L0State *s, int c, bool to_peripheral)
{
    uint32_t ccr = s->dma.ccr[c];

    return (ccr & CCR_EN) && !!(ccr & CCR_DIR) == to_peripheral && s->dma.cndtr[c];
}

static AddressSpace *cpu_space(STM32L0State *s)
{
    return cpu_get_address_space(CPU(s->armv7m.cpu), 0);
}

bool sub_dma_to_memory(STM32L0State *s, int c, uint32_t value)
{
    uint8_t bytes[4];

    if (c < 0 || !ready(s, c, false)) {
        return false;
    }
    stl_le_p(bytes, value);
    address_space_write(cpu_space(s), memory_address(s, c), MEMTXATTRS_UNSPECIFIED,
                        bytes, item_size(s, c));
    item_done(s, c);
    return true;
}

bool sub_dma_from_memory(STM32L0State *s, int c, uint32_t *value)
{
    uint8_t bytes[4] = { 0 };

    if (c < 0 || !ready(s, c, true)) {
        return false;
    }
    address_space_read(cpu_space(s), memory_address(s, c), MEMTXATTRS_UNSPECIFIED,
                       bytes, item_size(s, c));
    *value = ldl_le_p(bytes);
    item_done(s, c);
    return true;
}

bool sub_dma_pending(STM32L0State *s, int c)
{
    return c >= 0 && s->dma.cndtr[c] > 0;
}

/* The enabled channel among @first and @second (indexes) that serves @request. */
int sub_dma_channel(STM32L0State *s, int first, int second, int request)
{
    for (int c = 0; c < DMA_CHANNELS; c++) {
        if ((c == first || c == second) && (s->dma.ccr[c] & CCR_EN) &&
            extract32(s->dma.cselr, 4 * c, 4) == request) {
            return c;
        }
    }
    return -1;
}

/* Feed the USART1 and SPI1 transmitters from memory until their channels run dry. */
void sub_dma_run_transmitters(STM32L0State *s)
{
    uint32_t item;

    if (s->usart.cr3 & BIT(7)) {                            /* DMAT */
        while (sub_dma_from_memory(s, sub_dma_channel(s, 1, 3, 3), &item)) {
            sub_usart_send(s, item);
        }
    }
    if (sub_spi_tx_dma(s)) {
        while (sub_dma_from_memory(s, sub_dma_channel(s, 2, 2, 1), &item)) {
            sub_spi_master_send(s, item);
        }
    }
}

uint32_t sub_dma_read(STM32L0State *s, hwaddr off)
{
    if (off == 0x00) {
        return s->dma.isr;
    }
    if (off == 0xA8) {
        return s->dma.cselr;
    }
    if (off >= 0x08 && off < 0x08 + DMA_CHANNELS * 20) {
        int c = (off - 0x08) / 20;

        switch ((off - 0x08) % 20) {
        case 0:
            return s->dma.ccr[c];
        case 4:
            return s->dma.cndtr[c];
        case 8:
            return s->dma.cpar[c];
        case 12:
            return s->dma.cmar[c];
        }
    }
    return 0;
}

void sub_dma_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    if (off == 0x04) {
        s->dma.isr &= ~v;
    } else if (off == 0xA8) {
        s->dma.cselr = v;
    } else if (off >= 0x08 && off < 0x08 + DMA_CHANNELS * 20) {
        int c = (off - 0x08) / 20;

        switch ((off - 0x08) % 20) {
        case 0:
            if ((v & CCR_EN) && !(s->dma.ccr[c] & CCR_EN)) {
                s->dma.length[c] = s->dma.cndtr[c];
                s->dma.item[c] = 0;
                sub_log("dma ch%d on cpar=%08x cmar=%08x n=%u ccr=%x sel=%u", c + 1,
                        s->dma.cpar[c], s->dma.cmar[c], s->dma.cndtr[c], v,
                        extract32(s->dma.cselr, 4 * c, 4));
            }
            s->dma.ccr[c] = v;
            sub_dma_run_transmitters(s);
            sub_usart_poke(s);                  /* a receive channel may be ready now */
            break;
        case 4:
            s->dma.cndtr[c] = v & 0xFFFF;
            break;
        case 8:
            s->dma.cpar[c] = v;
            break;
        case 12:
            s->dma.cmar[c] = v;
            break;
        }
    }
    dma_update_irq(s);
}

void sub_dma_reset(STM32L0State *s)
{
    s->dma.isr = s->dma.cselr = 0;
    memset(s->dma.ccr, 0, sizeof(s->dma.ccr));
}
