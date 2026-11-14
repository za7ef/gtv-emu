/* gtv-emu — أجهزة virtio-mmio (الإصدار الثاني من الواجهة):
 * القرص (blk) والشبكة (net) والرسوميات (gpu) والإدخال (input).
 */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define VQ_MAX 4
#define VF_VERSION_1 (1ULL << 32)

enum { VT_NONE = 0, VT_NET = 1, VT_BLK = 2, VT_GPU = 16, VT_INPUT = 18 };

typedef struct { uint64_t addr; uint32_t len; int wr; } VDesc;

typedef struct VDev VDev;
struct VDev {
    int idx, type;
    uint64_t features;        /* ما يعرضه الجهاز */
    uint32_t status, dev_feat_sel, drv_feat_sel, queue_sel, int_status, cfg_gen;
    uint64_t drv_features;
    int nq;
    struct { uint32_t num, ready; uint64_t desc, avail, used; uint16_t last_avail; } q[VQ_MAX];
    void (*notify)(Machine *m, VDev *d, int q);
    void (*cfg_read)(VDev *d, uint8_t *cfg);      /* يملأ 256 بايت */
    void (*cfg_write)(VDev *d, uint64_t off, int size, uint64_t v);
    void (*reset)(VDev *d);
    void *priv;
};

static uint8_t *gpa(Machine *m, uint64_t pa, uint64_t len) {
    if (pa < RAM_BASE || pa + len > RAM_BASE + m->ram_size || pa + len < pa) return NULL;
    return m->ram + (pa - RAM_BASE);
}

/* ---------- طابور virtqueue ---------- */
/* يأخذ سلسلة من الطابور ويعيد رأسها، أو -1 إذا كان الطابور فارغًا */
static int vq_pop(Machine *m, VDev *d, int qi, VDesc *out, int max, int *n) {
    typeof(d->q[0]) *q = &d->q[qi];
    if (!q->ready || !q->num) return -1;
    uint8_t *avail = gpa(m, q->avail, 4 + 2 * q->num);
    uint8_t *desc = gpa(m, q->desc, 16 * q->num);
    if (!avail || !desc) return -1;
    uint16_t aidx; memcpy(&aidx, avail + 2, 2);
    if (q->last_avail == aidx) return -1;
    uint16_t head; memcpy(&head, avail + 4 + 2 * (q->last_avail % q->num), 2);
    q->last_avail++;
    int k = 0; uint16_t i = head;
    for (int guard = 0; guard < 256 && k < max; guard++) {
        uint8_t *e = desc + 16 * (i % q->num);
        uint64_t a; uint32_t l; uint16_t fl, nx;
        memcpy(&a, e, 8); memcpy(&l, e + 8, 4); memcpy(&fl, e + 12, 2); memcpy(&nx, e + 14, 2);
        out[k].addr = a; out[k].len = l; out[k].wr = (fl & 2) != 0; k++;
        if (!(fl & 1)) break;
        i = nx;
    }
    *n = k;
    return head;
}

static void vq_push(Machine *m, VDev *d, int qi, int head, uint32_t written) {
    typeof(d->q[0]) *q = &d->q[qi];
    uint8_t *used = gpa(m, q->used, 4 + 8 * q->num);
    if (!used) return;
    uint16_t uidx; memcpy(&uidx, used + 2, 2);
    uint8_t *ue = used + 4 + 8 * (uidx % q->num);
    uint32_t h = head;
    memcpy(ue, &h, 4); memcpy(ue + 4, &written, 4);
    uidx++;
    memcpy(used + 2, &uidx, 2);
}

static void vdev_irq(Machine *m, VDev *d) {
    d->int_status |= 1;
    gic_set_irq(m, IRQ_VIRTIO + d->idx, 1);
}

