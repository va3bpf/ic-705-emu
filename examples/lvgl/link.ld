/* IC-705 main CPU: code in the 16 MB SPI flash (XIP at 0x18000000), data in the
 * 10 MB on-chip RAM. 0x20800000.. holds the LCD frame buffer, 0x209xxxxx the
 * mode stacks (start.S). */
ENTRY(_start)
MEMORY {
    FLASH (rx)  : ORIGIN = 0x18000000, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 8M
}
SECTIONS {
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    .data : {
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.ARM.attributes) }
}
