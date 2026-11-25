#!/bin/sh
# Build lvgl_demo.bin: LVGL + FreeRTOS on the IC-705 main CPU (RZ/A1H, Cortex-A9),
# a flat image loaded at 0x18000000 (python3 ic705_emu.py examples/lvgl/lvgl_demo.bin).
# LVGL and the FreeRTOS kernel are fetched at pinned tags into third_party/.
set -e
cd "$(dirname "$0")"
LVGL_TAG=v9.2.2
FREERTOS_TAG=V11.1.0
mkdir -p third_party build
[ -d third_party/lvgl ] || git clone -q --depth 1 --branch $LVGL_TAG https://github.com/lvgl/lvgl.git third_party/lvgl
[ -d third_party/FreeRTOS-Kernel ] || git clone -q --depth 1 --branch $FREERTOS_TAG https://github.com/FreeRTOS/FreeRTOS-Kernel.git third_party/FreeRTOS-Kernel

CC=arm-none-eabi-gcc
RTOS=third_party/FreeRTOS-Kernel
PORT=$RTOS/portable/GCC/ARM_CA9
CPU="-mcpu=cortex-a9 -marm -mfloat-abi=soft -mno-unaligned-access"
CFLAGS="$CPU -O2 -g -ffreestanding -fno-common -ffunction-sections -fdata-sections -Wall
        -I. -Ithird_party -Ithird_party/lvgl -I$RTOS/include -I$PORT -DLV_CONF_INCLUDE_SIMPLE"

SRC="$(ls *.c) $(find third_party/lvgl/src -name '*.c')
     $RTOS/tasks.c $RTOS/list.c $RTOS/queue.c $RTOS/timers.c $RTOS/portable/MemMang/heap_4.c
     $PORT/port.c"
OBJS=""
for src in $SRC; do
    obj=build/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')
    OBJS="$OBJS $obj"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ lv_conf.h -nt "$obj" ] || [ FreeRTOSConfig.h -nt "$obj" ]; then
        extra=""
        case "$src" in */ARM_CA9/port.c) extra="-mfloat-abi=softfp -mfpu=vfpv3-d16";; esac
        $CC $CFLAGS $extra -c "$src" -o "$obj"
    fi
done
# the port's assembler saves VFP registers for tasks that ask for it (none here)
$CC $CPU -mfloat-abi=softfp -mfpu=neon -I. -I$RTOS/include -I$PORT -c $PORT/portASM.S -o build/portASM.o
$CC $CPU -c start.S -o build/start.o
$CC $CPU -nostdlib -Wl,--gc-sections -T link.ld build/start.o build/portASM.o $OBJS -lgcc -o lvgl_demo.elf
arm-none-eabi-objcopy -O binary lvgl_demo.elf lvgl_demo.bin
arm-none-eabi-size lvgl_demo.elf
