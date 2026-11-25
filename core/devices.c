/* gtv-emu — الأجهزة: GICv3 والمؤقت العام وPL011 (أجهزة virtio في virtio.c). */
#include "emu.h"
#include "host.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>

GtvHost *gtv_host; /* NULL في واجهة سطر الأوامر */

#define SR(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

/* ======================= GICv3 (متعدد المعالجات) ======================= */
#define NIRQ 256

typedef struct { uint8_t enabled, pending, active, level, edge, group, prio; } IrqSt;

typedef struct {
    IrqSt l[32];                  /* SGI وPPI خاصة بكل معالج */
    uint32_t rctlr, waker;
    uint64_t pmr, bpr1, igrpen1, ctlr;
    int ap_stack[NIRQ];
    int ap_depth;
} GicCpu;

typedef struct {
    IrqSt spi[NIRQ];              /* يُستخدم من 32 فما فوق */
    uint64_t irouter[NIRQ];
    uint32_t dctlr;
    GicCpu pc[MAX_CPUS];
    IrqSt dummy;
} GIC;

static IrqSt *ist(GIC *g, int cpu, int i) {
    if (i < 32) {
        if (cpu < 0) { memset(&g->dummy, 0, sizeof g->dummy); return &g->dummy; }
        return &g->pc[cpu].l[i];
    }
    return &g->spi[i];
}

static int cur_id(Machine *m) { return m->cur ? m->cur->id : 0; }

void *gic_create(Machine *m) {
    GIC *g = calloc(1, sizeof(GIC));
    for (int c = 0; c < MAX_CPUS; c++) {
        for (int i = 0; i < 16; i++) g->pc[c].l[i].edge = 1; /* SGI حوافّ */
        g->pc[c].waker = 6;
    }
    (void)m;
    return g;
}

static int spi_target(Machine *m, GIC *g, int i) {
    uint64_t r = g->irouter[i];
    if (r & (1ULL << 31)) return 0;
    int t = r & 0xFF;
    return t < m->ncpu ? t : 0;
}

static int gic_running_prio(GIC *g, int cpu) {
    GicCpu *p = &g->pc[cpu];
    if (p->ap_depth == 0) return 0x100;
    return ist(g, cpu, p->ap_stack[p->ap_depth - 1])->prio;
}

static int gic_best(Machine *m, GIC *g, int cpu) {
    GicCpu *p = &g->pc[cpu];
    int best = 1023, bp = 0x100;
    if (!(g->dctlr & 2) || !(p->igrpen1 & 1)) return 1023;
    for (int i = 0; i < NIRQ; i++) {
        IrqSt *s = ist(g, cpu, i);
        if (s->pending && s->enabled && !s->active && s->prio < bp) {
            if (i >= 32 && spi_target(m, g, i) != cpu) continue;
            bp = s->prio;
            best = i;
        }
    }
    if (best == 1023) return 1023;
    if (bp >= (int)(p->pmr & 0xFF)) return 1023;
    if (bp >= gic_running_prio(g, cpu)) return 1023;
    return best;
}

void gic_update(Machine *m) {
    GIC *g = m->gic;
    for (int c = 0; c < m->ncpu; c++) m->cpus[c].irq_line = gic_best(m, g, c) != 1023;
}

static void gic_set_irq_cpu(Machine *m, int cpu, int irq, int level) {
    GIC *g = m->gic;
    if (irq >= NIRQ) return;
    IrqSt *s = ist(g, cpu, irq);
    if (s->edge) {
        if (level && !s->level) s->pending = 1;
    } else {
        s->pending = level ? 1 : 0;
    }
    s->level = level;
    gic_update(m);
}

void gic_set_irq(Machine *m, int irq, int level) { gic_set_irq_cpu(m, 0, irq, level); }