/* نسخ من/إلى أجزاء السلسلة القابلة للقراءة أو الكتابة */
static uint32_t chain_read(Machine *m, VDesc *v, int n, uint8_t *dst, uint32_t max) {
    uint32_t got = 0;
    for (int i = 0; i < n && got < max; i++) {
        if (v[i].wr) continue;
        uint32_t l = v[i].len; if (l > max - got) l = max - got;
        uint8_t *p = gpa(m, v[i].addr, l);
        if (!p) break;
        memcpy(dst + got, p, l); got += l;
    }
    return got;
}
static uint32_t chain_write(Machine *m, VDesc *v, int n, const uint8_t *src, uint32_t len) {
    uint32_t put = 0;
    for (int i = 0; i < n && put < len; i++) {
        if (!v[i].wr) continue;
        uint32_t l = v[i].len; if (l > len - put) l = len - put;
        uint8_t *p = gpa(m, v[i].addr, l);
        if (!p) break;
        jit_dma_write(m, v[i].addr, l);
        memcpy(p, src + put, l); put += l;
    }
    return put;
}

/* =========================== القرص =========================== */
typedef struct { int fd; uint64_t capacity; } Blk;

static void blk_cfg(VDev *d, uint8_t *cfg) {
    Blk *b = d->priv;
    memcpy(cfg, &b->capacity, 8);
    uint32_t seg_max = 62; memcpy(cfg + 12, &seg_max, 4);
    uint32_t bs = 512; memcpy(cfg + 20, &bs, 4);
}

static void blk_notify(Machine *m, VDev *d, int qi) {
    Blk *b = d->priv;
    VDesc v[64]; int n, head, did = 0;
    while ((head = vq_pop(m, d, qi, v, 64, &n)) >= 0) {
        uint8_t st = 0; uint32_t written = 0;
        uint8_t *hdr = n ? gpa(m, v[0].addr, 16) : NULL;
        if (!hdr || n < 2) st = 1;
        else {
            uint32_t type; uint64_t sector;
            memcpy(&type, hdr, 4); memcpy(&sector, hdr + 8, 8);
            uint64_t pos = sector * 512;
            for (int k = 1; k < n - 1; k++) {
                uint8_t *p = gpa(m, v[k].addr, v[k].len);
                if (!p) { st = 1; break; }
                if (type == 0) {
                    jit_dma_write(m, v[k].addr, v[k].len);
                    ssize_t r = pread(b->fd, p, v[k].len, pos);
                    if (r < 0) st = 1;
                    else if ((uint32_t)r < v[k].len) memset(p + r, 0, v[k].len - r);
                    written += v[k].len;
                } else if (type == 1) {
                    if (pwrite(b->fd, p, v[k].len, pos) < 0) st = 1;
                } else if (type == 8) {
                    const char *id = "gtv-emu-disk";
                    uint32_t l = v[k].len < 20 ? v[k].len : 20;
                    jit_dma_write(m, v[k].addr, l);
                    memset(p, 0, l);
                    memcpy(p, id, strlen(id) < l ? strlen(id) : l);
                    written += l;
                }
                pos += v[k].len;
            }
            if (type == 4) fsync(b->fd);
            else if (type != 0 && type != 1 && type != 8) st = 2;
        }
        uint8_t *sp = n ? gpa(m, v[n - 1].addr, 1) : NULL;
        if (sp) *sp = st;
        vq_push(m, d, qi, head, written + 1);
        did = 1;
    }
    if (did) vdev_irq(m, d);
}

/* ========================= الرسوميات ========================= */
#define GPU_W 1280
#define GPU_H 720
#define GPU_MAXRES 64

typedef struct {
    uint32_t id, fmt, w, h;
    uint32_t *pix;                 /* نسخة المضيف من البكسلات */
    int nent; uint64_t *ent_addr; uint32_t *ent_len;
} GpuRes;

typedef struct {
    GpuRes res[GPU_MAXRES];
    uint32_t scanout_res;          /* المورد المعروض على الشاشة 0 */
    uint32_t sx, sy, sw, sh;
    uint64_t flushes;
    int announced;
} Gpu;

static Gpu *g_gpu;

static GpuRes *gpu_find(Gpu *g, uint32_t id) {
    for (int i = 0; i < GPU_MAXRES; i++) if (g->res[i].id == id && id) return &g->res[i];
    return NULL;
}

static void gpu_res_free(GpuRes *r) {
    free(r->pix); free(r->ent_addr); free(r->ent_len);
    memset(r, 0, sizeof(*r));
}

static void gpu_cfg(VDev *d, uint8_t *cfg) {
    (void)d;
    uint32_t num_scanouts = 1;
    memcpy(cfg + 8, &num_scanouts, 4);
}

