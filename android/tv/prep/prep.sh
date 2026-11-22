#!/bin/sh
# تجهيز صورة Android TV داخل آلة Alpine صغيرة على الجهاز نفسه.
# القرص /dev/vda أنشأه التطبيق من ملف Google (انظر core/atvprep.c)، وهذا السكربت يعدّل أقسامه:
#   1) vendor: توسيع نظام الملفات وفك الكتل المشتركة، ثم إضافة مكدّس الرسوميات من AOSP Cuttlefish
#      (SwiftShader Vulkan وminigbm وموزّع ranchu بوضع DRM) وتعديل fstab وinit.ranchu.rc
#   2) system: سياسة SELinux متساهلة لنطاقات النظام (تعديل في المكان بنفس الطول)
# يطبع GTVPREP-OK عند النجاح أو GTVPREP-FAIL مع السبب.
P=/prep
W=/tmp/w
mkdir -p $W
fail() { echo "GTVPREP-FAIL: $*"; sync; exit 1; }
step() { echo "GTVPREP-STEP: $*"; }

part() {
  for d in /sys/class/block/vda*; do
    grep -q "^PARTNAME=$1\$" "$d/uevent" 2>/dev/null && { echo "/dev/$(basename "$d")"; return 0; }
  done
  return 1
}
[ -b /dev/vda ] || fail "no disk"
SYS=$(part system) || fail "system partition missing"
VEN=$(part vendor) || fail "vendor partition missing"
for d in $SYS $VEN; do [ -b $d ] || mdev -s; done
[ -b $VEN ] || fail "no device node for $VEN"

ctx() { printf '%s\0' "u:object_r:$1:s0" > $W/ctx.$1; echo $W/ctx.$1; }

# ===== 1) vendor =====
step "vendor: resize"
resize2fs -f $VEN >/dev/null 2>&1 || fail "resize2fs vendor"
step "vendor: unshare blocks"
e2fsck -fy -E unshare_blocks $VEN >/dev/null 2>&1; rc=$?
[ $rc -le 1 ] || fail "e2fsck vendor rc=$rc"

dbg() { debugfs -w -R "$1" $VEN >/dev/null 2>&1; }
get() { debugfs -R "dump $1 $2" $VEN >/dev/null 2>&1 && [ -s "$2" ] || fail "cannot read $1"; }

