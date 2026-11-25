/*
 * IC-705 sub-CPU: an ST STM32L051 (Cortex-M0+) running Icom's own sub-CPU
 * firmware (qemu/ic705/subcpu/). This is what the board (board.c,
 * subcpu_link.c, panel_input.c) uses.
 *
 * GPIO lines: named qdev inputs "pa".."ph" and outputs "pa-out".."ph-out",
 * 16 each. QOM properties: "adc" (ADC channel levels, "ch=val,..." 12-bit),
 * "odr" (output registers, read only), "pins" (drive inputs, "C15=0",
 * "C15=z" releases), "firmware" (image path, at creation).
 */
#ifndef HW_ARM_IC705_STM32L0_H
#define HW_ARM_IC705_STM32L0_H

#include "qom/object.h"

#define TYPE_STM32L0 "stm32l0-soc"
OBJECT_DECLARE_SIMPLE_TYPE(STM32L0State, STM32L0)

/* USART1: bytes from the board in, bytes the firmware sends out to @fn */
void stm32l0_usart_rx(STM32L0State *s, uint8_t byte);
void stm32l0_set_usart_tx_hook(STM32L0State *s, void (*fn)(void *, uint8_t), void *opaque);

/* GPIO: drive an input pin (@port 'A'..'D', 'H'); read an output register */
void stm32l0_set_pin(STM32L0State *s, char port, int bit, int level);
uint32_t stm32l0_get_odr(STM32L0State *s, char port);

/* ADC: the level on channel @ch, 12-bit full scale */
void stm32l0_set_adc(STM32L0State *s, int ch, uint16_t value);

/* SPI1 as slave: the board's master clocks a byte in and gets the byte out */
uint8_t stm32l0_spi_slave_xfer(STM32L0State *s, uint8_t mosi);
bool stm32l0_spi_slave_tx_pending(STM32L0State *s);

#endif
