/* gtv-emu — الواجهة البرمجية العامة لتضمين المحاكي في تطبيق (iOS أو غيره).
 * تُستخدم من Swift عبر ترويسة الجسر، ومن واجهة سطر الأوامر main.c. */
#ifndef GTV_API_H
#define GTV_API_H
#include <stdint.h>
#include <stddef.h>
#include "host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtvConfig {
    /* مسار لينكس العادي */
    const char *kernel;        /* Image أو Image.gz */
    const char *initrd;
    /* مسار أندرويد */
    const char *bootimg, *vendor_boot, *init_boot;
    const char *androidboot[32]; int nandroidboot;
    const char *disk;          /* صورة قرص virtio (اختيارية) */
    const char *cmdline;       /* وسائط إضافية للنواة؛ NULL للقيمة الافتراضية */
    uint32_t ram_mb;           /* افتراضي 512 */
    int ncpu;                  /* 1..4 */
    int use_jit;               /* 1 لمحاولة المترجم الفوري */
    const char *keys, *audio_wav, *feed;
    int verbose;
} GtvConfig;

typedef struct Machine GtvMachine;

/* أوضاع حجز ذاكرة المترجم الفوري */
enum { GTV_JIT_AUTO = 0, GTV_JIT_TXM = 1, GTV_JIT_LEGACY = 2, GTV_JIT_OFF = 3 };
void gtv_set_jit_mode(int mode);
/* وصف نصي لنتيجة آخر محاولة لتفعيل المترجم الفوري */
const char *gtv_jit_status(void);

void gtv_set_host(GtvHost *host);
GtvMachine *gtv_create(const GtvConfig *cfg, char *err, size_t errlen);
/* يشغّل الآلة حتى الإيقاف أو بلوغ max_insns (0 بلا حد). يُستدعى من خيط مخصص. */
int gtv_run(GtvMachine *m, uint64_t max_insns);
void gtv_stop(GtvMachine *m);
uint64_t gtv_icount(GtvMachine *m);
int gtv_jit_active(GtvMachine *m);
/* يحرر ذاكرة الضيف بعد انتهاء gtv_run */
void gtv_destroy(GtvMachine *m);

#ifdef __cplusplus
}
#endif
#endif
