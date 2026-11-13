/* gtv-emu — الأجهزة: GICv3 والمؤقت العام وPL011 وvirtio-mmio للأقراص. */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>

#define SR(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

/* ======================= GICv3 ======================= */
#define NIRQ 256

typedef struct {
    uint8_t enabled[NIRQ], pending[NIRQ], active[NIRQ], level[NIRQ], edge[NIRQ], group[NIRQ];
    uint8_t prio[NIRQ];
    uint32_t dctlr, rctlr, waker;
    uint64_t pmr, bpr1, igrpen1, ctlr;
    int ap_stack[NIRQ];
    int ap_depth;
} GIC;

void *gic_create(Machine *m) {
    GIC *g = calloc(1, sizeof(GIC));
    for (int i = 0; i < 16; i++) g->edge[i] = 1; /* SGI حوافّ */
    g->waker = 6;
    (void)m;
    return g;
}

static int gic_running_prio(GIC *g) {
    if (g->ap_depth == 0) return 0x100;
    return g->prio[g->ap_stack[g->ap_depth - 1]];
}

static int gic_best(GIC *g) {
    int best = 1023, bp = 0x100;
    if (!(g->dctlr & 2) || !(g->igrpen1 & 1)) return 1023;
    for (int i = 0; i < NIRQ; i++) {
        if (g->pending[i] && g->enabled[i] && !g->active[i] && g->prio[i] < bp) {
            bp = g->prio[i];
            best = i;
        }
    }
    if (best == 1023) return 1023;
    if (bp >= (int)(g->pmr & 0xFF)) return 1023;
    if (bp >= gic_running_prio(g)) return 1023;
    return best;
}

void gic_update(Machine *m) {
    GIC *g = m->gic;
    m->irq_line = gic_best(g) != 1023;
}

void gic_set_irq(Machine *m, int irq, int level) {
    GIC *g = m->gic;
    if (irq >= NIRQ) return;
    if (g->edge[irq]) {
        if (level && !g->level[irq]) g->pending[irq] = 1;
    } else {
        g->pending[irq] = level ? 1 : 0;
    }
    g->level[irq] = level;
    gic_update(m);
}

static uint64_t bits_read(uint8_t *arr, int base) {
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) if (base + i < NIRQ && arr[base + i]) v |= 1u << i;
    return v;
}
static void bits_set(uint8_t *arr, int base, uint32_t v, int val) {
    for (int i = 0; i < 32; i++) if ((v >> i) & 1 && base + i < NIRQ) arr[base + i] = val;
}

static uint64_t gic_common_read(GIC *g, uint32_t off, int base_irq) {
    /* السجلات المشتركة بين الموزّع وإطار SGI */
    if (off >= 0x80 && off < 0x100) { int i = (off - 0x80) / 4 * 32 + base_irq; return bits_read(g->group, i); }
    if (off >= 0x100 && off < 0x180) { int i = (off - 0x100) / 4 * 32 + base_irq; return bits_read(g->enabled, i); }
    if (off >= 0x180 && off < 0x200) { int i = (off - 0x180) / 4 * 32 + base_irq; return bits_read(g->enabled, i); }
    if (off >= 0x200 && off < 0x280) { int i = (off - 0x200) / 4 * 32 + base_irq; return bits_read(g->pending, i); }
    if (off >= 0x280 && off < 0x300) { int i = (off - 0x280) / 4 * 32 + base_irq; return bits_read(g->pending, i); }
    if (off >= 0x300 && off < 0x380) { int i = (off - 0x300) / 4 * 32 + base_irq; return bits_read(g->active, i); }
    if (off >= 0x380 && off < 0x400) { int i = (off - 0x380) / 4 * 32 + base_irq; return bits_read(g->active, i); }
    if (off >= 0x400 && off < 0x800) {
        int i = off - 0x400 + base_irq; uint32_t v = 0;
        for (int k = 0; k < 4; k++) if (i + k < NIRQ) v |= (uint32_t)g->prio[i + k] << (8 * k);
        return v;
    }
    if (off >= 0xC00 && off < 0xD00) {
        int i = (off - 0xC00) / 4 * 16 + base_irq; uint32_t v = 0;
        for (int k = 0; k < 16; k++) if (i + k < NIRQ && g->edge[i + k]) v |= 2u << (2 * k);
        return v;
    }
    return 0;
}

