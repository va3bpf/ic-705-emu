/*
 * IC-705 sub-CPU, STM32L051: state shared by the peripheral files of
 * qemu/ic705/subcpu/. Register offsets and bits follow the STM32L0x1
 * reference manual (RM0377); only what Icom's sub-CPU firmware uses is
 * modelled.
 */
#ifndef HW_ARM_IC705_SUBCPU_H
#define HW_ARM_IC705_SUBCPU_H

#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/arm/armv7m.h"
#include "hw/cpu/cluster.h"
#include "stm32l0.h"

/* NVIC lines (RM0377 vector table) */
enum {
    NVIC_EXTI0_1 = 5, NVIC_EXTI2_3 = 6, NVIC_EXTI4_15 = 7,
    NVIC_DMA1_CH1 = 9, NVIC_DMA1_CH2_3 = 10, NVIC_DMA1_CH4_7 = 11,
    NVIC_ADC = 12, NVIC_TIM2 = 15, NVIC_TIM6 = 17, NVIC_TIM21 = 20, NVIC_TIM22 = 22,
    NVIC_SPI1 = 25, NVIC_USART1 = 27,
};

#define GPIO_PORTS      5               /* A, B, C, D, H */
#define DMA_CHANNELS    7
#define ADC_CHANNELS    19
#define TIMERS          4               /* TIM2, TIM6, TIM21, TIM22 */
#define USART_QUEUE     512

typedef struct SubTimer {
    STM32L0State *soc;
    const char *name;
    int number;                         /* 2, 6, 21, 22 */
    int nvic, apb;
    uint32_t cr1, cr2, smcr, dier, sr, ccmr1, ccmr2, ccer, psc, arr, ccr[4];
    uint32_t base_count;                /* CNT at base_ns */
    int64_t base_ns;
    QEMUTimer *update;
} SubTimer;

typedef struct SubGpio {
    uint32_t moder, otyper, ospeedr, pupdr, odr, afr[2];
    uint32_t driven, level;             /* inputs driven from outside, and their levels */
    uint32_t idr_seen;                  /* IDR at the last edge check */
    qemu_irq out[16];
} SubGpio;

struct STM32L0State {
    SysBusDevice parent_obj;
    CPUClusterState cluster;
    ARMv7MState armv7m;
    MemoryRegion space, flash, flash_at_0, sram, data_eeprom, periph, sysinfo;
    Clock *sysclk, *refclk;
    char *firmware;

    uint32_t rcc[0x60 / 4];
    uint32_t pwr_cr, pwr_csr;
    struct { uint32_t acr, pecr, sr, optr; } flash_if;
    uint32_t syscfg[0x24 / 4];
    struct { uint32_t imr, emr, rtsr, ftsr, pr; } exti;
    SubGpio gpio[GPIO_PORTS];
    SubTimer tim[TIMERS];
    struct {
        uint32_t isr, ier, cr, cfgr1, cfgr2, smpr, chselr, dr, ccr;
        uint16_t level[ADC_CHANNELS];
    } adc;
    struct {
        uint32_t isr, cselr;
        uint32_t ccr[DMA_CHANNELS], cndtr[DMA_CHANNELS], cpar[DMA_CHANNELS],
                 cmar[DMA_CHANNELS];
        uint32_t length[DMA_CHANNELS], item[DMA_CHANNELS];  /* CNDTR at enable, items done */
    } dma;
    struct {
        uint32_t cr1, cr2;
        uint8_t rx, tx;
        bool rx_full, tx_full;
    } spi;
    struct {
        uint32_t cr1, cr2, cr3, brr, isr, rdr;
        uint8_t queue[USART_QUEUE];
        unsigned first, count;
        QEMUTimer *deliver;
        void (*tx_hook)(void *opaque, uint8_t byte);
        void *tx_opaque;
    } usart;
};

/* soc.c */
void sub_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void sub_nvic(STM32L0State *s, int line, bool level);
extern const char sub_port_letter[GPIO_PORTS];

/* rcc.c: clock control and PWR */
uint32_t sub_rcc_read(STM32L0State *s, hwaddr off);
void sub_rcc_write(STM32L0State *s, hwaddr off, uint32_t v);
uint32_t sub_pwr_read(STM32L0State *s, hwaddr off);
void sub_pwr_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_rcc_reset(STM32L0State *s);
uint64_t sub_timer_clock_hz(STM32L0State *s, int apb);

/* flash.c: FLASH interface */
uint32_t sub_flash_read(STM32L0State *s, hwaddr off);
void sub_flash_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_flash_reset(STM32L0State *s);

/* exti.c: SYSCFG and EXTI */
uint32_t sub_syscfg_read(STM32L0State *s, hwaddr off);
void sub_syscfg_write(STM32L0State *s, hwaddr off, uint32_t v);
uint32_t sub_exti_read(STM32L0State *s, hwaddr off);
void sub_exti_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_exti_reset(STM32L0State *s);
void sub_exti_edge(STM32L0State *s, int port, int bit, bool rising);
void sub_exti_update(STM32L0State *s);

/* gpio.c */
uint32_t sub_gpio_read(STM32L0State *s, int port, hwaddr off);
void sub_gpio_write(STM32L0State *s, int port, hwaddr off, uint32_t v);
void sub_gpio_reset(STM32L0State *s);
void sub_gpio_drive(STM32L0State *s, int port, int bit, int level);  /* -1 releases */
int sub_gpio_port(char letter);                                       /* -1 if none */

/* timer.c */
uint32_t sub_timer_read(SubTimer *t, hwaddr off);
void sub_timer_write(SubTimer *t, hwaddr off, uint32_t v);
void sub_timer_reset(SubTimer *t);
void sub_timer_init(STM32L0State *s);

/* adc.c */
uint32_t sub_adc_read(STM32L0State *s, hwaddr off);
void sub_adc_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_adc_reset(STM32L0State *s);
void sub_adc_init(STM32L0State *s);
void sub_adc_trigger(STM32L0State *s, int timer);

/* dma.c */
uint32_t sub_dma_read(STM32L0State *s, hwaddr off);
void sub_dma_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_dma_reset(STM32L0State *s);
int sub_dma_channel(STM32L0State *s, int first, int second, int request);
bool sub_dma_to_memory(STM32L0State *s, int ch, uint32_t value);
bool sub_dma_from_memory(STM32L0State *s, int ch, uint32_t *value);
bool sub_dma_pending(STM32L0State *s, int ch);
void sub_dma_run_transmitters(STM32L0State *s);

/* spi.c */
uint32_t sub_spi_read(STM32L0State *s, hwaddr off);
void sub_spi_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_spi_reset(STM32L0State *s);
void sub_spi_master_send(STM32L0State *s, uint8_t byte);
bool sub_spi_tx_dma(STM32L0State *s);       /* master with TX DMA enabled */

/* usart.c */
uint32_t sub_usart_read(STM32L0State *s, hwaddr off);
void sub_usart_write(STM32L0State *s, hwaddr off, uint32_t v);
void sub_usart_reset(STM32L0State *s);
void sub_usart_init(STM32L0State *s);
void sub_usart_send(STM32L0State *s, uint8_t byte);
void sub_usart_poke(STM32L0State *s);       /* try to deliver queued bytes soon */

#endif
