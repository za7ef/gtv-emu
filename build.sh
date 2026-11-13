#!/bin/sh
# بناء المحاكي دون الحاجة إلى make
CC=${CC:-cc}
exec $CC -O2 -g -Wall -Wno-unused-function -o gtv-emu core/cpu.c core/simd.c core/devices.c core/fdt.c core/main.c -lm
