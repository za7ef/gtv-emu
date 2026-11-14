#!/usr/bin/env python3
"""بناء initramfs بصيغة cpio newc مضغوطة gzip، دون الحاجة لأداة cpio."""
import gzip, os, stat, sys, argparse

def entry(name, mode, data=b'', dev=(0, 0), ino=[1000]):
    ino[0] += 1
    nb = name.encode() + b'\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in (
        ino[0], mode, 0, 0, 1, 0, len(data), 0, 0, dev[0], dev[1], len(nb), 0))
    out = hdr + nb
    out += b'\0' * ((4 - len(out) % 4) % 4)
    out += data
    out += b'\0' * ((4 - len(data) % 4) % 4)
    return out

INIT = r'''#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin:/sbin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
echo
echo "=== gtv-emu: shell ready ==="
uname -a
for m in /lib/modules/*.ko; do [ -f "$m" ] && insmod "$m"; done
sleep 1
if [ -b /dev/vda ]; then
  echo "virtio-blk: /dev/vda sectors=$(cat /sys/block/vda/size)"
  echo "virtio-blk read: $(head -c 24 /dev/vda)"
  echo "WRITTEN-BY-GUEST" | dd of=/dev/vda bs=512 seek=1 conv=notrunc 2>/dev/null && sync && echo "virtio-blk write: ok"
fi
if [ -c /dev/tty1 ] && [ -e /sys/class/graphics/fb0 ]; then
  echo "virtio-gpu: $(cat /sys/class/graphics/fb0/name) $(cat /sys/class/graphics/fb0/virtual_size)"
  printf '\n  gtv-emu: virtio-gpu framebuffer console\n\n' > /dev/tty1
  setsid sh -c 'exec sh </dev/tty1 >/dev/tty1 2>&1' &
fi
[ -d /sys/class/net/eth0 ] && echo "virtio-net: eth0 $(cat /sys/class/net/eth0/address)"
for e in /sys/class/input/input*/name; do [ -f "$e" ] && echo "input: $(cat $e)"; done
exec setsid sh -c 'exec sh </dev/ttyAMA0 >/dev/ttyAMA0 2>&1'
'''

UDHCPC = r'''#!/bin/busybox sh
case "$1" in bound|renew)
  ip addr flush dev $interface
  ip addr add $ip/${mask:-24} dev $interface
  ip route add default via $router
  echo "nameserver $dns" > /etc/resolv.conf
  echo "dhcp: $ip gw $router dns $dns";;
esac
'''

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('busybox')
    ap.add_argument('out')
    ap.add_argument('--module', action='append', default=[])
    ap.add_argument('--file', action='append', default=[], help='مصدر:وجهة لإضافة ملف أو رابط رمزي')
    a = ap.parse_args()
    data = b''
    for d in ('bin', 'sbin', 'dev', 'proc', 'sys', 'etc', 'tmp', 'lib', 'lib/modules', 'mnt', 'root'):
        data += entry(d, stat.S_IFDIR | 0o755)
    data += entry('dev/console', stat.S_IFCHR | 0o600, dev=(5, 1))
    data += entry('dev/null', stat.S_IFCHR | 0o666, dev=(1, 3))
    data += entry('bin/busybox', stat.S_IFREG | 0o755, open(a.busybox, 'rb').read())
    data += entry('bin/sh', stat.S_IFLNK | 0o777, b'busybox')
    data += entry('init', stat.S_IFREG | 0o755, INIT.encode())
    data += entry('etc/udhcpc.sh', stat.S_IFREG | 0o755, UDHCPC.encode())
    data += entry('etc/hostname', stat.S_IFREG | 0o644, b'gtv-emu\n')
    made = set()
    for spec in a.file:
        src, dst = spec.split(':', 1)
        parts = dst.split('/')[:-1]
        for k in range(1, len(parts) + 1):
            dd = '/'.join(parts[:k])
            if dd and dd not in made and dd not in ('bin', 'sbin', 'dev', 'proc', 'sys', 'etc', 'tmp', 'lib', 'lib/modules', 'mnt', 'root'):
                data += entry(dd, stat.S_IFDIR | 0o755); made.add(dd)
        if os.path.islink(src):
            data += entry(dst, stat.S_IFLNK | 0o777, os.readlink(src).encode())
        else:
            data += entry(dst, stat.S_IFREG | (os.stat(src).st_mode & 0o777), open(src, 'rb').read())
    for i, m in enumerate(a.module):
        data += entry('lib/modules/%02d-%s' % (i, os.path.basename(m)), stat.S_IFREG | 0o644, open(m, 'rb').read())
    data += entry('TRAILER!!!', 0)
    data += b'\0' * ((512 - len(data) % 512) % 512)
    open(a.out, 'wb').write(gzip.compress(data, 9))

main()