static void gic_common_write(Machine *m, GIC *g, uint32_t off, int size, uint64_t v, int base_irq) {
    if (off >= 0x80 && off < 0x100) { int i = (off - 0x80) / 4 * 32 + base_irq; for (int k = 0; k < 32; k++) if (i + k < NIRQ) g->group[i + k] = (v >> k) & 1; }
    else if (off >= 0x100 && off < 0x180) { bits_set(g->enabled, (off - 0x100) / 4 * 32 + base_irq, v, 1); }
    else if (off >= 0x180 && off < 0x200) { bits_set(g->enabled, (off - 0x180) / 4 * 32 + base_irq, v, 0); }
    else if (off >= 0x200 && off < 0x280) { bits_set(g->pending, (off - 0x200) / 4 * 32 + base_irq, v, 1); }
    else if (off >= 0x280 && off < 0x300) { bits_set(g->pending, (off - 0x280) / 4 * 32 + base_irq, v, 0); }
    else if (off >= 0x300 && off < 0x380) { bits_set(g->active, (off - 0x300) / 4 * 32 + base_irq, v, 1); }
    else if (off >= 0x380 && off < 0x400) { bits_set(g->active, (off - 0x380) / 4 * 32 + base_irq, v, 0); }
    else if (off >= 0x400 && off < 0x800) {
        int i = off - 0x400 + base_irq;
        for (int k = 0; k < size; k++) if (i + k < NIRQ) g->prio[i + k] = (v >> (8 * k)) & 0xF8;
    } else if (off >= 0xC00 && off < 0xD00) {
        int i = (off - 0xC00) / 4 * 16 + base_irq;
        for (int k = 0; k < 16; k++) if (i + k >= 16 && i + k < NIRQ) g->edge[i + k] = (v >> (2 * k + 1)) & 1;
    }
    gic_update(m);
}

uint64_t gic_mmio_read(Machine *m, uint64_t pa, int size) {
    GIC *g = m->gic;
    (void)size;
    if (pa >= GIC_DIST_BASE && pa < GIC_DIST_BASE + GIC_DIST_SIZE) {
        uint32_t off = pa - GIC_DIST_BASE;
        switch (off) {
        case 0x0: return g->dctlr | (1u << 4); /* ARE_NS */
        case 0x4: return ((NIRQ / 32) - 1) | (9u << 19); /* TYPER */
        case 0x8: return 0x43B;
        case 0xC: return 0;
        case 0xFFE8: return 0x30;
        }
        if (off >= 0x6000 && off < 0x8000) return 0; /* IROUTER */
        if (off < 0x80) return 0;
        return gic_common_read(g, off, 0);
    }
    uint32_t off = pa - GIC_REDIST_BASE;
    if (off < 0x10000) {
        switch (off) {
        case 0x0: return g->rctlr;
        case 0x4: return 0x43B;
        case 0x8: return (size == 8) ? (1ULL << 4) : (1u << 4); /* TYPER: Last */
        case 0xC: return 0;
        case 0x14: return g->waker;
        case 0xFFE8: return 0x30;
        }
        return 0;
    }
    off -= 0x10000;
    return gic_common_read(g, off, 0);
}

void gic_mmio_write(Machine *m, uint64_t pa, int size, uint64_t v) {
    GIC *g = m->gic;
    if (pa >= GIC_DIST_BASE && pa < GIC_DIST_BASE + GIC_DIST_SIZE) {
        uint32_t off = pa - GIC_DIST_BASE;
        if (off == 0) { g->dctlr = v & 0x13; gic_update(m); return; }
        if (off >= 0x6000 && off < 0x8000) return;
        if (off < 0x80) return;
        if (off < 0x100 && off >= 0x80) { /* IGROUPR للـSPI فقط من الموزع */ }
        if (off >= 0x100 && off < 0x800 && (off & 0x7F) < 4 && ((off - 0x100) % 0x80) == 0 && 0) return;
        gic_common_write(m, g, off, size, v, 0);
        return;
    }
    uint32_t off = pa - GIC_REDIST_BASE;
    if (off < 0x10000) {
        if (off == 0x14) { g->waker = (v & 2) ? 6 : 0; }
        else if (off == 0) g->rctlr = v;
        return;
    }
    off -= 0x10000;
    gic_common_write(m, g, off, size, v, 0);
}

