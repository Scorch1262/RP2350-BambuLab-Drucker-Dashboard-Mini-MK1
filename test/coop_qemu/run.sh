#!/bin/sh
# Testet die kooperativen Aufgaben (firmware/.../coop.cpp) in QEMU auf einem
# Cortex-M33 (mps2-an505): Kontextwechsel inkl. FPU-Register, Interrupts auf
# Aufgaben-Stacks, Mutex, Aufraeumen beendeter Aufgaben, Stack-Ueberlauf
# (MSPLIM -> HardFault -> Neustart mit Grund).
# Voraussetzung: arm-none-eabi-g++ im PATH (z. B. aus arduino-pico), qemu-system-arm
set -e
cd "$(dirname "$0")"
SRC=../../firmware/DruckerDashboardRP2350-MK1
F="-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse -Os -g -DARDUINO -Ishim -I$SRC -ffunction-sections"
mkdir -p out
arm-none-eabi-g++ $F -fno-exceptions -fno-rtti -c $SRC/coop.cpp -o out/coop.o
arm-none-eabi-g++ $F -fno-exceptions -fno-rtti -c test.cpp -o out/test.o
arm-none-eabi-gcc $F -c startup.c -o out/startup.o
arm-none-eabi-gcc $F -c stubs.c -o out/stubs.o
arm-none-eabi-g++ $F -T link.ld -nostartfiles --specs=nosys.specs out/startup.o out/stubs.o out/coop.o out/test.o -o out/test.elf -Wl,--gc-sections
timeout 60 qemu-system-arm -M mps2-an505 -nographic -semihosting -kernel out/test.elf
