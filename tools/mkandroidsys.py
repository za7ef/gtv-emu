#!/usr/bin/env python3
"""بناء صورة نظام أندرويد مصغّرة (جذر واحد يضم system وvendor وsystem_ext وproduct) من
أرشيف target_files بعيد، بتنزيل الملفات المطلوبة وحدها عبر HTTP Range.

- تُختار الملفات بأنماط (glob) ثم تُضاف تبعيات ELF ‏(DT_NEEDED) تلقائيًا.
- تُطبَّق المالكية والصلاحيات والقدرات من META/*filesystem_config.txt.
- تُوسم الملفات بسمات SELinux من ملفات file_contexts النصية في الصورة نفسها.
الاستخدام:
  mkandroidsys.py --url URL --cache DIR --out system.img --list files.txt [--overlay src:dst:mode ...]
"""
import argparse
import fnmatch
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tfzip import RemoteZip            # noqa: E402
from ext2img import open_image          # noqa: E402

# بادئة الأرشيف -> (مسار داخل الصورة، ملف fs_config، المسار الظاهر وقت التشغيل)
PARTS = [
    ('ROOT/', '', 'META/root_filesystem_config.txt'),
    ('SYSTEM/', 'system/', 'META/filesystem_config.txt'),
    ('SYSTEM_EXT/', 'system_ext/', 'META/system_ext_filesystem_config.txt'),
    ('PRODUCT/', 'product/', 'META/product_filesystem_config.txt'),
    ('VENDOR/', 'vendor/', 'META/vendor_filesystem_config.txt'),
    ('ODM/', 'vendor/odm/', 'META/odm_filesystem_config.txt'),
]
CONTEXTS = ['SYSTEM/etc/selinux/plat_file_contexts', 'SYSTEM_EXT/etc/selinux/system_ext_file_contexts',
            'PRODUCT/etc/selinux/product_file_contexts', 'VENDOR/etc/selinux/vendor_file_contexts',
            'ODM/etc/selinux/odm_file_contexts']
LIBDIRS = {'SYSTEM/': ['SYSTEM/lib64/', 'SYSTEM_EXT/lib64/'],
           'SYSTEM_EXT/': ['SYSTEM_EXT/lib64/', 'SYSTEM/lib64/'],
           'PRODUCT/': ['PRODUCT/lib64/', 'SYSTEM/lib64/'],
           'VENDOR/': ['VENDOR/lib64/', 'VENDOR/lib64/hw/', 'SYSTEM/lib64/'],
           'ODM/': ['ODM/lib64/', 'VENDOR/lib64/', 'SYSTEM/lib64/']}


def map_path(name):
    for pre, dst, _ in PARTS:
        if name.startswith(pre):
            return dst + name[len(pre):]
    return None


def fs_config_key(name):
    for pre, dst, _ in PARTS:
        if name.startswith(pre):
            rest = name[len(pre):].rstrip('/')
            if pre == 'ROOT/':
                return rest
            return {'ODM/': 'odm/'}.get(pre, dst) + rest if rest else dst.rstrip('/')
    return None


def elf_needed(path):
    try:
        d = open(path, 'rb').read()
    except OSError:
        return []
    if d[:4] != b'\x7fELF' or d[4] != 2:
        return []
    phoff, = struct.unpack_from('<Q', d, 32)
    phentsize, phnum = struct.unpack_from('<HH', d, 54)
    dyn = None
    loads = []
    for i in range(phnum):
        o = phoff + i * phentsize
        ptype, = struct.unpack_from('<I', d, o)
        poff, pvaddr = struct.unpack_from('<QQ', d, o + 8)
        pfilesz, = struct.unpack_from('<Q', d, o + 32)
        if ptype == 2:
            dyn = (poff, pfilesz)
        elif ptype == 1:
            loads.append((pvaddr, poff, pfilesz))
    if not dyn:
        return []

    def v2o(v):
        for va, off, sz in loads:
            if va <= v < va + sz:
                return v - va + off
        return None
    needed, strtab = [], None
    for i in range(0, dyn[1], 16):
        tag, val = struct.unpack_from('<qQ', d, dyn[0] + i)
        if tag == 0:
            break
        if tag == 1:
            needed.append(val)
        elif tag == 5:
            strtab = v2o(val)
    if strtab is None:
        return []
    out = []
    for n in needed:
        e = d.index(b'\0', strtab + n)
        out.append(d[strtab + n:e].decode())
    return out