int gic_sysreg_read(Machine *m, uint32_t enc, uint64_t *v) {
    GIC *g = m->gic;
    switch (enc) {
    case SR(3,0,12,12,0): { /* ICC_IAR1_EL1 */
        int i = gic_best(g);
        if (i != 1023) {
            g->active[i] = 1;
            if (g->edge[i]) g->pending[i] = 0;
            g->ap_stack[g->ap_depth++] = i;
        }
        gic_update(m);
        *v = i;
        return 1;
    }
    case SR(3,0,12,12,2): *v = gic_best(g); return 1; /* HPPIR1 */
    case SR(3,0,12,11,3): *v = gic_running_prio(g) & 0xFF; if (g->ap_depth == 0) *v = 0xFF; return 1; /* RPR */
    case SR(3,0,4,6,0): *v = g->pmr; return 1;
    case SR(3,0,12,12,3): *v = g->bpr1; return 1;
    case SR(3,0,12,12,4): *v = (4u << 8) | g->ctlr; return 1; /* CTLR: PRIbits=5 */
    case SR(3,0,12,12,5): *v = 7; return 1; /* SRE */
    case SR(3,0,12,12,6): return *v = 0, 1; /* IGRPEN0 */
    case SR(3,0,12,12,7): *v = g->igrpen1; return 1;
    case SR(3,0,12,9,0): case SR(3,0,12,9,1): case SR(3,0,12,9,2): case SR(3,0,12,9,3): *v = 0; return 1; /* AP1R */
    case SR(3,0,12,8,4): case SR(3,0,12,8,5): case SR(3,0,12,8,6): case SR(3,0,12,8,7): *v = 0; return 1; /* AP0R */
    }
    return 0;
}

int gic_sysreg_write(Machine *m, uint32_t enc, uint64_t v) {
    GIC *g = m->gic;
    switch (enc) {
    case SR(3,0,12,12,1): { /* EOIR1 */
        int i = v & 0x3FF;
        if (i < NIRQ) {
            g->active[i] = 0;
            if (!g->edge[i]) g->pending[i] = g->level[i];
            for (int k = g->ap_depth - 1; k >= 0; k--)
                if (g->ap_stack[k] == i) {
                    memmove(&g->ap_stack[k], &g->ap_stack[k + 1], (g->ap_depth - k - 1) * sizeof(int));
                    g->ap_depth--;
                    break;
                }
        }
        gic_update(m);
        return 1;
    }
    case SR(3,0,12,11,1): { /* DIR */
        int i = v & 0x3FF;
        if (i < NIRQ) g->active[i] = 0;
        gic_update(m);
        return 1;
    }
    case SR(3,0,12,11,5): { /* SGI1R: إلى نفس المعالج */
        int intid = (v >> 24) & 0xF;
        if ((v & 1) || (v >> 40) & 1) { g->pending[intid] = 1; gic_update(m); }
        return 1;
    }
    case SR(3,0,4,6,0): g->pmr = v & 0xFF; gic_update(m); return 1;
    case SR(3,0,12,12,3): g->bpr1 = v & 7; return 1;
    case SR(3,0,12,12,4): g->ctlr = v & 3; return 1;
    case SR(3,0,12,12,5): return 1;
    case SR(3,0,12,12,6): return 1;
    case SR(3,0,12,12,7): g->igrpen1 = v & 1; gic_update(m); return 1;
    case SR(3,0,12,9,0): case SR(3,0,12,9,1): case SR(3,0,12,9,2): case SR(3,0,12,9,3): return 1;
    case SR(3,0,12,8,4): case SR(3,0,12,8,5): case SR(3,0,12,8,6): case SR(3,0,12,8,7): return 1;
    }
    return 0;
}

/* ======================= المؤقت العام ======================= */
void timer_update(Machine *m) {
    CPU *c = &m->cpu;
    uint64_t now = cpu_counter(m);
    int vfire = (now - c->cntvoff) >= c->cntv_cval;
    c->cntv_ctl = (c->cntv_ctl & 3) | (vfire ? 4 : 0);
    int vline = (c->cntv_ctl & 1) && !(c->cntv_ctl & 2) && vfire;
    int pfire = now >= c->cntp_cval;
    c->cntp_ctl = (c->cntp_ctl & 3) | (pfire ? 4 : 0);
    int pline = (c->cntp_ctl & 1) && !(c->cntp_ctl & 2) && pfire;
    GIC *g = m->gic;
    if (g->level[IRQ_VTIMER] != vline) gic_set_irq(m, IRQ_VTIMER, vline);
    if (g->level[IRQ_PTIMER] != pline) gic_set_irq(m, IRQ_PTIMER, pline);
}

