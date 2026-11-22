/* تجهيز صورة Android TV من ملف Google الأصلي على الجهاز نفسه.
 * يقرأ ملف zip الذي نزّله المستخدم من dl.google.com (صورة محاكي Android TV)، ويفك منه تدفقيًا:
 *   - system.img (قرص GPT فيه قسم super): تُنسخ الأقسام المنطقية منه إلى أقسام GPT عادية بالاسم
 *   - vendor.img: يُنسخ إلى قسم vendor أكبر (300 ميغابايت) ليتسع لمكونات الرسوميات
 *   - ramdisk.img (LZ4 legacy): يُفك ويُغلَّف في init_boot.img بترويسة الإصدار 4
 * ويضيف قسمي metadata وuserdata فارغين. الكتل الصفرية لا تُكتب فيبقى الملف متناثرًا.
 * لا يُعاد توزيع أي جزء من صورة Google؛ كل شيء يجري على جهاز المستخدم. */
#include "atvprep.h"
#include <zlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#define MB (1024ull * 1024ull)

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap);
}
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* ===== zip ===== */
typedef struct { uint64_t data_off, csize, usize; int method; } ZipEnt;

static int zip_find(int fd, const char *want, ZipEnt *out) {
    off_t size = lseek(fd, 0, SEEK_END);
    size_t tl = size < 70000 ? (size_t)size : 70000;
    uint8_t *t = malloc(tl);
    if (!t || pread(fd, t, tl, size - tl) != (ssize_t)tl) { free(t); return -1; }
    long e = -1;
    for (long i = (long)tl - 22; i >= 0; i--) if (le32(t + i) == 0x06054b50) { e = i; break; }
    if (e < 0) { free(t); return -1; }
    uint64_t cs = le32(t + e + 12), co = le32(t + e + 16);
    if (co == 0xFFFFFFFFu || cs == 0xFFFFFFFFu) {
        for (long i = e - 20; i >= 0; i--) if (le32(t + i) == 0x06064b50) { cs = le64(t + i + 40); co = le64(t + i + 48); break; }
    }
    free(t);
    uint8_t *cd = malloc(cs);
    if (!cd || pread(fd, cd, cs, co) != (ssize_t)cs) { free(cd); return -1; }
    int found = -1;
    for (uint64_t p = 0; p + 46 <= cs && le32(cd + p) == 0x02014b50;) {
        int meth = le16(cd + p + 10);
        uint64_t csz = le32(cd + p + 20), usz = le32(cd + p + 24);
        int n = le16(cd + p + 28), ex = le16(cd + p + 30), cm = le16(cd + p + 32);
        uint64_t off = le32(cd + p + 42);
        const uint8_t *x = cd + p + 46 + n;
        for (int q = 0; q + 4 <= ex;) {
            int id = le16(x + q), hl = le16(x + q + 2), k = 4;
            if (id == 1) {
                if (usz == 0xFFFFFFFFu) { usz = le64(x + q + k); k += 8; }
                if (csz == 0xFFFFFFFFu) { csz = le64(x + q + k); k += 8; }
                if (off == 0xFFFFFFFFu) off = le64(x + q + k);
            }
            q += 4 + hl;
        }
        size_t wl = strlen(want);
        if ((size_t)n >= wl && !memcmp(cd + p + 46 + n - wl, want, wl) && (n == (int)wl || cd[p + 46 + n - wl - 1] == '/')) {
            uint8_t lh[30];
            if (pread(fd, lh, 30, off) == 30 && le32(lh) == 0x04034b50) {
                out->data_off = off + 30 + le16(lh + 26) + le16(lh + 28);
                out->csize = csz; out->usize = usz; out->method = meth; found = 0;
            }
            break;
        }
        p += 46 + n + ex + cm;
    }
    free(cd);
    return found;
}

