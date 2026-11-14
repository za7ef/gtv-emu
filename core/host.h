/* gtv-emu — خطافات المضيف.
 * يستخدمها المحاكي للتواصل مع واجهة مضمِّنة (تطبيق iOS مثلًا) بدل الطرفية والملفات.
 * عندما يكون gtv_host فارغًا (NULL) يعمل السلوك الافتراضي لواجهة سطر الأوامر. */
#ifndef GTV_HOST_H
#define GTV_HOST_H
#include <stdint.h>
#include <stddef.h>

typedef struct GtvHost {
    void *ctx;
    /* مخرجات المنفذ التسلسلي (الطرفية) */
    void (*console_out)(void *ctx, const char *s, size_t n);
    /* إدخال الطرفية دون انتظار: يعيد عدد البايتات المقروءة */
    size_t (*console_in)(void *ctx, char *buf, size_t max);
    int (*console_in_ready)(void *ctx);
    /* عينات PCM بصيغة S16 متشابكة */
    void (*audio_out)(void *ctx, const int16_t *pcm, size_t frames, uint32_t rate, int channels);
    /* يُستدعى من خيط المحاكي بعد كل RESOURCE_FLUSH للشاشة */
    void (*frame_ready)(void *ctx);
} GtvHost;

extern GtvHost *gtv_host;

/* virtio.c: أحداث إدخال آمنة من أي خيط، تُسلَّم للضيف في الحلقة الرئيسية */
void gtv_post_key(int code, int down);
void gtv_post_touch(int x, int y, int phase); /* 0 بدء، 1 تحريك، 2 رفع */
/* نسخة من الإطار المعروض بصيغة BGRA؛ يعيد رقم الجيل أو 0 إن لم يوجد إطار */
uint64_t gtv_copy_frame(void *dst, size_t cap, uint32_t *w, uint32_t *h);
#endif