static uint64_t bits_read(GIC *g, int cpu, int field, int base) {
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) if (base + i < NIRQ && ((uint8_t *)ist(g, cpu, base + i))[field]) v |= 1u << i;
    return v;
}
static void bits_set(GIC *g, int cpu, int field, int base, uint32_t v, int val) {
    for (int i = 0; i < 32; i++) if ((v >> i) & 1 && base + i < NIRQ) ((uint8_t *)ist(g, cpu, base + i))[field] = val;
}
#define F_EN offsetof(IrqSt, enabled)
#define F_PEND offsetof(IrqSt, pending)
#define F_ACT offsetof(IrqSt, active)
#define F_GRP offsetof(IrqSt, group)

/* السجلات المشتركة بين الموزّع (cpu = -1) وإطار SGI لكل معالج */
static uint64_t gic_common_read(GIC *g, int cpu, uint32_t off) {
    if (off >= 0x80 && off < 0x100) return bits_read(g, cpu, F_GRP, (off - 0x80) / 4 * 32);
    if (off >= 0x100 && off < 0x180) return bits_read(g, cpu, F_EN, (off - 0x100) / 4 * 32);
    if (off >= 0x180 && off < 0x200) return bits_read(g, cpu, F_EN, (off - 0x180) / 4 * 32);
    if (off >= 0x200 && off < 0x280) return bits_read(g, cpu, F_PEND, (off - 0x200) / 4 * 32);
    if (off >= 0x280 && off < 0x300) return bits_read(g, cpu, F_PEND, (off - 0x280) / 4 * 32);
    if (off >= 0x300 && off < 0x380) return bits_read(g, cpu, F_ACT, (off - 0x300) / 4 * 32);
    if (off >= 0x380 && off < 0x400) return bits_read(g, cpu, F_ACT, (off - 0x380) / 4 * 32);
    if (off >= 0x400 && off < 0x800) {
        int i = off - 0x400; uint32_t v = 0;
        for (int k = 0; k < 4; k++) if (i + k < NIRQ) v |= (uint32_t)ist(g, cpu, i + k)->prio << (8 * k);
        return v;
    }
    if (off >= 0xC00 && off < 0xD00) {
        int i = (off - 0xC00) / 4 * 16; uint32_t v = 0;
        for (int k = 0; k < 16; k++) if (i + k < NIRQ && ist(g, cpu, i + k)->edge) v |= 2u << (2 * k);
        return v;
    }
    return 0;
}

static void gic_common_write(Machine *m, GIC *g, int cpu, uint32_t off, int size, uint64_t v) {
    if (off >= 0x80 && off < 0x100) { int i = (off - 0x80) / 4 * 32; for (int k = 0; k < 32; k++) if (i + k < NIRQ) ist(g, cpu, i + k)->group = (v >> k) & 1; }
    else if (off >= 0x100 && off < 0x180) bits_set(g, cpu, F_EN, (off - 0x100) / 4 * 32, v, 1);
    else if (off >= 0x180 && off < 0x200) bits_set(g, cpu, F_EN, (off - 0x180) / 4 * 32, v, 0);
    else if (off >= 0x200 && off < 0x280) bits_set(g, cpu, F_PEND, (off - 0x200) / 4 * 32, v, 1);
    else if (off >= 0x280 && off < 0x300) bits_set(g, cpu, F_PEND, (off - 0x280) / 4 * 32, v, 0);
    else if (off >= 0x300 && off < 0x380) bits_set(g, cpu, F_ACT, (off - 0x300) / 4 * 32, v, 1);
    else if (off >= 0x380 && off < 0x400) bits_set(g, cpu, F_ACT, (off - 0x380) / 4 * 32, v, 0);
    else if (off >= 0x400 && off < 0x800) {
        int i = off - 0x400;
        for (int k = 0; k < size; k++) if (i + k < NIRQ) ist(g, cpu, i + k)->prio = (v >> (8 * k)) & 0xF8;
    } else if (off >= 0xC00 && off < 0xD00) {
        int i = (off - 0xC00) / 4 * 16;
        for (int k = 0; k < 16; k++) if (i + k >= 16 && i + k < NIRQ) ist(g, cpu, i + k)->edge = (v >> (2 * k + 1)) & 1;
    }
    gic_update(m);
}

