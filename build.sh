#!/bin/sh
# بناء المحاكي دون الحاجة إلى make
CC=${CC:-cc}
exec $CC -O2 -g -Wall -Wno-unused-function -o gtv-emu core/cpu.c core/simd.c core/devices.c core/virtio.c core/netstack.c core/bootimg.c core/fdt.c core/jit.c core/gtv_api.c core/atvprep.c core/main.c ${ZLIB:--lz} -lm -lpthread
