#!/bin/sh
# Build power.bin, a flat image loaded at 0x18000000 by `qemu-system-arm -M rz-a1h -kernel`.
set -e
cd "$(dirname "$0")"
arm-none-eabi-gcc -mcpu=cortex-a9 -marm -O2 -ffreestanding -nostdlib -Wall \
    -T link.ld start.S power.c -o power.elf
arm-none-eabi-objcopy -O binary power.elf power.bin
ls -l power.bin