uint64_t gic_mmio_read(Machine *m, uint64_t pa, int size) {
    GIC *g = m->gic;
    if (pa >= GIC_DIST_BASE && pa < GIC_DIST_BASE + GIC_DIST_SIZE) {
        uint32_t off = pa - GIC_DIST_BASE;
        switch (off) {
        case 0x0: return g->dctlr | (1u << 4); /* ARE_NS */
        case 0x4: return ((NIRQ / 32) - 1) | (9u << 19) | ((uint32_t)(m->ncpu - 1) << 5); /* TYPER */
        case 0x8: return 0x43B;
        case 0xC: return 0;
        case 0xFFE8: return 0x30;
        }
        if (off >= 0x6100 && off < 0x6100 + 8 * NIRQ) { /* IROUTER */
            int i = (off - 0x6000) / 8;
            uint64_t v = g->irouter[i];
            return (off & 4) ? v >> 32 : (size == 8 ? v : (uint32_t)v);
        }
        if (off < 0x80) return 0;
        return gic_common_read(g, -1, off);
    }
    uint32_t roff = pa - GIC_REDIST_BASE;
    int cpu = roff / 0x20000;
    uint32_t off = roff % 0x20000;
    if (cpu >= m->ncpu) return 0;
    GicCpu *p = &g->pc[cpu];
    if (off < 0x10000) {
        uint64_t typer = ((uint64_t)cpu << 32) | ((uint64_t)cpu << 8) | (cpu == m->ncpu - 1 ? (1u << 4) : 0);
        switch (off) {
        case 0x0: return p->rctlr;
        case 0x4: return 0x43B;
        case 0x8: return size == 8 ? typer : (uint32_t)typer;
        case 0xC: return typer >> 32;
        case 0x14: return p->waker;
        case 0xFFE8: return 0x30;
        }
        return 0;
    }
    return gic_common_read(g, cpu, off - 0x10000);
}

void gic_mmio_write(Machine *m, uint64_t pa, int size, uint64_t v) {
    GIC *g = m->gic;
    if (pa >= GIC_DIST_BASE && pa < GIC_DIST_BASE + GIC_DIST_SIZE) {
        uint32_t off = pa - GIC_DIST_BASE;
        if (off == 0) { g->dctlr = v & 0x13; gic_update(m); return; }
        if (off >= 0x6100 && off < 0x6100 + 8 * NIRQ) {
            int i = (off - 0x6000) / 8;
            if (size == 8) g->irouter[i] = v;
            else if (off & 4) g->irouter[i] = (g->irouter[i] & 0xFFFFFFFFULL) | (v << 32);
            else g->irouter[i] = (g->irouter[i] & ~0xFFFFFFFFULL) | (uint32_t)v;
            gic_update(m);
            return;
        }
        if (off < 0x80) return;
        gic_common_write(m, g, -1, off, size, v);
        return;
    }
    uint32_t roff = pa - GIC_REDIST_BASE;
    int cpu = roff / 0x20000;
    uint32_t off = roff % 0x20000;
    if (cpu >= m->ncpu) return;
    if (off < 0x10000) {
        if (off == 0x14) g->pc[cpu].waker = (v & 2) ? 6 : 0;
        else if (off == 0) g->pc[cpu].rctlr = v;
        return;
    }
    gic_common_write(m, g, cpu, off - 0x10000, size, v);
}