/* قارئ تدفقي لعضو مضغوط deflate */
typedef struct {
    int fd; uint64_t pos, end; z_stream z; uint8_t in[1 << 18]; int eof, stored;
} Inflater;

static int inf_open(Inflater *s, int fd, const ZipEnt *e) {
    memset(s, 0, sizeof *s);
    s->fd = fd; s->pos = e->data_off; s->end = e->data_off + e->csize; s->stored = e->method == 0;
    if (s->stored) return 0;
    return inflateInit2(&s->z, -15) == Z_OK ? 0 : -1;
}
static void inf_close(Inflater *s) { if (!s->stored) inflateEnd(&s->z); }
/* يقرأ حتى n بايت؛ يعيد عدد البايتات أو -1 عند الخطأ */
static long inf_read(Inflater *s, uint8_t *buf, size_t n) {
    if (s->stored) {
        uint64_t left = s->end - s->pos; if (n > left) n = (size_t)left;
        ssize_t r = n ? pread(s->fd, buf, n, s->pos) : 0;
        if (r < 0) return -1; s->pos += r; return r;
    }
    size_t got = 0;
    while (got < n && !s->eof) {
        if (s->z.avail_in == 0 && s->pos < s->end) {
            uint64_t want = s->end - s->pos; if (want > sizeof s->in) want = sizeof s->in;
            ssize_t r = pread(s->fd, s->in, want, s->pos);
            if (r <= 0) return -1;
            s->pos += r; s->z.next_in = s->in; s->z.avail_in = (uInt)r;
        }
        s->z.next_out = buf + got; s->z.avail_out = (uInt)(n - got);
        int rc = inflate(&s->z, Z_NO_FLUSH);
        got = n - s->z.avail_out;
        if (rc == Z_STREAM_END) s->eof = 1;
        else if (rc != Z_OK && rc != Z_BUF_ERROR) return -1;
        else if (rc == Z_BUF_ERROR && s->z.avail_in == 0 && s->pos >= s->end) return -1;
    }
    return (long)got;
}
static int inf_fill(Inflater *s, uint8_t *buf, size_t n) {
    size_t g = 0;
    while (g < n) { long r = inf_read(s, buf + g, n - g); if (r <= 0) return -1; g += r; }
    return 0;
}

/* ===== الإخراج ===== */
typedef struct {
    char name[36]; uint64_t first_lba, last_lba;
} OutPart;

/* كتابة متناثرة: تُتخطى الكتل الصفرية */
static int sparse_write(int fd, const uint8_t *b, size_t n, uint64_t off) {
    const size_t B = 4096;
    size_t i = 0;
    while (i < n) {
        size_t l = n - i < B ? n - i : B;
        const uint8_t *p = b + i;
        int zero = 1;
        for (size_t k = 0; k < l; k += 8) {
            uint64_t v = 0; memcpy(&v, p + k, l - k >= 8 ? 8 : l - k);
            if (v) { zero = 0; break; }
        }
        if (!zero) {
            /* دمج الكتل غير الصفرية المتتالية في كتابة واحدة */
            size_t j = i + l;
            while (j < n) {
                size_t l2 = n - j < B ? n - j : B; int z2 = 1;
                for (size_t k = 0; k < l2; k += 8) { uint64_t v = 0; memcpy(&v, b + j + k, l2 - k >= 8 ? 8 : l2 - k); if (v) { z2 = 0; break; } }
                if (z2) break;
                j += l2;
            }
            if (pwrite(fd, b + i, j - i, off + i) != (ssize_t)(j - i)) return -1;
            i = j;
        } else i += l;
    }
    return 0;
}

static void guid(uint8_t *g, uint32_t seed) {
    for (int i = 0; i < 16; i++) { seed = seed * 1103515245u + 12345u; g[i] = seed >> 16; }
    g[7] = (g[7] & 0x0F) | 0x40; g[8] = (g[8] & 0x3F) | 0x80;
}

