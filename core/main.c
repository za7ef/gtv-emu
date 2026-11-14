/* gtv-emu — واجهة سطر الأوامر لتشغيل نواة لينكس على المحاكي. */
#include "emu.h"
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
static void on_sig(int s) { (void)s; if (gm) for (int i = 0; i < MAX_CPUS; i++) gm->cpus[i].stop = 1; }

static long load_file(const char *path, uint8_t *dst, uint64_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    long n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

static void usage(void) {
    fprintf(stderr,
        "الاستخدام: gtv-emu (-k Image[.gz] | --bootimg boot.img [--vendor-boot vendor_boot.img] [--init-boot init_boot.img] [--androidboot مفتاح=قيمة]) [-i initrd] [-d disk.img] [-m ميغابايت] [-a \"bootargs\"]\n"
        "           [--feed \"أوامر\"] [--keys \"نص\"] [--screenshot out.png] [--audio out.wav] [--events \"key:HOME;tap:640,360\"] [--smp N]\n           [--max-insns N] [--no-jit] [-v]\n");
    exit(1);
}

int main(int argc, char **argv) {
    const char *bootimg = NULL, *vendorimg = NULL, *initbootimg = NULL, *props[32]; int nprops = 0;
    const char *kernel = NULL, *initrd = NULL, *disk = NULL, *feed = NULL, *keys = NULL, *shot = NULL, *audio = NULL;
    const char *bootargs = "console=ttyAMA0 earlycon=pl011,0x09000000 nokaslr rdinit=/init";
    uint64_t ram_mb = 512, max_insns = 0;
    int verbose = 0, use_jit = 1, ncpu = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc) kernel = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) initrd = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) ram_mb = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) bootargs = argv[++i];
        else if (!strcmp(argv[i], "--feed") && i + 1 < argc) feed = argv[++i];
        else if (!strcmp(argv[i], "--max-insns") && i + 1 < argc) max_insns = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "--no-jit")) use_jit = 0;
        else if ((!strcmp(argv[i], "--smp") || !strcmp(argv[i], "-c")) && i + 1 < argc) ncpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio = argv[++i];
        else if (!strcmp(argv[i], "--events") && i + 1 < argc) input_set_events(argv[++i]);
        else if (!strcmp(argv[i], "--jit")) use_jit = 1;
        else if (!strcmp(argv[i], "--bootimg") && i + 1 < argc) bootimg = argv[++i];
        else if (!strcmp(argv[i], "--vendor-boot") && i + 1 < argc) vendorimg = argv[++i];
        else if (!strcmp(argv[i], "--init-boot") && i + 1 < argc) initbootimg = argv[++i];
        else if (!strcmp(argv[i], "--androidboot") && i + 1 < argc && nprops < 32) props[nprops++] = argv[++i];
        else usage();
    }
    if ((!kernel && !bootimg) || ncpu < 1 || ncpu > MAX_CPUS) usage();

    Machine *m = calloc(1, sizeof(Machine));
    gm = m;
    m->verbose = verbose;
    m->quiet_unimpl = 0;
    m->ram_size = ram_mb << 20;
    m->ram = calloc(1, m->ram_size);
    if (!m->ram) { fprintf(stderr, "تعذّر حجز الذاكرة\n"); return 1; }
    m->start_ns = host_ns();
    m->gic = gic_create(m);
    m->uart = uart_create(m);
    virtio_create_all(m, disk, keys, audio);

    /* النواة عند RAM+2MB */
    uint64_t kaddr = RAM_BASE + 0x200000;
    AndroidBoot ab;
    static char abargs[8192];
    long ksz;
    if (bootimg) {
        if (android_load(&ab, bootimg, vendorimg, initbootimg, props, nprops) < 0) return 1;
        if (ab.kernel_len > m->ram_size - 0x200000) { fprintf(stderr, "النواة أكبر من الذاكرة\n"); return 1; }
        memcpy(m->ram + (kaddr - RAM_BASE), ab.kernel, ab.kernel_len);
        ksz = ab.kernel_len;
        /* سطر الأوامر: ما في الصور، ثم ما يضيفه المستخدم عبر -a */
        snprintf(abargs, sizeof abargs, "%s %s", ab.cmdline, bootargs);
        bootargs = abargs;
        if (initbootimg) fprintf(stderr, "[gtv] init_boot.img: قرص أولي عام %zu بايت\n", ab.initboot_len);
        fprintf(stderr, "[gtv] boot.img إصدار %u%s: نواة %zu بايت%s، قرص أولي %zu بايت، bootconfig %zu بايت\n",
                ab.header_version, vendorimg ? " مع vendor_boot" : "", ab.kernel_len, ab.kernel_gz ? " (فُكّ ضغطها من gzip)" : "",
                ab.ramdisk_len, ab.bootconfig_len);
    } else {
        ksz = load_file(kernel, m->ram + (kaddr - RAM_BASE), m->ram_size - 0x200000);
        uint8_t *kp = m->ram + (kaddr - RAM_BASE);
        if (ksz > 2 && kp[0] == 0x1f && kp[1] == 0x8b) { /* Image.gz */
            size_t ol; uint8_t *o = gunzip(kp, ksz, &ol);
            if (!o || ol > m->ram_size - 0x200000) { fprintf(stderr, "فشل فك ضغط النواة\n"); return 1; }
            memcpy(kp, o, ol); ksz = ol; free(o);
        }
    }
    uint64_t image_size;
    memcpy(&image_size, m->ram + (kaddr - RAM_BASE) + 16, 8);
    if (image_size < (uint64_t)ksz) image_size = ksz;

    uint64_t initrd_start = 0, initrd_end = 0;
    if (bootimg && ab.ramdisk_len) {
        initrd_start = RAM_BASE + (m->ram_size / 2 > 0x8000000 ? 0x8000000 : m->ram_size / 2);
        if (initrd_start < kaddr + image_size + 0x200000) initrd_start = (kaddr + image_size + 0x400000) & ~0xFFFFFULL;
        memcpy(m->ram + (initrd_start - RAM_BASE), ab.ramdisk, ab.ramdisk_len);
        initrd_end = initrd_start + ab.ramdisk_len;
    } else if (initrd) {
        initrd_start = RAM_BASE + (m->ram_size / 2 > 0x8000000 ? 0x8000000 : m->ram_size / 2);
        if (initrd_start < kaddr + image_size + 0x200000) initrd_start = (kaddr + image_size + 0x400000) & ~0xFFFFFULL;
        long isz = load_file(initrd, m->ram + (initrd_start - RAM_BASE), m->ram_size - (initrd_start - RAM_BASE));
        initrd_end = initrd_start + isz;
    }

    uint64_t dtb_addr = RAM_BASE;
    size_t dsz = fdt_build(m->ram, 0x100000, m->ram_size, bootargs, initrd_start, initrd_end, VIRTIO_COUNT, ncpu);
    if (!dsz) { fprintf(stderr, "فشل بناء شجرة الأجهزة\n"); return 1; }

    if (use_jit && !jit_create(m)) fprintf(stderr, "[gtv] المترجم الفوري غير متاح على هذا المضيف؛ يعمل المفسّر وحده\n");
    m->ncpu = ncpu;
    for (int i = 0; i < ncpu; i++) { cpu_reset(&m->cpus[i], m); m->cpus[i].id = i; }
    m->cpus[0].powered = 1;
    m->cpus[0].pc = kaddr;
    m->cpus[0].x[0] = dtb_addr;
    m->cur = &m->cpus[0];

    if (feed) uart_feed(m, feed);
    else if (isatty(0)) {
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

    fprintf(stderr, "[gtv] تشغيل النواة (%ld بايت) بذاكرة %llu ميغابايت\n", ksz, (unsigned long long)ram_mb);
    uint64_t t0 = host_ns();
    machine_run(m, max_insns);
    double secs = (host_ns() - t0) / 1e9;
    fprintf(stderr, "\n[gtv] توقف بعد %llu تعليمة خلال %.1f ثانية (%.1f مليون تعليمة/ثانية) pc=%016llx\n",
            (unsigned long long)machine_icount(m), secs, machine_icount(m) / secs / 1e6,
            (unsigned long long)m->cpus[0].pc);
    if (ncpu > 1) fprintf(stderr, "[gtv] المعالجات: %d\n", ncpu);
    jit_stats(m);
    snd_close(m);
    if (shot && gpu_screenshot(m, shot) < 0) fprintf(stderr, "[gtv] gpu: لا توجد صورة معروضة لالتقاطها\n");
    return m->cpus[0].exit_code;
}
