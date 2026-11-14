/* gtv-emu — تهيئة الآلة وتشغيلها، مشتركة بين واجهة سطر الأوامر والتطبيقات المضمِّنة. */
#include "emu.h"
#include "gtv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void fail(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}

static long load_file(const char *path, uint8_t *dst, uint64_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

void gtv_set_host(GtvHost *host) { gtv_host = host; }

GtvMachine *gtv_create(const GtvConfig *cfg, char *err, size_t errlen) {
    const char *bootargs = cfg->cmdline ? cfg->cmdline : "console=ttyAMA0 earlycon=pl011,0x09000000 nokaslr rdinit=/init";
    uint64_t ram_mb = cfg->ram_mb ? cfg->ram_mb : 512;
    int ncpu = cfg->ncpu ? cfg->ncpu : 1;
    if ((!cfg->kernel && !cfg->bootimg) || ncpu < 1 || ncpu > MAX_CPUS) { fail(err, errlen, "إعدادات غير صالحة: لا نواة أو عدد معالجات خارج 1..%d", MAX_CPUS); return NULL; }

    Machine *m = calloc(1, sizeof(Machine));
    if (!m) { fail(err, errlen, "تعذّر حجز الذاكرة"); return NULL; }
    m->verbose = cfg->verbose;
    m->ram_size = ram_mb << 20;
    m->ram = calloc(1, m->ram_size);
    if (!m->ram) { free(m); fail(err, errlen, "تعذّر حجز %llu ميغابايت لذاكرة الضيف", (unsigned long long)ram_mb); return NULL; }
    m->start_ns = host_ns();
    m->gic = gic_create(m);
    m->uart = uart_create(m);
    virtio_create_all(m, cfg->disk, cfg->keys, cfg->audio_wav);

    uint64_t kaddr = RAM_BASE + 0x200000;
    AndroidBoot ab; memset(&ab, 0, sizeof ab);
    static char abargs[8192];
    long ksz;
    if (cfg->bootimg) {
        if (android_load(&ab, cfg->bootimg, cfg->vendor_boot, cfg->init_boot, (const char **)cfg->androidboot, cfg->nandroidboot) < 0) { fail(err, errlen, "تعذّرت قراءة صور الإقلاع"); goto bad; }
        if (ab.kernel_len > m->ram_size - 0x200000) { fail(err, errlen, "النواة أكبر من الذاكرة"); goto bad; }
        memcpy(m->ram + (kaddr - RAM_BASE), ab.kernel, ab.kernel_len);
        ksz = ab.kernel_len;
        snprintf(abargs, sizeof abargs, "%s %s", ab.cmdline, bootargs);
        bootargs = abargs;
        if (cfg->verbose >= 0) {
            if (cfg->init_boot) fprintf(stderr, "[gtv] init_boot.img: قرص أولي عام %zu بايت\n", ab.initboot_len);
            fprintf(stderr, "[gtv] boot.img إصدار %u%s: نواة %zu بايت%s، قرص أولي %zu بايت، bootconfig %zu بايت\n",
                    ab.header_version, cfg->vendor_boot ? " مع vendor_boot" : "", ab.kernel_len, ab.kernel_gz ? " (فُكّ ضغطها من gzip)" : "",
                    ab.ramdisk_len, ab.bootconfig_len);
        }
    } else {
        ksz = load_file(cfg->kernel, m->ram + (kaddr - RAM_BASE), m->ram_size - 0x200000);
        if (ksz <= 0) { fail(err, errlen, "تعذّرت قراءة النواة: %s", cfg->kernel); goto bad; }
        uint8_t *kp = m->ram + (kaddr - RAM_BASE);
        if (ksz > 2 && kp[0] == 0x1f && kp[1] == 0x8b) { /* Image.gz */
            size_t ol; uint8_t *o = gunzip(kp, ksz, &ol);
            if (!o || ol > m->ram_size - 0x200000) { fail(err, errlen, "فشل فك ضغط النواة"); free(o); goto bad; }
            memcpy(kp, o, ol); ksz = ol; free(o);
        }
    }
    uint64_t image_size;
    memcpy(&image_size, m->ram + (kaddr - RAM_BASE) + 16, 8);
    if (image_size < (uint64_t)ksz) image_size = ksz;

    uint64_t initrd_start = 0, initrd_end = 0;
    if ((cfg->bootimg && ab.ramdisk_len) || (!cfg->bootimg && cfg->initrd)) {
        initrd_start = RAM_BASE + (m->ram_size / 2 > 0x8000000 ? 0x8000000 : m->ram_size / 2);
        if (initrd_start < kaddr + image_size + 0x200000) initrd_start = (kaddr + image_size + 0x400000) & ~0xFFFFFULL;
        if (cfg->bootimg) {
            memcpy(m->ram + (initrd_start - RAM_BASE), ab.ramdisk, ab.ramdisk_len);
            initrd_end = initrd_start + ab.ramdisk_len;
        } else {
            long isz = load_file(cfg->initrd, m->ram + (initrd_start - RAM_BASE), m->ram_size - (initrd_start - RAM_BASE));
            if (isz <= 0) { fail(err, errlen, "تعذّرت قراءة القرص الأولي: %s", cfg->initrd); goto bad; }
            initrd_end = initrd_start + isz;
        }
    }

    size_t dsz = fdt_build(m->ram, 0x100000, m->ram_size, bootargs, initrd_start, initrd_end, VIRTIO_COUNT, ncpu);
    if (!dsz) { fail(err, errlen, "فشل بناء شجرة الأجهزة"); goto bad; }

    if (cfg->use_jit && !jit_create(m) && cfg->verbose >= 0)
        fprintf(stderr, "[gtv] المترجم الفوري غير متاح (%s)؛ يعمل المفسّر وحده\n", gtv_jit_status());
    m->ncpu = ncpu;
    for (int i = 0; i < ncpu; i++) { cpu_reset(&m->cpus[i], m); m->cpus[i].id = i; }
    m->cpus[0].powered = 1;
    m->cpus[0].pc = kaddr;
    m->cpus[0].x[0] = RAM_BASE; /* عنوان شجرة الأجهزة */
    m->cur = &m->cpus[0];
    if (cfg->feed) uart_feed(m, cfg->feed);
    if (cfg->verbose >= 0)
        fprintf(stderr, "[gtv] تشغيل النواة (%ld بايت) بذاكرة %llu ميغابايت\n", ksz, (unsigned long long)ram_mb);
    return m;
bad:
    free(m->ram); free(m);
    return NULL;
}

int gtv_run(GtvMachine *m, uint64_t max_insns) { return machine_run(m, max_insns); }
void gtv_stop(GtvMachine *m) { for (int i = 0; i < MAX_CPUS; i++) m->cpus[i].stop = 1; }
uint64_t gtv_icount(GtvMachine *m) { return machine_icount(m); }
int gtv_jit_active(GtvMachine *m) { return m->jit != NULL; }
void gtv_destroy(GtvMachine *m) { if (!m) return; free(m->ram); m->ram = NULL; /* يبقى هيكل الآلة لأن الأجهزة تشير إليه */ }