static int write_gpt(int fd, const OutPart *parts, int np, uint64_t total_lba) {
    static const uint8_t linux_data[16] = { 0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4 };
    uint8_t *tbl = calloc(128, 128);
    if (!tbl) return -1;
    for (int i = 0; i < np; i++) {
        uint8_t *e = tbl + i * 128;
        memcpy(e, linux_data, 16); guid(e + 16, 0x47545600u + i);
        put64(e + 32, parts[i].first_lba); put64(e + 40, parts[i].last_lba);
        for (int k = 0; parts[i].name[k] && k < 36; k++) e[56 + k * 2] = parts[i].name[k];
    }
    uint32_t tcrc = crc32(0, tbl, 128 * 128);
    uint8_t mbr[512] = {0};
    mbr[446 + 1] = 0; mbr[446 + 2] = 2; mbr[446 + 4] = 0xEE; mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1); put32(mbr + 446 + 12, total_lba - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(total_lba - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    uint8_t dg[16]; guid(dg, 0x6774766du);
    for (int pass = 0; pass < 2; pass++) {
        uint8_t h[512] = {0};
        uint64_t cur = pass ? total_lba - 1 : 1, alt = pass ? 1 : total_lba - 1, tl = pass ? total_lba - 33 : 2;
        memcpy(h, "EFI PART", 8); put32(h + 8, 0x10000); put32(h + 12, 92);
        put64(h + 24, cur); put64(h + 32, alt); put64(h + 40, 34); put64(h + 48, total_lba - 34);
        memcpy(h + 56, dg, 16); put64(h + 72, tl); put32(h + 80, 128); put32(h + 84, 128); put32(h + 88, tcrc);
        put32(h + 16, crc32(0, h, 92));
        if (pwrite(fd, tbl, 128 * 128, tl * 512) != 128 * 128 || pwrite(fd, h, 512, cur * 512) != 512) { free(tbl); return -1; }
    }
    free(tbl);
    return pwrite(fd, mbr, 512, 0) == 512 ? 0 : -1;
}

/* ===== LZ4 legacy ===== */
static long lz4_block(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned t = src[i++]; size_t l = t >> 4;
        if (l == 15) { unsigned b; do { if (i >= n) return -1; b = src[i++]; l += b; } while (b == 255); }
        if (i + l > n || o + l > cap) return -1;
        memcpy(dst + o, src + i, l); i += l; o += l;
        if (i >= n) break;
        if (i + 2 > n) return -1;
        size_t off = src[i] | src[i + 1] << 8; i += 2;
        size_t m = t & 15;
        if (m == 15) { unsigned b; do { if (i >= n) return -1; b = src[i++]; m += b; } while (b == 255); }
        m += 4;
        if (off == 0 || off > o || o + m > cap) return -1;
        for (size_t k = 0; k < m; k++) dst[o + k] = dst[o - off + k];
        o += m;
    }
    return (long)o;
}

static uint8_t *lz4_legacy(const uint8_t *d, size_t n, size_t *outlen) {
    if (n < 4 || le32(d) != 0x184C2102u) return NULL;
    size_t cap = 64 * MB, o = 0, p = 4;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    while (p + 4 <= n) {
        uint32_t sz = le32(d + p);
        if (sz == 0x184C2102u) { p += 4; continue; }
        p += 4;
        if (sz == 0 || p + sz > n) break;
        long r = lz4_block(d + p, sz, out + o, cap - o);
        if (r < 0) { free(out); return NULL; }
        o += r; p += sz;
    }
    *outlen = o;
    return out;
}

/* ===== التجهيز ===== */
typedef struct { uint64_t src, len, dst; } MapRange;
static int cmp_range(const void *a, const void *b) {
    const MapRange *x = a, *y = b; return x->src < y->src ? -1 : x->src > y->src;
}

