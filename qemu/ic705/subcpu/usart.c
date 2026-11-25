/*
 * Sub-CPU USART1 (0x40013800): the line to the RZ/A1H's SCIF3 in
 * asynchronous mode, used by the sub-CPU's boot loader.
 *
 *   +0x00 CR1 (UE 0, RE 2, IDLEIE 4, RXNEIE 5, TCIE 6, TXEIE 7)
 *   +0x04 CR2  +0x08 CR3 (DMAR 6, DMAT 7)  +0x0C BRR    stored
 *   +0x1C ISR  IDLE 4, RXNE 5, TC 6, TXE 7; TEACK and REACK always 1
 *   +0x20 ICR  write 1 to clear  +0x24 RDR  +0x28 TDR
 *
 * A byte written to TDR (or fed by DMA) goes to the board at once and TXE
 * and TC stay set. Bytes from the board queue up and are delivered 20 us
 * after the queue is touched (a byte arriving, RDR read, any register
 * write), while UE and RE are set: into RDR when it is free, or to the RX
 * DMA channel. When the queue runs empty IDLE is set.
 */
#include "qemu/osdep.h"
#include "subcpu.h"

#define CR1_UE      BIT(0)
#define CR1_RE      BIT(2)
#define CR3_DMAR    BIT(6)
#define ISR_IDLE    BIT(4)
#define ISR_RXNE    BIT(5)
#define ISR_TC      BIT(6)
#define ISR_TXE     BIT(7)
#define ISR_TEACK   BIT(21)
#define ISR_REACK   BIT(22)
#define DELIVER_NS  20000

static void usart_update_irq(STM32L0State *s)
{
    uint32_t enabled = s->usart.cr1 & (ISR_IDLE | ISR_RXNE | ISR_TC | ISR_TXE);

    sub_nvic(s, NVIC_USART1, s->usart.isr & enabled);
}

void sub_usart_poke(STM32L0State *s)
{
    timer_mod(s->usart.deliver, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DELIVER_NS);
}

void sub_usart_send(STM32L0State *s, uint8_t byte)
{
    sub_log("usart1 tx %02x", byte);
    if (s->usart.tx_hook) {
        s->usart.tx_hook(s->usart.tx_opaque, byte);
    }
    s->usart.isr |= ISR_TXE | ISR_TC;
    usart_update_irq(s);
}

static void deliver(void *opaque)
{
    STM32L0State *s = opaque;

    if (!(s->usart.cr1 & CR1_UE) || !(s->usart.cr1 & CR1_RE)) {
        return;
    }
    while (s->usart.count) {
        uint8_t byte = s->usart.queue[s->usart.first];
        int ch = s->usart.cr3 & CR3_DMAR ? sub_dma_channel(s, 2, 4, 3) : -1;

        if (ch >= 0) {
            if (!sub_dma_to_memory(s, ch, byte)) {
                break;
            }
        } else if (s->usart.isr & ISR_RXNE) {
            break;                              /* the last byte is still unread */
        } else {
            s->usart.rdr = byte;
            s->usart.isr |= ISR_RXNE;
        }
        s->usart.first = (s->usart.first + 1) % USART_QUEUE;
        s->usart.count--;
        sub_log("usart1 rx %02x", byte);
    }
    if (!s->usart.count) {
        s->usart.isr |= ISR_IDLE;
    }
    usart_update_irq(s);
}

uint32_t sub_usart_read(STM32L0State *s, hwaddr off)
{
    switch (off) {
    case 0x00:
        return s->usart.cr1;
    case 0x04:
        return s->usart.cr2;
    case 0x08:
        return s->usart.cr3;
    case 0x0C:
        return s->usart.brr;
    case 0x1C:
        return s->usart.isr | ISR_TEACK | ISR_REACK;
    case 0x24:
        s->usart.isr &= ~ISR_RXNE;
        usart_update_irq(s);
        sub_usart_poke(s);
        return s->usart.rdr;
    default:
        return 0;
    }
}

void sub_usart_write(STM32L0State *s, hwaddr off, uint32_t v)
{
    switch (off) {
    case 0x00:
        s->usart.cr1 = v;
        break;
    case 0x04:
        s->usart.cr2 = v;
        break;
    case 0x08:
        s->usart.cr3 = v;
        sub_dma_run_transmitters(s);
        break;
    case 0x0C:
        s->usart.brr = v;
        break;
    case 0x20:
        s->usart.isr &= ~(v & 0x1F5F);
        break;
    case 0x28:
        sub_usart_send(s, v);
        break;
    }
    usart_update_irq(s);
    sub_usart_poke(s);
}

void sub_usart_reset(STM32L0State *s)
{
    s->usart.cr1 = s->usart.cr2 = s->usart.cr3 = s->usart.brr = 0;
    s->usart.isr = ISR_TXE | ISR_TC;
    s->usart.first = s->usart.count = 0;
}

void sub_usart_init(STM32L0State *s)
{
    s->usart.deliver = timer_new_ns(QEMU_CLOCK_VIRTUAL, deliver, s);
}

/* ------------------------------------------------- board-facing interface */

void stm32l0_usart_rx(STM32L0State *s, uint8_t byte)
{
    if (s->usart.count < USART_QUEUE - 1) {
        s->usart.queue[(s->usart.first + s->usart.count++) % USART_QUEUE] = byte;
    }
    sub_usart_poke(s);
}

void stm32l0_set_usart_tx_hook(STM32L0State *s, void (*fn)(void *, uint8_t), void *opaque)
{
    s->usart.tx_hook = fn;
    s->usart.tx_opaque = opaque;
}
