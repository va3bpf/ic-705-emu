/*
 * IC-705 power key and RZ/A1H deep standby (power.c).
 *
 * The radio is "off" when the RZ/A1H is in deep standby: the firmware arms
 * its wake-up sources in DSSSR (bits 3, 4, 10 and 13), sets STBCR1 STBY and
 * DEEP (0xC0) and executes WFI. A wake-up event resets the CPU and leaves
 * the reason in DSFR (0xFCFF1808), which the firmware's power-state code
 * reads at boot. Here the CPU counts as being in deep standby when STBCR1
 * holds 0xC0 and the CPU is halted.
 *
 * Wake-up sources (docs/ic705/IO_MAP.md):
 *   PWRK, the POWER key, P8_7 falling   DSFR bit 10 (power on)
 *   VBUS, USB power, P3_9 falling       DSFR bit RZA1H_DSWAKE_BIT (default 13)
 *
 * The reset runs on the CPU's own thread: done from the main loop it would
 * leave a vCPU that sleeps in WFI asleep when QEMU runs without -icount.
 * RAM and peripherals keep their state, as in deep standby.
 *
 * RZA1H_DSFR sets the value DSFR holds after a machine reset (default 0, so
 * a cold start finds no wake-up reason and the firmware returns to deep
 * standby).
 */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/arm/cpu-qom.h"
#include "hw/arm/rz_a1h.h"
#include "hw/arm/rza1h_debug.h"
#include "ic705.h"

#define STBCR1          0xFCFE0020
#define STBCR1_DEEP     0xC0            /* STBY | DEEP */
#define DSFR            0xFCFF1808
#define WAKE_PWRK       10

static CPUState *a9;

static void write_dsfr(uint16_t value)
{
    address_space_write(&address_space_memory, DSFR, MEMTXATTRS_UNSPECIFIED, &value, 2);
}

static bool in_deep_standby(void)
{
    uint8_t stbcr1 = 0;

    address_space_read(&address_space_memory, STBCR1, MEMTXATTRS_UNSPECIFIED, &stbcr1, 1);
    return (stbcr1 & STBCR1_DEEP) == STBCR1_DEEP && a9->halted;
}

static void wake_on_cpu(CPUState *cs, run_on_cpu_data data)
{
    uint8_t zero = 0;

    address_space_write(&address_space_memory, STBCR1, MEMTXATTRS_UNSPECIFIED, &zero, 1);
    cpu_reset(cs);
    cpu_set_pc(cs, RZA1H_FLASH_BASE);
    write_dsfr(1u << data.host_int);
}

static void wake(int reason)
{
    rza1h_debug("dstby", "deep standby wake-up, DSFR bit %d", reason);
    async_run_on_cpu(a9, wake_on_cpu, RUN_ON_CPU_HOST_INT(reason));
}

/* board.c: an input pin driven from outside (QMP, the window) changed */
void ic705_power_pin(int port, int bit, int level)
{
    if (!a9 || level || !in_deep_standby()) {
        return;
    }
    if (port == 8 && bit == 7) {
        wake(WAKE_PWRK);
    } else if (port == 3 && bit == 9) {
        const char *e = getenv("RZA1H_DSWAKE_BIT");

        wake(e ? atoi(e) : 13);
    }
}

/* rz_a1h.c: after a machine reset of the CPU */
void ic705_power_reset(void)
{
    const char *e = getenv("RZA1H_DSFR");

    write_dsfr(e ? strtoul(e, NULL, 0) : 0);
}

void ic705_power_init(ARMCPU *cpu)
{
    a9 = CPU(cpu);
}
