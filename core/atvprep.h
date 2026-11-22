/* تجهيز صورة Android TV من ملف Google الأصلي على الجهاز (انظر atvprep.c) */
#ifndef GTV_ATVPREP_H
#define GTV_ATVPREP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*gtv_progress_fn)(void *ctx, double fraction, const char *stage);
/* يعيد 0 عند النجاح؛ عند الفشل يكتب السبب في err ويحذف الملفات الناقصة */
int gtv_atv_extract(const char *zip_path, const char *disk_out, const char *initboot_out,
                    gtv_progress_fn progress, void *ctx, char *err, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
