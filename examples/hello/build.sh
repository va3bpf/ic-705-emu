#!/bin/sh
# Build hello.bin, a flat image loaded at 0x18000000 by `qemu-system-arm -M rz-a1h -kernel`.
set -e
cd "$(dirname "$0")"
arm-none-eabi-gcc -mcpu=cortex-a9 -marm -O2 -ffreestanding -nostdlib -Wall \
    -T link.ld start.S hello.c -o hello.elf
arm-none-eabi-objcopy -O binary hello.elf hello.bin
ls -l hello.bin
