#!/bin/sh
# اختبار الإقلاع الكامل: ينزّل نواة Alpine وbusybox، ويبني initramfs، ويقلع حتى سطر الأوامر،
# ثم يتحقق من القراءة والكتابة على قرص virtio ومن تنفيذ أوامر داخل الصدفة.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/gtv-emu-test}
ALP=https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64
KVER_APK=linux-virt-6.12.117-r0.apk
BB_APK=busybox-static-1.37.0-r20.apk
mkdir -p "$WORK" && cd "$WORK"

[ -f "$KVER_APK" ] || curl -sfO "$ALP/$KVER_APK"
[ -f "$BB_APK" ] || curl -sfO "$ALP/$BB_APK"
mkdir -p k bb
tar xzf "$KVER_APK" -C k boot lib/modules 2>/dev/null || true
tar xzf "$BB_APK" -C bb 2>/dev/null || true

# فك ضغط النواة من غلاف EFI zboot
python3 - <<'PY'
import struct, gzip, glob
d = open(glob.glob('k/boot/vmlinuz-virt')[0], 'rb').read()
assert d[4:8] == b'zimg'
off, sz = struct.unpack('<II', d[8:16])
open('Image', 'wb').write(gzip.decompress(d[off:off + sz]))
PY

M=$(ls -d k/lib/modules/*/kernel/drivers)
for f in "$M/virtio/virtio_mmio.ko.gz" "$M/block/virtio_blk.ko.gz"; do gunzip -kf "$f"; done
python3 "$ROOT/tools/mkinitramfs.py" bb/bin/busybox.static initrd.gz \
    --module "$M/virtio/virtio_mmio.ko" --module "$M/block/virtio_blk.ko"
python3 -c "d=bytearray(4<<20); d[0:24]=b'GTV-DISK-HELLO-FROM-HOST'; open('disk.img','wb').write(d)"

# EMU_WRAP يسمح بالتشغيل عبر qemu-aarch64 على مضيف x86، وEMU_ARGS لتمرير --no-jit مثلًا
$EMU_WRAP "${EMU:-$ROOT/gtv-emu}" $EMU_ARGS -k Image -i initrd.gz -d disk.img --max-insns 20000000000 \
    --feed "$(printf '\002echo SHELL-$((6*7)); poweroff -f\r')" > boot.log 2> emu.log || true

fail=0
check() { if grep -q "$1" "$2"; then echo "نجح: $3"; else echo "فشل: $3"; fail=1; fi; }
check "Run /init as init process" boot.log "إقلاع النواة حتى init"
check "gtv-emu: shell ready" boot.log "تشغيل الصدفة"
check "virtio-blk read: GTV-DISK-HELLO-FROM-HOST" boot.log "القراءة من قرص virtio"
check "SHELL-42" boot.log "تنفيذ أوامر داخل الصدفة"
check "reboot: Power down" boot.log "الإيقاف عبر PSCI"
if head -c 600 disk.img | grep -aq "WRITTEN-BY-GUEST"; then echo "نجح: الكتابة على قرص virtio"; else echo "فشل: الكتابة على قرص virtio"; fail=1; fi
tail -1 emu.log
exit $fail
