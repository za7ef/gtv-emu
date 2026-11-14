#!/bin/sh
# بناء صورة النظام المصغّرة (ext2 بجذر واحد) من ملف target_files لبناء Cuttlefish
# الاستخدام: build-system.sh <مجلد-العمل>
set -e
W=${1:-.}; shift 2>/dev/null || true; G=$(cd "$(dirname "$0")/../.." && pwd); R=$G/android/rc
cd "$W"
python3 $G/tools/mkandroidsys.py \
  --url https://ci.android.com/builds/submitted/16644040/aosp_cf_arm64_only_phone-userdebug/latest/raw/aosp_cf_arm64_only_phone-target_files-16644040.zip \
  --cache tf --out sys.img --list $G/android/files.txt --dir vendor/apex --dir vendor/odm/apex \
  --overlay $R/init.gtv.rc:vendor/etc/init/hw/init.gtv.rc:644 \
  --overlay $R/init.boringssl.zygote64.rc:system/etc/init/hw/init.boringssl.zygote64.rc:644 "$@"
# توفير المساحة: حذف الملفات المخزّنة مؤقتًا (يبقى الفهرس وMETA)
rm -rf tf/SYSTEM tf/VENDOR tf/PRODUCT tf/SYSTEM_EXT tf/ODM
