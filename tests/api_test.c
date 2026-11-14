/* اختبار الواجهة البرمجية المضمِّنة (gtv.h) كما يستخدمها تطبيق iOS:
 * خطافات الطرفية والصوت والإطار، وأحداث الريموت واللمس من خيط آخر.
 * الاستخدام: api_test Image initrd.gz */
#include "../core/gtv.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char out[1 << 20]; static size_t outn;
static char in[4096]; static size_t inh, int_;
static size_t audio_frames; static uint32_t audio_rate; static int frames_ready;

static void con_out(void *ctx, const char *s, size_t n) {
    (void)ctx;
    pthread_mutex_lock(&mu);
    if (outn + n < sizeof out - 1) { memcpy(out + outn, s, n); outn += n; out[outn] = 0; }
    pthread_mutex_unlock(&mu);
    fwrite(s, 1, n, stdout);
}
static size_t con_in(void *ctx, char *buf, size_t max) {
    (void)ctx; size_t k = 0;
    pthread_mutex_lock(&mu);
    while (int_ < inh && k < max) buf[k++] = in[int_++];
    pthread_mutex_unlock(&mu);
    return k;
}
static int con_ready(void *ctx) { (void)ctx; return int_ < inh; }
static void aud(void *ctx, const int16_t *p, size_t frames, uint32_t rate, int ch) { (void)ctx; (void)p; (void)ch; audio_frames += frames; audio_rate = rate; }
static void frame(void *ctx) { (void)ctx; frames_ready++; }

static void type(const char *s) { pthread_mutex_lock(&mu); size_t n = strlen(s); memcpy(in + inh, s, n); inh += n; pthread_mutex_unlock(&mu); }
static int seen(const char *s) { pthread_mutex_lock(&mu); int r = strstr(out, s) != NULL; pthread_mutex_unlock(&mu); return r; }
static int wait_for(const char *s, int secs) { for (int i = 0; i < secs * 10; i++) { if (seen(s)) return 1; usleep(100000); } return 0; }

static GtvMachine *m;
static void *runner(void *a) { (void)a; gtv_run(m, 0); return NULL; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "api_test Image initrd.gz\n"); return 2; }
    static GtvHost h = { NULL, con_out, con_in, con_ready, aud, frame };
    gtv_set_host(&h);
    GtvConfig c; memset(&c, 0, sizeof c);
    c.kernel = argv[1]; c.initrd = argv[2]; c.ram_mb = 512; c.ncpu = 2; c.use_jit = 1;
    char err[256];
    m = gtv_create(&c, err, sizeof err);
    if (!m) { fprintf(stderr, "gtv_create: %s\n", err); return 1; }
    pthread_t t; pthread_create(&t, NULL, runner, NULL);
    int fail = 0;
#define CHECK(cond, name) do { if (cond) printf("\nنجح: %s\n", name); else { printf("\nفشل: %s\n", name); fail = 1; } } while (0)
    CHECK(wait_for("gtv-emu: shell ready", 600) && wait_for("# ", 60), "الطرفية عبر خطاف console_out");
    type("echo API-$((6*7))\r");
    CHECK(wait_for("API-42", 60), "إدخال الطرفية عبر console_in");
    type("K=$(grep -l keyboard /sys/class/input/event*/device/name | cut -d/ -f5); T=$(grep -l touchscreen /sys/class/input/event*/device/name | cut -d/ -f5); "
         "cat /dev/input/$K > /tmp/k.bin & cat /dev/input/$T > /tmp/t.bin & sleep 2; echo READERS-$((1+1))-UP\r");
    wait_for("READERS-2-UP", 120);
    gtv_post_key(172, 1); gtv_post_key(172, 0);
    gtv_post_touch(640, 360, 0); gtv_post_touch(700, 360, 1); gtv_post_touch(700, 360, 2);
    sleep(2);
    type("sleep 2; kill %1 %2; od -An -tu2 -w24 /tmp/k.bin /tmp/t.bin | awk '$9>0{print \"EV\",$9,$10,$11}' | sort -u | tr '\\n' ' '; echo EV-$((1+1))-DONE\r");
    CHECK(wait_for("EV-2-DONE", 120) && seen("EV 1 172 1"), "زر الريموت عبر gtv_post_key");
    CHECK(seen("EV 3 53 640") && seen("EV 3 53 700") && seen("EV 1 330 1"), "اللمس عبر gtv_post_touch");
    type("/usr/bin/aplay -D hw:0,0 /tone.wav && echo APLAY-$((1+1))-DONE\r");
    CHECK(wait_for("APLAY-2-DONE", 300) && audio_frames >= 24000, "الصوت عبر خطاف audio_out");
    printf("[api] عينات الصوت: %zu بمعدل %u\n", audio_frames, audio_rate);
    uint32_t w = 0, hh = 0;
    uint64_t gen = gtv_copy_frame(NULL, 0, &w, &hh);
    void *buf = malloc((size_t)w * hh * 4 + 4);
    gen = gtv_copy_frame(buf, (size_t)w * hh * 4, &w, &hh);
    size_t lit = 0; for (size_t i = 0; i < (size_t)w * hh; i++) if (((uint32_t *)buf)[i] & 0xFFFFFF) lit++;
    printf("[api] إطار: %ux%u، الجيل %llu، بكسلات مضيئة %zu، إشعارات %d\n", w, hh, (unsigned long long)gen, lit, frames_ready);
    CHECK(gen > 0 && w == 1280 && hh == 720 && lit > 0 && frames_ready > 0, "نسخ الإطار عبر gtv_copy_frame");
    printf("[api] JIT: %s (%s)\n", gtv_jit_active(m) ? "نعم" : "لا", gtv_jit_status());
    type("poweroff -f\r");
    pthread_join(t, NULL);
    gtv_destroy(m);
    printf("[api] انتهى بعد %llu تعليمة\n", (unsigned long long)gtv_icount(m));
    return fail;
}
