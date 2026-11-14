#!/bin/sh
# اختبار الواجهة المضمِّنة (التي يستخدمها تطبيق iOS) على صور Alpine التي يبنيها boot_test.sh.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/gtv-emu-test}
CC=${CC:-cc}
$CC -O2 -w -o "$WORK/api_test" "$ROOT/tests/api_test.c" "$ROOT"/core/cpu.c "$ROOT"/core/simd.c "$ROOT"/core/devices.c \
    "$ROOT"/core/virtio.c "$ROOT"/core/netstack.c "$ROOT"/core/bootimg.c "$ROOT"/core/fdt.c "$ROOT"/core/jit.c "$ROOT"/core/gtv_api.c -lm -lpthread
cd "$WORK"; rc=0; ./api_test Image initrd.gz > api.log 2> api.err || rc=$?
grep -a "نجح\|فشل\|\[api\]" api.log
exit $rc