class Labeler:
    def __init__(self, files):
        meta, plain = [], []
        for f in files:
            if not os.path.exists(f):
                continue
            for line in open(f, errors='replace'):
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split()
                if len(parts) == 2:
                    rx, ftype, ctx = parts[0], None, parts[1]
                elif len(parts) == 3:
                    rx, ftype, ctx = parts
                else:
                    continue
                try:
                    c = re.compile('^(?:' + rx + ')$')
                except re.error:
                    continue
                spec = (c, ftype, ctx)
                (meta if re.search(r'[.^$?*+|\[\](){}\\]', rx) else plain).append(spec)
        self.specs = meta + plain   # الأدق (بلا رموز خاصة) يأتي أخيرًا فيفوز

    def label(self, path, kind):
        want = {'f': '--', 'd': '-d', 'l': '-l'}[kind]
        for c, ftype, ctx in reversed(self.specs):
            if ftype is not None and ftype != want:
                continue
            if c.match(path):
                return None if ctx == '<<none>>' else ctx
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--url', required=True)
    ap.add_argument('--cache', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--list', required=True, help='أنماط الملفات، نمط في كل سطر؛ يبدأ الاستبعاد بـ!')
    ap.add_argument('--overlay', action='append', default=[], help='مصدر:وجهة[:صلاحيات] ملف يضاف أو يستبدل')
    ap.add_argument('--dir', action='append', default=[], help='دليل فارغ إضافي داخل الصورة')
    ap.add_argument('--max-size', type=int, default=48 << 20)
    a = ap.parse_args()
    z = RemoteZip(a.url, a.cache)
    names = list(z.ents)
    inc, exc = [], []
    for line in open(a.list):
        line = line.split('#', 1)[0].strip()
        if line:
            (exc if line.startswith('!') else inc).append(line.lstrip('!'))
    sel = set()
    for n in names:
        if n.endswith('/'):
            continue
        if any(fnmatch.fnmatchcase(n, p) for p in inc) and not any(fnmatch.fnmatchcase(n, p) for p in exc):
            if z.ents[n][3] <= a.max_size:
                sel.add(n)
    # كل الروابط الرمزية في أدلة الأوامر (إلى toybox وما شابه)؛ صغيرة جدًا
    for n in names:
        if (n.startswith('SYSTEM/bin/') or n.startswith('VENDOR/bin/')) and z.is_symlink(n):
            sel.add(n)
    meta = [p[2] for p in PARTS] + CONTEXTS
    z.fetch_many([m for m in meta if m in z.ents])
    # تبعيات ELF
    todo = list(sel)
    missing = set()
    while todo:
        z.fetch_many([n for n in todo if not z.is_symlink(n)])
        nxt = []
        for n in todo:
            if z.is_symlink(n):
                continue
            pre = n.split('/', 1)[0] + '/'
            for lib in elf_needed(z.path(n)):
                for d in LIBDIRS.get(pre, ['SYSTEM/lib64/']):
                    cand = d + lib
                    if cand in z.ents:
                        if cand not in sel:
                            sel.add(cand); nxt.append(cand)
                        break
                else:
                    missing.add(lib)
        todo = nxt
    # fs_config
    cfg = {}
    for _, _, f in PARTS:
        p = z.path(f)
        if os.path.exists(p):
            for line in open(p):
                parts = line.rstrip('\n').split(' ')
                if len(parts) >= 4:
                    cap = 0
                    for x in parts[4:]:
                        if x.startswith('capabilities='):
                            cap = int(x.split('=')[1], 16)
                    cfg[parts[0]] = (int(parts[1]), int(parts[2]), int(parts[3], 8), cap)
    lab = Labeler([z.path(c) for c in CONTEXTS])

    def xattrs(img_path, kind, cap=0):
        x = {}
        ctx = lab.label('/' + img_path.rstrip('/') if img_path else '/', kind)
        if ctx:
            x['security.selinux'] = ctx.encode() + b'\0'
        if cap:
            x['security.capability'] = struct.pack('<IIIII', 0x02000001, cap & 0xFFFFFFFF, 0, cap >> 32, 0)
        return x

    fs = open_image(a.out)
    overlays = {}
    for o in a.overlay:
        p = o.split(':')
        overlays[p[1].strip('/')] = (p[0], int(p[2], 8) if len(p) > 2 else 0o644)
    # الأدلة: كل أدلة الجذر وأدلة الأقسام الأساسية، ثم أدلة آباء الملفات المختارة
    dirs = set()
    for n in names:
        if n.startswith('ROOT/') and n.endswith('/'):
            dirs.add(n)
    for n in sel:
        parts = n.split('/')
        for k in range(1, len(parts)):
            dirs.add('/'.join(parts[:k]) + '/')
    for d in sorted(dirs, key=lambda s: s.count('/')):
        ip = map_path(d)
        if ip is None:
            continue
        ip = ip.rstrip('/')
        uid, gid, mode, _ = cfg.get(fs_config_key(d), (0, 0, 0o755, 0))
        # قد يكون المدخل في fs_config لرابط رمزي في القسم الأصلي (مثل vendor/odm)،
        # فيُضمن للدليل إذن العبور
        mode |= (mode & 0o444) >> 2
        if ip == '':
            fs.mkdir('', mode, uid, gid, xattrs('', 'd'))
        else:
            fs.mkdir(ip, mode, uid, gid, xattrs(ip, 'd'))
    for d in a.dir:
        fs.mkdir(d.strip('/'), 0o755, 0, 0, xattrs(d.strip('/'), 'd'))
    root_links = [n for n in names if n.startswith('ROOT/') and z.is_symlink(n)]
    z.fetch_many(root_links)
    total = 0
    for n in sorted(sel | set(root_links)):
        ip = map_path(n)
        if ip is None or ip in overlays:
            continue
        uid, gid, mode, cap = cfg.get(fs_config_key(n), (0, 0, 0o644, 0))
        if z.is_symlink(n):
            tgt = open(z.fetch(n)).read()
            fs.add_symlink(ip, tgt, uid, gid, xattrs(ip, 'l'))
            continue
        p = z.path(n)

        def gen(p=p):
            with open(p, 'rb') as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    yield b
        fs.add_file(ip, gen(), mode, uid, gid, xattrs(ip, 'f', cap))
        total += os.path.getsize(p)
    for ip, (src, mode) in overlays.items():
        fs.add_file(ip, open(src, 'rb').read(), mode, 0, 0, xattrs(ip, 'f'))
    size = fs.write(a.out)
    print('mkandroidsys: %d ملف، %.1f ميغابايت بيانات، صورة %.0f ميغابايت (متفرقة)' % (len(sel), total / 1e6, size / 1e6))
    if missing:
        print('mkandroidsys: مكتبات غير موجودة خارج APEX:', ' '.join(sorted(missing)))


if __name__ == '__main__':
    main()
