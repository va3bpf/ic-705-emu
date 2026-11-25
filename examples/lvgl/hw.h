/* IC-705 main CPU (RZ/A1H) registers used by the demo. Pins: docs/ic705/IO_MAP.md. */
#ifndef HW_H
#define HW_H

#include <stdint.h>
#include <stdbool.h>

#define REG8(a)     (*(volatile uint8_t *)(a))
#define REG16(a)    (*(volatile uint16_t *)(a))
#define REG32(a)    (*(volatile uint32_t *)(a))

/* GPIO: port n */
#define GPIO_P(n)       REG16(0xFCFE3000 + 4 * (n))     /* output latch */
#define GPIO_PPR(n)     REG16(0xFCFE3200 + 4 * (n))     /* pin levels */
#define GPIO_PM(n)      REG16(0xFCFE3300 + 4 * (n))     /* direction, 1 = input */
#define GPIO_PIBC(n)    REG16(0xFCFE7000 + 4 * (n))     /* input buffer enable */

/* GIC */
#define GICD_CTLR       REG32(0xE8201000)
#define GICD_ISENABLER(n) REG32(0xE8201100 + 4 * (n))
#define GICD_IPRIORITYR(id) REG8(0xE8201400 + (id))
#define GICD_ITARGETSR(id)  REG8(0xE8201800 + (id))
#define GICD_ICFGR(n)   REG32(0xE8201C00 + 4 * (n))
#define GICC_CTLR       REG32(0xE8202000)
#define GICC_BPR        REG32(0xE8202008)

/* OS timers: OSTM0 drives the FreeRTOS tick, OSTM1 free-runs as a clock */
#define OSTM_CMP(b)     REG32((b) + 0x00)
#define OSTM_CNT(b)     REG32((b) + 0x04)
#define OSTM_TS(b)      REG8((b) + 0x14)
#define OSTM_TT(b)      REG8((b) + 0x18)
#define OSTM_CTL(b)     REG8((b) + 0x20)
#define OSTM0           0xFCFEC000
#define OSTM1           0xFCFEC400
#define OSTM0_IRQ       134
#define OSTM_HZ         32000000u

/* MTU2 channel 1: phase counting from the main dial */
#define MTU2_TSTR       REG8(0xFCFF0280)
#define MTU2_TMDR_1     REG8(0xFCFF0381)
#define MTU2_TCNT_1     REG16(0xFCFF0386)

/* VDC5 channel 0, graphics plane GR0 */
#define GR0(off)        REG32(0xFCFF7600 + (off))

/* Power: deep standby */
#define STBCR1          REG8(0xFCFE0020)
#define DSSSR           REG16(0xFCFF1802)
#define DSFR            REG16(0xFCFF1808)

#define LCD_W           480
#define LCD_H           272
#define FRAME_BUFFER    ((uint16_t *)0x20800000)

/* display.c, touch.c, dial.c, power.c, ui.c */
void display_init(void);
void display_off(void);
void touch_init(void);
void dial_init(void);
int dial_read_delta(void);
bool power_woken_by_key(void);
void power_on(void);
void power_off_now(void) __attribute__((noreturn));
bool power_key_down(void);
void ui_create(void);
void ui_poll(void);
void ui_show_power_off(void);

/* serial.c: debug log on SCIF0, 115200 8N1 */
void serial_init(void);
void serial_write(const char *s);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