/* نسخ len بايت من ذاكرة الضيف المرفقة بالمورد بدءًا من الإزاحة off */
static void gpu_backing_copy(Machine *m, GpuRes *r, uint64_t off, uint8_t *dst, uint32_t len) {
    uint64_t base = 0;
    for (int i = 0; i < r->nent && len; i++) {
        uint64_t end = base + r->ent_len[i];
        if (off < end) {
            uint64_t in = off - base;
            uint32_t l = r->ent_len[i] - in; if (l > len) l = len;
            uint8_t *p = gpa(m, r->ent_addr[i] + in, l);
            if (p) memcpy(dst, p, l); else memset(dst, 0, l);
            dst += l; off += l; len -= l;
        }
        base = end;
    }
}

static void gpu_cmd(Machine *m, Gpu *g, const uint8_t *req, uint32_t rl, uint8_t *resp, uint32_t *resp_len) {
    uint32_t type = 0, flags = 0; uint64_t fence = 0;
    if (rl >= 24) { memcpy(&type, req, 4); memcpy(&flags, req + 4, 4); memcpy(&fence, req + 8, 8); }
    const uint8_t *a = req + 24;
    uint32_t rtype = 0x1100; /* OK_NODATA */
    memset(resp, 0, 24);
    *resp_len = 24;
#define U32(o) ({ uint32_t _v; memcpy(&_v, a + (o), 4); _v; })
    switch (type) {
    case 0x0100: { /* GET_DISPLAY_INFO */
        rtype = 0x1101;
        uint32_t pm[6] = { 0, 0, GPU_W, GPU_H, 1, 0 };
        memset(resp + 24, 0, 16 * 24);
        memcpy(resp + 24, pm, 24);
        *resp_len = 24 + 16 * 24;
        break;
    }
    case 0x0101: { /* RESOURCE_CREATE_2D */
        uint32_t id = U32(0), fmt = U32(4), w = U32(8), h = U32(12);
        GpuRes *r = NULL;
        for (int i = 0; i < GPU_MAXRES; i++) if (!g->res[i].id) { r = &g->res[i]; break; }
        if (!r || !id || w > 8192 || h > 8192 || gpu_find(g, id)) { rtype = 0x1200; break; }
        r->id = id; r->fmt = fmt; r->w = w; r->h = h;
        r->pix = calloc((size_t)w * h, 4);
        break;
    }
    case 0x0102: { GpuRes *r = gpu_find(g, U32(0)); if (r) { if (g->scanout_res == r->id) g->scanout_res = 0; gpu_res_free(r); } else rtype = 0x1203; break; }
    case 0x0103: { /* SET_SCANOUT */
        g->sx = U32(0); g->sy = U32(4); g->sw = U32(8); g->sh = U32(12);
        uint32_t sid = U32(16), rid = U32(20);
        if (sid != 0) { rtype = 0x1202; break; }
        g->scanout_res = rid;
        if (rid && !g->announced && m->verbose >= 0) {
            fprintf(stderr, "\n[gtv] gpu: عرض المورد %u بأبعاد %ux%u\n", rid, g->sw, g->sh);
            g->announced = 1;
        }
        break;
    }
    case 0x0104: g->flushes++; break; /* RESOURCE_FLUSH */
    case 0x0105: { /* TRANSFER_TO_HOST_2D */
        uint32_t x = U32(0), y = U32(4), w = U32(8), h = U32(12);
        uint64_t off; memcpy(&off, a + 16, 8);
        GpuRes *r = gpu_find(g, U32(24));
        if (!r) { rtype = 0x1203; break; }
        if (x + w > r->w || y + h > r->h) { rtype = 0x1205; break; }
        for (uint32_t row = 0; row < h; row++) {
            uint64_t src = off + (uint64_t)row * r->w * 4;
            gpu_backing_copy(m, r, src, (uint8_t *)(r->pix + (size_t)(y + row) * r->w + x), w * 4);
        }
        break;
    }
    case 0x0106: { /* RESOURCE_ATTACH_BACKING */
        GpuRes *r = gpu_find(g, U32(0));
        uint32_t n = U32(4);
        if (!r || n > 16384 || rl < 32 + 16ULL * n) { rtype = 0x1200; break; }
        free(r->ent_addr); free(r->ent_len);
        r->nent = n;
        r->ent_addr = calloc(n, 8); r->ent_len = calloc(n, 4);
        for (uint32_t i = 0; i < n; i++) {
            memcpy(&r->ent_addr[i], a + 8 + 16 * i, 8);
            memcpy(&r->ent_len[i], a + 16 + 16 * i, 4);
        }
        break;
    }
    case 0x0107: { GpuRes *r = gpu_find(g, U32(0)); if (r) { free(r->ent_addr); free(r->ent_len); r->ent_addr = NULL; r->ent_len = NULL; r->nent = 0; } break; }
    case 0x0300: case 0x0301: break; /* UPDATE_CURSOR / MOVE_CURSOR */
    default: rtype = 0x1200; break; /* ERR_UNSPEC */
    }
#undef U32
    memcpy(resp, &rtype, 4);
    if (flags & 1) { uint32_t f = 1; memcpy(resp + 4, &f, 4); memcpy(resp + 8, &fence, 8); }
}

