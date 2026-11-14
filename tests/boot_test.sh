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

# الوحدات المطلوبة مع اعتمادياتها بالترتيب الصحيح (من modules.dep)
MODS=$(python3 - <<'PY'
import glob, gzip, os
base = glob.glob('k/lib/modules/*')[0]
dep = {}
for line in open(base + '/modules.dep'):
    k, _, v = line.partition(':')
    dep[k] = v.split()
want = ['kernel/drivers/virtio/virtio_mmio.ko.gz', 'kernel/drivers/block/virtio_blk.ko.gz',
        'kernel/drivers/net/virtio_net.ko.gz', 'kernel/drivers/gpu/drm/virtio/virtio-gpu.ko.gz',
        'kernel/drivers/virtio/virtio_input.ko.gz']
order = []
def visit(m):
    if m in order: return
    for d in dep.get(m, []): visit(d)
    order.append(m)
for w in want: visit(w)
out = []
for m in order:
    src = base + '/' + m
    dst = 'mods/' + os.path.basename(m)[:-3]
    os.makedirs('mods', exist_ok=True)
    open(dst, 'wb').write(gzip.open(src).read())
    out.append('--module ' + dst)
print(' '.join(out))
PY
)
python3 "$ROOT/tools/mkinitramfs.py" bb/bin/busybox.static initrd.gz $MODS
python3 -c "d=bytearray(4<<20); d[0:24]=b'GTV-DISK-HELLO-FROM-HOST'; open('disk.img','wb').write(d)"

# EMU_WRAP يسمح بالتشغيل عبر qemu-aarch64 على مضيف x86، وEMU_ARGS لتمرير --no-jit مثلًا
# \002 ينتظر موجه الصدفة، و\003 يبدأ الكتابة عبر لوحة مفاتيح virtio على الطرفية الرسومية، و\001 ينتظر 1.5 ثانية
rm -f screen.png
$EMU_WRAP "${EMU:-$ROOT/gtv-emu}" $EMU_ARGS -k Image -i initrd.gz -d disk.img --max-insns 40000000000 \
    --keys "$(printf 'echo KBD-$((6*7)) > /tmp/kbd.txt; echo typed on virtio keyboard; ls /dev/dri\r')" \
    --screenshot screen.png \
    --feed "$(printf '\002\003\001\001\001ip link set eth0 up; ip addr add 10.0.2.15/24 dev eth0; ping -c 2 10.0.2.2; cat /tmp/kbd.txt\r\002echo SHELL-$((6*7)); poweroff -f\r')" > boot.log 2> emu.log || true

fail=0
check() { if grep -q "$1" "$2"; then echo "نجح: $3"; else echo "فشل: $3"; fail=1; fi; }
check "Run /init as init process" boot.log "إقلاع النواة حتى init"
check "gtv-emu: shell ready" boot.log "تشغيل الصدفة"
check "virtio-blk read: GTV-DISK-HELLO-FROM-HOST" boot.log "القراءة من قرص virtio"
check "SHELL-42" boot.log "تنفيذ أوامر داخل الصدفة"
check "reboot: Power down" boot.log "الإيقاف عبر PSCI"
check "virtio-gpu: virtio_gpudrmfb 1280,720" boot.log "تشغيل بطاقة الرسوميات virtio-gpu"
check "2 packets received" boot.log "الشبكة: ping إلى البوابة عبر virtio-net"
check "^KBD-42" boot.log "الكتابة عبر لوحة مفاتيح virtio-input"
check "لقطة شاشة 1280x720" emu.log "لقطة شاشة من الإطار المعروض"
if head -c 600 disk.img | grep -aq "WRITTEN-BY-GUEST"; then echo "نجح: الكتابة على قرص virtio"; else echo "فشل: الكتابة على قرص virtio"; fail=1; fi
grep -E "توقف|JIT|gpu" emu.log
exit $fail
