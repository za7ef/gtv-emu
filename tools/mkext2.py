#!/usr/bin/env python3
"""إنشاء صورة ext2 صغيرة (مراجعة 0، كتل 1024 بايت، مجموعة واحدة) من شجرة ملفات وصفية.

لا يحتاج أدوات e2fsprogs. يكفي لصور اختبار حتى 8 ميغابايت، ويركّبها تعريف ext4 في لينكس.
الاستخدام:
  mkext2.py out.img --dir system/bin --file busybox:system/bin/busybox:755 \
                    --symlink busybox:system/bin/sh --file init.sh:system/bin/init:755
"""
import argparse
import struct
import time

BS = 1024
NBLOCKS = 8192
NINODES = 256
ITABLE = 3 + 2          # الكتلة الأولى لجدول العُقد
ITBLOCKS = NINODES * 128 // BS


class FS:
    def __init__(self):
        self.img = bytearray(NBLOCKS * BS)
        self.next_block = ITABLE + ITBLOCKS
        self.next_ino = 11
        self.inodes = {}     # رقم العقدة -> بايتات
        self.dirs = {'': 2}  # المسار -> رقم العقدة
        self.entries = {2: [('.', 2), ('..', 2)]}
        self.ndirs = 1
        self.now = int(time.time())

    def alloc(self, n):
        b = self.next_block
        self.next_block += n
        if self.next_block > NBLOCKS:
            raise SystemExit('الصورة ممتلئة')
        return b

    def write_data(self, data):
        """يكتب البيانات ويعيد (مؤشرات i_block، عدد الكتل المستخدمة)."""
        nb = (len(data) + BS - 1) // BS
        blocks = []
        for i in range(nb):
            b = self.alloc(1)
            self.img[b * BS:b * BS + len(data[i * BS:(i + 1) * BS])] = data[i * BS:(i + 1) * BS]
            blocks.append(b)
        ptr = blocks[:12] + [0] * (12 - min(12, len(blocks)))
        used = len(blocks)
        rest = blocks[12:]
        per = BS // 4
        if rest:
            ind = self.alloc(1); used += 1
            chunk = rest[:per]; rest = rest[per:]
            struct.pack_into('<%dI' % len(chunk), self.img, ind * BS, *chunk)
            ptr.append(ind)
        else:
            ptr.append(0)
        if rest:
            dind = self.alloc(1); used += 1
            k = 0
            while rest:
                ind = self.alloc(1); used += 1
                chunk = rest[:per]; rest = rest[per:]
                struct.pack_into('<%dI' % len(chunk), self.img, ind * BS, *chunk)
                struct.pack_into('<I', self.img, dind * BS + 4 * k, ind)
                k += 1
                if k >= per:
                    raise SystemExit('الملف أكبر من المدعوم')
            ptr.append(dind)
        else:
            ptr.append(0)
        ptr.append(0)
        return ptr, used

    def inode(self, mode, size, ptr, used, links, raw_block=None):
        ino = bytearray(128)
        struct.pack_into('<HHIIIIIHHI', ino, 0, mode, 0, size, self.now, self.now, self.now, 0, 0, links, used * (BS // 512))
        if raw_block is not None:
            ino[40:40 + len(raw_block)] = raw_block
        else:
            struct.pack_into('<15I', ino, 40, *ptr)
        return ino

    def mkdir(self, path):
        path = path.strip('/')
        if path in self.dirs:
            return self.dirs[path]
        parent = path.rsplit('/', 1)[0] if '/' in path else ''
        pino = self.mkdir(parent)
        ino = self.next_ino; self.next_ino += 1
        self.dirs[path] = ino
        self.entries[ino] = [('.', ino), ('..', pino)]
        self.entries[pino].append((path.rsplit('/', 1)[-1], ino))
        self.ndirs += 1
        return ino

    def add(self, path, mode, data=b'', link=None):
        path = path.strip('/')
        parent = path.rsplit('/', 1)[0] if '/' in path else ''
        pino = self.mkdir(parent)
        ino = self.next_ino; self.next_ino += 1
        if link is not None:
            t = link.encode()
            self.inodes[ino] = self.inode(0o120777, len(t), None, 0, 1, raw_block=t)
        else:
            ptr, used = self.write_data(data)
            self.inodes[ino] = self.inode(0o100000 | mode, len(data), ptr, used, 1)
        self.entries[pino].append((path.rsplit('/', 1)[-1], ino))

    def finish(self, out):
        # كتابة المجلدات (كتلة واحدة لكل مجلد تكفي لاختبارنا)
        for path, ino in self.dirs.items():
            ents = self.entries[ino]
            blk = bytearray(BS)
            off = 0
            for i, (name, child) in enumerate(ents):
                nb = name.encode()
                rec = (8 + len(nb) + 3) & ~3
                if i == len(ents) - 1:
                    rec = BS - off
                if off + rec > BS:
                    raise SystemExit('مجلد كبير جدًا')
                struct.pack_into('<IHH', blk, off, child, rec, len(nb))
                blk[off + 8:off + 8 + len(nb)] = nb
                off += rec
            ptr, used = self.write_data(bytes(blk))
            nsub = sum(1 for n, c in ents[2:] if c in self.dirs.values())
            self.inodes[ino] = self.inode(0o040755, BS, ptr, used, 2 + nsub)
        for ino, raw in self.inodes.items():
            o = ITABLE * BS + (ino - 1) * 128
            self.img[o:o + 128] = raw
        used_blocks = self.next_block
        free_blocks = NBLOCKS - used_blocks
        free_inodes = NINODES - (self.next_ino - 1)
        # خريطة الكتل: البت i يقابل الكتلة 1+i
        bb = bytearray(BS)
        for i in range(8192):
            blk = 1 + i
            if blk < used_blocks or blk >= NBLOCKS:
                bb[i // 8] |= 1 << (i % 8)
        self.img[3 * BS:4 * BS] = bb
        ib = bytearray(BS)
        for i in range(8192):
            if i < self.next_ino - 1 or i >= NINODES:
                ib[i // 8] |= 1 << (i % 8)
        self.img[4 * BS:5 * BS] = ib
        struct.pack_into('<IIIHHH', self.img, 2 * BS, 3, 4, ITABLE, free_blocks, free_inodes, self.ndirs)
        sb = bytearray(1024)
        struct.pack_into('<IIIIIIIIIIIIIHhHHHHIIII', sb, 0,
                         NINODES, NBLOCKS, 0, free_blocks, free_inodes, 1, 0, 0, 8192, 8192, NINODES,
                         self.now, self.now, 0, -1, 0xEF53, 1, 1, 0, self.now, 0, 0, 0)
        self.img[1024:2048] = sb
        open(out, 'wb').write(self.img)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out')
    ap.add_argument('--dir', action='append', default=[])
    ap.add_argument('--file', action='append', default=[], help='مصدر:وجهة[:صلاحيات ثمانية]')
    ap.add_argument('--symlink', action='append', default=[], help='هدف:وجهة')
    a = ap.parse_args()
    fs = FS()
    for d in a.dir:
        fs.mkdir(d)
    for spec in a.file:
        p = spec.split(':')
        fs.add(p[1], int(p[2], 8) if len(p) > 2 else 0o644, open(p[0], 'rb').read())
    for spec in a.symlink:
        t, d = spec.split(':', 1)
        fs.add(d, 0, link=t)
    fs.finish(a.out)


if __name__ == '__main__':
    main()
