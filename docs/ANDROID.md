# مسار إقلاع أندرويد

## ما يعمل الآن

- **قراءة صور أندرويد مباشرة:** الخيار `--bootimg boot.img` يقرأ الإصدارات 0 إلى 4 من صيغة boot.img، والخيار `--vendor-boot vendor_boot.img` يقرأ الإصدارين 3 و4.
- **فك ضغط النواة:** يدعم المحاكي نوى `Image.gz` بفك ضغط gzip مدمج دون مكتبات خارجية (ويصلح أيضًا للخيار `-k`). صيغة LZ4 غير مدعومة بعد.
- **الأقراص الأولية:** يُدمج القرص الأولي للمورّد أولًا ثم القرص العام، كما يفعل محمّل الإقلاع في الأجهزة الحقيقية.
- **bootconfig:** تُلحق خصائص `vendor_boot` وما يُمرَّر بالخيار `--androidboot مفتاح=قيمة` بنهاية القرص الأولي بالتنسيق الذي تتوقعه النواة (البيانات، ثم الحجم، ثم المجموع، ثم الوسم `#BOOTCONFIG`)، ويُضاف `bootconfig` إلى سطر الأوامر.
- **سطر الأوامر:** سطر `vendor_boot` ثم سطر `boot.img` ثم ما يضيفه المستخدم بالخيار `-a`.
- **الأداة `tools/mkbootimg.py`:** تنشئ صورتي boot.img وvendor_boot.img بالإصدار الرابع لأغراض الاختبار.

## الإثبات

يُنزّل اختبار الإقلاع صورة boot.img الرسمية الموقّعة من Google لنواة GKI (‏android14-6.1، ‏`gki-certified-boot-android14-6.1-2026-09_r8-gz`)، ويبني vendor_boot.img تحمل صدفة busybox، ثم يُقلعهما المحاكي بمعالجين. يتحقق الاختبار من:

- إقلاع النواة `6.1.176-android14-11` حتى الصدفة.
- رسالة النواة `Load bootconfig` وظهور `androidboot.hardware`، و`androidboot.serialno` في `/proc/bootconfig`.
- تشغيل المعالجين معًا.

كما يظهر نظام ملفات binder في `/proc/filesystems`، وهو شرط أساسي لأندرويد.

## وحدات virtio لنواة GKI

نواة GKI لا تتضمن تعريفات virtio، لذلك تُنزَّل الوحدات من البناء نفسه للنواة (رقم البناء `16875195`، الهدف `kernel_virt_aarch64` في فرع `aosp_kernel-common-android14-6.1-2026-09`) حتى تطابق رموزها النواة الموقّعة. الوحدات المستخدمة بترتيب التحميل:

`virtio_mmio`، `virtio_dma_buf`، `virtio-gpu`، `virtio_blk`، `failover`، `net_failover`، `virtio_net`، `virtio_input`، `virtio_snd`.

يتحقق الاختبار فوق نواة GKI من: القراءة من قرص virtio، والحصول على عنوان DHCP عبر NAT، وبطاقة الصوت وعقدة ALSA، وجهازي الإدخال، وبطاقة DRM. ولأن أندرويد لا يستخدم fbdev، أضفنا الأداة `tools/drmtest.c`: عميل DRM/KMS صغير يكتشف الموصل والنمط، وينشئ مخزنًا، ويرسم أشرطة ألوان، ثم يعرضها بـ`SETCRTC`، وهو المسار نفسه الذي تسلكه طبقة hwcomposer. يحتفظ المحاكي بآخر إطار معروض بعد أن يطفئ الضيف الشاشة، لتعمل لقطة الشاشة حتى بعد خروج البرنامج.

![نمط اختبار معروض عبر DRM/KMS فوق نواة GKI](screenshots/m5-gki-drm.png)

## init أندرويد الحقيقي (المرحلة الأولى)