uint64_t timer_next_deadline(Machine *m) {
    CPU *c = &m->cpu;
    uint64_t dl = ~0ULL;
    if ((c->cntv_ctl & 1) && !(c->cntv_ctl & 2)) dl = c->cntv_cval + c->cntvoff;
    if ((c->cntp_ctl & 1) && !(c->cntp_ctl & 2) && c->cntp_cval < dl) dl = c->cntp_cval;
    return dl;
}

/* ======================= PL011 ======================= */
typedef struct {
    uint8_t rx[256];
    int rx_head, rx_tail;
    uint32_t cr, lcr, ibrd, fbrd, ifls, imsc, ris_tx;
    int stdin_ok;
    const char *feed;
    uint64_t feed_ns;
} UART;

void *uart_create(Machine *m) {
    UART *u = calloc(1, sizeof(UART));
    u->cr = 0x300;
    u->ifls = 0x12;
    u->stdin_ok = isatty(0) || 1;
    int fl = fcntl(0, F_GETFL);
    if (fl >= 0) fcntl(0, F_SETFL, fl | O_NONBLOCK);
    (void)m;
    return u;
}

static int rx_count(UART *u) { return (u->rx_head - u->rx_tail) & 255; }

static void uart_irq(Machine *m) {
    UART *u = m->uart;
    uint32_t ris = (rx_count(u) ? (1u << 4) | (1u << 6) : 0) | (1u << 5);
    gic_set_irq(m, IRQ_UART, (ris & u->imsc) != 0);
}

void uart_feed(Machine *m, const char *s) {
    UART *u = m->uart;
    u->feed = s;
    u->feed_ns = 0;
}

int uart_has_input(Machine *m) {
    UART *u = m->uart;
    struct pollfd p = { 0, POLLIN, 0 };
    if (u->feed && *u->feed) return 1;
    return poll(&p, 1, 0) > 0;
}

void uart_poll(Machine *m) {
    UART *u = m->uart;
    int changed = 0;
    if (u->feed && *u->feed) {
        uint64_t now = host_ns();
        if (u->feed_ns == 0) u->feed_ns = now;
        /* سطر واحد كل فترة لمحاكاة الكتابة */
        while (*u->feed && rx_count(u) < 200 && now >= u->feed_ns) {
            char ch = *u->feed++;
            if (ch == '\x01') { u->feed_ns = now + 1500000000ULL; continue; } /* انتظار */
            u->rx[u->rx_head] = ch; u->rx_head = (u->rx_head + 1) & 255; changed = 1;
            if (ch == '\n' || ch == '\r') { u->feed_ns = now + 300000000ULL; break; }
        }
    } else {
        char buf[64];
        int space = 250 - rx_count(u);
        if (space > 64) space = 64;
        if (space > 0) {
            ssize_t n = read(0, buf, space);
            for (ssize_t i = 0; i < n; i++) {
                char ch = buf[i] == '\n' ? '\r' : buf[i];
                u->rx[u->rx_head] = ch; u->rx_head = (u->rx_head + 1) & 255; changed = 1;
            }
        }
    }
    if (changed) uart_irq(m);
}

uint64_t uart_read(Machine *m, uint64_t off, int size) {
    UART *u = m->uart;
    (void)size;
    switch (off) {
    case 0x00: {
        if (!rx_count(u)) return 0;
        uint8_t ch = u->rx[u->rx_tail];
        u->rx_tail = (u->rx_tail + 1) & 255;
        uart_irq(m);
        return ch;
    }
    case 0x04: return 0;
    case 0x18: return (rx_count(u) ? 0 : (1u << 4)) | (1u << 7); /* FR */
    case 0x24: return u->ibrd;
    case 0x28: return u->fbrd;
    case 0x2C: return u->lcr;
    case 0x30: return u->cr;
    case 0x34: return u->ifls;
    case 0x38: return u->imsc;
    case 0x3C: return (rx_count(u) ? 0x50 : 0) | 0x20;
    case 0x40: return ((rx_count(u) ? 0x50 : 0) | 0x20) & u->imsc;
    case 0x48: return 0;
    case 0xFE0: return 0x11;
    case 0xFE4: return 0x10;
    case 0xFE8: return 0x34;
    case 0xFEC: return 0x00;
    case 0xFF0: return 0x0D;
    case 0xFF4: return 0xF0;
    case 0xFF8: return 0x05;
    case 0xFFC: return 0xB1;
    }
    return 0;
}

