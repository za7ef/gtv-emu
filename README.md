# مكوّنات الرسوميات من AOSP Cuttlefish

ملفات مأخوذة دون تعديل من target_files لبناء AOSP الرسمي `aosp_cf_arm64_only_phone-userdebug` (البناء 16644040) على ci.android.com، وهي مرخّصة بترخيص Apache 2.0 (مشروع Android المفتوح المصدر).

تُضاف إلى قسم vendor في صورة Android TV أثناء خطوة التجهيز على الجهاز (انظر `android/tv/prep/prep.sh` في الفرع الرئيسي)، ويُبنى منها `tv-prep-initrd.gz` بالأداة `tools/mktvprep.py`.

- `com.google.cf.vulkan.apex`: ‏SwiftShader Vulkan
- `com.google.cf.gralloc.apex` و`mapper.minigbm.so` و`libminigbm_gralloc.so` و`mapper.minigbm.xml`: ‏minigbm gralloc
- `com.android.hardware.graphics.composer.ranchu.apex`: موزّع ranchu
- `libc++.so` و`libdmabufheap.so`: مكتبات يعتمد عليها ما سبق