int gic_sysreg_read(Machine *m, uint32_t enc, uint64_t *v) {
    GIC *g = m->gic;
    int cpu = cur_id(m);
    GicCpu *p = &g->pc[cpu];
    switch (enc) {
    case SR(3,0,12,12,0): { /* ICC_IAR1_EL1 */
        int i = gic_best(m, g, cpu);
        if (i != 1023) {
            IrqSt *s = ist(g, cpu, i);
            s->active = 1;
            if (s->edge) s->pending = 0;
            p->ap_stack[p->ap_depth++] = i;
        }
        gic_update(m);
        *v = i;
        return 1;
    }
    case SR(3,0,12,12,2): *v = gic_best(m, g, cpu); return 1; /* HPPIR1 */
    case SR(3,0,12,11,3): *v = gic_running_prio(g, cpu) & 0xFF; if (p->ap_depth == 0) *v = 0xFF; return 1; /* RPR */
    case SR(3,0,4,6,0): *v = p->pmr; return 1;
    case SR(3,0,12,12,3): *v = p->bpr1; return 1;
    case SR(3,0,12,12,4): *v = (4u << 8) | p->ctlr; return 1; /* CTLR: PRIbits=5 */
    case SR(3,0,12,12,5): *v = 7; return 1; /* SRE */
    case SR(3,0,12,12,6): return *v = 0, 1; /* IGRPEN0 */
    case SR(3,0,12,12,7): *v = p->igrpen1; return 1;
    case SR(3,0,12,9,0): case SR(3,0,12,9,1): case SR(3,0,12,9,2): case SR(3,0,12,9,3): *v = 0; return 1; /* AP1R */
    case SR(3,0,12,8,4): case SR(3,0,12,8,5): case SR(3,0,12,8,6): case SR(3,0,12,8,7): *v = 0; return 1; /* AP0R */
    }
    return 0;
}

int gic_sysreg_write(Machine *m, uint32_t enc, uint64_t v) {
    GIC *g = m->gic;
    int cpu = cur_id(m);
    GicCpu *p = &g->pc[cpu];
    switch (enc) {
    case SR(3,0,12,12,1): { /* EOIR1 */
        int i = v & 0x3FF;
        if (i < NIRQ) {
            IrqSt *s = ist(g, cpu, i);
            s->active = 0;
            if (!s->edge) s->pending = s->level;
            for (int k = p->ap_depth - 1; k >= 0; k--)
                if (p->ap_stack[k] == i) {
                    memmove(&p->ap_stack[k], &p->ap_stack[k + 1], (p->ap_depth - k - 1) * sizeof(int));
                    p->ap_depth--;
                    break;
                }
        }
        gic_update(m);
        return 1;
    }
    case SR(3,0,12,11,1): { /* DIR */
        int i = v & 0x3FF;
        if (i < NIRQ) ist(g, cpu, i)->active = 0;
        gic_update(m);
        return 1;
    }
    case SR(3,0,12,11,5): { /* SGI1R: مقاطعة بين المعالجات */
        int intid = (v >> 24) & 0xF;
        int aff1 = (v >> 16) & 0xFF, rs = (v >> 44) & 0xF;
        for (int t = 0; t < m->ncpu; t++) {
            int hit;
            if ((v >> 40) & 1) hit = (t != cpu);              /* IRM: كل المعالجات عدا المرسل */
            else hit = aff1 == 0 && rs == 0 && ((v >> t) & 1); /* قائمة الأهداف */
            if (hit) g->pc[t].l[intid].pending = 1;
        }
        gic_update(m);
        return 1;
    }
    case SR(3,0,4,6,0): p->pmr = v & 0xFF; gic_update(m); return 1;
    case SR(3,0,12,12,3): p->bpr1 = v & 7; return 1;
    case SR(3,0,12,12,4): p->ctlr = v & 3; return 1;
    case SR(3,0,12,12,5): return 1;
    case SR(3,0,12,12,6): return 1;
    case SR(3,0,12,12,7): p->igrpen1 = v & 1; gic_update(m); return 1;
    case SR(3,0,12,9,0): case SR(3,0,12,9,1): case SR(3,0,12,9,2): case SR(3,0,12,9,3): return 1;
    case SR(3,0,12,8,4): case SR(3,0,12,8,5): case SR(3,0,12,8,6): case SR(3,0,12,8,7): return 1;
    }
    return 0;
}

/* ======================= المؤقت العام (لكل معالج) ======================= */
void timer_update(Machine *m) {
    GIC *g = m->gic;
    uint64_t now = cpu_counter(m);
    for (int i = 0; i < m->ncpu; i++) {
        CPU *c = &m->cpus[i];
        int vfire = (now - c->cntvoff) >= c->cntv_cval;
        c->cntv_ctl = (c->cntv_ctl & 3) | (vfire ? 4 : 0);
        int vline = (c->cntv_ctl & 1) && !(c->cntv_ctl & 2) && vfire;
        int pfire = now >= c->cntp_cval;
        c->cntp_ctl = (c->cntp_ctl & 3) | (pfire ? 4 : 0);
        int pline = (c->cntp_ctl & 1) && !(c->cntp_ctl & 2) && pfire;
        if (g->pc[i].l[IRQ_VTIMER].level != vline) gic_set_irq_cpu(m, i, IRQ_VTIMER, vline);
        if (g->pc[i].l[IRQ_PTIMER].level != pline) gic_set_irq_cpu(m, i, IRQ_PTIMER, pline);
    }
}