void uart_write(Machine *m, uint64_t off, int size, uint64_t v) {
    UART *u = m->uart;
    (void)size;
    switch (off) {
    case 0x00: { char ch = v & 0xFF; if (write(1, &ch, 1) < 0) {} uart_irq(m); return; }
    case 0x24: u->ibrd = v; return;
    case 0x28: u->fbrd = v; return;
    case 0x2C: u->lcr = v; return;
    case 0x30: u->cr = v; return;
    case 0x34: u->ifls = v; return;
    case 0x38: u->imsc = v & 0x7FF; uart_irq(m); return;
    case 0x44: uart_irq(m); return;
    }
}

/* ======================= virtio-mmio (قرص) ======================= */
typedef struct {
    int idx;
    int fd;
    uint64_t capacity; /* بالقطاعات */
    uint32_t status, dev_feat_sel, drv_feat_sel, queue_sel, int_status;
    uint64_t drv_features;
    struct {
        uint32_t num, ready;
        uint64_t desc, avail, used;
        uint16_t last_avail;
    } q;
} VBlk;

void *virtio_blk_create(Machine *m, int idx, const char *path) {
    VBlk *b = calloc(1, sizeof(VBlk));
    b->idx = idx;
    b->fd = path ? open(path, O_RDWR) : -1;
    if (path && b->fd < 0) b->fd = open(path, O_RDONLY);
    if (b->fd >= 0) {
        off_t sz = lseek(b->fd, 0, SEEK_END);
        b->capacity = sz / 512;
    }
    (void)m;
    return b;
}

static uint8_t *gpa(Machine *m, uint64_t pa, uint64_t len) {
    if (pa < RAM_BASE || pa + len > RAM_BASE + m->ram_size) return NULL;
    return m->ram + (pa - RAM_BASE);
}

static void vblk_process(Machine *m, VBlk *b) {
    if (!b->q.ready || !b->q.num) return;
    uint8_t *avail = gpa(m, b->q.avail, 4 + 2 * b->q.num);
    uint8_t *used = gpa(m, b->q.used, 4 + 8 * b->q.num);
    uint8_t *desc = gpa(m, b->q.desc, 16 * b->q.num);
    if (!avail || !used || !desc) return;
    uint16_t aidx;
    memcpy(&aidx, avail + 2, 2);
    int did = 0;
    while (b->q.last_avail != aidx) {
        uint16_t head;
        memcpy(&head, avail + 4 + 2 * (b->q.last_avail % b->q.num), 2);
        b->q.last_avail++;
        /* اجمع السلسلة */
        uint64_t addrs[64]; uint32_t lens[64]; int wr[64]; int n = 0;
        uint16_t i = head;
        for (;;) {
            uint8_t *d = desc + 16 * i;
            uint64_t a; uint32_t l; uint16_t fl, nx;
            memcpy(&a, d, 8); memcpy(&l, d + 8, 4); memcpy(&fl, d + 12, 2); memcpy(&nx, d + 14, 2);
            if (n < 64) { addrs[n] = a; lens[n] = l; wr[n] = fl & 2; n++; }
            if (!(fl & 1)) break;
            i = nx;
        }
        uint32_t written = 0;
        uint8_t st = 0;
        uint8_t *hdr = gpa(m, addrs[0], 16);
        if (!hdr || n < 2) st = 1;
        else {
            uint32_t type; uint64_t sector;
            memcpy(&type, hdr, 4); memcpy(&sector, hdr + 8, 8);
            uint64_t pos = sector * 512;
            for (int k = 1; k < n - 1; k++) {
                uint8_t *p = gpa(m, addrs[k], lens[k]);
                if (!p) { st = 1; break; }
                if (type == 0) {
                    ssize_t r = pread(b->fd, p, lens[k], pos);
                    if (r < 0) st = 1;
                    else if ((uint32_t)r < lens[k]) memset(p + r, 0, lens[k] - r);
                    written += lens[k];
                } else if (type == 1) {
                    if (pwrite(b->fd, p, lens[k], pos) < 0) st = 1;
                } else if (type == 8) {
                    const char *id = "gtv-emu-disk";
                    uint32_t l = lens[k] < 20 ? lens[k] : 20;
                    memset(p, 0, l);
                    memcpy(p, id, strlen(id) < l ? strlen(id) : l);
                    written += l;
                }
                pos += lens[k];
            }
            if (type == 4) { fsync(b->fd); }
            else if (type != 0 && type != 1 && type != 8) st = 2;
        }
        uint8_t *sp = gpa(m, addrs[n - 1], 1);
        if (sp) *sp = st;
        written += 1;
        uint16_t uidx;
        memcpy(&uidx, used + 2, 2);
        uint8_t *ue = used + 4 + 8 * (uidx % b->q.num);
        uint32_t h32 = head;
        memcpy(ue, &h32, 4);
        memcpy(ue + 4, &written, 4);
        uidx++;
        memcpy(used + 2, &uidx, 2);
        did = 1;
    }
    if (did) {
        b->int_status |= 1;
        gic_set_irq(m, IRQ_VIRTIO + b->idx, 1);
    }
}

