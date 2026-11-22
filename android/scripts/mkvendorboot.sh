#!/bin/sh
# بناء vendor_boot.img مع bootconfig الخاص بمكدّس الرسوميات (ANGLE + SwiftShader + minigbm + ranchu)
# الاستخدام: mkvendorboot.sh <مجلد-gki-فيه-ko> [مفاتيح bootconfig إضافية]
T=$(cd "$(dirname "$0")/../../tools" && pwd)
set -e; cd "${1:-.}"; shift 2>/dev/null || true
# FSTAB_FILE: ملف fstab بديل (مثل صورة Android TV متعددة الأقسام)
if [ -n "$FSTAB_FILE" ]; then FS="--fstab-file $FSTAB_FILE"; else FS=""; fi
python3 $T/mkvendorramdisk.py vr.gz --hardware ${HW:-gtv} ${FS:---fstab "/dev/block/vda /system ext4 ro wait,first_stage_mount"} $(for m in system_heap virtio_mmio virtio_dma_buf virtio-gpu virtio_blk failover net_failover virtio_net virtio_input virtio_snd; do echo --module ko/$m.ko; done) >/dev/null
B=""
for kv in hardware=${HW:-gtv} boot_devices=a000000.virtio_mmio selinux=permissive console=ttyAMA0 \
  hardware.egl=angle hardware.vulkan=pastel hardware.gralloc=minigbm hardware.hwcomposer=ranchu \
  hardware.hwcomposer.mode=client lcd_density=240 opengles.version=196609 cpuvulkan.version=4202496 \
  vendor.apex.com.android.hardware.graphics.composer=com.android.hardware.graphics.composer.ranchu hardware.hwcomposer.display_finder_mode=drm hardware.hwcomposer.display_framebuffer_format=rgba "$@"; do
  B="$B --bootconfig androidboot.$kv"; done
python3 $T/mkbootimg.py --vendor-ramdisk vr.gz --vendor-cmdline "console=ttyAMA0 earlycon=pl011,0x09000000 printk.devkmsg=on" $B --vendor-out vb.img