int gtv_atv_extract(const char *zip_path, const char *disk_out, const char *initboot_out,
                    gtv_progress_fn progress, void *ctx, char *err, size_t errlen) {
    int zfd = open(zip_path, O_RDONLY);
    if (zfd < 0) { seterr(err, errlen, "تعذّر فتح الملف المنزَّل"); return -1; }
    ZipEnt esys, even, eram;
    if (zip_find(zfd, "system.img", &esys) || zip_find(zfd, "vendor.img", &even) || zip_find(zfd, "ramdisk.img", &eram)) {
        close(zfd); seterr(err, errlen, "الملف لا يبدو صورة Android TV من Google (لم يُعثر على system.img/vendor.img/ramdisk.img)"); return -1;
    }
    uint64_t total_c = esys.csize + even.csize + eram.csize, done_c = 0;
    int rc = -1, ofd = -1;
    uint8_t *head = NULL, *buf = NULL, *ram = NULL, *cpio = NULL;
    Inflater *inf = calloc(1, sizeof *inf);
    MapRange ranges[256]; int nr = 0;
    OutPart parts[16]; int np = 0;
    if (!inf) goto out;

    /* 1) init_boot من ramdisk.img (اختياري: التطبيق يستخدم init_boot من AOSP Cuttlefish، فيمرّر NULL) */
    if (!initboot_out) done_c += eram.csize;
    else {
        if (progress) progress(ctx, 0, "فك القرص الأولي");
        ram = malloc(eram.usize + 1);
        if (!ram || inf_open(inf, zfd, &eram) || inf_fill(inf, ram, eram.usize)) { inf_close(inf); seterr(err, errlen, "فشل فك ramdisk.img"); goto out; }
        inf_close(inf);
        size_t cl = 0;
        cpio = lz4_legacy(ram, eram.usize, &cl);
        if (!cpio) { /* ربما ليس مضغوطًا */
            if (eram.usize >= 6 && !memcmp(ram, "070701", 6)) { cpio = ram; ram = NULL; cl = eram.usize; }
            else { seterr(err, errlen, "صيغة ramdisk.img غير معروفة"); goto out; }
        }
        /* ضغط gzip: النواة لا تتعرف على cpio غير مضغوط بعد أرشيف مضغوط في القرص الأولي المدمج */
        {
            uLong cap = compressBound(cl) + 64;
            uint8_t *gz = malloc(cap);
            z_stream zs; memset(&zs, 0, sizeof zs);
            if (!gz || deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) { free(gz); seterr(err, errlen, "فشل ضغط القرص الأولي"); goto out; }
            zs.next_in = cpio; zs.avail_in = (uInt)cl; zs.next_out = gz; zs.avail_out = (uInt)cap;
            int zr = deflate(&zs, Z_FINISH); size_t gl = zs.total_out; deflateEnd(&zs);
            if (zr != Z_STREAM_END) { free(gz); seterr(err, errlen, "فشل ضغط القرص الأولي"); goto out; }
            free(cpio); cpio = gz; cl = gl;
        }
        uint8_t hdr[4096] = {0};
        memcpy(hdr, "ANDROID!", 8); put32(hdr + 8, 0); put32(hdr + 12, (uint32_t)cl);
        put32(hdr + 20, 1584); put32(hdr + 40, 4);
        int ifd = open(initboot_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ifd < 0 || write(ifd, hdr, 4096) != 4096 || write(ifd, cpio, cl) != (ssize_t)cl) { if (ifd >= 0) close(ifd); seterr(err, errlen, "تعذّرت كتابة init_boot"); goto out; }
        size_t pad = (4096 - cl % 4096) % 4096; uint8_t z[4096] = {0};
        if (pad && write(ifd, z, pad) != (ssize_t)pad) { close(ifd); goto out; }
        close(ifd);
        done_c += eram.csize;
    }

    /* 2) رأس system.img: GPT ثم بيانات super الوصفية */
    if (inf_open(inf, zfd, &esys)) { seterr(err, errlen, "فشل فك system.img"); goto out; }
    const size_t HEAD = 4 * MB;
    head = malloc(HEAD);
    if (!head || inf_fill(inf, head, HEAD)) { seterr(err, errlen, "system.img قصير"); goto out_sys; }
    uint64_t super_off = 0;
    if (!memcmp(head + 512, "EFI PART", 8)) {
        uint64_t tl = le64(head + 512 + 72); uint32_t ne = le32(head + 512 + 80), es = le32(head + 512 + 84);
        for (uint32_t i = 0; i < ne && tl * 512 + (i + 1) * es <= HEAD; i++) {
            const uint8_t *e = head + tl * 512 + i * es; char nm[37] = {0};
            for (int k = 0; k < 36; k++) nm[k] = (char)e[56 + k * 2];
            if (!strcmp(nm, "super")) { super_off = le64(e + 32) * 512; break; }
        }
    }
    if (super_off + 12288 + 4096 > HEAD || le32(head + super_off + 4096) != 0x616c4467u) {
        seterr(err, errlen, "لم يُعثر على قسم super في system.img"); goto out_sys;
    }
    {
        const uint8_t *m = head + super_off + 12288;
        if (le32(m) != 0x414C5030u) { seterr(err, errlen, "بيانات super الوصفية غير صالحة"); goto out_sys; }
        uint32_t hsz = le32(m + 8);
        const uint8_t *t = m + hsz;
        uint32_t po = le32(m + 80), pn = le32(m + 84), pes = le32(m + 88);
        uint32_t xo = le32(m + 92), xn = le32(m + 96), xes = le32(m + 100);
        uint64_t lba = 2048;
        for (uint32_t i = 0; i < pn && np < 12; i++) {
            const uint8_t *e = t + po + i * pes;
            char nm[37] = {0}; memcpy(nm, e, 36);
            uint32_t fe = le32(e + 40), ne = le32(e + 44);
            size_t L = strlen(nm);
            if (L > 2 && (!strcmp(nm + L - 2, "_b"))) continue;            /* الخانة الثانية */
            if (L > 2 && !strcmp(nm + L - 2, "_a")) nm[L - 2] = 0;
            if (!strcmp(nm, "vendor") || ne == 0) continue;                  /* vendor يأتي من vendor.img */
            uint64_t psz = 0, at = lba * 512;
            for (uint32_t k = fe; k < fe + ne && k < xn; k++) {
                const uint8_t *x = t + xo + k * xes;
                uint64_t ns = le64(x); uint32_t tt = le32(x + 8); uint64_t td = le64(x + 12);
                if (tt == 0 && nr < 256) { ranges[nr].src = super_off + td * 512; ranges[nr].len = ns * 512; ranges[nr].dst = at + psz; nr++; }
                psz += ns * 512;
            }
            snprintf(parts[np].name, 36, "%s", nm);
            parts[np].first_lba = lba;
            uint64_t sec = (psz + 511) / 512; sec = (sec + 2047) / 2048 * 2048;
            parts[np].last_lba = lba + sec - 1; lba += sec; np++;
        }
        if (np == 0) { seterr(err, errlen, "لا أقسام في super"); goto out_sys; }
        const struct { const char *n; uint64_t mb; } extra[] = { { "vendor", 300 }, { "metadata", 32 }, { "userdata", 3072 } };
        for (int i = 0; i < 3; i++) {
            snprintf(parts[np].name, 36, "%s", extra[i].n);
            parts[np].first_lba = lba; lba += extra[i].mb * 2048; parts[np].last_lba = lba - 1; np++;
        }
        uint64_t total_lba = lba + 2048;
        qsort(ranges, nr, sizeof ranges[0], cmp_range);
        ofd = open(disk_out, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0 || ftruncate(ofd, (off_t)(total_lba * 512))) { seterr(err, errlen, "تعذّر إنشاء ملف القرص (مساحة غير كافية؟)"); goto out_sys; }
        if (write_gpt(ofd, parts, np, total_lba)) { seterr(err, errlen, "تعذّرت كتابة جدول الأقسام"); goto out_sys; }
    }
    /* 3) نسخ النطاقات تدفقيًا */
    {
        const size_t CH = 4 * MB;
        buf = malloc(CH);
        if (!buf) goto out_sys;
        uint64_t pos = 0; long n = HEAD; uint8_t *cur = head; int ri = 0;
        uint64_t last_end = ranges[nr - 1].src + ranges[nr - 1].len;
        for (;;) {
            uint64_t a = pos, b = pos + n;
            while (ri < nr && ranges[ri].src + ranges[ri].len <= a) ri++;
            for (int k = ri; k < nr && ranges[k].src < b; k++) {
                uint64_t s = ranges[k].src > a ? ranges[k].src : a;
                uint64_t e = ranges[k].src + ranges[k].len < b ? ranges[k].src + ranges[k].len : b;
                if (e > s && sparse_write(ofd, cur + (s - a), e - s, ranges[k].dst + (s - ranges[k].src))) { seterr(err, errlen, "فشلت الكتابة (المساحة ممتلئة؟)"); goto out_sys; }
            }
            pos = b;
            if (pos >= last_end) break;
            if (progress) progress(ctx, (double)(done_c + (inf->pos - esys.data_off)) / total_c, "نسخ أقسام النظام");
            n = inf_read(inf, buf, CH); cur = buf;
            if (n <= 0) { seterr(err, errlen, "انتهى system.img قبل الأوان (الملف ناقص؟)"); goto out_sys; }
        }
        inf_close(inf);
        done_c += esys.csize;
    }
    /* 4) vendor.img: قرص GPT فيه قسم vendor */
    {
        if (inf_open(inf, zfd, &even)) goto out;
        if (inf_fill(inf, head, 1 * MB)) { seterr(err, errlen, "vendor.img قصير"); goto out_sys; }
        uint64_t vs = 0, ve = 0;
        if (!memcmp(head + 512, "EFI PART", 8)) {
            uint64_t tl = le64(head + 512 + 72);
            const uint8_t *e = head + tl * 512;
            vs = le64(e + 32) * 512; ve = (le64(e + 40) + 1) * 512;
        } else { vs = 0; ve = even.usize; }
        uint64_t vdst = 0;
        for (int i = 0; i < np; i++) if (!strcmp(parts[i].name, "vendor")) vdst = parts[i].first_lba * 512;
        if (ve - vs > 300 * MB) { seterr(err, errlen, "vendor.img أكبر من المتوقع"); goto out_sys; }
        uint64_t pos = 1 * MB;
        if (vs < pos) { if (sparse_write(ofd, head + vs, pos - vs, vdst)) goto out_sys; }
        while (pos < ve) {
            long n = inf_read(inf, buf, 4 * MB);
            if (n <= 0) break;
            uint64_t a = pos, b = pos + n;
            uint64_t s = a > vs ? a : vs, e = b < ve ? b : ve;
            if (e > s && sparse_write(ofd, buf + (s - a), e - s, vdst + (s - vs))) { seterr(err, errlen, "فشلت الكتابة"); goto out_sys; }
            pos = b;
            if (progress) progress(ctx, (double)(done_c + (inf->pos - even.data_off)) / total_c, "نسخ قسم vendor");
        }
        inf_close(inf);
    }
    if (fsync(ofd)) { seterr(err, errlen, "فشل حفظ القرص"); goto out; }
    if (progress) progress(ctx, 1, "اكتمل الاستخراج");
    rc = 0;
    goto out;
out_sys:
    inf_close(inf);
out:
    if (ofd >= 0) close(ofd);
    close(zfd);
    free(inf); free(head); free(buf); free(ram); free(cpio);
    if (rc) { unlink(disk_out); if (initboot_out) unlink(initboot_out); }
    return rc;
}