uint64_t virtio_read(Machine *m, int idx, uint64_t off, int size) {
    VBlk *b = m->virtio[idx];
    if (!b || b->fd < 0) {
        if (off == 0) return 0x74726976;
        if (off == 4) return 2;
        return 0; /* DeviceID 0 = لا جهاز */
    }
    if (off >= 0x100) {
        uint8_t cfg[64] = {0};
        memcpy(cfg, &b->capacity, 8);
        uint32_t seg_max = 62; memcpy(cfg + 12, &seg_max, 4);
        uint32_t blk = 512; memcpy(cfg + 20, &blk, 4);
        uint64_t v = 0;
        uint64_t o = off - 0x100;
        if (o + size <= sizeof(cfg)) memcpy(&v, cfg + o, size);
        return v;
    }
    switch (off) {
    case 0x000: return 0x74726976;
    case 0x004: return 2;
    case 0x008: return 2;
    case 0x00C: return 0x554D4551;
    case 0x010:
        if (b->dev_feat_sel == 0) return (1u << 2) | (1u << 6) | (1u << 9); /* SEG_MAX, BLK_SIZE, FLUSH */
        if (b->dev_feat_sel == 1) return 1; /* VERSION_1 */
        return 0;
    case 0x034: return 256;
    case 0x038: return b->q.num;
    case 0x044: return b->q.ready;
    case 0x060: return b->int_status;
    case 0x070: return b->status;
    case 0x0FC: return 0;
    }
    return 0;
}

void virtio_write(Machine *m, int idx, uint64_t off, int size, uint64_t v) {
    VBlk *b = m->virtio[idx];
    (void)size;
    if (!b || b->fd < 0) return;
    switch (off) {
    case 0x014: b->dev_feat_sel = v; return;
    case 0x020:
        if (b->drv_feat_sel == 0) b->drv_features = (b->drv_features & ~0xFFFFFFFFULL) | (uint32_t)v;
        else b->drv_features = (b->drv_features & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        return;
    case 0x024: b->drv_feat_sel = v; return;
    case 0x030: b->queue_sel = v; return;
    case 0x038: if (b->queue_sel == 0) b->q.num = v; return;
    case 0x044: if (b->queue_sel == 0) b->q.ready = v & 1; return;
    case 0x050: vblk_process(m, b); return;
    case 0x064:
        b->int_status &= ~v;
        if (!b->int_status) gic_set_irq(m, IRQ_VIRTIO + b->idx, 0);
        return;
    case 0x070:
        b->status = v;
        if (v == 0) { memset(&b->q, 0, sizeof(b->q)); b->int_status = 0; gic_set_irq(m, IRQ_VIRTIO + b->idx, 0); }
        return;
    case 0x080: b->q.desc = (b->q.desc & ~0xFFFFFFFFULL) | (uint32_t)v; return;
    case 0x084: b->q.desc = (b->q.desc & 0xFFFFFFFFULL) | ((uint64_t)v << 32); return;
    case 0x090: b->q.avail = (b->q.avail & ~0xFFFFFFFFULL) | (uint32_t)v; return;
    case 0x094: b->q.avail = (b->q.avail & 0xFFFFFFFFULL) | ((uint64_t)v << 32); return;
    case 0x0A0: b->q.used = (b->q.used & ~0xFFFFFFFFULL) | (uint32_t)v; return;
    case 0x0A4: b->q.used = (b->q.used & 0xFFFFFFFFULL) | ((uint64_t)v << 32); return;
    }
}
