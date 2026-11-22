#!/bin/sh
# بناء ملفات الصور التي ينزّلها تطبيق iOS من إصدار GitHub، مع manifest.json يصفها.
# الاستخدام: mkios-assets.sh <مجلد-الإخراج> [--android]
#   بدون --android: ملف تعريف Alpine للاختبار فقط (نواة + قرص أولي، نحو 25 ميغابايت).
#   مع --android: يضيف ملف تعريف أندرويد (boot وvendor_boot وinit_boot وصورة النظام مضغوطة).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "${1:-assets}" && cd "${1:-assets}" && pwd)
ANDROID=0; TV=1
for a in "$2" "$3"; do [ "$a" = "--android" ] && ANDROID=1; [ "$a" = "--no-tv" ] && TV=0; done
# ملف تعريف Android TV (يُبنى افتراضيًا، ويُتخطى بـ --no-tv). صورة Google لا تُنشر؛ التطبيق ينزّلها من dl.google.com ويجهّزها على الجهاز.
# ‏hardwareegl=angle لازم: init.ranchu.rc في صورة Google يضبط منه ro.hardware.egl (الافتراضي emulation يُسقط SurfaceFlinger).
# المنشور هنا: نواة GKI (GPL) وvendor_boot ووحدات virtio، وinit_boot من AOSP Cuttlefish، وقرص التجهيز
# الأولي (Alpine + e2fsprogs + مكوّنات رسوميات Cuttlefish بترخيص Apache 2.0).
# المكوّنات تُنزَّل من فرع tv-cf-parts في المستودع، أو من مجلد محلي في CF_PARTS.
WORK=${WORK:-$OUT/work}
mkdir -p "$WORK" && cd "$WORK"

# ===== Alpine: نواة linux-virt وbusybox ووحدات virtio وaplay =====
ALP=https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64
KVER_APK=linux-virt-6.12.117-r0.apk
BB_APK=busybox-static-1.37.0-r20.apk
SND_APKS="alsa-lib-1.2.14-r0.apk alsa-utils-1.2.14-r0.apk musl-1.2.5-r13.apk"
[ -f "$KVER_APK" ] || curl -sfO "$ALP/$KVER_APK"
[ -f "$BB_APK" ] || curl -sfO "$ALP/$BB_APK"
mkdir -p k bb snd
tar xzf "$KVER_APK" -C k boot lib/modules 2>/dev/null || true
tar xzf "$BB_APK" -C bb 2>/dev/null || true
for a in $SND_APKS; do [ -f "$a" ] || curl -sfO "$ALP/$a"; tar xzf "$a" -C snd 2>/dev/null || true; done
python3 - <<'PY'
import struct, math, gzip, glob
rate, n = 48000, 48000
pcm = b''.join(struct.pack('<hh', int(9000 * math.sin(2 * math.pi * 440 * i / rate)), int(6000 * math.sin(2 * math.pi * 660 * i / rate))) for i in range(n))
hdr = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16) + b'data' + struct.pack('<I', len(pcm))
open('tone.wav', 'wb').write(hdr + pcm)
# النواة من غلاف EFI zboot إلى Image ثم gzip (يفك المحاكي ضغطها عند الإقلاع)
d = open(glob.glob('k/boot/vmlinuz-virt')[0], 'rb').read()
assert d[4:8] == b'zimg'
off, sz = struct.unpack('<II', d[8:16])
open('Image.gz', 'wb').write(gzip.compress(gzip.decompress(d[off:off + sz]), 9, mtime=0))
PY
SNDFILES="--file snd/usr/bin/aplay:usr/bin/aplay --file snd/lib/ld-musl-aarch64.so.1:lib/ld-musl-aarch64.so.1 --file snd/lib/libc.musl-aarch64.so.1:lib/libc.musl-aarch64.so.1 --file tone.wav:tone.wav"
for f in snd/usr/lib/libasound.so.2*; do SNDFILES="$SNDFILES --file $f:usr/lib/$(basename $f)"; done
for f in $(cd snd && find usr/share/alsa -type f -o -type l); do SNDFILES="$SNDFILES --file snd/$f:$f"; done
MODS=$(python3 - <<'PY'
import glob, gzip, os
base = glob.glob('k/lib/modules/*')[0]
dep = {}
for line in open(base + '/modules.dep'):
    k, _, v = line.partition(':')
    dep[k] = v.split()
want = ['kernel/drivers/virtio/virtio_mmio.ko.gz', 'kernel/drivers/block/virtio_blk.ko.gz',
        'kernel/drivers/net/virtio_net.ko.gz', 'kernel/drivers/gpu/drm/virtio/virtio-gpu.ko.gz',
        'kernel/drivers/virtio/virtio_input.ko.gz', 'kernel/sound/virtio/virtio_snd.ko.gz', 'kernel/net/packet/af_packet.ko.gz', 'kernel/drivers/input/evdev.ko.gz']
order = []
def visit(m):
    if m in order: return
    for d in dep.get(m, []): visit(d)
    order.append(m)
for w in want: visit(w)
out = []
for m in order:
    dst = 'mods/' + os.path.basename(m)[:-3]
    os.makedirs('mods', exist_ok=True)
    open(dst, 'wb').write(gzip.open(base + '/' + m).read())
    out.append('--module ' + dst)
print(' '.join(out))
PY
)
python3 "$ROOT/tools/mkinitramfs.py" bb/bin/busybox.static initrd.gz $MODS $SNDFILES
cp Image.gz "$OUT/alpine-Image.gz"
cp initrd.gz "$OUT/alpine-initrd.gz"

