/* gtv-emu — مسار إقلاع أندرويد:
 *  - فك ضغط gzip (inflate) دون مكتبات خارجية، لنوى Image.gz.
 *  - قراءة صورة boot.img (الإصدارات 0–4) وvendor_boot.img (الإصدار 3–4).
 *  - دمج الأقراص الأولية (vendor أولًا ثم generic) وإلحاق bootconfig بصيغة النواة.
 */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================== inflate =========================== */
typedef struct {
    const uint8_t *src, *end;
    uint32_t bits; int nbits;
    uint8_t *out; size_t olen, ocap;
    int err;
} Inf;

typedef struct { uint16_t count[16], sym[288]; } Huff;

static int getbit(Inf *s) {
    if (!s->nbits) {
        if (s->src >= s->end) { s->err = 1; return 0; }
        s->bits = *s->src++; s->nbits = 8;
    }
    int b = s->bits & 1; s->bits >>= 1; s->nbits--;
    return b;
}
static uint32_t getbits(Inf *s, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)getbit(s) << i;
    return v;
}
static void build(Huff *h, const uint8_t *len, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = i;
}
static int decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= getbit(s);
        int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1; return -1;
}
static void put(Inf *s, uint8_t b) {
    if (s->olen == s->ocap) {
        s->ocap = s->ocap ? s->ocap * 2 : 1 << 20;
        s->out = realloc(s->out, s->ocap);
        if (!s->out) { s->err = 1; s->ocap = s->olen = 0; return; }
    }
    s->out[s->olen++] = b;
}
static const uint16_t LBASE[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void block(Inf *s, const Huff *lt, const Huff *dt) {
    for (;;) {
        int sym = decode(s, lt);
        if (s->err) return;
        if (sym < 256) { put(s, sym); continue; }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) { s->err = 1; return; }
        int len = LBASE[sym] + getbits(s, LEXT[sym]);
        int ds = decode(s, dt);
        if (ds < 0 || ds >= 30) { s->err = 1; return; }
        size_t dist = DBASE[ds] + getbits(s, DEXT[ds]);
        if (dist > s->olen) { s->err = 1; return; }
        for (int i = 0; i < len && !s->err; i++) put(s, s->out[s->olen - dist]);
    }
}

