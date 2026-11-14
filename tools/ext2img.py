#!/usr/bin/env python3
"""كاتب صور ext2 (مراجعة 1) يدعم عدة مجموعات وكتلًا بحجم 4 كيلوبايت وعُقدًا بحجم 256 بايت
مع سمات ممتدة (security.selinux وsecurity.capability)، دون الحاجة لأدوات e2fsprogs.

يُكتب الملف بشكل متفرق (sparse) فلا يستهلك القرص إلا ما كُتب فعلًا. يركّبه تعريف ext4 في لينكس.
الاستخدام البرمجي:
    fs = Ext2Image()
    fs.mkdir('system/bin', mode=0o755, uid=0, gid=2000, xattrs={'selinux': b'u:object_r:system_file:s0\\0'})
    fs.add_file('system/bin/sh', data, mode=0o755)
    fs.add_symlink('system/bin/ls', 'toybox')
    fs.write('system.img')
"""
import struct
import time

BS = 4096
BPG = 32768            # كتل لكل مجموعة
IPG = 4096             # عقد لكل مجموعة
ISZ = 256
ITB = IPG * ISZ // BS  # كتل جدول العقد لكل مجموعة
META = 4 + ITB         # كتل البيانات الوصفية في بداية كل مجموعة

XATTR_INDEX = {'user': 1, 'trusted': 4, 'security': 6}


def _xattr_entry_hash(name, value):
    h = 0
    for ch in name:
        h = ((h << 5) ^ (h >> 27) ^ ch) & 0xFFFFFFFF
    v = value + b'\0' * ((4 - len(value) % 4) % 4)
    for i in range(0, len(v), 4):
        w, = struct.unpack_from('<I', v, i)
        h = ((h << 16) ^ (h >> 16) ^ w) & 0xFFFFFFFF
    return h


