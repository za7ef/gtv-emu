#!/usr/bin/env python3
"""وصول عشوائي إلى ملفات أرشيف zip بعيد (مثل target_files لبناء أندرويد) عبر HTTP Range.

يُقرأ الفهرس المركزي مرة واحدة ويُحفظ، ثم يُنزَّل كل ملف مطلوب وحده ويُخزَّن مفكوك الضغط.
"""
import json
import os
import struct
import subprocess
import threading
import urllib.error
import urllib.request
import zlib
from concurrent.futures import ThreadPoolExecutor


_LOCK = threading.Lock()


def _rng(url, a, b):
    req = urllib.request.Request(url, headers={'Range': 'bytes=%d-%d' % (a, b)})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                return r.read()
        except Exception:
            if attempt == 3:
                raise


class RemoteZip:
    def __init__(self, url, cache):
        self.cache = cache
        os.makedirs(cache, exist_ok=True)
        idx = os.path.join(cache, '.index.json')
        if os.path.exists(idx):
            d = json.load(open(idx))
        else:
            d = self._index(url)
            json.dump(d, open(idx, 'w'))
        self.src = url if '://' in url else d.get('src', d['url'])
        self.url = d['url']
        if self.src != d.get('src'):
            d['src'] = self.src
            json.dump(d, open(idx, 'w'))
        self._idx = idx
        self.ents = {e[0]: e for e in d['ents']}

    @staticmethod
    def _resolve(url):
        if 'ci.android.com' in url:   # يعيد الموقع توجيهًا إلى رابط تخزين موقّع تنتهي صلاحيته
            url = subprocess.check_output(['curl', '-s', '-m', '60', '-o', '/dev/null', '-w', '%{redirect_url}', url]).decode()
        return url

    def rng(self, a, b):
        """قراءة مدى من الأرشيف، مع تجديد الرابط الموقّع إن انتهت صلاحيته."""
        try:
            return _rng(self.url, a, b)
        except urllib.error.HTTPError as e:
            if e.code not in (400, 401, 403):
                raise
            with _LOCK:
                self.url = self._resolve(self.src)
                d = json.load(open(self._idx))
                d['url'] = self.url
                json.dump(d, open(self._idx, 'w'))
            return _rng(self.url, a, b)

    @classmethod
    def _index(cls, url):
        src = url
        url = cls._resolve(url)
        with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=60) as r:
            size = int(r.headers['Content-Length'])
        tail = _rng(url, max(0, size - 70000), size - 1)
        i = tail.rfind(b'PK\x05\x06')
        n, cdsz, cdoff = struct.unpack_from('<HII', tail, i + 10)
        j = tail.rfind(b'PK\x06\x06')
        if j >= 0:
            n, cdsz, cdoff = struct.unpack_from('<QQQ', tail, j + 32)
        cd = _rng(url, cdoff, cdoff + cdsz - 1)
        p, out = 0, []
        while p < len(cd) and cd[p:p + 4] == b'PK\x01\x02':
            meth, = struct.unpack_from('<H', cd, p + 10)
            csz, usz = struct.unpack_from('<II', cd, p + 20)
            nl, el, cl = struct.unpack_from('<HHH', cd, p + 28)
            ext, off = struct.unpack_from('<II', cd, p + 38)
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
            out.append([name, meth, csz, usz, off, ext >> 16])
            p += 46 + nl + el + cl
        return {'url': url, 'src': src, 'ents': out}

    def is_symlink(self, name):
        e = self.ents.get(name)
        return e is not None and (e[5] & 0o170000) == 0o120000

    def path(self, name):
        return os.path.join(self.cache, name)

    def fetch(self, name):
        dst = self.path(name)
        if os.path.exists(dst):
            return dst
        meth, csz, usz, off = self.ents[name][1:5]
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if usz == 0:
            open(dst, 'wb').close()
            return dst
        lh = self.rng(off, off + 29)
        nl, el = struct.unpack_from('<HH', lh, 26)
        start = off + 30 + nl + el
        tmp = dst + '.part'
        with open(tmp, 'wb') as f:
            d = zlib.decompressobj(-15) if meth == 8 else None
            pos, step = 0, 8 << 20
            while pos < csz:
                chunk = self.rng(start + pos, start + min(csz, pos + step) - 1)
                pos += len(chunk)
                f.write(d.decompress(chunk) if d else chunk)
            if d:
                f.write(d.flush())
        if os.path.getsize(tmp) != usz:
            os.unlink(tmp)
            raise IOError('حجم غير متوقع: ' + name)
        os.rename(tmp, dst)
        return dst

    def fetch_many(self, names, jobs=8):
        names = [n for n in names if not os.path.exists(self.path(n))]
        with ThreadPoolExecutor(jobs) as ex:
            list(ex.map(self.fetch, names))
