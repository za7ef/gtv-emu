#!/usr/bin/env python3
"""بناء قرص أولي للمورّد (vendor ramdisk) يفهمه init أندرويد في المرحلة الأولى.

يحتوي:
  - fstab.<hardware> في الجذر وفي /first_stage_ramdisk.
  - وحدات النواة في /lib/modules مع modules.load وmodules.dep (تُستخرج التبعيات من modinfo).
الاستخدام:
  mkvendorramdisk.py out.gz --hardware gtv --fstab "/dev/block/vda /system ext4 ro wait,first_stage_mount" \
                     --module virtio_mmio.ko --module virtio_blk.ko
"""
import argparse
import gzip
import os
import re
import stat


def entry(name, mode, data=b'', ino=[2000]):
    ino[0] += 1
    nb = name.encode() + b'\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in (
        ino[0], mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0))
    out = hdr + nb
    out += b'\0' * ((4 - len(out) % 4) % 4)
    out += data
    out += b'\0' * ((4 - len(data) % 4) % 4)
    return out


def modinfo(data, key):
    m = re.search(rb'\0' + key + rb'=([^\0]*)', data)
    return m.group(1).decode() if m else ''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out')
    ap.add_argument('--hardware', default='gtv')
    ap.add_argument('--fstab', action='append', default=[])
    ap.add_argument('--fstab-file', help='ملف fstab كامل بدل --fstab')
    ap.add_argument('--module', action='append', default=[])
    a = ap.parse_args()
    data = b''
    for d in ('first_stage_ramdisk', 'lib', 'lib/modules'):
        data += entry(d, stat.S_IFDIR | 0o755)
    fstab = open(a.fstab_file, 'rb').read() if a.fstab_file else ''.join(l + '\n' for l in a.fstab).encode()
    data += entry('fstab.' + a.hardware, stat.S_IFREG | 0o644, fstab)
    data += entry('first_stage_ramdisk/fstab.' + a.hardware, stat.S_IFREG | 0o644, fstab)
    names = {}
    blobs = []
    for m in a.module:
        b = open(m, 'rb').read()
        fn = os.path.basename(m)
        names[(modinfo(b, b'name') or fn[:-3]).replace('-', '_')] = fn
        blobs.append((fn, b))
    dep = ''
    for fn, b in blobs:
        deps = [names[d.replace('-', '_')] for d in modinfo(b, b'depends').split(',') if d and d.replace('-', '_') in names]
        dep += '%s:%s\n' % (fn, ''.join(' ' + d for d in deps))
        data += entry('lib/modules/' + fn, stat.S_IFREG | 0o644, b)
    data += entry('lib/modules/modules.load', stat.S_IFREG | 0o644, ''.join(fn + '\n' for fn, _ in blobs).encode())
    data += entry('lib/modules/modules.dep', stat.S_IFREG | 0o644, dep.encode())
    data += entry('lib/modules/modules.alias', stat.S_IFREG | 0o644, b'')
    data += entry('lib/modules/modules.softdep', stat.S_IFREG | 0o644, b'')
    data += entry('TRAILER!!!', 0)
    data += b'\0' * ((512 - len(data) % 512) % 512)
    open(a.out, 'wb').write(gzip.compress(data, 9))


if __name__ == '__main__':
    main()
