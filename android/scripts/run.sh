#!/bin/sh
# تشغيل أندرويد على المحاكي: run.sh <أقصى-عدد-تعليمات> [نص-الإدخال]
# المتغيرات: EMU (مسار المحاكي) و GKI (مجلد الصور) و SYS (صورة النظام) و TO و MEM (افتراضي 768) و SMP و SHOT
cd "${GKI:-.}"
timeout ${TO:-900} ${EMU:-gtv-emu} --bootimg boot-6.1-gz.img --vendor-boot vb.img --init-boot init_boot.img \
  -d ${SYS:-../sys.img} -m ${MEM:-768} --smp ${SMP:-2} --max-insns $1 ${SHOT:+--screenshot $SHOT} --feed "$2" > a.log 2> ae.log
