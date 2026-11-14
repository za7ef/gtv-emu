#!/usr/bin/env python3
"""استخراج ملف واحد من أرشيف zip بعيد عبر طلبات HTTP Range، دون تنزيل الأرشيف كاملًا.

مفيد لصور بناء أندرويد على ci.android.com، إذ يتجاوز حجم الأرشيف غيغابايتًا بينما
لا يتجاوز init_boot.img بضعة ميغابايتات.
الاستخدام: fetchzipmember.py URL اسم_الملف_داخل_الأرشيف ملف_الإخراج
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tfzip import RemoteZip  # noqa: E402


def main():
    url, want, out = sys.argv[1:4]
    tmp = tempfile.mkdtemp(prefix='fzm-')
    try:
        z = RemoteZip(url, tmp)
        if want not in z.ents:
            sys.exit('لم يوجد %s في الأرشيف' % want)
        shutil.move(z.fetch(want), out)
        print('%s: %d بايت' % (want, os.path.getsize(out)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