class Ext2Image:
    def __init__(self, ngroups=None):
        self.now = int(time.time())
        self.inodes = {}           # رقم -> (بايتات 256)
        self.next_ino = 11
        self.chunks = {}           # رقم الكتلة -> بيانات (تُكتب لاحقًا)
        self.cur_group = 0
        self.cur_block = META      # أول كتلة بيانات في المجموعة 0
        self.dirs = {'': 2}
        self.entries = {2: [('.', 2, 2), ('..', 2, 2)]}
        self.dirmeta = {2: (0o755, 0, 0, {})}
        self.out = None
        self.used_blocks = {}      # المجموعة -> عدد كتل البيانات المستخدمة
        self.ndirs = {0: 1}
        self.max_groups = ngroups or 128

    # ---------- التخصيص ----------
    def _open(self, path):
        self.out = open(path, 'wb+')

    def alloc(self):
        if self.cur_block >= BPG:
            self.cur_group += 1
            self.cur_block = META
            if self.cur_group >= self.max_groups:
                raise SystemExit('ext2img: الصورة ممتلئة')
        b = self.cur_group * BPG + self.cur_block
        self.cur_block += 1
        self.used_blocks[self.cur_group] = self.used_blocks.get(self.cur_group, 0) + 1
        return b

    def put_block(self, b, data):
        self.out.seek(b * BS)
        self.out.write(data)

    def new_ino(self):
        ino = self.next_ino
        self.next_ino += 1
        if (ino - 1) // IPG >= self.max_groups:
            raise SystemExit('ext2img: نفدت العقد')
        return ino

    # ---------- كتابة البيانات ----------
    def _write_data(self, data_iter):
        """يكتب كتل البيانات من مولّد ويعيد (المؤشرات الخمسة عشر، عدد الكتل الكلي، الحجم)."""
        blocks = []
        size = 0
        for chunk in data_iter:
            for i in range(0, len(chunk), BS):
                part = chunk[i:i + BS]
                if size % BS:
                    raise ValueError('يجب أن تكون الأجزاء مضاعفات لحجم الكتلة عدا الأخير')
                b = self.alloc()
                self.put_block(b, part)
                blocks.append(b)
                size += len(part)
        used = len(blocks)
        per = BS // 4
        ptr = blocks[:12] + [0] * (12 - min(12, len(blocks)))
        rest = blocks[12:]

        def pack_ind(lst):
            nonlocal used
            b = self.alloc(); used += 1
            self.put_block(b, struct.pack('<%dI' % len(lst), *lst))
            return b
        if rest:
            ptr.append(pack_ind(rest[:per])); rest = rest[per:]
        else:
            ptr.append(0)
        if rest:
            l1 = []
            while rest and len(l1) < per:
                l1.append(pack_ind(rest[:per])); rest = rest[per:]
            ptr.append(pack_ind(l1))
        else:
            ptr.append(0)
        if rest:
            raise SystemExit('ext2img: ملف أكبر من 4 غيغابايت غير مدعوم')
        ptr.append(0)
        return ptr, used, size

    def _xattr_body(self, xattrs):
        """يحاول وضع السمات داخل العقدة؛ يعيد بايتات المنطقة أو None إن لم تتسع."""
        ents = []
        for k, v in xattrs.items():
            pre, name = k.split('.', 1) if '.' in k else ('security', k)
            ents.append((XATTR_INDEX[pre], name.encode(), v))
        return ents

    def _make_inode(self, mode, uid, gid, size, ptr, used, links, xattrs, raw_block=None):
        ino = bytearray(ISZ)
        file_acl = 0
        blocks512 = used * (BS // 512)
        ents = self._xattr_body(xattrs or {})
        # المساحة داخل العقدة: 256 - 128 - 32 - 4 (الرقم السحري)
        need = sum(16 + ((len(n) + 3) & ~3) + ((len(v) + 3) & ~3) for _, n, v in ents) + 4
        if ents and need <= ISZ - 128 - 32 - 4:
            base = 128 + 32
            struct.pack_into('<I', ino, base, 0xEA020000)
            eoff = base + 4
            voff_end = ISZ - base - 4   # القيم تُرتب من نهاية المنطقة، والإزاحة نسبة لأول مدخل
            for idx, name, v in ents:
                vlen = (len(v) + 3) & ~3
                voff_end -= vlen
                struct.pack_into('<BBHIII', ino, eoff, len(name), idx, voff_end, 0, len(v), 0)
                ino[eoff + 16:eoff + 16 + len(name)] = name
                ino[base + 4 + voff_end:base + 4 + voff_end + len(v)] = v
                eoff += 16 + ((len(name) + 3) & ~3)
        elif ents:
            blk = bytearray(BS)
            eoff = 32
            vend = BS
            bh = 0
            for idx, name, v in sorted(ents, key=lambda e: (e[0], len(e[1]), e[1])):
                vlen = (len(v) + 3) & ~3
                vend -= vlen
                eh = _xattr_entry_hash(name, v)
                struct.pack_into('<BBHIII', blk, eoff, len(name), idx, vend, 0, len(v), eh)
                blk[eoff + 16:eoff + 16 + len(name)] = name
                blk[vend:vend + len(v)] = v
                eoff += 16 + ((len(name) + 3) & ~3)
                bh = ((bh << 16) ^ (bh >> 16) ^ eh) & 0xFFFFFFFF
            struct.pack_into('<IIII', blk, 0, 0xEA020000, 1, 1, bh)
            file_acl = self.alloc()
            self.put_block(file_acl, bytes(blk))
            blocks512 += BS // 512
        struct.pack_into('<HHIIIIIHHII', ino, 0, mode, uid & 0xFFFF, size & 0xFFFFFFFF,
                         self.now, self.now, self.now, 0, gid & 0xFFFF, links, blocks512, 0)
        if raw_block is not None:
            ino[40:40 + len(raw_block)] = raw_block
        else:
            struct.pack_into('<15I', ino, 40, *ptr)
        struct.pack_into('<I', ino, 104, file_acl)
        struct.pack_into('<I', ino, 108, size >> 32)
        struct.pack_into('<HH', ino, 120, uid >> 16, gid >> 16)
        struct.pack_into('<H', ino, 128, 32)
        return bytes(ino)

    # ---------- الواجهة ----------
    def mkdir(self, path, mode=0o755, uid=0, gid=0, xattrs=None):
        path = path.strip('/')
        if path in self.dirs:
            ino = self.dirs[path]
            if xattrs is not None or mode != 0o755 or uid or gid:
                self.dirmeta[ino] = (mode, uid, gid, xattrs or self.dirmeta[ino][3])
            return ino
        parent = path.rsplit('/', 1)[0] if '/' in path else ''
        pino = self.mkdir(parent)
        ino = self.new_ino()
        self.dirs[path] = ino
        self.entries[ino] = [('.', ino, 2), ('..', pino, 2)]
        self.dirmeta[ino] = (mode, uid, gid, xattrs or {})
        self.entries[pino].append((path.rsplit('/', 1)[-1], ino, 2))
        g = (ino - 1) // IPG
        self.ndirs[g] = self.ndirs.get(g, 0) + 1
        return ino

    def exists(self, path):
        path = path.strip('/')
        if path in self.dirs:
            return True
        parent = path.rsplit('/', 1)[0] if '/' in path else ''
        name = path.rsplit('/', 1)[-1]
        p = self.dirs.get(parent)
        return p is not None and any(e[0] == name for e in self.entries[p])

    def add_file(self, path, data, mode=0o644, uid=0, gid=0, xattrs=None):
        """data: بايتات أو مولّد أجزاء (كل جزء مضاعف لـ4096 عدا الأخير)."""
        path = path.strip('/')
        parent = path.rsplit('/', 1)[0] if '/' in path else ''
        pino = self.mkdir(parent)
        ino = self.new_ino()
        it = [data] if isinstance(data, (bytes, bytearray)) else data
        ptr, used, size = self._write_data(it)
        self.inodes[ino] = self._make_inode(0o100000 | (mode & 0o7777), uid, gid, size, ptr, used, 1, xattrs)
        self.entries[pino].append((path.rsplit('/', 1)[-1], ino, 1))
        return ino

    def add_symlink(self, path, target, uid=0, gid=0, xattrs=None):
        path = path.strip('/')
        parent = path.rsplit('/', 1)[0] if '/' in path else ''
        pino = self.mkdir(parent)
        ino = self.new_ino()
        t = target.encode()
        if len(t) < 60:
            self.inodes[ino] = self._make_inode(0o120777, uid, gid, len(t), None, 0, 1, xattrs, raw_block=t)
        else:
            ptr, used, size = self._write_data([t])
            self.inodes[ino] = self._make_inode(0o120777, uid, gid, len(t), ptr, used, 1, xattrs)
        self.entries[pino].append((path.rsplit('/', 1)[-1], ino, 7))
        return ino

    def _write_dirs(self):
        for path, ino in self.dirs.items():
            blocks = []
            blk = bytearray(BS)
            off = 0
            last = None
            for name, child, ftype in self.entries[ino]:
                nb = name.encode()
                rec = (8 + len(nb) + 3) & ~3
                if off + rec > BS:
                    struct.pack_into('<H', blk, last + 4, BS - last)
                    blocks.append(bytes(blk))
                    blk = bytearray(BS); off = 0
                struct.pack_into('<IHBB', blk, off, child, rec, len(nb), ftype)
                blk[off + 8:off + 8 + len(nb)] = nb
                last = off
                off += rec
            struct.pack_into('<H', blk, last + 4, BS - last)
            blocks.append(bytes(blk))
            ptr, used, size = self._write_data(blocks)
            nsub = sum(1 for e in self.entries[ino][2:] if e[2] == 2)
            mode, uid, gid, xattrs = self.dirmeta[ino]
            self.inodes[ino] = self._make_inode(0o040000 | mode, uid, gid, size, ptr, used, 2 + nsub, xattrs)

    def write(self, path):
        """يجب استدعاء open_output أولًا ثم إضافة الملفات، ثم write لإنهاء الصورة."""
        self._write_dirs()
        ngroups = max(self.cur_group + 1, (self.next_ino - 2) // IPG + 1)
        nblocks = ngroups * BPG
        ninodes = ngroups * IPG
        gdt = bytearray(BS)
        tot_free_b = tot_free_i = 0
        for g in range(ngroups):
            base = g * BPG
            used_data = self.used_blocks.get(g, 0)
            # خريطة الكتل: الكتل الوصفية ثم كتل البيانات مستخدمة بالتتابع
            nused = META + used_data
            bb = bytearray(BS)
            for i in range(nused):
                bb[i // 8] |= 1 << (i % 8)
            self.put_block(base + 2, bytes(bb))
            first = g * IPG + 1
            iused = max(0, min(IPG, self.next_ino - first))
            ib = bytearray(BS)
            for i in range(iused):
                ib[i // 8] |= 1 << (i % 8)
            for i in range(IPG, BS * 8):
                ib[i // 8] |= 1 << (i % 8)
            self.put_block(base + 3, bytes(ib))
            # جدول العقد
            tbl = bytearray(ITB * BS)
            for ino in range(first, first + IPG):
                raw = self.inodes.get(ino)
                if raw:
                    o = (ino - first) * ISZ
                    tbl[o:o + ISZ] = raw
            if any(tbl):
                self.put_block(base + 4, bytes(tbl))
            free_b = BPG - nused
            free_i = IPG - iused
            tot_free_b += free_b; tot_free_i += free_i
            struct.pack_into('<IIIHHH', gdt, g * 32, base + 2, base + 3, base + 4, free_b, free_i, self.ndirs.get(g, 0))
        sb = bytearray(1024)
        struct.pack_into('<IIIIIIIIIIIIIHhHHHHIIIIHH', sb, 0,
                         ninodes, nblocks, 0, tot_free_b, tot_free_i, 0, 2, 2, BPG, BPG, IPG,
                         self.now, self.now, 0, -1, 0xEF53, 1, 1, 0, self.now, 0, 0, 1, 0, 0)
        struct.pack_into('<IH', sb, 84, 11, ISZ)             # s_first_ino وs_inode_size
        struct.pack_into('<III', sb, 92, 0x8, 0x2, 0)        # ext_attr / filetype / بلا ro_compat
        sb[104:120] = b'gtv-emu-android!'                    # UUID ثابت
        sb[120:136] = b'system'.ljust(16, b'\0')
        struct.pack_into('<H', sb, 348, 32)                  # s_min_extra_isize
        struct.pack_into('<H', sb, 350, 32)                  # s_want_extra_isize
        for g in range(ngroups):
            base = g * BPG
            s = bytearray(sb)
            struct.pack_into('<H', s, 90, g)                 # s_block_group_nr
            if g == 0:
                self.out.seek(1024); self.out.write(s)
            else:
                self.out.seek(base * BS); self.out.write(s)
            self.put_block(base + 1, bytes(gdt))
        self.out.truncate(nblocks * BS)
        self.out.close()
        return nblocks * BS


def open_image(path, max_groups=None):
    fs = Ext2Image(max_groups)
    fs._open(path)
    return fs