uint64_t timer_next_deadline(Machine *m) {
    uint64_t dl = ~0ULL;
    for (int i = 0; i < m->ncpu; i++) {
        CPU *c = &m->cpus[i];
        if (!c->powered) continue;
        if ((c->cntv_ctl & 1) && !(c->cntv_ctl & 2) && c->cntv_cval + c->cntvoff < dl) dl = c->cntv_cval + c->cntvoff;
        if ((c->cntp_ctl & 1) && !(c->cntp_ctl & 2) && c->cntp_cval < dl) dl = c->cntp_cval;
    }
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
    char last_tx;
    int prompt_seen;      /* شاهدنا "# " في المخرجات: الصدفة جاهزة للإدخال */
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
    if (u->feed && *u->feed) {
        int blocked = (*u->feed == '\x02' && !u->prompt_seen) || host_ns() < u->feed_ns;
        if (!blocked) return 1;
        return virtio_has_work(m);
    }
    if (gtv_host && gtv_host->console_in_ready) return gtv_host->console_in_ready(gtv_host->ctx) || virtio_has_work(m);
    return poll(&p, 1, 0) > 0;
}

void uart_poll(Machine *m) {
    UART *u = m->uart;
    virtio_poll(m);
    int changed = 0;
    if (u->feed && *u->feed) {
        uint64_t now = host_ns();
        if (u->feed_ns == 0) u->feed_ns = now;
        /* سطر واحد كل فترة لمحاكاة الكتابة */
        while (*u->feed && rx_count(u) < 200 && now >= u->feed_ns) {
            char ch = *u->feed++;
            if (ch == '\x02') { /* انتظر ظهور موجه الصدفة قبل المتابعة */
                if (!u->prompt_seen) { u->feed--; break; }
                u->prompt_seen = 0; u->feed_ns = now + 200000000ULL; break;
            }
            if (ch == '\x03') { input_arm_keys(m); continue; } /* ابدأ الكتابة عبر لوحة مفاتيح virtio */
            if (ch == '\x04') { input_arm_events(); continue; } /* ابدأ سيناريو الريموت واللمس */
            if (ch == '\x01') { u->feed_ns = now + 1500000000ULL; continue; } /* انتظار */
            u->rx[u->rx_head] = ch; u->rx_head = (u->rx_head + 1) & 255; changed = 1;
            if (ch == '\n' || ch == '\r') { u->feed_ns = now + 300000000ULL; break; }
        }
    } else {
        char buf[64];
        int space = 250 - rx_count(u);
        if (space > 64) space = 64;
        if (space > 0) {
            ssize_t n = gtv_host && gtv_host->console_in ? (ssize_t)gtv_host->console_in(gtv_host->ctx, buf, space)
                                                         : read(0, buf, space);
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
    case 0x00: { char ch = v & 0xFF;
        if (gtv_host && gtv_host->console_out) gtv_host->console_out(gtv_host->ctx, &ch, 1);
        else if (write(1, &ch, 1) < 0) {}
        UART *uu = m->uart; if ((uu->last_tx == '#' || uu->last_tx == '$') && ch == ' ') uu->prompt_seen = 1; uu->last_tx = ch;
        uart_irq(m); return; }
    case 0x24: u->ibrd = v; return;
    case 0x28: u->fbrd = v; return;
    case 0x2C: u->lcr = v; return;
    case 0x30: u->cr = v; return;
    case 0x34: u->ifls = v; return;
    case 0x38: u->imsc = v & 0x7FF; uart_irq(m); return;
    case 0x44: uart_irq(m); return;
    }
}