- **الخيار `--init-boot init_boot.img`:** يقرأ القرص الأولي العام (أندرويد 13 وما بعده) ويدمجه بعد قرص المورّد. يُنزّل الاختبار `init_boot.img` من بناء أندرويد 16 الرسمي لـCuttlefish على arm64 (البناء `16644040`، الهدف `aosp_cf_arm64_only_phone-userdebug`) باستخراج هذا الملف وحده (2.7 ميغابايت) من أرشيف حجمه 1.1 غيغابايت عبر طلبات HTTP Range بالأداة `tools/fetchzipmember.py`.
- **القرص الأولي للمورّد:** تبنيه الأداة `tools/mkvendorramdisk.py`، ويحمل `fstab.gtv` (في الجذر وفي `/first_stage_ramdisk`) والوحدات مع `modules.load` و`modules.dep` المستخرج من معلومات الوحدات.
- **صورة النظام:** تنشئ الأداة `tools/mkext2.py` صورة ext2 صغيرة دون e2fsprogs، فيها busybox وسكربت في `/system/bin/init`.
- **التسلسل المُثبت:** يبدأ init أندرويد المرحلة الأولى، ويحمّل الوحدات التسع، ويقرأ `fstab.gtv`، وينتظر `/dev/block/vda`، ويركّبه على `/system` بنظام ext4، وينقل الجذر إليه، ثم ينفّذ `/system/bin/init selinux_setup`. هنا يستلم سكربتنا التنفيذ بصفته العملية رقم 1 ويفتح صدفة.
- **تعليمات أضيفت:** شغّل init أندرويد تعليمات CRC32 وCRC32C، فأضيفت إلى المفسّر. كما تُطبع الآن أول تعليمات غير معرّفة في وضع المستخدم للمساعدة على التشخيص.

لم يُشغَّل بعد init الحقيقي في المرحلة الثانية (يحتاج SELinux وصورة system كاملة من AOSP).

## ما يلزم لإقلاع Android TV كاملًا (لم يُنفَّذ بعد)

1. ~~وحدات النواة الافتراضية~~ (تمت).
2. **صور النظام:** ‏`super.img` أو صور `system` و`vendor` منفصلة (‏ext4 أو EROFS) على قرص virtio. مسار fstab والمرحلة الأولى يعمل الآن؛ المتبقي صور AOSP حقيقية مع SELinux والمرحلة الثانية من init.
3. **الرسوميات:** يحتاج SurfaceFlinger إلى مكتبات gralloc وhwcomposer تعمل فوق virtio-gpu (مثل drm_hwcomposer وminigbm)، ومع غياب التسريع الثلاثي الأبعاد يلزم مسار برمجي مثل SwiftShader، وهو بطيء على مفسّر.
4. **الذاكرة والأداء:** يحتاج Android TV إلى 2 غيغابايت تقريبًا على الأقل، وإلى ترجمة فورية أسرع بكثير مما هي عليه الآن.
5. **مصدر الصور:** أقرب طريق هو بناء هدف أجهزة افتراضية من AOSP لمعمارية arm64 بنسخة TV، مع ملاءمة شجرة الأجهزة والأجهزة الافتراضية. لم نتحقق بعد من الهدف الأنسب.

## ملاحظة أداء

كان بطء نواة GKI على المترجم الفوري سببه تفريغ ذاكرة الترجمة كاملة كلما عدّلت النواة شيفرتها (‏static keys وalternatives). أصبح الإبطال الآن على مستوى الصفحة (التفاصيل في [JIT.md](JIT.md)). تحت qemu ولأول 200 مليون تعليمة: من 26.7 ثانية و165 تفريغة كاملة إلى 13.1 ثانية مع تفريغة واحدة و296 إبطال صفحة. أرقام qemu مشوّهة ولا تمثل جهازًا حقيقيًا.

## المرحلة السادسة: المرحلة الثانية من init ثم SurfaceFlinger

### ما يعمل (مُثبت بسجل الإقلاع في [boot-log-m6.txt](boot-log-m6.txt))
- إقلاع نواة GKI الرسمية 6.1 مع صورة نظام مصغّرة (نحو 1233 ملفًا، 188 ميغابايت) مأخوذة من ملف target_files لبناء Cuttlefish رقم 16644040 (أندرويد 17)، وتُجلب الملفات المطلوبة فقط عبر طلبات النطاق دون تنزيل الأرشيف كاملًا.
- المرحلة الأولى لـ init، ثم تحميل سياسة SELinux (وضع متساهل)، ثم «init second stage started!».
- apexd-bootstrap وapexd يفعّلان حزم APEX الخاصة بالنظام والمورّد، ويعمل linkerconfig.
- اختبار BoringSSL الذاتي (boringssl_self_test64) يمرّ بنجاح بعد إصلاحات المعالج.
- logd وservicemanager وtombstoned والصدفة على المنفذ التسلسلي (getprop وps وlogcat).
- خدمة التخصيص minigbm (allocator) تعمل.
- SurfaceFlinger يبدأ ويهيّئ RenderEngine بالكامل: ‏ANGLE فوق Vulkan عبر SwiftShader (مع مترجم LLVM 16 داخل الضيف)، ‏OpenGL ES 3.1.
- crash_dump64 يكتب تقارير الأعطال كاملة مع تتبّع المكدّس، وهذا يسهّل تشخيص المراحل القادمة.

