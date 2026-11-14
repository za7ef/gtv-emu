#!/usr/bin/env python3
"""استخراج ملف واحد من أرشيف zip بعيد عبر طلبات HTTP Range، دون تنزيل الأرشيف كاملًا.

مفيد لصور بناء أندرويد على ci.android.com، إذ يتجاوز حجم الأرشيف غيغابايتًا بينما
لا يتجاوز init_boot.img بضعة ميغابايتات.
الاستخدام: fetchzipmember.py URL اسم_الملف_داخل_الأرشيف ملف_الإخراج
"""
import struct
import sys
import urllib.request
import zlib


def rng(url, a, b):
    req = urllib.request.Request(url, headers={'Range': 'bytes=%d-%d' % (a, b)})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def main():
    url, want, out = sys.argv[1:4]
    with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=60) as r:
        url = r.geturl()
        size = int(r.headers['Content-Length'])
    tail = rng(url, max(0, size - 70000), size - 1)
    i = tail.rfind(b'PK\x05\x06')
    n, cdsz, cdoff = struct.unpack_from('<HII', tail, i + 10)
    j = tail.rfind(b'PK\x06\x06')
    if j >= 0:  # zip64
        n, cdsz, cdoff = struct.unpack_from('<QQQ', tail, j + 32)
    cd = rng(url, cdoff, cdoff + cdsz - 1)
    p = 0
    while p < len(cd) and cd[p:p + 4] == b'PK\x01\x02':
        meth, = struct.unpack_from('<H', cd, p + 10)
        csz, usz = struct.unpack_from('<II', cd, p + 20)
        nl, el, cl = struct.unpack_from('<HHH', cd, p + 28)
        off, = struct.unpack_from('<I', cd, p + 42)
        name = cd[p + 46:p + 46 + nl].decode()
        ex = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(ex):
            hid, hl = struct.unpack_from('<HH', ex, q)
            if hid == 1:
                f = q + 4
                if usz == 0xFFFFFFFF: usz, = struct.unpack_from('<Q', ex, f); f += 8
                if csz == 0xFFFFFFFF: csz, = struct.unpack_from('<Q', ex, f); f += 8
                if off == 0xFFFFFFFF: off, = struct.unpack_from('<Q', ex, f); f += 8
            q += 4 + hl
        if name == want:
            lh = rng(url, off, off + 29)
            lnl, lel = struct.unpack_from('<HH', lh, 26)
            data = rng(url, off + 30 + lnl + lel, off + 30 + lnl + lel + csz - 1)
            data = zlib.decompress(data, -15) if meth == 8 else data
            if len(data) != usz:
                sys.exit('حجم غير متوقع لـ %s' % want)
            open(out, 'wb').write(data)
            print('%s: %d بايت' % (want, len(data)))
            return
        p += 46 + nl + el + cl
    sys.exit('لم يوجد %s في الأرشيف' % want)


if __name__ == '__main__':
    main()
