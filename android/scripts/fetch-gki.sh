#!/bin/sh
# تنزيل صور الإقلاع اللازمة: نواة GKI الرسمية 6.1 وinit_boot من بناء Cuttlefish ووحدات virtio
# الاستخدام: fetch-gki.sh <مجلد-gki>
set -e
D=${1:-gki}; T=$(cd "$(dirname "$0")/../../tools" && pwd)
mkdir -p "$D/ko"; cd "$D"
[ -f boot-6.1-gz.img ] || python3 $T/fetchzipmember.py https://dl.google.com/android/gki/gki-certified-boot-android14-6.1-2026-09_r8-gz.zip boot-6.1-gz.img boot-6.1-gz.img
[ -f init_boot.img ] || python3 $T/fetchzipmember.py "$(curl -sL -o /dev/null -w '%{url_effective}' https://ci.android.com/builds/submitted/16644040/aosp_cf_arm64_only_phone-userdebug/latest/raw/aosp_cf_arm64_only_phone-img-16644040.zip)" init_boot.img init_boot.img
for m in system_heap virtio_mmio virtio_dma_buf virtio-gpu virtio_blk failover net_failover virtio_net virtio_input virtio_snd; do
  [ -f ko/$m.ko ] || curl -sfL -o ko/$m.ko https://ci.android.com/builds/submitted/16875195/kernel_virt_aarch64/latest/raw/$m.ko
done
ls -la . ko