step "vendor: config files"
get /etc/fstab.ranchu $W/fstab.ranchu
awk '
  /^\/dev\/block\/vdc[ \t]/ { print "/dev/block/by-name/userdata /data ext4 noatime,nosuid,nodev wait,check,formattable,latemount"; next }
  /[ \t]\/metadata[ \t]/ { print "/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait,formattable"; next }
  /logical/ || /zram/ { if ($0 !~ /^#/) { print "#" $0; next } }
  { print }' $W/fstab.ranchu > $W/fstab.new
get /etc/init/hw/init.ranchu.rc $W/init.rc
awk '
  /setprop vendor.qemu.dev.bootcomplete 0/ && !done {
    print "    setprop ro.vendor.hwcomposer.display_finder_mode ${ro.boot.hardware.hwcomposer.display_finder_mode}"
    print "    setprop ro.vendor.hwcomposer.mode ${ro.boot.hardware.hwcomposer.mode}"
    print "    setprop ro.vendor.hwcomposer.display_framebuffer_format ${ro.boot.hardware.hwcomposer.display_framebuffer_format}"
    print "    setprop ro.sf.lcd_density ${ro.boot.lcd_density}"
    done = 1 }
  { print }' $W/init.rc > $W/init.new
grep -q display_finder_mode $W/init.new || fail "init.ranchu.rc patch"
get /etc/selinux/vendor_sepolicy.cil $W/vendor.cil
cat >> $W/vendor.cil <<'EOF'
(allow ueventd tmpfs (chr_file (relabelfrom setattr getattr)))
(allow ueventd tmpfs (blk_file (relabelfrom setattr getattr)))
(allow ueventd dev_type (chr_file (relabelto setattr getattr)))
(allow ueventd dev_type (blk_file (relabelto setattr getattr)))
(allow domain tmpfs (chr_file (getattr open read write ioctl)))
EOF
# بصمة مختلفة تُجبر init على إعادة ترجمة السياسة بدل استخدام النسخة المترجمة مسبقًا
echo 0000000000000000000000000000000000000000000000000000000000000000 > $W/sha

CF=$(ctx vendor_configs_file); AP=$(ctx vendor_apex_file); SP=$(ctx same_process_hal_file)
put() { # put <src> <dst> <ctxfile>
  dbg "rm $2"
  dbg "write $1 $2"
  dbg "ea_set -f $3 $2 security.selinux"
  dbg "sif $2 mode 0100644"
  dbg "sif $2 uid 0"
  dbg "sif $2 gid 0"
  debugfs -R "stat $2" $VEN 2>/dev/null | grep -q "Size: $(wc -c < $1)" || fail "write $2"
}
put $W/fstab.new /etc/fstab.ranchu $CF
put $W/init.new /etc/init/hw/init.ranchu.rc $CF
put $W/vendor.cil /etc/selinux/vendor_sepolicy.cil $CF
put $W/sha /etc/selinux/precompiled_sepolicy.plat_sepolicy_and_mapping.sha256 $CF

step "vendor: graphics stack"
# خدمات الرسوميات الأصلية تعتمد أنبوب goldfish غير الموجود؛ يحلّ محلها minigbm وranchu بوضع DRM
for f in /etc/init/android.hardware.graphics.allocator-service.ranchu.rc /etc/init/hwc3.rc \
         /etc/vintf/manifest/android.hardware.graphics.gralloc.ranchu.xml /etc/vintf/manifest/hwc3.xml; do
  dbg "rm $f"
done
for a in com.google.cf.vulkan.apex com.google.cf.gralloc.apex com.android.hardware.graphics.composer.ranchu.apex; do
  put $P/cf/$a /apex/$a $AP
done
put $P/cf/mapper.minigbm.so /lib64/hw/mapper.minigbm.so $SP
put $P/cf/libminigbm_gralloc.so /lib64/libminigbm_gralloc.so $SP
put $P/cf/libc++.so /lib64/libc++.so $SP
put $P/cf/libdmabufheap.so /lib64/libdmabufheap.so $SP
put $P/cf/mapper.minigbm.xml /etc/vintf/manifest/mapper.minigbm.xml $CF
e2fsck -fy $VEN >/dev/null 2>&1; rc=$?
[ $rc -le 1 ] || fail "vendor check rc=$rc"

# ===== 2) system: SELinux =====
step "system: selinux"
F=/etc/selinux/plat_sepolicy.cil
debugfs -R "dump $F $W/plat.cil" $SYS >/dev/null 2>&1 || fail "read plat_sepolicy.cil"
if grep -q "^(typepermissive aconfigd)" $W/plat.cil; then
  echo "already patched"
else
  awk -F'\t' 'NR==FNR { p[$1] = $2; has[$1] = 1; next }
    (FNR in has) { t = p[FNR]; if (length(t) > length($0)) exit 3; while (length(t) < length($0)) t = t " "; print t; next }
    { print }' $P/plat-patch.tsv $W/plat.cil > $W/plat.new || fail "plat patch"
  [ "$(wc -c < $W/plat.new)" = "$(wc -c < $W/plat.cil)" ] || fail "plat size mismatch"
  BL=$(debugfs -R "blocks $F" $SYS 2>/dev/null)
  [ -n "$BL" ] || fail "plat blocks"
  i=0
  for b in $BL; do
    dd if=$W/plat.new of=$SYS bs=4096 skip=$i seek=$b count=1 conv=notrunc 2>/dev/null || fail "plat write"
    i=$((i + 1))
  done
  sync; echo 3 > /proc/sys/vm/drop_caches
  debugfs -R "dump $F $W/plat.chk" $SYS >/dev/null 2>&1
  cmp -s $W/plat.chk $W/plat.new || fail "plat verify"
fi
sync
echo "GTVPREP-OK"