### إصلاحات المعالج في هذه المرحلة
- **REV32**: كانت تُنفَّذ كأنها REV64، فيفشل اختبار SHA-256 في BoringSSL ويتوقف init بإشارة SIGABRT.
- **امتدادات التشفير**: ‏AESE/AESD/AESMC/AESIMC وSHA1 وSHA256 وPMULL ذات 64 بت، مع إعلانها في ID_AA64ISAR0. تحقّقنا منها بمقارنة تفاضلية مع qemu.
- **مجموعة «ثلاثة مختلفة»**: ‏UADDW/SADDW/USUBW/SSUBW وSQDMULL/SQDMLAL/SQDMLSL (مع علم FPSR.QC)، وتصحيح التقريب في RADDHN/RSUBHN. غياب UADDW كان يسبب SIGILL في الخدمات.

### الإطار الأول (تحديث المرحلة السادسة)
أصبح أندرويد يعرض أول إطار حقيقي: رسم الإقلاع (bootanimation) الرسمي يظهر على شاشة virtio-gpu بدقة 1280×720، بعد أن يمر عبر SurfaceFlinger وRenderEngine ‏(ANGLE فوق SwiftShader) ثم المؤلِّف ranchu ثم KMS. اللقطة في [screenshots/m6-bootanimation.png](screenshots/m6-bootanimation.png) والسجل في [boot-log-m6b.txt](boot-log-m6b.txt).

ما لزم لذلك:
- **المؤلِّف**: خدمة APEX الخاصة به تعمل في مساحة أسماء معزولة لا تصل إلى mapper.minigbm.so في /vendor/lib64/hw، فتتوقف برسالة «gralloc-mapper is missing». الحل في init.gtv.rc: إيقاف خدمة APEX، ونسخ الملف التنفيذي عند الإقلاع إلى /data/local/tests/vendor (يعامله linkerconfig كقسم vendor)، وتشغيله كخدمة gtv.hwc3 مع LD_LIBRARY_PATH يشير إلى مكتبات الحزمة ثم /vendor/lib64/hw.
- **كومة DMA-BUF**: إضافة الوحدة system_heap.ko إلى قرص المورّد، إذ يحتاجها minigbm لتخصيص المخازن.
- **تعليمات معالج جديدة**: ‏FRINTA/FRINTX/FRINTI المتجهية، وFRECPE/FRSQRTE المتجهية (بقيمة دقيقة بدل التقدير)، وSHLL/SHLL2، وFMLA/FMLS/FMUL/FMULX القياسية بالعنصر. كان غيابها يُسقط SurfaceFlinger وbootanimation بإشارة SIGILL.
- **ملفات إضافية**: ‏/system/bin/bootanimation وframework-res.apk (لصور الشعار الافتراضي).

### ما لم يكتمل بعد
- **Zygote والواجهة (launcher)**: لم تُجرَّب. تحتاج حزمة ART وملفات framework (مئات الميغابايتات) وsystem_server، وذاكرة ضيف لا تقل عمليًا عن 2 غيغابايت. على هذا المضيف لا تتوفر إلا نحو 1.5 غيغابايت ذاكرة متاحة و1.4 غيغابايت قرص، والمفسّر يعمل بنحو 64 مليون تعليمة في الثانية (الوصول إلى رسم الإقلاع يستغرق نحو 12 دقيقة)، فالمرحلة التالية تحتاج المترجم الفوري (JIT) على جهاز ARM64 حقيقي أو مضيفًا أكبر.
- تقديرات FRECPE/FRSQRTE ليست مطابقة بتًا ببت للعتاد، وتعليمات FRINTX/FRINTI تفترض وضع التقريب الافتراضي.

### إعادة الإنتاج
- `android/scripts/build-system.sh <مجلد>`: يبني sys.img من قائمة `android/files.txt` مع ملفات rc في `android/rc/`.
- `android/scripts/mkvendorboot.sh <مجلد-gki>`: يبني vendor_boot.img مع إعدادات bootconfig الخاصة بالرسوميات.
- `android/scripts/run.sh <عدد-التعليمات> [إدخال]`: يشغّل المحاكي بذاكرة 768 ميغابايت ومعالجين.
