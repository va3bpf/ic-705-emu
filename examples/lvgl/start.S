/*
 * Start-up for the IC-705 main CPU (RZ/A1H, Cortex-A9). The machine starts
 * here, at 0x18000000, in SVC mode with the MMU and caches off.
 *
 * Exception vectors: VBAR points at the table below. SWI and IRQ go to the
 * FreeRTOS port (portASM.S); the others stop in a loop that names them.
 * Stacks (top of the 10 MB on-chip RAM): SVC 0x20A00000 (main, then the
 * port's interrupt nesting), IRQ 0x209F0000, abort/undefined 0x209E0000.
 * .data is copied from flash, .bss cleared, then main() runs.
 */
    .syntax unified
    .arm

    .section .vectors, "ax"
    .global _start
    .align 5
vectors:
_start:
    b   reset
    b   undefined
    ldr pc, =FreeRTOS_SWI_Handler
    b   prefetch_abort
    b   data_abort
    b   .
    ldr pc, =FreeRTOS_IRQ_Handler
    b   .                           @ FIQ unused

reset:
    ldr r0, =vectors
    mcr p15, 0, r0, c12, c0, 0      @ VBAR

    cps #0x12                       @ IRQ
    ldr sp, =0x209F0000
    cps #0x17                       @ abort
    ldr sp, =0x209E0000
    cps #0x1B                       @ undefined
    ldr sp, =0x209E0000
    cps #0x13                       @ back to SVC
    ldr sp, =0x20A00000

    ldr r0, =__data_load            @ .data: flash -> RAM
    ldr r1, =__data_start
    ldr r2, =__data_end
1:  cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 1b

    ldr r1, =__bss_start            @ .bss: clear
    ldr r2, =__bss_end
    mov r3, #0
2:  cmp r1, r2
    strlo r3, [r1], #4
    blo 2b

    bl  main
3:  wfi
    b   3b

undefined:      b undefined
prefetch_abort: b prefetch_abort
data_abort:     b data_abort
    .ltorg
