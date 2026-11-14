#!/usr/bin/env python3
"""إنشاء صورتي boot.img وvendor_boot.img بالإصدار الرابع من صيغة أندرويد.

أداة بسيطة للاختبار، متوافقة مع تخطيط mkbootimg الرسمي:
  boot.img:        ترويسة (4096) ثم النواة ثم القرص الأولي العام، كل منها بمحاذاة 4096.
  vendor_boot.img: ترويسة ثم القرص الأولي للمورّد ثم dtb ثم جدول الأقراص ثم bootconfig.
"""
import argparse
import struct

PAGE = 4096


def pad(b, a=PAGE):
    return b + b'\0' * ((-len(b)) % a)


def boot_img(kernel, ramdisk, cmdline):
    hdr = b'ANDROID!' + struct.pack('<IIIII', len(kernel), len(ramdisk), 0, 1584, 0)
    hdr += struct.pack('<IIII', 0, 0, 0, 4)               # reserved[3]، ثم header_version
    hdr = hdr[:40] + struct.pack('<I', 4)
    hdr += cmdline.encode().ljust(1536, b'\0')
    hdr += struct.pack('<I', 0)                          # signature_size
    assert len(hdr) == 1584, len(hdr)
    return pad(hdr) + pad(kernel) + pad(ramdisk)


def vendor_boot_img(ramdisk, cmdline, bootconfig, name='gtv'):
    entry = struct.pack('<III', len(ramdisk), 0, 1)       # size، offset، type=PLATFORM
    entry += name.encode().ljust(32, b'\0') + b'\0' * 64  # name[32]، board_id[16]
    hdr = b'VNDRBOOT' + struct.pack('<IIII', 4, PAGE, 0, 0) + struct.pack('<I', len(ramdisk))
    hdr += cmdline.encode().ljust(2048, b'\0')
    hdr += struct.pack('<I', 0) + name.encode().ljust(16, b'\0')
    hdr += struct.pack('<IIQ', 2128, 0, 0)               # header_size، dtb_size، dtb_addr
    hdr += struct.pack('<IIII', len(entry), 1, len(entry), len(bootconfig))
    assert len(hdr) == 2128, len(hdr)
    return pad(hdr) + pad(ramdisk) + pad(entry) + pad(bootconfig)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel')
    ap.add_argument('--ramdisk')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--out')
    ap.add_argument('--vendor-ramdisk')
    ap.add_argument('--vendor-cmdline', default='')
    ap.add_argument('--bootconfig', action='append', default=[], help='سطر مثل androidboot.hardware=gtv')
    ap.add_argument('--vendor-out')
    a = ap.parse_args()
    if a.out:
        k = open(a.kernel, 'rb').read()
        r = open(a.ramdisk, 'rb').read() if a.ramdisk else b''
        open(a.out, 'wb').write(boot_img(k, r, a.cmdline))
    if a.vendor_out:
        r = open(a.vendor_ramdisk, 'rb').read() if a.vendor_ramdisk else b''
        bc = ''.join('%s = "%s"\n' % tuple(x.split('=', 1)) for x in a.bootconfig).encode()
        open(a.vendor_out, 'wb').write(vendor_boot_img(r, a.vendor_cmdline, bc))


if __name__ == '__main__':
    main()
