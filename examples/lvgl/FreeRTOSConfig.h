/* FreeRTOS configuration for the IC-705 main CPU (RZ/A1H, Cortex-A9, ARM_CA9 port). */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configCPU_CLOCK_HZ                      400000000UL
#define configTICK_RATE_HZ                      1000
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                256
#define configMAX_TASK_NAME_LEN                 12
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_TASK_NOTIFICATIONS            1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   (512 * 1024)
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            512

#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_xTaskGetTickCount               1
#define INCLUDE_vTaskSuspend                    1

/* GIC (RZ/A1H: distributor 0xE8201000, CPU interface 0xE8202000, 5 priority bits) */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS         0xE8201000UL
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET 0x1000UL
#define configUNIQUE_INTERRUPT_PRIORITIES               32
#define configMAX_API_CALL_INTERRUPT_PRIORITY           18

/* Tasks may register a floating-point context; none does (soft float). */
#define configUSE_TASK_FPU_SUPPORT              1

/* The tick comes from OSTM0 (main.c). */
void vConfigureTickInterrupt(void);
#define configSETUP_TICK_INTERRUPT()            vConfigureTickInterrupt()

void vAssertCalled(const char *file, int line);
#define configASSERT(x)                         if (!(x)) vAssertCalled(__FILE__, __LINE__)

#endif
