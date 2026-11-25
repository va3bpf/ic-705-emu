/*
 * LVGL + FreeRTOS demo for the IC-705 main CPU (RZ/A1H, Cortex-A9).
 *
 *   cold start          deep standby, LCD dark (as the Icom firmware does)
 *   press POWER         power on: rails and POWER LED on, FreeRTOS starts
 *   GUI task            LVGL: frequency display, band and step buttons on the
 *                       touch screen, the main dial tunes
 *   power task          watches POWER; a 1 s hold switches off
 *
 * The FreeRTOS tick is OSTM0 at 1 kHz through the GIC (interrupt 134).
 */
#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "hw.h"

static TaskHandle_t gui_task_handle;

/* ------------------------------------------------------------ interrupts */

extern void FreeRTOS_Tick_Handler(void);

/* Called by the port's IRQ entry (portASM.S) with the acknowledged ID. */
void vApplicationIRQHandler(uint32_t iar)
{
    if ((iar & 0x3FF) == OSTM0_IRQ) {
        FreeRTOS_Tick_Handler();
    }
}

static void gic_init(void)
{
    GICD_CTLR = 1;
    GICC_CTLR = 1;
    GICC_BPR = 0;
}

/* configSETUP_TICK_INTERRUPT(): OSTM0 in interval mode, one interrupt per ms. */
void vConfigureTickInterrupt(void)
{
    OSTM_TT(OSTM0) = 1;
    OSTM_CTL(OSTM0) = 0x01;                 /* interval mode, interrupt enabled */
    OSTM_CMP(OSTM0) = OSTM_HZ / configTICK_RATE_HZ;
    GICD_IPRIORITYR(OSTM0_IRQ) = (configUNIQUE_INTERRUPT_PRIORITIES - 2) << 3;
    GICD_ITARGETSR(OSTM0_IRQ) = 1;
    GICD_ICFGR(OSTM0_IRQ / 16) |= 2u << (2 * (OSTM0_IRQ % 16));  /* edge */
    GICD_ISENABLER(OSTM0_IRQ / 32) = 1u << (OSTM0_IRQ % 32);
    OSTM_TS(OSTM0) = 1;
}

/* ----------------------------------------------------------- hooks, errors */

static volatile const char *fail_file;
static volatile int fail_line;

static void __attribute__((noreturn)) stop(const char *file, int line)
{
    __asm volatile("cpsid i");
    fail_file = file;
    fail_line = line;
    for (;;) {
    }
}

void vAssertCalled(const char *file, int line)
{
    stop(file, line);
}

void vApplicationMallocFailedHook(void)
{
    stop("malloc failed", 0);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    stop(name, 0);
}

/* ------------------------------------------------------------------- tasks */

static uint32_t lvgl_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void gui_task(void *arg)
{
    lv_init();
    lv_tick_set_cb(lvgl_ms);
    display_init();
    touch_init();
    dial_init();
    ui_create();
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, 0)) {  /* the power task asks to switch off */
            ui_show_power_off();
            for (int i = 0; i < 40; i++) {  /* draw it and keep it up ~1 s */
                lv_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            while (power_key_down()) {      /* let go first, or the next press is lost */
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            display_off();
            power_off_now();
        }
        ui_poll();
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void power_task(void *arg)
{
    TickType_t held = 0;

    while (power_key_down()) {              /* still held from the press that woke us */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        held = power_key_down() ? held + 20 : 0;
        if (held >= 1000) {
            xTaskNotifyGive(gui_task_handle);
            vTaskSuspend(NULL);
        }
    }
}

int main(void)
{
    if (!power_woken_by_key()) {
        power_off_now();                    /* cold start: back to deep standby */
    }
    power_on();
    gic_init();
    xTaskCreate(gui_task, "gui", 16384, NULL, 2, &gui_task_handle);
    xTaskCreate(power_task, "power", 512, NULL, 3, NULL);
    vTaskStartScheduler();
    stop("scheduler returned", 0);
}