# ===== أندرويد (اختياري) =====
if [ $ANDROID = 1 ]; then
  sh "$ROOT/android/scripts/fetch-gki.sh" "$WORK/gki"
  sh "$ROOT/android/scripts/mkvendorboot.sh" "$WORK/gki"
  sh "$ROOT/android/scripts/build-system.sh" "$WORK"
  cp gki/boot-6.1-gz.img "$OUT/android-boot.img"
  cp gki/vb.img "$OUT/android-vendor_boot.img"
  cp gki/init_boot.img "$OUT/android-init_boot.img"
  gzip -c -6 sys.img > "$OUT/android-system.img.gz"
  stat -c %s sys.img > "$OUT/.system-size"
fi

if [ $TV = 1 ]; then
  sh "$ROOT/android/scripts/fetch-gki.sh" "$WORK/gki"
  (cd "$WORK/gki" && HW=ranchu FSTAB_FILE="$ROOT/android/tv/fstab.gtv" sh "$ROOT/android/scripts/mkvendorboot.sh" . hardwareegl=angle && cp vb.img "$OUT/tv-vendor_boot.img")
  cp "$WORK/gki/boot-6.1-gz.img" "$OUT/tv-boot.img"
  cp "$WORK/gki/init_boot.img" "$OUT/tv-init_boot.img"
  if [ -z "$CF_PARTS" ]; then
    CF_PARTS="$WORK/cf"; mkdir -p "$CF_PARTS"
    for f in com.google.cf.vulkan.apex com.google.cf.gralloc.apex com.android.hardware.graphics.composer.ranchu.apex \
             mapper.minigbm.so libminigbm_gralloc.so libc++.so libdmabufheap.so mapper.minigbm.xml; do
      [ -f "$CF_PARTS/$f" ] || curl -sfL -o "$CF_PARTS/$f" "https://raw.githubusercontent.com/za7ef/gtv-emu/tv-cf-parts/cf/$f"
    done
  fi
  APK_CACHE="$WORK/apk" python3 "$ROOT/tools/mktvprep.py" "$OUT/alpine-initrd.gz" "$CF_PARTS" "$OUT/tv-prep-initrd.gz"
fi

# ===== manifest.json =====
cd "$OUT"
python3 - "$ANDROID" "$TV" <<'PY'
import hashlib, json, os, sys
def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()
def ent(role, name, asset=None, gunzip=False, size=None):
    a = asset or name
    e = {"role": role, "name": name, "sha256": sha(a), "size": size or os.path.getsize(a)}
    if asset: e["asset"] = asset
    if gunzip: e["gunzip"] = True
    return e
profiles = [{
    "id": "alpine-test", "name": "Alpine Linux (اختبار)", "kind": "linux",
    "note": "نظام خفيف لاختبار الشاشة والريموت والصوت",
    "description": "نواة Alpine linux-virt مع busybox ووحدات virtio وaplay. تقلع إلى صدفة على الطرفية خلال ثوانٍ مع JIT، وتختبر الشاشة واللمس والريموت والصوت.",
    "ramMB": 512, "cores": 2,
    "files": [ent("kernel", "alpine-Image.gz"), ent("initrd", "alpine-initrd.gz")]}]
if sys.argv[1] == "1":
    profiles.append({
        "id": "android-gtv", "name": "Android TV", "kind": "android",
        "experimental": True, "note": "تجريبي: أندرويد أساسي يتوقف عند شعار الإقلاع، وواجهة التلفاز لم تكتمل بعد",
        "description": "نواة GKI الرسمية مع vendor_boot خاص وصورة نظام مصغّرة. تصل إلى شاشة الإقلاع؛ تحتاج JIT ووقتًا طويلًا.",
        "ramMB": 1024, "cores": 2,
        "files": [ent("boot", "android-boot.img"), ent("vendor_boot", "android-vendor_boot.img"), ent("init_boot", "android-init_boot.img"),
                  ent("disk", "android-system.img", asset="android-system.img.gz", gunzip=True, size=int(open(".system-size").read()))]})
if len(sys.argv) > 2 and sys.argv[2] == "1":
    profiles.append({
        "id": "android-tv", "name": "Android TV 16", "kind": "android",
        "experimental": True,
        "note": "تجريبي: يصل إلى شعار الإقلاع، وواجهة التلفاز لم تكتمل بعد",
        "description": "نظام Android TV 16 الرسمي من Google (API 36). يُنزَّل من خوادم Google مباشرة، ثم يُجهَّز على الجهاز: استخراج الأقسام وإضافة مكدّس رسوميات برمجي من AOSP. يحتاج JIT ونحو 3 غيغابايت من المساحة.",
        "notice": "صورة النظام مملوكة لشركة Google وتخضع لترخيص Android SDK. تُنزَّل من dl.google.com إلى جهازك مباشرة، ولا يعيد هذا التطبيق توزيعها.",
        "ramMB": 1536, "cores": 2,
        "files": [
            {"role": "source", "name": "android-tv-36_r04.zip", "url": "https://dl.google.com/android/repository/sys-img/android-tv/arm64-v8a-36_r04.zip",
             "size": 970375839, "sha256": "7398e2f00e5df28332027d5a131fcbefb8d5823bc7103a65a035575d271ffcca", "transient": True},
            ent("boot", "tv-boot.img"), ent("vendor_boot", "tv-vendor_boot.img"), ent("init_boot", "tv-init_boot.img"),
            dict(ent("prep_kernel", "prep-Image.gz", asset="alpine-Image.gz")),
            ent("prep_initrd", "tv-prep-initrd.gz"),
            {"role": "disk", "name": "tvdisk.img", "generated": True, "size": 5247074304}]})
json.dump({"version": 1, "profiles": profiles}, open("manifest.json", "w"), ensure_ascii=False, indent=2)
print(open("manifest.json").read())
PY
rm -f .system-size
ls -la "$OUT"
