ENTRY(_start)
MEMORY { FLASH (rx) : ORIGIN = 0x18000000, LENGTH = 1M }
SECTIONS {
    .text : { *(.vectors) *(.text*) *(.rodata*) } > FLASH
    /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
