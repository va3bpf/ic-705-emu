/* Entry at 0x18000000: the RZ/A1H machine starts the Cortex-A9 here, MMU and caches off. */
    .section .vectors, "ax"
    .global _start
_start:
    ldr sp, =0x20A00000      @ top of the 10 MB on-chip RAM
    bl  main
1:  wfi
    b   1b
