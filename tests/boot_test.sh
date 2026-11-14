#!/bin/sh
# اختبار الإقلاع الكامل: ينزّل نواة Alpine وbusybox، ويبني initramfs، ويقلع حتى سطر الأوامر،
# ثم يتحقق من القراءة والكتابة على قرص virtio ومن تنفيذ أوامر داخل الصدفة.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/gtv-emu-test}
ALP=https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64
KVER_APK=linux-virt-6.12.117-r0.apk
BB_APK=busybox-static-1.37.0-r20.apk
SND_APKS="alsa-lib-1.2.14-r0.apk alsa-utils-1.2.14-r0.apk musl-1.2.5-r13.apk"
mkdir -p "$WORK" && cd "$WORK"

[ -f "$KVER_APK" ] || curl -sfO "$ALP/$KVER_APK"
[ -f "$BB_APK" ] || curl -sfO "$ALP/$BB_APK"
mkdir -p k bb
tar xzf "$KVER_APK" -C k boot lib/modules 2>/dev/null || true
tar xzf "$BB_APK" -C bb 2>/dev/null || true
# aplay ومكتباته لاختبار الصوت
mkdir -p snd
for a in $SND_APKS; do [ -f "$a" ] || curl -sfO "$ALP/$a"; tar xzf "$a" -C snd 2>/dev/null || true; done
python3 - <<'PY'
import struct, math
rate, n = 48000, 24000
pcm = b''.join(struct.pack('<hh', int(12000 * math.sin(2 * math.pi * 440 * i / rate)), int(8000 * math.sin(2 * math.pi * 660 * i / rate))) for i in range(n))
hdr = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16) + b'data' + struct.pack('<I', len(pcm))
open('tone.wav', 'wb').write(hdr + pcm)
PY
SNDFILES="--file snd/usr/bin/aplay:usr/bin/aplay --file snd/lib/ld-musl-aarch64.so.1:lib/ld-musl-aarch64.so.1 --file snd/lib/libc.musl-aarch64.so.1:lib/libc.musl-aarch64.so.1 --file tone.wav:tone.wav"
for f in snd/usr/lib/libasound.so.2*; do SNDFILES="$SNDFILES --file $f:usr/lib/$(basename $f)"; done
for f in $(cd snd && find usr/share/alsa -type f -o -type l); do SNDFILES="$SNDFILES --file snd/$f:$f"; done

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
        'kernel/drivers/virtio/virtio_input.ko.gz', 'kernel/sound/virtio/virtio_snd.ko.gz', 'kernel/net/packet/af_packet.ko.gz']
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
python3 "$ROOT/tools/mkinitramfs.py" bb/bin/busybox.static initrd.gz $MODS $SNDFILES
python3 -c "d=bytearray(4<<20); d[0:24]=b'GTV-DISK-HELLO-FROM-HOST'; open('disk.img','wb').write(d)"

# EMU_WRAP يسمح بالتشغيل عبر qemu-aarch64 على مضيف x86، وEMU_ARGS لتمرير --no-jit مثلًا
# \002 ينتظر موجه الصدفة، و\003 يبدأ الكتابة عبر لوحة مفاتيح virtio على الطرفية الرسومية، و\001 ينتظر 1.5 ثانية
rm -f screen.png out.wav
# خادم HTTP محلي على المضيف لاختبار NAT (يصل إليه الضيف عبر 10.0.2.2)
mkdir -p www && echo "NAT-HOST-OK-$(date +%s)" > www/nat.txt
python3 -m http.server 8123 --bind 127.0.0.1 --directory www >/dev/null 2>&1 &
HTTP_PID=$!
# إذا كان المضيف يصل إلى الإنترنت عبر وكيل محلي (كبيئة الاختبار هذه)، يستخدمه الضيف عبر البوابة 10.0.2.2
GUEST_PROXY=
case "${HTTP_PROXY:-$http_proxy}" in
  http://localhost:*|http://127.0.0.1:*) GUEST_PROXY="http_proxy=http://10.0.2.2:${HTTP_PROXY##*:}";;