static void gpu_notify(Machine *m, VDev *d, int qi) {
    Gpu *g = d->priv;
    VDesc v[64]; int n, head, did = 0;
    static uint8_t req[16 * 16384 + 64];
    uint8_t resp[24 + 16 * 24];
    while ((head = vq_pop(m, d, qi, v, 64, &n)) >= 0) {
        uint32_t rl = chain_read(m, v, n, req, sizeof req), wl = 0;
        gpu_cmd(m, g, req, rl, resp, &wl);
        uint32_t put = chain_write(m, v, n, resp, wl);
        vq_push(m, d, qi, head, put);
        did = 1;
    }
    if (did) vdev_irq(m, d);
}

/* كتابة PNG دون مكتبات خارجية: ضغط deflate بكتل مخزّنة غير مضغوطة */
static uint32_t crc_tab[256];
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) {
        uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static size_t zstore(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0; uint32_t a = 1, b = 0;
    out[o++] = 0x78; out[o++] = 0x01;
    for (size_t i = 0; i < n || i == 0; ) {
        size_t l = n - i > 65535 ? 65535 : n - i;
        out[o++] = (i + l == n);
        out[o++] = l; out[o++] = l >> 8; out[o++] = ~l; out[o++] = (~l) >> 8;
        memcpy(out + o, in + i, l); o += l; i += l;
        if (!l) break;
    }
    for (size_t i = 0; i < n; i++) { a = (a + in[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    out[o++] = ad >> 24; out[o++] = ad >> 16; out[o++] = ad >> 8; out[o++] = ad;
    return o;
}
static void png_chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t be[4] = { n >> 24, n >> 16, n >> 8, n };
    fwrite(be, 1, 4, f);
    uint32_t c = crc32_upd(0, (const uint8_t *)t, 4);
    if (n) c = crc32_upd(c, d, n);
    fwrite(t, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uint8_t cb[4] = { c >> 24, c >> 16, c >> 8, c };
    fwrite(cb, 1, 4, f);
}

int gpu_screenshot(Machine *m, const char *path) {
    (void)m;
    Gpu *g = g_gpu;
    GpuRes *r = g ? gpu_find(g, g->scanout_res) : NULL;
    if (!r) return -1;
    uint32_t w = r->w, h = r->h;
    size_t rawlen = (size_t)h * (1 + 3 * w);
    uint8_t *raw = malloc(rawlen);
    uint64_t lit = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + y * (1 + 3 * w);
        row[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = r->pix[(size_t)y * w + x];
            uint8_t b0 = p, b1 = p >> 8, b2 = p >> 16, b3 = p >> 24, R, G, B;
            switch (r->fmt) {
            case 1: case 2: R = b2; G = b1; B = b0; break;    /* B8G8R8A8 / B8G8R8X8 */
            case 3: case 4: R = b1; G = b2; B = b3; break;    /* A8R8G8B8 / X8R8G8B8 */
            case 67: case 134: R = b0; G = b1; B = b2; break; /* R8G8B8X8 / R8G8B8A8 */
            default: R = b3; G = b2; B = b1; break;           /* X8B8G8R8 / A8B8G8R8 */
            }
            row[1 + 3 * x] = R; row[2 + 3 * x] = G; row[3 + 3 * x] = B;
            if (R | G | B) lit++;
        }
    }
    uint8_t *z = malloc(rawlen + rawlen / 65535 * 5 + 64);
    size_t zl = zstore(raw, rawlen, z);
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ih, 13);
    png_chunk(f, "IDAT", z, zl);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    fprintf(stderr, "[gtv] gpu: لقطة شاشة %ux%u في %s (بكسلات غير سوداء: %llu، تحديثات: %llu)\n",
            w, h, path, (unsigned long long)lit, (unsigned long long)g->flushes);
    return 0;
}

/* ========================== الإدخال ========================== */
typedef struct {
    int kind;                 /* 0 لوحة مفاتيح */
    uint8_t select, subsel;
    uint16_t pending[512][3]; /* أحداث تنتظر مخازن الضيف: type, code, value */
    int ph, pt;
    const char *keys;         /* نص يُكتب آليًا عند الطلب */
    int keys_armed;
    uint64_t next_ns;
} Input;

static Input *g_kbd;

static void input_cfg(VDev *d, uint8_t *cfg) {
    Input *in = d->priv;
    memset(cfg, 0, 136);
    cfg[0] = in->select; cfg[1] = in->subsel;
    uint8_t *u = cfg + 8, size = 0;
    switch (in->select) {
    case 1: { const char *s = "gtv-emu keyboard"; size = strlen(s); memcpy(u, s, size); break; } /* ID_NAME */
    case 2: { const char *s = "gtv0"; size = strlen(s); memcpy(u, s, size); break; }           /* ID_SERIAL */
    case 3: { uint16_t ids[4] = { 6, 0x1af4, 0x0001, 1 }; memcpy(u, ids, 8); size = 8; break; } /* BUS_VIRTUAL */
    case 0x11: /* EV_BITS */
        if (in->subsel == 1) { for (int i = 1; i < 256; i++) u[i / 8] |= 1 << (i % 8); size = 32; }
        else if (in->subsel == 0x14) { size = 0; } /* EV_REP يدار في النواة */
        break;
    }
    cfg[2] = size;
}

static void input_cfg_write(VDev *d, uint64_t off, int size, uint64_t v) {
    Input *in = d->priv;
    (void)size;
    if (off == 0) in->select = v;
    else if (off == 1) in->subsel = v;
}

static void input_queue(Input *in, uint16_t t, uint16_t c, uint32_t v) {
    int nx = (in->ph + 1) % 512;
    if (nx == in->pt) return;
    in->pending[in->ph][0] = t; in->pending[in->ph][1] = c; in->pending[in->ph][2] = v;
    in->ph = nx;
}

static void input_flush(Machine *m, VDev *d) {
    Input *in = d->priv;
    VDesc v[4]; int n, head, did = 0;
    while (in->pt != in->ph) {
        if ((head = vq_pop(m, d, 0, v, 4, &n)) < 0) break;
        uint8_t ev[8];
        uint16_t t = in->pending[in->pt][0], c = in->pending[in->pt][1];
        uint32_t val = in->pending[in->pt][2];
        memcpy(ev, &t, 2); memcpy(ev + 2, &c, 2); memcpy(ev + 4, &val, 4);
        uint32_t put = chain_write(m, v, n, ev, 8);
        vq_push(m, d, 0, head, put);
        in->pt = (in->pt + 1) % 512;
        did = 1;
    }
    if (did) vdev_irq(m, d);
}

static void input_notify(Machine *m, VDev *d, int qi) {
    if (qi == 1) { /* statusq: حالة المصابيح، نستهلكها فقط */
        VDesc v[4]; int n, head, did = 0;
        while ((head = vq_pop(m, d, 1, v, 4, &n)) >= 0) { vq_push(m, d, 1, head, 0); did = 1; }
        if (did) vdev_irq(m, d);
        return;
    }
    input_flush(m, d);
}

/* تحويل حرف ASCII إلى رمز مفتاح لينكس (مع Shift عند الحاجة) */
static int ascii_key(int ch, int *shift) {
    static const char *row1 = "1234567890-=", *row1s = "!@#$%^&*()_+";
    static const char *q = "qwertyuiop[]", *qs = "QWERTYUIOP{}";
    static const char *a = "asdfghjkl;'`", *as = "ASDFGHJKL:\"~";
    static const char *z = "zxcvbnm,./", *zs = "ZXCVBNM<>?";
    const char *p;
    *shift = 0;
    if (ch == '\n' || ch == '\r') return 28;
    if (ch == ' ') return 57;
    if (ch == '\t') return 15;
    if (ch == '\\') return 43;
    if (ch == '|') { *shift = 1; return 43; }
    if ((p = strchr(row1, ch))) return 2 + (p - row1);
    if ((p = strchr(row1s, ch))) { *shift = 1; return 2 + (p - row1s); }
    if ((p = strchr(q, ch))) return 16 + (p - q);
    if ((p = strchr(qs, ch))) { *shift = 1; return 16 + (p - qs); }
    if ((p = strchr(a, ch))) { int i = p - a; return i < 11 ? 30 + i : 41; }
    if ((p = strchr(as, ch))) { int i = p - as; *shift = 1; return i < 11 ? 30 + i : 41; }
    if ((p = strchr(z, ch))) return 44 + (p - z);
    if ((p = strchr(zs, ch))) { *shift = 1; return 44 + (p - zs); }
    return -1;
}

void input_set_keys(Machine *m, const char *keys) { (void)m; if (g_kbd) g_kbd->keys = keys; }
void input_arm_keys(Machine *m) { (void)m; if (g_kbd && g_kbd->keys) g_kbd->keys_armed = 1; }

static void input_poll(Machine *m, VDev *d) {
    Input *in = d->priv;
    if (in->keys_armed && in->keys && *in->keys && host_ns() >= in->next_ns) {
        int shift, k = ascii_key((unsigned char)*in->keys++, &shift);
        if (k > 0) {
            if (shift) input_queue(in, 1, 42, 1);
            input_queue(in, 1, k, 1); input_queue(in, 0, 0, 0);
            input_queue(in, 1, k, 0);
            if (shift) input_queue(in, 1, 42, 0);
            input_queue(in, 0, 0, 0);
        }
        in->next_ns = host_ns() + 20000000ULL;
    }
    if (in->pt != in->ph) input_flush(m, d);
}

/* =========================== الشبكة ===========================
 * واجهة خلفية مبسطة داخل المحاكي: بوابة افتراضية 10.0.2.2 ترد على ARP وping.
 * الوصول إلى الإنترنت عبر المضيف (NAT) لم يُنفَّذ بعد. */
#define NET_HDR 12
typedef struct {
    uint8_t mac[6];
    uint8_t rx[16][1600]; uint32_t rxlen[16]; int rh, rt;
    uint64_t tx_frames, rx_frames;
} Net;

static const uint8_t GW_MAC[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const uint8_t GW_IP[4] = { 10, 0, 2, 2 };

static void net_cfg(VDev *d, uint8_t *cfg) {
    Net *nt = d->priv;
    memcpy(cfg, nt->mac, 6);
    uint16_t st = 1; memcpy(cfg + 6, &st, 2); /* LINK_UP */
}

static uint16_t ip_csum(const uint8_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return ~s;
}

static void net_rx_enqueue(Net *nt, const uint8_t *f, uint32_t n) {
    int nx = (nt->rh + 1) % 16;
    if (nx == nt->rt || n > 1514) return;
    memset(nt->rx[nt->rh], 0, NET_HDR);
    memcpy(nt->rx[nt->rh] + NET_HDR, f, n);
    nt->rxlen[nt->rh] = n + NET_HDR;
    nt->rh = nx;
}

static void net_gateway(Net *nt, const uint8_t *f, uint32_t n) {
    if (n < 14) return;
    uint16_t et = (f[12] << 8) | f[13];
    uint8_t o[1514];
    if (et == 0x0806 && n >= 42 && f[21] == 1 && !memcmp(f + 38, GW_IP, 4)) { /* طلب ARP */
        memcpy(o, f + 6, 6); memcpy(o + 6, GW_MAC, 6); o[12] = 8; o[13] = 6;
        memcpy(o + 14, f + 14, 6); o[20] = 0; o[21] = 2;
        memcpy(o + 22, GW_MAC, 6); memcpy(o + 28, GW_IP, 4);
        memcpy(o + 32, f + 22, 6); memcpy(o + 38, f + 28, 4);
        net_rx_enqueue(nt, o, 42);
    } else if (et == 0x0800 && n >= 34 && f[23] == 1 && !memcmp(f + 30, GW_IP, 4)) { /* ICMP */
        int ihl = (f[14] & 15) * 4;
        uint32_t tot = (f[16] << 8) | f[17];
        if (tot + 14 > n || f[14 + ihl] != 8) return;
        memcpy(o, f, 14 + tot);
        memcpy(o, f + 6, 6); memcpy(o + 6, GW_MAC, 6);
        memcpy(o + 26, f + 30, 4); memcpy(o + 30, f + 26, 4);
        o[24] = o[25] = 0; uint16_t c = ip_csum(o + 14, ihl); o[24] = c >> 8; o[25] = c;
        uint8_t *ic = o + 14 + ihl; ic[0] = 0; ic[2] = ic[3] = 0;
        c = ip_csum(ic, tot - ihl); ic[2] = c >> 8; ic[3] = c;
        net_rx_enqueue(nt, o, 14 + tot);
    }
}

static void net_rx_deliver(Machine *m, VDev *d) {
    Net *nt = d->priv;
    VDesc v[8]; int n, head, did = 0;
    while (nt->rt != nt->rh) {
        if ((head = vq_pop(m, d, 0, v, 8, &n)) < 0) break;
        uint8_t *pk = nt->rx[nt->rt];
        uint16_t one = 1; memcpy(pk + 10, &one, 2); /* num_buffers */
        uint32_t put = chain_write(m, v, n, pk, nt->rxlen[nt->rt]);
        vq_push(m, d, 0, head, put);
        nt->rt = (nt->rt + 1) % 16; nt->rx_frames++;
        did = 1;
    }
    if (did) vdev_irq(m, d);
}

static void net_notify(Machine *m, VDev *d, int qi) {
    Net *nt = d->priv;
    if (qi == 1) {
        VDesc v[8]; int n, head, did = 0;
        static uint8_t buf[65536 + NET_HDR];
        while ((head = vq_pop(m, d, 1, v, 8, &n)) >= 0) {
            uint32_t l = chain_read(m, v, n, buf, sizeof buf);
            if (l > NET_HDR) net_gateway(nt, buf + NET_HDR, l - NET_HDR);
            nt->tx_frames++;
            vq_push(m, d, 1, head, 0);
            did = 1;
        }
        if (did) vdev_irq(m, d);
    }
    net_rx_deliver(m, d);
}

/* ======================= الناقل العام ======================= */
static VDev *vdev_new(int idx, int type, int nq) {
    VDev *d = calloc(1, sizeof(VDev));
    d->idx = idx; d->type = type; d->nq = nq;
    d->features = VF_VERSION_1;
    return d;
}

void virtio_create_all(Machine *m, const char *disk, const char *keys) {
    VDev *d;
    /* 0: قرص */
    if (disk) {
        Blk *b = calloc(1, sizeof(Blk));
        b->fd = open(disk, O_RDWR);
        if (b->fd < 0) b->fd = open(disk, O_RDONLY);
        if (b->fd >= 0) b->capacity = lseek(b->fd, 0, SEEK_END) / 512;
        d = vdev_new(0, b->fd >= 0 ? VT_BLK : VT_NONE, 1);
        d->features |= (1u << 2) | (1u << 6) | (1u << 9);
        d->priv = b; d->notify = blk_notify; d->cfg_read = blk_cfg;
        m->virtio[0] = d;
    }
    /* 1: شبكة */
    Net *nt = calloc(1, sizeof(Net));
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    memcpy(nt->mac, mac, 6);
    d = vdev_new(1, VT_NET, 2);
    d->features |= (1u << 5) | (1u << 16); /* MAC, STATUS */
    d->priv = nt; d->notify = net_notify; d->cfg_read = net_cfg;
    m->virtio[1] = d;
    /* 2: رسوميات */
    Gpu *g = calloc(1, sizeof(Gpu));
    g_gpu = g;
    d = vdev_new(2, VT_GPU, 2);
    d->priv = g; d->notify = gpu_notify; d->cfg_read = gpu_cfg;
    m->virtio[2] = d;
    /* 3: لوحة مفاتيح */
    Input *in = calloc(1, sizeof(Input));
    in->keys = keys;
    g_kbd = in;
    d = vdev_new(3, VT_INPUT, 2);
    d->priv = in; d->notify = input_notify; d->cfg_read = input_cfg; d->cfg_write = input_cfg_write;
    m->virtio[3] = d;
}

void virtio_poll(Machine *m) {
    for (int i = 0; i < VIRTIO_COUNT; i++) {
        VDev *d = m->virtio[i];
        if (!d || !(d->status & 4)) continue; /* DRIVER_OK */
        if (d->type == VT_INPUT) input_poll(m, d);
        else if (d->type == VT_NET) net_rx_deliver(m, d);
    }
}

int virtio_has_work(Machine *m) {
    (void)m;
    return g_kbd && g_kbd->keys_armed && g_kbd->keys && *g_kbd->keys;
}

uint64_t virtio_read(Machine *m, int idx, uint64_t off, int size) {
    VDev *d = m->virtio[idx];
    if (off == 0) return 0x74726976;
    if (off == 4) return 2;
    if (!d || d->type == VT_NONE) return 0;
    if (off >= 0x100) {
        uint8_t cfg[256] = {0};
        if (d->cfg_read) d->cfg_read(d, cfg);
        uint64_t v = 0, o = off - 0x100;
        if (o + size <= sizeof(cfg)) memcpy(&v, cfg + o, size);
        return v;
    }
    int qs = d->queue_sel < VQ_MAX ? d->queue_sel : 0;
    switch (off) {
    case 0x008: return d->type;
    case 0x00C: return 0x554D4551;
    case 0x010: return d->dev_feat_sel == 0 ? (uint32_t)d->features : d->dev_feat_sel == 1 ? (uint32_t)(d->features >> 32) : 0;
    case 0x034: return (int)d->queue_sel < d->nq ? 256 : 0;
    case 0x038: return d->q[qs].num;
    case 0x044: return d->q[qs].ready;
    case 0x060: return d->int_status;
    case 0x070: return d->status;
    case 0x0FC: return d->cfg_gen;
    case 0x0B0: case 0x0B4: case 0x0B8: case 0x0BC: return 0xFFFFFFFF; /* لا توجد مناطق ذاكرة مشتركة */
    }
    return 0;
}

void virtio_write(Machine *m, int idx, uint64_t off, int size, uint64_t v) {
    VDev *d = m->virtio[idx];
    if (!d || d->type == VT_NONE) return;
    if (off >= 0x100) { if (d->cfg_write) d->cfg_write(d, off - 0x100, size, v); return; }
    int qs = d->queue_sel < VQ_MAX ? d->queue_sel : 0;
    switch (off) {
    case 0x014: d->dev_feat_sel = v; return;
    case 0x020:
        if (d->drv_feat_sel == 0) d->drv_features = (d->drv_features & ~0xFFFFFFFFULL) | (uint32_t)v;
        else d->drv_features = (d->drv_features & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        return;
    case 0x024: d->drv_feat_sel = v; return;
    case 0x030: d->queue_sel = v; return;
    case 0x038: d->q[qs].num = v; return;
    case 0x044: d->q[qs].ready = v & 1; return;
    case 0x050: if ((int)v < d->nq && d->notify) d->notify(m, d, (int)v); return;
    case 0x064:
        d->int_status &= ~v;
        if (!d->int_status) gic_set_irq(m, IRQ_VIRTIO + d->idx, 0);
        return;
    case 0x070:
        d->status = v;
        if (v == 0) {
            memset(d->q, 0, sizeof(d->q)); d->int_status = 0;
            gic_set_irq(m, IRQ_VIRTIO + d->idx, 0);
        }
        return;
    case 0x080: d->q[qs].desc = (d->q[qs].desc & ~0xFFFFFFFFULL) | (uint32_t)v; return;
    case 0x084: d->q[qs].desc = (d->q[qs].desc & 0xFFFFFFFFULL) | ((uint64_t)v << 32); return;
    case 0x090: d->q[qs].avail = (d->q[qs].avail & ~0xFFFFFFFFULL) | (uint32_t)v; return;
    case 0x094: d->q[qs].avail = (d->q[qs].avail & 0xFFFFFFFFULL) | ((uint64_t)v << 32); return;
    case 0x0A0: d->q[qs].used = (d->q[qs].used & ~0xFFFFFFFFULL) | (uint32_t)v; return;
    case 0x0A4: d->q[qs].used = (d->q[qs].used & 0xFFFFFFFFULL) | ((uint64_t)v << 32); return;
    }
}
