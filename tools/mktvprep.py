#!/usr/bin/env python3
"""بناء tv-prep-initrd.gz: قرص Alpine الأولي المنشور مع طبقة إضافية فيها e2fsprogs وسكربت تجهيز Android TV
ومكوّنات الرسوميات من AOSP Cuttlefish (رخصة Apache 2.0).

الاستخدام: mktvprep.py <alpine-initrd.gz> <مجلد-مكونات-cf> <الإخراج>
مجلد المكونات يحوي: com.google.cf.vulkan.apex وcom.google.cf.gralloc.apex
وcom.android.hardware.graphics.composer.ranchu.apex وmapper.minigbm.so وlibminigbm_gralloc.so
وlibc++.so وlibdmabufheap.so وmapper.minigbm.xml، مأخوذة من target_files لبناء Cuttlefish
(aosp_cf_arm64_only_phone-userdebug، البناء 16644040).
تُضاف حزم e2fsprogs من مستودع Alpine v3.22 (aarch64).
"""
import gzip, zlib, io, os, stat, sys, tarfile, urllib.request

ALP = 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64/'
APKS = ['e2fsprogs-1.47.2-r2', 'e2fsprogs-extra-1.47.2-r2', 'e2fsprogs-libs-1.47.2-r2', 'libcom_err-1.47.2-r2',
        'libblkid-2.41.6-r1', 'libuuid-2.41.6-r1', 'libeconf-0.6.3-r0', 'musl-1.2.5-r13']
CF = ['com.google.cf.vulkan.apex', 'com.google.cf.gralloc.apex', 'com.android.hardware.graphics.composer.ranchu.apex',
      'mapper.minigbm.so', 'libminigbm_gralloc.so', 'libc++.so', 'libdmabufheap.so', 'mapper.minigbm.xml']
HERE = os.path.dirname(os.path.abspath(__file__))
PREP = os.path.join(HERE, '..', 'android', 'tv', 'prep')

INIT = r'''#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null || { mount -t tmpfs dev /dev; mdev -s; }
mkdir -p /tmp && mount -t tmpfs tmp /tmp
for m in /lib/modules/*.ko; do [ -f "$m" ] && insmod "$m" 2>/dev/null; done
sleep 1; mdev -s
echo "=== gtv-emu: Android TV prepare ==="
sh /prep/prep.sh
sync
poweroff -f
'''

ino = [5000]
def entry(name, mode, data=b'', dev=(0, 0)):
    ino[0] += 1
    nb = name.encode() + b'\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in (ino[0], mode, 0, 0, 1, 0, len(data), 0, 0, dev[0], dev[1], len(nb), 0))
    out = hdr + nb
    out += b'\0' * ((4 - len(out) % 4) % 4)
    out += data + b'\0' * ((4 - len(data) % 4) % 4)
    return out

def main():
    base, cfdir, out = sys.argv[1:4]
    cache = os.environ.get('APK_CACHE', '/tmp/apk-cache'); os.makedirs(cache, exist_ok=True)
    data = b''; dirs = set()
    def mkdirs(path):
        nonlocal data
        parts = path.split('/')[:-1]
        for k in range(1, len(parts) + 1):
            d = '/'.join(parts[:k])
            if d not in dirs:
                dirs.add(d); data += entry(d, stat.S_IFDIR | 0o755)
    for a in APKS:
        p = os.path.join(cache, a + '.apk')
        if not os.path.exists(p): urllib.request.urlretrieve(ALP + a + '.apk', p)
        # حزمة apk عبارة عن عدة أرشيفات gzip متتالية؛ الأخير فيه الملفات
        raw = open(p, 'rb').read(); pos = 0; members = []
        while pos < len(raw):
            d = zlib.decompressobj(31); chunk = d.decompress(raw[pos:])
            members.append(chunk); used = len(raw) - pos - len(d.unused_data); pos += used
            if not d.unused_data: break
        tf = tarfile.open(fileobj=io.BytesIO(members[-1]))
        for m in tf.getmembers():
            if m.name.startswith('.') or not (m.isfile() or m.issym()): continue
            if not (m.name.startswith(('lib/', 'usr/lib/', 'sbin/', 'usr/sbin/', 'bin/', 'usr/bin/'))): continue
            mkdirs(m.name)
            if m.issym(): data += entry(m.name, stat.S_IFLNK | 0o777, m.linkname.encode())
            else: data += entry(m.name, stat.S_IFREG | (m.mode & 0o777), tf.extractfile(m).read())
    data += entry('prep', stat.S_IFDIR | 0o755); dirs.add('prep')
    data += entry('prep/cf', stat.S_IFDIR | 0o755)
    for f in ('prep.sh', 'plat-patch.tsv'):
        data += entry('prep/' + f, stat.S_IFREG | 0o755, open(os.path.join(PREP, f), 'rb').read())
    for f in CF:
        data += entry('prep/cf/' + f, stat.S_IFREG | 0o644, open(os.path.join(cfdir, f), 'rb').read())
    data += entry('init', stat.S_IFREG | 0o755, INIT.encode())
    data += entry('TRAILER!!!', 0)
    data += b'\0' * ((512 - len(data) % 512) % 512)
    with open(out, 'wb') as f:
        f.write(open(base, 'rb').read())
        f.write(gzip.compress(data, 9, mtime=0))
    print(out, os.path.getsize(out))

main()