esac
sleep 1
$EMU_WRAP "${EMU:-$ROOT/gtv-emu}" --smp ${SMP:-4} $EMU_ARGS -k Image -i initrd.gz -d disk.img --max-insns 40000000000 \
    --keys "$(printf 'echo KBD-$((6*7)) > /tmp/kbd.txt; echo typed on virtio keyboard; ls /dev/dri\r')" \
    --screenshot screen.png --audio out.wav \
    --feed "$(printf '\002\003\001\001\001ip link set eth0 up; udhcpc -i eth0 -n -q -s /etc/udhcpc.sh; ping -c 2 10.0.2.2; wget -q -O - http://10.0.2.2:8123/nat.txt; '"$GUEST_PROXY"' wget -q -T 10 -O - http://example.com/ | grep -o Example.Domain | head -1; cat /tmp/kbd.txt\r\002echo CPUS-$(nproc); for i in 1 2 3 4; do (j=0; while [ $j -lt 4000 ]; do j=$((j+1)); done; echo PAR-$i) & done; wait; grep -E IPI[01]: /proc/interrupts\r\002/usr/bin/aplay -D hw:0,0 /tone.wav && echo APLAY-OK\r\002echo SHELL-$((6*7)); poweroff -f\r')" > boot.log 2> emu.log || true

kill $HTTP_PID 2>/dev/null || true
fail=0
check() { if grep -q "$1" "$2"; then echo "نجح: $3"; else echo "فشل: $3"; fail=1; fi; }
check "Run /init as init process" boot.log "إقلاع النواة حتى init"
check "gtv-emu: shell ready" boot.log "تشغيل الصدفة"
check "virtio-blk read: GTV-DISK-HELLO-FROM-HOST" boot.log "القراءة من قرص virtio"
check "SHELL-42" boot.log "تنفيذ أوامر داخل الصدفة"
check "reboot: Power down" boot.log "الإيقاف عبر PSCI"
check "virtio-gpu: virtio_gpudrmfb 1280,720" boot.log "تشغيل بطاقة الرسوميات virtio-gpu"
check "2 packets received" boot.log "الشبكة: ping إلى البوابة عبر virtio-net"
check "dhcp: 10.0.2.15 gw 10.0.2.2" boot.log "الشبكة: عنوان من DHCP"
check "^$(cat www/nat.txt)" boot.log "الشبكة: اتصال TCP بخادم على المضيف عبر NAT"
if grep -q "^Example Domain" boot.log; then echo "نجح: الشبكة: الإنترنت عبر NAT مع DNS (example.com)"; else echo "تخطٍّ أو فشل: الإنترنت (example.com) — يعتمد على اتصال المضيف"; fi
check "^KBD-42" boot.log "الكتابة عبر لوحة مفاتيح virtio-input"
check "smp: Brought up 1 node, ${SMP:-4} CPU" boot.log "تشغيل ${SMP:-4} معالجات عبر PSCI"
check "^CPUS-${SMP:-4}" boot.log "رؤية الصدفة لكل المعالجات"
if [ "$(grep -c '^PAR-' boot.log)" = 4 ]; then echo "نجح: أربع مهام متوازية"; else echo "فشل: أربع مهام متوازية"; fail=1; fi
check "^APLAY-OK" boot.log "تشغيل الصوت عبر virtio-snd (aplay)"
if python3 -c "
import sys
a = open('tone.wav','rb').read()[44:]; b = open('out.wav','rb').read()
sys.exit(0 if b[:4] == b'RIFF' and b[44:44+len(a)] == a else 1)" 2>/dev/null; then echo "نجح: ملف WAV الناتج يطابق العينات المرسلة"; else echo "فشل: ملف WAV الناتج يطابق العينات المرسلة"; fail=1; fi
check "لقطة شاشة 1280x720" emu.log "لقطة شاشة من الإطار المعروض"
if head -c 600 disk.img | grep -aq "WRITTEN-BY-GUEST"; then echo "نجح: الكتابة على قرص virtio"; else echo "فشل: الكتابة على قرص virtio"; fail=1; fi
grep -E "توقف|JIT|gpu" emu.log
exit $fail