/* يفك تدفق gzip؛ يعيد مخزنًا مخصّصًا وطوله أو NULL */
uint8_t *gunzip(const uint8_t *d, size_t n, size_t *outlen) {
    if (n < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) return NULL;
    Inf s = { .src = d + 10, .end = d + n };
    uint8_t fl = d[3];
    if (fl & 4) { size_t x = s.src[0] | s.src[1] << 8; s.src += 2 + x; }
    if (fl & 8) while (s.src < s.end && *s.src++) {}
    if (fl & 16) while (s.src < s.end && *s.src++) {}
    if (fl & 2) s.src += 2;
    static Huff fl_l, fl_d; static int fixed_ready;
    if (!fixed_ready) {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        build(&fl_l, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        build(&fl_d, l, 30);
        fixed_ready = 1;
    }
    int last;
    do {
        last = getbit(&s);
        int type = getbits(&s, 2);
        if (type == 0) {
            s.nbits = 0;
            if (s.src + 4 > s.end) { s.err = 1; break; }
            uint16_t len = s.src[0] | s.src[1] << 8;
            s.src += 4;
            if (s.src + len > s.end) { s.err = 1; break; }
            for (int i = 0; i < len; i++) put(&s, s.src[i]);
            s.src += len;
        } else if (type == 1) {
            block(&s, &fl_l, &fl_d);
        } else if (type == 2) {
            static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            int hlit = getbits(&s, 5) + 257, hdist = getbits(&s, 5) + 1, hclen = getbits(&s, 4) + 4;
            uint8_t lens[320] = {0};
            for (int i = 0; i < hclen; i++) lens[ord[i]] = getbits(&s, 3);
            Huff ch, lt, dt;
            build(&ch, lens, 19);
            memset(lens, 0, sizeof lens);
            for (int i = 0; i < hlit + hdist && !s.err; ) {
                int sym = decode(&s, &ch);
                if (sym < 16) lens[i++] = sym;
                else {
                    int rep, val = 0;
                    if (sym == 16) { if (!i) { s.err = 1; break; } val = lens[i - 1]; rep = 3 + getbits(&s, 2); }
                    else if (sym == 17) rep = 3 + getbits(&s, 3);
                    else rep = 11 + getbits(&s, 7);
                    if (i + rep > hlit + hdist) { s.err = 1; break; }
                    while (rep--) lens[i++] = val;
                }
            }
            build(&lt, lens, hlit);
            build(&dt, lens + hlit, hdist);
            block(&s, &lt, &dt);
        } else s.err = 1;
    } while (!last && !s.err);
    if (s.err) { free(s.out); return NULL; }
    *outlen = s.olen;
    return s.out;
}

/* ======================== صور أندرويد ======================== */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? sz : 1);
    if (b && fread(b, 1, sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = sz;
    return b;
}

static void append(uint8_t **buf, size_t *len, const void *d, size_t n) {
    *buf = realloc(*buf, *len + n + 1);
    memcpy(*buf + *len, d, n);
    *len += n;
}

/* يحمّل boot.img وvendor_boot.img (اختياري) ويملأ ab */
int android_load(AndroidBoot *ab, const char *boot, const char *vendor, const char *initboot, const char **props, int nprops) {
    memset(ab, 0, sizeof *ab);
    size_t n;
    uint8_t *b = read_all(boot, &n);
    if (!b || n < 4096 || memcmp(b, "ANDROID!", 8)) { fprintf(stderr, "[gtv] boot.img غير صالحة: %s\n", boot); free(b); return -1; }
    uint32_t ver = le32(b + 40);
    uint32_t ksz, rsz, page;
    char cmd[3072] = "";
    if (ver >= 3) {
        page = 4096; ksz = le32(b + 8); rsz = le32(b + 12);
        snprintf(cmd, sizeof cmd, "%.1536s", (const char *)b + 44);
    } else {
        ksz = le32(b + 8); rsz = le32(b + 16); page = le32(b + 36);
        if (page < 2048) page = 2048;
        snprintf(cmd, sizeof cmd, "%.512s%.1024s", (const char *)b + 64, (const char *)b + 608);
    }
    ab->header_version = ver;
    size_t koff = page, roff = koff + align_up(ksz, page);
    if (roff + rsz > n) { fprintf(stderr, "[gtv] boot.img مقطوعة\n"); free(b); return -1; }
    /* النواة: خام أو gzip */
    if (ksz >= 2 && b[koff] == 0x1f && b[koff + 1] == 0x8b) {
        ab->kernel = gunzip(b + koff, ksz, &ab->kernel_len);
        if (!ab->kernel) { fprintf(stderr, "[gtv] فشل فك ضغط النواة\n"); free(b); return -1; }
        ab->kernel_gz = 1;
    } else if (ksz >= 4 && le32(b + koff) == 0x184C2102) {
        fprintf(stderr, "[gtv] النواة مضغوطة بـLZ4، وهي غير مدعومة بعد؛ استخدم صورة Image أو Image.gz\n");
        free(b); return -1;
    } else {
        ab->kernel = malloc(ksz); memcpy(ab->kernel, b + koff, ksz); ab->kernel_len = ksz;
    }
    char vcmd[2048] = "";
    uint8_t *bootconfig = NULL; size_t bclen = 0;
    if (vendor) {
        size_t vn;
        uint8_t *v = read_all(vendor, &vn);
        if (!v || vn < 2112 || memcmp(v, "VNDRBOOT", 8)) { fprintf(stderr, "[gtv] vendor_boot.img غير صالحة: %s\n", vendor); free(v); free(b); return -1; }
        uint32_t vver = le32(v + 8), vpage = le32(v + 12), vrsz = le32(v + 24);
        uint32_t hsz = le32(v + 2096), dtbsz = le32(v + 2100);
        snprintf(vcmd, sizeof vcmd, "%.2048s", (const char *)v + 28);
        size_t off = align_up(hsz, vpage);
        if (off + vrsz <= vn) append(&ab->ramdisk, &ab->ramdisk_len, v + off, vrsz); /* vendor أولًا */
        off += align_up(vrsz, vpage);
        off += align_up(dtbsz, vpage);
        if (vver >= 4) {
            uint32_t tsz = le32(v + 2112), bcsz = le32(v + 2124);
            off += align_up(tsz, vpage);
            if (bcsz && off + bcsz <= vn) { bootconfig = malloc(bcsz); memcpy(bootconfig, v + off, bcsz); bclen = bcsz; }
        }
        ab->vendor_version = vver;
        free(v);
    }
    /* init_boot.img (أندرويد 13+): يحمل القرص الأولي العام وفيه init المرحلة الأولى */
    if (initboot) {
        size_t in_;
        uint8_t *ib = read_all(initboot, &in_);
        if (!ib || in_ < 4096 || memcmp(ib, "ANDROID!", 8) || le32(ib + 40) < 3) {
            fprintf(stderr, "[gtv] init_boot.img غير صالحة: %s\n", initboot); free(ib); free(b); return -1;
        }
        uint32_t iks = le32(ib + 8), irs = le32(ib + 12);
        size_t io = 4096 + align_up(iks, 4096);
        if (io + irs <= in_) append(&ab->ramdisk, &ab->ramdisk_len, ib + io, irs);
        ab->initboot_len = irs;
        free(ib);
    }
    if (rsz) append(&ab->ramdisk, &ab->ramdisk_len, b + roff, rsz);
    free(b);
    /* bootconfig: من vendor_boot ثم الخصائص الإضافية */
    for (int i = 0; i < nprops; i++) {
        char line[512];
        const char *eq = strchr(props[i], '=');
        int l = eq ? snprintf(line, sizeof line, "androidboot.%.*s = \"%s\"\n", (int)(eq - props[i]), props[i], eq + 1)
                   : snprintf(line, sizeof line, "androidboot.%s = \"1\"\n", props[i]);
        append(&bootconfig, &bclen, line, l);
    }
    if (bclen) {
        /* محاذاة نهاية البيانات على 4 بايت بأصفار تُحسب ضمن الحجم، كما تفعل أداة bootconfig */
        while ((ab->ramdisk_len + bclen) & 3) append(&bootconfig, &bclen, "", 1);
        /* تنسيق النواة: البيانات، ثم الحجم، ثم مجموع البايتات، ثم الوسم */
        uint32_t sum = 0;
        for (size_t i = 0; i < bclen; i++) sum += bootconfig[i];
        uint32_t sz32 = bclen;
        append(&ab->ramdisk, &ab->ramdisk_len, bootconfig, bclen);
        append(&ab->ramdisk, &ab->ramdisk_len, &sz32, 4);
        append(&ab->ramdisk, &ab->ramdisk_len, &sum, 4);
        append(&ab->ramdisk, &ab->ramdisk_len, "#BOOTCONFIG\n", 12);
        ab->bootconfig_len = bclen;
    }
    free(bootconfig);
    snprintf(ab->cmdline, sizeof ab->cmdline, "%s%s%s%s", vcmd, vcmd[0] && cmd[0] ? " " : "", cmd,
             ab->bootconfig_len && !strstr(vcmd, "bootconfig") && !strstr(cmd, "bootconfig") ? " bootconfig" : "");
    return 0;
}
