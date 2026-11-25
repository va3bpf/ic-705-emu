/*
 * Debug log on SCIF0: 115200 8N1, transmit only, polled.
 *
 * In the emulator SCIF0 and SCIF1 go to TCP port 4500+N (N = the VNC display;
 * ic705_emu.py) and ic705_panel.py shows them in the loader's serial log pane.
 * On a real radio SCIF0's TxD pin would also need its pin function set; the
 * emulator does not model pin multiplexing, so this file does not do that.
 *
 *   log_printf("dial %d\n", delta);     printf-style, "\n" becomes "\r\n"
 *
 * Safe from any task. Not from interrupt handlers: it waits for the FIFO.
 */
#include <stdarg.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "lvgl.h"
#include "hw.h"

#define SCIF0           0xE8007000
#define SCSMR           REG16(SCIF0 + 0x00)     /* mode: 8N1, P1 clock / 1 */
#define SCBRR           REG8(SCIF0 + 0x04)      /* bit rate */
#define SCSCR           REG16(SCIF0 + 0x08)     /* control: TE */
#define SCFTDR          REG8(SCIF0 + 0x0C)      /* transmit FIFO */
#define SCFSR           REG16(SCIF0 + 0x10)     /* status: TDFE, TEND */
#define SCFCR           REG16(SCIF0 + 0x18)     /* FIFO control */
#define SCEMR           REG16(SCIF0 + 0x28)     /* extended mode */
#define STBCR4          REG8(0xFCFE0424)        /* module stop: bit 7 = SCIF0 */

#define SCR_TE          (1u << 5)
#define FSR_TEND        (1u << 6)
#define FSR_TDFE        (1u << 5)
#define FCR_TFRST       (1u << 2)
#define FCR_RFRST       (1u << 1)

#define P1_HZ           66666667u               /* peripheral clock 1 */
#define BAUD            115200u

static SemaphoreHandle_t lock;

void serial_init(void)
{
    STBCR4 &= ~0x80;                        /* clock the module */
    (void)STBCR4;
    SCSCR = 0;
    SCFCR = FCR_TFRST | FCR_RFRST;
    SCFSR = 0;
    SCSMR = 0;                              /* async, 8 bits, no parity, 1 stop */
    SCEMR = 0;
    SCBRR = P1_HZ / (32 * BAUD) - 1;        /* 17: 115.7 kbit/s */
    SCFCR = 0;
    SCSCR = SCR_TE;
    lock = xSemaphoreCreateMutex();
}

static void put(char c)
{
    while (!(SCFSR & FSR_TDFE)) {
    }
    SCFTDR = c;
    SCFSR = (uint16_t)~(FSR_TDFE | FSR_TEND);
}

void serial_write(const char *s)
{
    bool tasks = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;

    if (tasks && lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    for (; *s; s++) {
        if (*s == '\n') {
            put('\r');
        }
        put(*s);
    }
    if (tasks && lock) {
        xSemaphoreGive(lock);
    }
}

void log_printf(const char *fmt, ...)
{
    char line[160];
    va_list args;

    va_start(args, fmt);
    lv_vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    serial_write(line);
}
