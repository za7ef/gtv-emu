/* gtv-emu — واجهة سطر الأوامر لتشغيل نواة لينكس على المحاكي. */
#include "emu.h"
#include "gtv.h"
#include "atvprep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

static struct termios saved_tio;
static int tio_saved;
static Machine *gm;

static void restore_tty(void) { if (tio_saved) tcsetattr(0, TCSANOW, &saved_tio); }
static void on_sig(int s) { (void)s; if (gm) gtv_stop(gm); }

static void usage(void) {
    fprintf(stderr,
        "الاستخدام: gtv-emu (-k Image[.gz] | --bootimg boot.img [--vendor-boot vendor_boot.img] [--init-boot init_boot.img] [--androidboot مفتاح=قيمة]) [-i initrd] [-d disk.img] [-m ميغابايت] [-a \"bootargs\"]\n"
        "           [--feed \"أوامر\"] [--keys \"نص\"] [--screenshot out.png] [--audio out.wav] [--events \"key:HOME;tap:640,360\"] [--smp N]\n           [--max-insns N] [--no-jit] [-v]\n");
    exit(1);
}

static void prog(void *ctx, double f, const char *st) {
    static int last = -1; (void)ctx;
    if ((int)(f * 100) != last) { last = (int)(f * 100); fprintf(stderr, "\r[gtv] %s %d%%   ", st, last); }
}

int main(int argc, char **argv) {
    /* تجهيز صورة Android TV: gtv-emu --atv-extract ملف.zip قرص.img init_boot.img */
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "--atv-extract")) {
        char e[256] = "";
        int r = gtv_atv_extract(argv[2], argv[3], argc == 5 ? argv[4] : NULL, prog, NULL, e, sizeof e);
        fprintf(stderr, "\n[gtv] %s\n", r ? e : "تم");
        return r ? 1 : 0;
    }
    GtvConfig c; memset(&c, 0, sizeof c);
    const char *shot = NULL;
    uint64_t max_insns = 0;
    c.cmdline = "console=ttyAMA0 earlycon=pl011,0x09000000 nokaslr rdinit=/init";
    c.ram_mb = 512; c.ncpu = 1; c.use_jit = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc) c.kernel = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) c.initrd = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) c.disk = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) c.ram_mb = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) c.cmdline = argv[++i];
        else if (!strcmp(argv[i], "--feed") && i + 1 < argc) c.feed = argv[++i];
        else if (!strcmp(argv[i], "--max-insns") && i + 1 < argc) max_insns = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-v")) c.verbose++;
        else if (!strcmp(argv[i], "--no-jit")) c.use_jit = 0;
        else if ((!strcmp(argv[i], "--smp") || !strcmp(argv[i], "-c")) && i + 1 < argc) c.ncpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) c.keys = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) c.audio_wav = argv[++i];
        else if (!strcmp(argv[i], "--events") && i + 1 < argc) input_set_events(argv[++i]);
        else if (!strcmp(argv[i], "--jit")) c.use_jit = 1;
        else if (!strcmp(argv[i], "--bootimg") && i + 1 < argc) c.bootimg = argv[++i];
        else if (!strcmp(argv[i], "--vendor-boot") && i + 1 < argc) c.vendor_boot = argv[++i];
        else if (!strcmp(argv[i], "--init-boot") && i + 1 < argc) c.init_boot = argv[++i];
        else if (!strcmp(argv[i], "--androidboot") && i + 1 < argc && c.nandroidboot < 32) c.androidboot[c.nandroidboot++] = argv[++i];
        else usage();
    }
    if ((!c.kernel && !c.bootimg) || c.ncpu < 1 || c.ncpu > MAX_CPUS) usage();

    char err[256];
    Machine *m = gtv_create(&c, err, sizeof err);
    if (!m) { fprintf(stderr, "[gtv] %s\n", err); return 1; }
    gm = m;

    if (!c.feed && isatty(0)) {
        struct termios t;
        tcgetattr(0, &saved_tio);
        tio_saved = 1;
        atexit(restore_tty);
        t = saved_tio;
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(0, TCSANOW, &t);
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    uint64_t t0 = host_ns();
    gtv_run(m, max_insns);
    double secs = (host_ns() - t0) / 1e9;
    fprintf(stderr, "\n[gtv] توقف بعد %llu تعليمة خلال %.1f ثانية (%.1f مليون تعليمة/ثانية) pc=%016llx\n",
            (unsigned long long)machine_icount(m), secs, machine_icount(m) / secs / 1e6,
            (unsigned long long)m->cpus[0].pc);
    if (c.ncpu > 1) fprintf(stderr, "[gtv] المعالجات: %d\n", c.ncpu);
    jit_stats(m);
    snd_close(m);
    if (shot && gpu_screenshot(m, shot) < 0) fprintf(stderr, "[gtv] gpu: لا توجد صورة معروضة لالتقاطها\n");
    return m->cpus[0].exit_code;
}
