/* gtv-emu — مفسّر تعليمات AArch64 (EL0/EL1) مع وحدة إدارة الذاكرة والاستثناءات. */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BIT(x, n) (((x) >> (n)) & 1)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1ULL << ((hi) - (lo) + 1)) - 1))
#define SEXT(v, bits) ((int64_t)((uint64_t)(v) << (64 - (bits))) >> (64 - (bits)))
#define MASK(n) ((n) >= 64 ? ~0ULL : ((1ULL << (n)) - 1))

#define SR(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

uint64_t host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

uint64_t cpu_counter(Machine *m) {
    return (host_ns() - m->start_ns) / 16; /* 62.5 MHz */
}

/* ---------------- السجلات ---------------- */
static inline uint64_t *spp(CPU *c) {
    return (c->el == 0 || c->spsel == 0) ? &c->sp_el0 : &c->sp_el1;
}
uint64_t xreg(CPU *c, int n) { return n == 31 ? 0 : c->x[n]; }
uint64_t xreg_sp(CPU *c, int n) { return n == 31 ? *spp(c) : c->x[n]; }
void set_xreg(CPU *c, int n, uint64_t v) { if (n != 31) c->x[n] = v; }
void set_xreg_sp(CPU *c, int n, uint64_t v) { if (n == 31) *spp(c) = v; else c->x[n] = v; }
#define X(n) xreg(c, n)
#define XSP(n) xreg_sp(c, n)

static inline uint64_t pstate_get(CPU *c) {
    return c->nzcv | c->daif | ((uint64_t)c->el << 2) | (uint64_t)c->spsel;
}

/* ---------------- الاستثناءات ---------------- */
enum { EXC_SYNC = 0, EXC_IRQ = 0x80, EXC_FIQ = 0x100, EXC_SERR = 0x180 };

static void take_exception(CPU *c, int type, uint64_t esr, uint64_t far, int set_far, uint64_t ret_pc) {
    uint64_t off;
    if (c->el == 0) off = 0x400;
    else off = c->spsel ? 0x200 : 0;
    c->spsr = pstate_get(c);
    c->elr = ret_pc;
    if (type == EXC_SYNC) {
        c->esr = esr;
        if (set_far) c->far = far;
    }
    c->el = 1;
    c->spsel = 1;
    c->daif = 0xF << 6;
    c->pc = c->vbar + off + type;
    c->excl_valid = 0;
    c->fetch_tag = 0;
    c->halted = 0;
}

static void sync_exception(CPU *c, uint64_t esr, uint64_t far, int set_far) {
    take_exception(c, EXC_SYNC, esr, far, set_far, c->cur_pc);
    longjmp(c->jb, 1);
}

void raise_undef(CPU *c) {
    if (c->m->verbose >= 2 || (!c->m->quiet_unimpl && c->el == 1))
        fprintf(stderr, "[gtv] تعليمة غير معرّفة %08x عند pc=%016llx EL%d\n", c->cur_insn,
                (unsigned long long)c->cur_pc, c->el);
    sync_exception(c, (0ULL << 26) | (1ULL << 25), 0, 0);
}

void fp_check(CPU *c) {
    int fpen = (c->cpacr >> 20) & 3;
    if (fpen == 3 || (fpen == 1 && c->el == 1)) return;
    sync_exception(c, (0x07ULL << 26) | (1ULL << 25), 0, 0);
}

/* ---------------- وحدة إدارة الذاكرة ---------------- */
void tlb_flush(CPU *c) {
    memset(c->tlb, 0, sizeof(c->tlb));
    c->fetch_tag = 0;
}

enum { ACC_R = 0, ACC_W = 1, ACC_X = 2 };

typedef struct {
    uint64_t pa;
    uint8_t perm;
    int fault;      /* 0 لا خطأ، وإلا رمز FSC */
} WalkRes;

static int pa_valid(uint64_t pa) { return pa < (1ULL << 48); }

static uint64_t phys_read64(Machine *m, uint64_t pa, int *ok) {
    if (pa >= RAM_BASE && pa + 8 <= RAM_BASE + m->ram_size) {
        uint64_t v;
        memcpy(&v, m->ram + (pa - RAM_BASE), 8);
        *ok = 1;
        return v;
    }
    *ok = 0;
    return 0;
}

static WalkRes walk(CPU *c, uint64_t va) {
    WalkRes r = {0, 0, 0};
    if (!(c->sctlr & 1)) {
        r.pa = va & MASK(48);
        r.perm = 0x3F;
        if (va >> 48) r.fault = 0x00; /* حجم عنوان */
        return r;
    }
    int top = BIT(va, 55);
    uint64_t tcr = c->tcr;
    int tsz = top ? BITS(tcr, 21, 16) : BITS(tcr, 5, 0);
    int epd = top ? BIT(tcr, 23) : BIT(tcr, 7);
    int tbi = top ? BIT(tcr, 38) : BIT(tcr, 37);
    int vabits = 64 - tsz;
    uint64_t chk = tbi ? (uint64_t)SEXT(va, 56) : va;
    /* التحقق من البتات العليا */
    uint64_t hi = (uint64_t)((int64_t)chk >> vabits);
    if (!((hi == 0 && !top) || (hi == ~0ULL && top)) || epd) {
        r.fault = 0x04; /* خطأ ترجمة مستوى 0 */
        return r;
    }
    uint64_t ttbr = top ? c->ttbr1 : c->ttbr0;
    uint64_t table = ttbr & 0x0000FFFFFFFFFFFEULL;
    int level = 4 - ((vabits - 12) + 8) / 9;
    if (level < 0) level = 0;
    int ap_table = 0, uxn_table = 0, pxn_table = 0;
    for (;;) {
        int shift = 12 + 9 * (3 - level);
        uint64_t idx;
        if (level == 4 - ((vabits - 12) + 8) / 9)
            idx = (va >> shift) & MASK(vabits - shift);
        else
            idx = (va >> shift) & 0x1FF;
        int ok;
        uint64_t d = phys_read64(c->m, (table & ~0xFFFULL) + idx * 8, &ok);
        if (!ok) { r.fault = 0x04 + level; return r; }
        if (!(d & 1)) { r.fault = 0x04 + level; return r; }
        if (level < 3 && (d & 2)) {
            ap_table |= BITS(d, 62, 61);
            uxn_table |= BIT(d, 60);
            pxn_table |= BIT(d, 59);
            table = d & 0x0000FFFFFFFFF000ULL;
            level++;
            continue;
        }
        if (level == 3 && !(d & 2)) { r.fault = 0x04 + level; return r; }
        if (level == 0) { r.fault = 0x04; return r; }
        uint64_t blk = 12 + 9 * (3 - level);
        uint64_t oa = (d & 0x0000FFFFFFFFF000ULL & ~MASK(blk)) | (va & MASK(blk));
        if (!BIT(d, 10)) { r.fault = 0x08 + level; return r; } /* علم الوصول */
        int ap = BITS(d, 7, 6);
        if (ap_table & 2) ap |= 2;
        if (ap_table & 1) ap &= ~1;
        int uxn = BIT(d, 54) | uxn_table;
        int pxn = BIT(d, 53) | pxn_table;
        uint8_t p = 0;
        switch (ap) {
        case 0: p = P_R1 | P_W1; break;
        case 1: p = P_R1 | P_W1 | P_R0 | P_W0; break;
        case 2: p = P_R1; break;
        case 3: p = P_R1 | P_R0; break;
        }
        if (!uxn && (p & P_R0)) p |= P_X0;
        if (!pxn && !(p & P_W0)) p |= P_X1;
        r.pa = oa;
        r.perm = p;
        return r;
    }
}

/* يترجم العنوان أو يرفع استثناء */
static TLBEntry *translate(CPU *c, uint64_t va, int acc) {
    uint64_t vpn = va >> 12;
    TLBEntry *e = &c->tlb[vpn & (TLB_SIZE - 1)];
    uint8_t need;
    if (c->el == 0) need = acc == ACC_R ? P_R0 : acc == ACC_W ? P_W0 : P_X0;
    else need = acc == ACC_R ? P_R1 : acc == ACC_W ? P_W1 : P_X1;
    if (e->tag == ((vpn << 1) | 1) && (e->perm & need)) return e;
    WalkRes w = walk(c, va);
    if (!w.fault && !(w.perm & need)) w.fault = 0x0C + 3; /* خطأ صلاحيات */
    if (w.fault) {
        uint64_t esr;
        int lower = c->el == 0;
        if (acc == ACC_X) esr = ((uint64_t)(lower ? 0x20 : 0x21) << 26);
        else esr = ((uint64_t)(lower ? 0x24 : 0x25) << 26) | (acc == ACC_W ? (1 << 6) : 0);
        esr |= (1ULL << 25) | w.fault;
        e->tag = 0;
        sync_exception(c, esr, va, 1);
    }
    e->tag = (vpn << 1) | 1;
    e->perm = w.perm;
    e->pa = w.pa & ~0xFFFULL;
    e->code = c->m->jit ? (uint8_t)jit_page_is_code(c->m, e->pa) : 0;
    if (e->pa >= RAM_BASE && e->pa < RAM_BASE + c->m->ram_size)
        e->host = c->m->ram + (e->pa - RAM_BASE);
    else
        e->host = NULL;
    return e;
}

static uint64_t mmio_read(Machine *m, uint64_t pa, int size) {
    if (pa >= GIC_DIST_BASE && pa < GIC_DIST_BASE + GIC_DIST_SIZE) return gic_mmio_read(m, pa, size);
    if (pa >= GIC_REDIST_BASE && pa < GIC_REDIST_BASE + GIC_REDIST_SIZE) return gic_mmio_read(m, pa, size);
    if (pa >= UART_BASE && pa < UART_BASE + UART_SIZE) return uart_read(m, pa - UART_BASE, size);
    if (pa >= VIRTIO_BASE && pa < VIRTIO_BASE + VIRTIO_SIZE * VIRTIO_COUNT) {
        int idx = (pa - VIRTIO_BASE) / VIRTIO_SIZE;
        return virtio_read(m, idx, (pa - VIRTIO_BASE) % VIRTIO_SIZE, size);
    }
    if (m->verbose) fprintf(stderr, "[gtv] قراءة MMIO غير معروفة pa=%llx\n", (unsigned long long)pa);
    return 0;
}

static void mmio_write(Machine *m, uint64_t pa, int size, uint64_t v) {
    if (pa >= GIC_DIST_BASE && pa < GIC_DIST_BASE + GIC_DIST_SIZE) { gic_mmio_write(m, pa, size, v); return; }
    if (pa >= GIC_REDIST_BASE && pa < GIC_REDIST_BASE + GIC_REDIST_SIZE) { gic_mmio_write(m, pa, size, v); return; }
    if (pa >= UART_BASE && pa < UART_BASE + UART_SIZE) { uart_write(m, pa - UART_BASE, size, v); return; }
    if (pa >= VIRTIO_BASE && pa < VIRTIO_BASE + VIRTIO_SIZE * VIRTIO_COUNT) {
        int idx = (pa - VIRTIO_BASE) / VIRTIO_SIZE;
        virtio_write(m, idx, (pa - VIRTIO_BASE) % VIRTIO_SIZE, size, v);
        return;
    }
    if (m->verbose) fprintf(stderr, "[gtv] كتابة MMIO غير معروفة pa=%llx\n", (unsigned long long)pa);
}

uint64_t mem_read(CPU *c, uint64_t va, int size) {
    uint64_t off = va & 0xFFF;
    if (off + size > 0x1000) {
        /* عبور حد الصفحة: ترجم الصفحتين أولًا */
        translate(c, va, ACC_R);
        translate(c, va + size - 1, ACC_R);
        uint64_t v = 0;
        for (int i = 0; i < size; i++) v |= mem_read(c, va + i, 1) << (8 * i);
        return v;
    }
    TLBEntry *e = translate(c, va, ACC_R);
    if (e->host) {
        uint8_t *p = e->host + off;
        switch (size) {
        case 1: return *p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        default: { uint64_t v; memcpy(&v, p, 8); return v; }
        }
    }
    return mmio_read(c->m, e->pa | off, size);
}

void mem_write(CPU *c, uint64_t va, int size, uint64_t val) {
    uint64_t off = va & 0xFFF;
    if (off + size > 0x1000) {
        translate(c, va, ACC_W);
        translate(c, va + size - 1, ACC_W);
        for (int i = 0; i < size; i++) mem_write(c, va + i, 1, val >> (8 * i));
        return;
    }
    TLBEntry *e = translate(c, va, ACC_W);
    if (e->host) {
        uint8_t *p = e->host + off;
        if (e->code) jit_code_write(c, e->pa);
        switch (size) {
        case 1: *p = val; return;
        case 2: { uint16_t v = val; memcpy(p, &v, 2); return; }
        case 4: { uint32_t v = val; memcpy(p, &v, 4); return; }
        default: memcpy(p, &val, 8); return;
        }
    }
    mmio_write(c->m, e->pa | off, size, val);
}

static uint64_t va_to_pa(CPU *c, uint64_t va, int acc) {
    TLBEntry *e = translate(c, va, acc);
    return e->pa | (va & 0xFFF);
}

static uint32_t fetch(CPU *c) {
    uint64_t pc = c->pc;
    if ((pc >> 12) != c->fetch_tag || !c->fetch_host) {
        if (pc & 3) sync_exception(c, (0x22ULL << 26) | (1ULL << 25), pc, 1);
        TLBEntry *e = translate(c, pc, ACC_X);
        if (!e->host) {
            fprintf(stderr, "[gtv] تنفيذ من خارج الذاكرة pc=%llx\n", (unsigned long long)pc);
            exit(3);
        }
        c->fetch_tag = pc >> 12;
        c->fetch_host = e->host;
    }
    uint32_t insn;
    memcpy(&insn, c->fetch_host + (pc & 0xFFF), 4);
    return insn;
}

/* ---------------- العمليات الحسابية ---------------- */
static uint64_t add_with_carry(uint64_t a, uint64_t b, int carry, int sf, uint32_t *nzcv) {
    uint64_t r;
    uint32_t n, z, cf, v;
    if (sf) {
        unsigned __int128 u = (unsigned __int128)a + b + carry;
        r = (uint64_t)u;
        cf = (u >> 64) != 0;
        v = (((a ^ r) & (b ^ r)) >> 63) & 1;
        n = r >> 63;
        z = r == 0;
    } else {
        a &= 0xFFFFFFFF; b &= 0xFFFFFFFF;
        uint64_t u = a + b + carry;
        r = u & 0xFFFFFFFF;
        cf = (u >> 32) & 1;
        v = (((a ^ r) & (b ^ r)) >> 31) & 1;
        n = (r >> 31) & 1;
        z = r == 0;
    }
    if (nzcv) *nzcv = (n << 31) | (z << 30) | (cf << 29) | (v << 28);
    return r;
}

static int cond_holds(CPU *c, int cond) {
    uint32_t f = c->nzcv;
    int N = BIT(f, 31), Z = BIT(f, 30), C = BIT(f, 29), V = BIT(f, 28);
    int r;
    switch (cond >> 1) {
    case 0: r = Z; break;
    case 1: r = C; break;
    case 2: r = N; break;
    case 3: r = V; break;
    case 4: r = C && !Z; break;
    case 5: r = N == V; break;
    case 6: r = (N == V) && !Z; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static uint64_t shift_reg(uint64_t v, int type, int amt, int sf) {
    int ds = sf ? 64 : 32;
    if (!sf) v &= 0xFFFFFFFF;
    amt &= ds - 1;
    if (amt == 0) return v;
    switch (type) {
    case 0: v = v << amt; break;
    case 1: v = v >> amt; break;
    case 2: v = sf ? (uint64_t)((int64_t)v >> amt) : (uint64_t)(uint32_t)((int32_t)v >> amt); break;
    case 3: v = sf ? (v >> amt) | (v << (64 - amt)) : ((v >> amt) | (v << (32 - amt))); break;
    }
    return sf ? v : v & 0xFFFFFFFF;
}

static uint64_t extend_reg(uint64_t v, int opt, int shift) {
    switch (opt) {
    case 0: v &= 0xFF; break;
    case 1: v &= 0xFFFF; break;
    case 2: v &= 0xFFFFFFFF; break;
    case 3: break;
    case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
    case 7: break;
    }
    return v << shift;
}

static int decode_bitmask(int sf, int N, int immr, int imms, uint64_t *out) {
    int comb = (N << 6) | (~imms & 0x3F);
    if (comb == 0) return 0;
    int len = 31 - __builtin_clz(comb);
    if (len < 1) return 0;
    int size = 1 << len;
    int levels = size - 1;
    int S = imms & levels, R = immr & levels;
    if (S == levels) return 0;
    uint64_t welem = MASK(S + 1);
    uint64_t emask = MASK(size);
    if (R) welem = ((welem >> R) | (welem << (size - R))) & emask;
    uint64_t res = 0;
    for (int i = 0; i < 64; i += size) res |= welem << i;
    if (!sf) {
        if (N) return 0;
        res &= 0xFFFFFFFF;
    }
    *out = res;
    return 1;
}

static inline void set_nz(CPU *c, uint64_t r, int sf) {
    uint32_t n = sf ? (r >> 63) : ((r >> 31) & 1);
    uint32_t z = sf ? (r == 0) : ((uint32_t)r == 0);
    c->nzcv = (n << 31) | (z << 30);
}

/* ---------------- مسجلات النظام ---------------- */
static uint64_t sysreg_read(CPU *c, uint32_t enc, int *ok) {
    Machine *m = c->m;
    *ok = 1;
    uint64_t v;
    if (gic_sysreg_read(m, enc, &v)) return v;
    switch (enc) {
    case SR(3,0,1,0,0): return c->sctlr;
    case SR(3,0,1,0,1): return c->actlr;
    case SR(3,0,1,0,2): return c->cpacr;
    case SR(3,0,2,0,0): return c->ttbr0;
    case SR(3,0,2,0,1): return c->ttbr1;
    case SR(3,0,2,0,2): return c->tcr;
    case SR(3,0,4,0,0): return c->spsr;
    case SR(3,0,4,0,1): return c->elr;
    case SR(3,0,4,1,0): return c->sp_el0;
    case SR(3,0,4,2,0): return c->spsel;
    case SR(3,0,4,2,2): return (uint64_t)c->el << 2;
    case SR(3,3,4,2,1): return c->daif;
    case SR(3,3,4,2,0): return c->nzcv;
    case SR(3,3,4,4,0): return c->fpcr;
    case SR(3,3,4,4,1): return c->fpsr;
    case SR(3,0,5,1,0): return c->afsr0;
    case SR(3,0,5,1,1): return c->afsr1;
    case SR(3,0,5,2,0): return c->esr;
    case SR(3,0,6,0,0): return c->far;
    case SR(3,0,7,4,0): return c->par;
    case SR(3,0,10,2,0): return c->mair;
    case SR(3,0,10,3,0): return c->amair;
    case SR(3,0,12,0,0): return c->vbar;
    case SR(3,0,12,0,1): return 0; /* RVBAR */
    case SR(3,0,13,0,1): return c->contextidr;
    case SR(3,0,13,0,4): return c->tpidr_el1;
    case SR(3,3,13,0,2): return c->tpidr_el0;
    case SR(3,3,13,0,3): return c->tpidrro_el0;
    case SR(3,3,14,0,0): return TIMER_FREQ;
    case SR(3,3,14,0,1): case SR(3,3,14,0,5): return cpu_counter(m);
    case SR(3,3,14,0,2): case SR(3,3,14,0,6): return cpu_counter(m) - c->cntvoff;
    case SR(3,3,14,2,0): return (uint32_t)(c->cntp_cval - cpu_counter(m));
    case SR(3,3,14,2,1): timer_update(m); return c->cntp_ctl;
    case SR(3,3,14,2,2): return c->cntp_cval;
    case SR(3,3,14,3,0): return (uint32_t)(c->cntv_cval - (cpu_counter(m) - c->cntvoff));
    case SR(3,3,14,3,1): timer_update(m); return c->cntv_ctl;
    case SR(3,3,14,3,2): return c->cntv_cval;
    case SR(3,0,14,1,0): return c->cntkctl;
    case SR(3,0,0,0,0): return 0x410FD083ULL; /* Cortex-A72 r0p3 */
    case SR(3,0,0,0,5): return 0x80000000ULL | (uint64_t)c->id; /* MPIDR: Aff0 = رقم المعالج */
    case SR(3,0,0,0,6): return 0;
    case SR(3,0,0,4,0): return 0x0000000001000011ULL; /* PFR0: EL0/EL1 AArch64، FP، SIMD، GICv3 */
    case SR(3,0,0,5,0): return 0x6; /* DFR0 */
    case SR(3,0,0,6,0): return 0; /* ISAR0 */
    case SR(3,0,0,6,1): return 0;
    case SR(3,0,0,7,0): return 0x000000000F000005ULL; /* MMFR0: PA 48 بت، 4K فقط */
    case SR(3,0,0,7,1): return 0;
    case SR(3,0,0,7,2): return 0;
    case SR(3,3,0,0,1): return 0x84448004ULL; /* CTR_EL0 */
    case SR(3,3,0,0,7): return 4;              /* DCZID: 64 بايت */
    case SR(3,1,0,0,1): return 0x0A200023ULL;  /* CLIDR */
    case SR(3,1,0,0,0): return 0x700FE01AULL;  /* CCSIDR */
    case SR(2,0,0,2,2): return c->mdscr;
    case SR(2,0,1,1,4): return 0x8; /* OSLSR */
    }
    if (m->verbose) fprintf(stderr, "[gtv] قراءة مسجل نظام غير معروف %d,%d,c%d,c%d,%d\n",
                            enc >> 14, (enc >> 11) & 7, (enc >> 7) & 15, (enc >> 3) & 15, enc & 7);
    return 0;
}

static void sysreg_write(CPU *c, uint32_t enc, uint64_t v) {
    Machine *m = c->m;
    if (gic_sysreg_write(m, enc, v)) return;
    switch (enc) {
    case SR(3,0,1,0,0): c->sctlr = v; tlb_flush(c); return;
    case SR(3,0,1,0,1): c->actlr = v; return;
    case SR(3,0,1,0,2): c->cpacr = v; return;
    case SR(3,0,2,0,0): c->ttbr0 = v; tlb_flush(c); return;
    case SR(3,0,2,0,1): c->ttbr1 = v; tlb_flush(c); return;
    case SR(3,0,2,0,2): c->tcr = v; tlb_flush(c); return;
    case SR(3,0,4,0,0): c->spsr = v; return;
    case SR(3,0,4,0,1): c->elr = v; return;
    case SR(3,0,4,1,0): c->sp_el0 = v; return;
    case SR(3,0,4,2,0): c->spsel = v & 1; return;
    case SR(3,3,4,2,1): c->daif = v & 0x3C0; return;
    case SR(3,3,4,2,0): c->nzcv = v & 0xF0000000; return;
    case SR(3,3,4,4,0): c->fpcr = v; return;
    case SR(3,3,4,4,1): c->fpsr = v; return;
    case SR(3,0,5,1,0): c->afsr0 = v; return;
    case SR(3,0,5,1,1): c->afsr1 = v; return;
    case SR(3,0,5,2,0): c->esr = v; return;
    case SR(3,0,6,0,0): c->far = v; return;
    case SR(3,0,7,4,0): c->par = v; return;
    case SR(3,0,10,2,0): c->mair = v; return;
    case SR(3,0,10,3,0): c->amair = v; return;
    case SR(3,0,12,0,0): c->vbar = v; return;
    case SR(3,0,13,0,1): c->contextidr = v; return;
    case SR(3,0,13,0,4): c->tpidr_el1 = v; return;
    case SR(3,3,13,0,2): c->tpidr_el0 = v; return;
    case SR(3,3,13,0,3): c->tpidrro_el0 = v; return;
    case SR(3,3,14,2,0): c->cntp_cval = cpu_counter(m) + (int64_t)(int32_t)v; timer_update(m); return;
    case SR(3,3,14,2,1): c->cntp_ctl = (c->cntp_ctl & 4) | (v & 3); timer_update(m); return;
    case SR(3,3,14,2,2): c->cntp_cval = v; timer_update(m); return;
    case SR(3,3,14,3,0): c->cntv_cval = cpu_counter(m) - c->cntvoff + (int64_t)(int32_t)v; timer_update(m); return;
    case SR(3,3,14,3,1): c->cntv_ctl = (c->cntv_ctl & 4) | (v & 3); timer_update(m); return;
    case SR(3,3,14,3,2): c->cntv_cval = v; timer_update(m); return;
    case SR(3,0,14,1,0): c->cntkctl = v; return;
    case SR(2,0,0,2,2): c->mdscr = v; return;
    }
    if (m->verbose) fprintf(stderr, "[gtv] كتابة مسجل نظام غير معروف %d,%d,c%d,c%d,%d = %llx\n",
                            enc >> 14, (enc >> 11) & 7, (enc >> 7) & 15, (enc >> 3) & 15, enc & 7,
                            (unsigned long long)v);
}

/* ---------------- PSCI عبر HVC/SMC ---------------- */
static void do_psci(CPU *c) {
    uint32_t fn = (uint32_t)c->x[0];
    int64_t ret = -1;
    switch (fn) {
    case 0x84000000: ret = 0x10000; break;          /* PSCI_VERSION 1.0 */
    case 0x84000006: ret = 2; break;                /* MIGRATE_INFO_TYPE */
    case 0x8400000A: {                              /* PSCI_FEATURES */
        uint32_t q = (uint32_t)c->x[1];
        ret = (q == 0x84000000 || q == 0x84000008 || q == 0x84000009 || q == 0x8400000A ||
               q == 0xC4000003 || q == 0x84000003 || q == 0xC4000004 || q == 0x84000004 || q == 0x84000002) ? 0 : -1;
        break;
    }
    case 0x84000008:                                /* SYSTEM_OFF */
        fprintf(stderr, "\n[gtv] النظام طلب الإيقاف (PSCI SYSTEM_OFF)\n");
        c->stop = 1; c->exit_code = 0; break;
    case 0x84000009:                                /* SYSTEM_RESET */
        fprintf(stderr, "\n[gtv] النظام طلب إعادة التشغيل (PSCI SYSTEM_RESET)\n");
        c->stop = 1; c->exit_code = 0; break;
    case 0xC4000003: case 0x84000003: {             /* CPU_ON */
        Machine *m = c->m;
        uint64_t t = c->x[1] & 0xFF;
        if (t >= (uint64_t)m->ncpu || (c->x[1] & 0xFFFFFF00ULL)) { ret = -2; break; } /* INVALID_PARAMS */
        CPU *o = &m->cpus[t];
        if (o->powered) { ret = -4; break; }                                            /* ALREADY_ON */
        uint64_t entry = c->x[2], ctx = c->x[3];
        int id = o->id;
        cpu_reset(o, m);
        o->id = id;
        o->pc = entry;
        o->x[0] = ctx;
        o->cntvoff = c->cntvoff;
        o->powered = 1;
        if (m->verbose) fprintf(stderr, "[gtv] PSCI: تشغيل المعالج %d عند %llx\n", id, (unsigned long long)entry);
        ret = 0;
        break;
    }
    case 0xC4000004: case 0x84000004: {             /* AFFINITY_INFO */
        uint64_t t = c->x[1] & 0xFF;
        ret = t < (uint64_t)c->m->ncpu ? (c->m->cpus[t].powered ? 0 : 1) : -2;
        break;
    }
    case 0x84000002:                                /* CPU_OFF */
        c->powered = 0; c->halted = 1; ret = 0;
        break;
    default: ret = -1; break;
    }
    c->x[0] = (uint64_t)ret;
}

/* ---------------- تعليمات النظام ---------------- */
static void exec_system(CPU *c, uint32_t insn) {
    int L = BIT(insn, 21);
    int op0 = BITS(insn, 20, 19);
    int op1 = BITS(insn, 18, 16);
    int crn = BITS(insn, 15, 12);
    int crm = BITS(insn, 11, 8);
    int op2 = BITS(insn, 7, 5);
    int rt = insn & 31;

    if (op0 == 0) {
        if (crn == 2 && !L) { /* تلميحات */
            int h = (crm << 3) | op2;
            if (h == 3) { /* WFI */
                if (c->el == 0) { /* مسموح عادة */ }
                c->halted = 1;
            }
            return;
        }
        if (crn == 3 && !L) { /* حواجز */
            if (op2 == 2) c->excl_valid = 0; /* CLREX */
            return;
        }
        if (crn == 4 && !L) { /* MSR (قيمة فورية) */
            if (c->el == 0 && !(op1 == 3 && (op2 == 6 || op2 == 7))) raise_undef(c);
            if (op1 == 0 && op2 == 5) { c->spsel = crm & 1; return; }
            if (op1 == 3 && op2 == 6) { c->daif |= (crm & 0xF) << 6; return; }
            if (op1 == 3 && op2 == 7) { c->daif &= ~((crm & 0xF) << 6); return; }
            return; /* PAN/UAO/SSBS وغيرها: تجاهل */
        }
        raise_undef(c);
    }
    if (op0 == 1) { /* SYS / SYSL */
        if (L) raise_undef(c);
        if (crn == 8) { /* TLBI */
            if (c->el == 0) raise_undef(c);
            /* نبطل ذاكرة الترجمة في كل المعالجات (تغطي صيغ IS المذاعة) */
            for (int k = 0; k < c->m->ncpu; k++) tlb_flush(&c->m->cpus[k]);
            return;
        }
        if (crn == 7) {
            if (crm == 4 && op1 == 3 && op2 == 1) { /* DC ZVA */
                uint64_t a = X(rt) & ~63ULL;
                for (int i = 0; i < 64; i += 8) mem_write(c, a + i, 8, 0);
                return;
            }
            if (crm == 8 && c->el == 1) { /* AT */
                uint64_t va = X(rt);
                WalkRes w = walk(c, va);
                int acc_w = op2 & 1;
                int el0 = (op2 >> 1) & 1;
                uint8_t need = el0 ? (acc_w ? P_W0 : P_R0) : (acc_w ? P_W1 : P_R1);
                if (!w.fault && !(w.perm & need)) w.fault = 0x0F;
                if (w.fault) c->par = 1 | ((uint64_t)w.fault << 1);
                else c->par = (w.pa & 0x0000FFFFFFFFF000ULL) | (0xFFULL << 56);
                return;
            }
            return; /* صيانة الذاكرة المؤقتة: لا شيء */
        }
        return;
    }
    /* MRS / MSR (مسجل) */
    uint32_t enc = SR(op0, op1, crn, crm, op2);
    if (c->el == 0 && op1 != 3) raise_undef(c);
    if (L) {
        int ok;
        uint64_t v = sysreg_read(c, enc, &ok);
        set_xreg(c, rt, v);
    } else {
        sysreg_write(c, enc, X(rt));
    }
}

static void do_eret(CPU *c) {
    uint64_t spsr = c->spsr;
    c->nzcv = spsr & 0xF0000000;
    c->daif = spsr & 0x3C0;
    int m = spsr & 0xF;
    c->el = (m >> 2) & 1;
    c->spsel = m & 1;
    if (c->el == 0) c->spsel = 0;
    c->pc = c->elr;
    c->excl_valid = 0;
    c->fetch_tag = 0;
}

/* ---------------- التحميل والتخزين ---------------- */
static uint64_t ld_ext(CPU *c, uint64_t addr, int size, int sign, int to64) {
    uint64_t v = mem_read(c, addr, size);
    if (sign) {
        v = (uint64_t)SEXT(v, size * 8);
        if (!to64) v &= 0xFFFFFFFF;
    }
    return v;
}

static void exec_ldst_reg(CPU *c, uint32_t insn, uint64_t addr, int wb_mode, uint64_t wb_addr) {
    /* wb_mode: 0 بدون، 1 اكتب العنوان المحدث */
    int size = BITS(insn, 31, 30);
    int V = BIT(insn, 26);
    int opc = BITS(insn, 23, 22);
    int rt = insn & 31;
    int rn = BITS(insn, 9, 5);
    if (V) {
        fp_check(c);
        int sz = size | ((opc & 2) << 1);
        int bytes = 1 << sz;
        if (sz > 4) raise_undef(c);
        if (opc & 1) { /* تحميل */
            v128 val = {0, 0};
            if (bytes == 16) {
                val.lo = mem_read(c, addr, 8);
                val.hi = mem_read(c, addr + 8, 8);
            } else {
                val.lo = mem_read(c, addr, bytes);
            }
            c->v[rt] = val;
        } else {
            if (bytes == 16) {
                mem_write(c, addr, 8, c->v[rt].lo);
                mem_write(c, addr + 8, 8, c->v[rt].hi);
            } else {
                mem_write(c, addr, bytes, c->v[rt].lo);
            }
        }
    } else {
        int bytes = 1 << size;
        if (opc == 0) {
            mem_write(c, addr, bytes, X(rt));
        } else if (opc == 1) {
            uint64_t v = mem_read(c, addr, bytes);
            if (wb_mode && rn == rt && rn != 31) { /* سلوك غير متوقع؛ نتبع التحميل */ }
            if (wb_mode) set_xreg_sp(c, rn, wb_addr);
            set_xreg(c, rt, v);
            return;
        } else {
            if (size == 3) { /* PRFM */
                if (opc == 2) return;
                raise_undef(c);
            }
            int to64 = opc == 2;
            if (size == 2 && opc == 3) raise_undef(c);
            uint64_t v = ld_ext(c, addr, bytes, 1, to64);
            if (wb_mode) set_xreg_sp(c, rn, wb_addr);
            set_xreg(c, rt, v);
            return;
        }
    }
    if (wb_mode) set_xreg_sp(c, rn, wb_addr);
}

static void exec_loadstore(CPU *c, uint32_t insn) {
    /* حصري */
    if ((insn & 0x3F000000) == 0x08000000) {
        int size = BITS(insn, 31, 30);
        int o2 = BIT(insn, 23), L = BIT(insn, 22), o1 = BIT(insn, 21);
        int rs = BITS(insn, 20, 16), rt2 = BITS(insn, 14, 10), rn = BITS(insn, 9, 5), rt = insn & 31;
        uint64_t addr = XSP(rn);
        int bytes = 1 << size;
        if (o1 && o2 == 0) { /* LDXP/STXP */
            if (size < 2) raise_undef(c);
            int eb = size == 3 ? 8 : 4;
            if (L) {
                uint64_t a = mem_read(c, addr, eb), b = mem_read(c, addr + eb, eb);
                c->excl_addr = va_to_pa(c, addr, ACC_R);
                c->excl_valid = 1;
                set_xreg(c, rt, a);
                set_xreg(c, rt2, b);
            } else {
                uint64_t pa = va_to_pa(c, addr, ACC_W);
                if (c->excl_valid && c->excl_addr == pa) {
                    mem_write(c, addr, eb, X(rt));
                    mem_write(c, addr + eb, eb, X(rt2));
                    set_xreg(c, rs, 0);
                } else set_xreg(c, rs, 1);
                c->excl_valid = 0;
            }
            return;
        }
        if (o1) raise_undef(c); /* CAS (LSE) غير مدعوم */
        if (o2 == 0) { /* LDXR/STXR وأخواتها */
            if (L) {
                uint64_t v = mem_read(c, addr, bytes);
                c->excl_addr = va_to_pa(c, addr, ACC_R);
                c->excl_valid = 1;
                set_xreg(c, rt, v);
            } else {
                uint64_t pa = va_to_pa(c, addr, ACC_W);
                if (c->excl_valid && c->excl_addr == pa) {
                    mem_write(c, addr, bytes, X(rt));
                    set_xreg(c, rs, 0);
                } else set_xreg(c, rs, 1);
                c->excl_valid = 0;
            }
        } else { /* LDAR/STLR */
            if (L) set_xreg(c, rt, mem_read(c, addr, bytes));
            else mem_write(c, addr, bytes, X(rt));
        }
        return;
    }
    /* تحميل حرفي */
    if ((insn & 0x3B000000) == 0x18000000) {
        int opc = BITS(insn, 31, 30), V = BIT(insn, 26), rt = insn & 31;
        uint64_t addr = c->cur_pc + (SEXT(BITS(insn, 23, 5), 19) << 2);
        if (V) {
            fp_check(c);
            v128 val = {0, 0};
            if (opc == 0) val.lo = mem_read(c, addr, 4);
            else if (opc == 1) val.lo = mem_read(c, addr, 8);
            else if (opc == 2) { val.lo = mem_read(c, addr, 8); val.hi = mem_read(c, addr + 8, 8); }
            else raise_undef(c);
            c->v[rt] = val;
        } else {
            if (opc == 0) set_xreg(c, rt, mem_read(c, addr, 4));
            else if (opc == 1) set_xreg(c, rt, mem_read(c, addr, 8));
            else if (opc == 2) set_xreg(c, rt, (uint64_t)(int64_t)(int32_t)mem_read(c, addr, 4));
            /* PRFM */
        }
        return;
    }
    /* زوج */
    if ((insn & 0x3A000000) == 0x28000000) {
        int opc = BITS(insn, 31, 30), V = BIT(insn, 26), mode = BITS(insn, 24, 23), L = BIT(insn, 22);
        int rt2 = BITS(insn, 14, 10), rn = BITS(insn, 9, 5), rt = insn & 31;
        int64_t imm7 = SEXT(BITS(insn, 21, 15), 7);
        int scale;
        if (V) { fp_check(c); scale = 2 + opc; if (opc == 3) raise_undef(c); }
        else scale = 2 + (opc >> 1);
        int64_t off = imm7 << scale;
        uint64_t base = XSP(rn);
        uint64_t addr = (mode == 1) ? base : base + off;
        int bytes = 1 << scale;
        if (V) {
            if (L) {
                v128 a = {0, 0}, b = {0, 0};
                if (bytes == 16) {
                    a.lo = mem_read(c, addr, 8); a.hi = mem_read(c, addr + 8, 8);
                    b.lo = mem_read(c, addr + 16, 8); b.hi = mem_read(c, addr + 24, 8);
                } else {
                    a.lo = mem_read(c, addr, bytes); b.lo = mem_read(c, addr + bytes, bytes);
                }
                c->v[rt] = a; c->v[rt2] = b;
            } else {
                if (bytes == 16) {
                    mem_write(c, addr, 8, c->v[rt].lo); mem_write(c, addr + 8, 8, c->v[rt].hi);
                    mem_write(c, addr + 16, 8, c->v[rt2].lo); mem_write(c, addr + 24, 8, c->v[rt2].hi);
                } else {
                    mem_write(c, addr, bytes, c->v[rt].lo); mem_write(c, addr + bytes, bytes, c->v[rt2].lo);
                }
            }
        } else {
            if (L) {
                int sign = opc == 1;
                uint64_t a = mem_read(c, addr, bytes), b = mem_read(c, addr + bytes, bytes);
                if (sign) { a = (uint64_t)(int64_t)(int32_t)a; b = (uint64_t)(int64_t)(int32_t)b; }
                if (mode == 1 || mode == 3) set_xreg_sp(c, rn, base + off);
                set_xreg(c, rt, a);
                set_xreg(c, rt2, b);
                return;
            } else {
                uint64_t a = X(rt), b = X(rt2);
                mem_write(c, addr, bytes, a);
                mem_write(c, addr + bytes, bytes, b);
            }
        }
        if (mode == 1 || mode == 3) set_xreg_sp(c, rn, base + off);
        return;
    }
    /* مسجل */
    if ((insn & 0x3A000000) == 0x38000000) {
        int size = BITS(insn, 31, 30), V = BIT(insn, 26), opc = BITS(insn, 23, 22);
        int rn = BITS(insn, 9, 5);
        int scale = size;
        if (V && (opc & 2)) scale = 4;
        if (BIT(insn, 24)) { /* إزاحة غير موقعة */
            uint64_t addr = XSP(rn) + (BITS(insn, 21, 10) << scale);
            exec_ldst_reg(c, insn, addr, 0, 0);
            return;
        }
        if (BIT(insn, 21)) {
            if (BITS(insn, 11, 10) == 2) { /* إزاحة مسجل */
                int rm = BITS(insn, 20, 16), opt = BITS(insn, 15, 13), S = BIT(insn, 12);
                uint64_t off = extend_reg(X(rm), opt, S ? scale : 0);
                exec_ldst_reg(c, insn, XSP(rn) + off, 0, 0);
                return;
            }
            raise_undef(c); /* عمليات LSE الذرية غير مدعومة */
        }
        int64_t imm9 = SEXT(BITS(insn, 20, 12), 9);
        uint64_t base = XSP(rn);
        switch (BITS(insn, 11, 10)) {
        case 0: exec_ldst_reg(c, insn, base + imm9, 0, 0); return;        /* غير مقيس */
        case 1: exec_ldst_reg(c, insn, base, 1, base + imm9); return;     /* لاحق */
        case 2: { /* غير مميز LDTR/STTR */
            int save = c->el;
            if (c->el == 1) c->el = 0;
            /* نحتاج أخطاء بمستوى EL1 عند الخطأ */
            uint64_t a = base + imm9;
            if (save == 1) {
                /* ترجمة مسبقة بصلاحيات EL0 */
                int acc = (opc == 0) ? ACC_W : ACC_R;
                int bytes = 1 << size;
                WalkRes w = walk(c, a);
                uint8_t need = acc == ACC_W ? P_W0 : P_R0;
                WalkRes w2 = walk(c, a + bytes - 1);
                c->el = save;
                if (w.fault || !(w.perm & need) || w2.fault || !(w2.perm & need)) {
                    int f = w.fault ? w.fault : (!(w.perm & need) ? 0x0F : (w2.fault ? w2.fault : 0x0F));
                    uint64_t fa = (w.fault || !(w.perm & need)) ? a : a + bytes - 1;
                    uint64_t esr = (0x25ULL << 26) | (1ULL << 25) | (acc == ACC_W ? (1 << 6) : 0) | f;
                    sync_exception(c, esr, fa, 1);
                }
            }
            c->el = save;
            exec_ldst_reg(c, insn, a, 0, 0);
            return;
        }
        case 3: exec_ldst_reg(c, insn, base + imm9, 1, base + imm9); return; /* مسبق */
        }
    }
    if (exec_simd_ldst(c, insn)) return;
    raise_undef(c);
}

/* ---------------- معالجة البيانات ---------------- */
static void exec_dp_imm(CPU *c, uint32_t insn) {
    int sf = BIT(insn, 31);
    int rd = insn & 31, rn = BITS(insn, 9, 5);
    switch (BITS(insn, 25, 23)) {
    case 0: case 1: { /* ADR/ADRP */
        int64_t imm = SEXT((BITS(insn, 23, 5) << 2) | BITS(insn, 30, 29), 21);
        if (BIT(insn, 31)) set_xreg(c, rd, (c->cur_pc & ~0xFFFULL) + (imm << 12));
        else set_xreg(c, rd, c->cur_pc + imm);
        return;
    }
    case 2: { /* ADD/SUB فوري */
        int op = BIT(insn, 30), S = BIT(insn, 29), sh = BIT(insn, 22);
        uint64_t imm = BITS(insn, 21, 10) << (sh ? 12 : 0);
        uint64_t a = XSP(rn);
        uint32_t f;
        uint64_t r = op ? add_with_carry(a, ~imm, 1, sf, &f) : add_with_carry(a, imm, 0, sf, &f);
        if (S) { c->nzcv = f; set_xreg(c, rd, r); }
        else set_xreg_sp(c, rd, r);
        return;
    }
    case 4: { /* منطقي فوري */
        int opc = BITS(insn, 30, 29);
        uint64_t imm;
        if (!decode_bitmask(sf, BIT(insn, 22), BITS(insn, 21, 16), BITS(insn, 15, 10), &imm)) raise_undef(c);
        uint64_t a = X(rn), r;
        switch (opc) {
        case 0: r = a & imm; break;
        case 1: r = a | imm; break;
        case 2: r = a ^ imm; break;
        default: r = a & imm; break;
        }
        if (!sf) r &= 0xFFFFFFFF;
        if (opc == 3) { set_nz(c, r, sf); set_xreg(c, rd, r); }
        else set_xreg_sp(c, rd, r);
        return;
    }
    case 5: { /* نقل واسع */
        int opc = BITS(insn, 30, 29), hw = BITS(insn, 22, 21);
        if (!sf && hw > 1) raise_undef(c);
        uint64_t imm = BITS(insn, 20, 5) << (hw * 16);
        uint64_t r;
        if (opc == 0) r = ~imm;
        else if (opc == 2) r = imm;
        else if (opc == 3) r = (X(rd) & ~(0xFFFFULL << (hw * 16))) | imm;
        else raise_undef(c);
        if (!sf) r &= 0xFFFFFFFF;
        set_xreg(c, rd, r);
        return;
    }
    case 6: { /* حقول البتات */
        int opc = BITS(insn, 30, 29);
        int immr = BITS(insn, 21, 16), imms = BITS(insn, 15, 10);
        int ds = sf ? 64 : 32;
        uint64_t src = X(rn);
        if (!sf) src &= 0xFFFFFFFF;
        uint64_t dst = X(rd), r;
        if (imms >= immr) {
            int w = imms - immr + 1;
            uint64_t field = (src >> immr) & MASK(w);
            if (opc == 0) r = (uint64_t)SEXT(field, w);
            else if (opc == 1) r = (dst & ~MASK(w)) | field;
            else r = field;
        } else {
            int w = imms + 1, pos = ds - immr;
            uint64_t field = (src & MASK(w)) << pos;
            if (opc == 0) r = (uint64_t)SEXT(field, pos + w);
            else if (opc == 1) r = (dst & ~(MASK(w) << pos)) | field;
            else r = field;
        }
        if (opc == 3) raise_undef(c);
        if (!sf) r &= 0xFFFFFFFF;
        set_xreg(c, rd, r);
        return;
    }
    case 7: { /* EXTR */
        int rm = BITS(insn, 20, 16), lsb = BITS(insn, 15, 10);
        uint64_t r;
        if (sf) {
            r = lsb ? (X(rm) >> lsb) | (X(rn) << (64 - lsb)) : X(rm);
        } else {
            uint64_t cat = ((X(rn) & 0xFFFFFFFF) << 32) | (X(rm) & 0xFFFFFFFF);
            r = (cat >> lsb) & 0xFFFFFFFF;
        }
        set_xreg(c, rd, r);
        return;
    }
    }
    raise_undef(c);
}

static void exec_branch_sys(CPU *c, uint32_t insn) {
    if ((insn & 0x7C000000) == 0x14000000) { /* B/BL */
        int64_t off = SEXT(insn & 0x3FFFFFF, 26) << 2;
        if (BIT(insn, 31)) c->x[30] = c->cur_pc + 4;
        c->pc = c->cur_pc + off;
        return;
    }
    if ((insn & 0x7E000000) == 0x34000000) { /* CBZ/CBNZ */
        int sf = BIT(insn, 31);
        uint64_t v = X(insn & 31);
        if (!sf) v &= 0xFFFFFFFF;
        int nz = BIT(insn, 24);
        if ((v != 0) == nz) c->pc = c->cur_pc + (SEXT(BITS(insn, 23, 5), 19) << 2);
        return;
    }
    if ((insn & 0x7E000000) == 0x36000000) { /* TBZ/TBNZ */
        int b = (BIT(insn, 31) << 5) | BITS(insn, 23, 19);
        int bit = BIT(X(insn & 31), b);
        if (bit == BIT(insn, 24)) c->pc = c->cur_pc + (SEXT(BITS(insn, 18, 5), 14) << 2);
        return;
    }
    if ((insn & 0xFF000010) == 0x54000000) { /* B.cond */
        if (cond_holds(c, insn & 15)) c->pc = c->cur_pc + (SEXT(BITS(insn, 23, 5), 19) << 2);
        return;
    }
    if ((insn & 0xFF000000) == 0xD4000000) { /* استثناءات */
        int opc = BITS(insn, 23, 21), ll = insn & 3;
        uint32_t imm = BITS(insn, 20, 5);
        if (opc == 0 && ll == 1) { /* SVC */
            take_exception(c, EXC_SYNC, (0x15ULL << 26) | (1ULL << 25) | imm, 0, 0, c->cur_pc + 4);
            return;
        }
        if (opc == 0 && (ll == 2 || ll == 3)) { /* HVC/SMC => PSCI */
            if (c->el == 0) raise_undef(c);
            do_psci(c);
            return;
        }
        if (opc == 1 && ll == 0) { /* BRK */
            sync_exception(c, (0x3CULL << 26) | (1ULL << 25) | imm, 0, 0);
        }
        raise_undef(c);
    }
    if ((insn & 0xFFC00000) == 0xD5000000) { exec_system(c, insn); return; }
    if ((insn & 0xFE000000) == 0xD6000000) { /* فرع إلى مسجل */
        int opc = BITS(insn, 24, 21);
        int rn = BITS(insn, 9, 5);
        if (insn == 0xD69F03E0) { if (c->el == 0) raise_undef(c); do_eret(c); return; }
        if (BITS(insn, 20, 16) != 31) raise_undef(c);
        if (BITS(insn, 15, 10) != 0) raise_undef(c); /* صيغ PAC غير مدعومة */
        uint64_t t = X(rn);
        if (opc == 0) c->pc = t;
        else if (opc == 1) { c->x[30] = c->cur_pc + 4; c->pc = t; }
        else if (opc == 2) c->pc = t;
        else raise_undef(c);
        return;
    }
    raise_undef(c);
}

static void exec_dp_reg(CPU *c, uint32_t insn) {
    int sf = BIT(insn, 31);
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16);
    if ((insn & 0x1F000000) == 0x0A000000) { /* منطقي بإزاحة */
        int opc = BITS(insn, 30, 29), shift = BITS(insn, 23, 22), N = BIT(insn, 21);
        int amt = BITS(insn, 15, 10);
        if (!sf && amt >= 32) raise_undef(c);
        uint64_t b = shift_reg(X(rm), shift, amt, sf);
        if (N) b = ~b;
        uint64_t a = X(rn), r;
        switch (opc) {
        case 0: case 3: r = a & b; break;
        case 1: r = a | b; break;
        default: r = a ^ b; break;
        }
        if (!sf) r &= 0xFFFFFFFF;
        if (opc == 3) set_nz(c, r, sf);
        set_xreg(c, rd, r);
        return;
    }
    if ((insn & 0x1F200000) == 0x0B000000) { /* ADD/SUB بإزاحة */
        int op = BIT(insn, 30), S = BIT(insn, 29), shift = BITS(insn, 23, 22), amt = BITS(insn, 15, 10);
        if (shift == 3) raise_undef(c);
        uint64_t b = shift_reg(X(rm), shift, amt, sf);
        uint32_t f;
        uint64_t r = op ? add_with_carry(X(rn), ~b, 1, sf, &f) : add_with_carry(X(rn), b, 0, sf, &f);
        if (S) c->nzcv = f;
        set_xreg(c, rd, r);
        return;
    }
    if ((insn & 0x1F200000) == 0x0B200000) { /* ADD/SUB بتمديد */
        int op = BIT(insn, 30), S = BIT(insn, 29), opt = BITS(insn, 15, 13), sh = BITS(insn, 12, 10);
        if (sh > 4) raise_undef(c);
        uint64_t b = extend_reg(X(rm), opt, sh);
        uint32_t f;
        uint64_t r = op ? add_with_carry(XSP(rn), ~b, 1, sf, &f) : add_with_carry(XSP(rn), b, 0, sf, &f);
        if (S) { c->nzcv = f; set_xreg(c, rd, r); }
        else set_xreg_sp(c, rd, r);
        return;
    }
    if ((insn & 0x1FE00000) == 0x1A000000) { /* ADC/SBC */
        int op = BIT(insn, 30), S = BIT(insn, 29);
        int carry = BIT(c->nzcv, 29);
        uint32_t f;
        uint64_t b = op ? ~X(rm) : X(rm);
        uint64_t r = add_with_carry(X(rn), b, carry, sf, &f);
        if (S) c->nzcv = f;
        set_xreg(c, rd, r);
        return;
    }
    if ((insn & 0x1FE00000) == 0x1A400000) { /* CCMN/CCMP */
        int op = BIT(insn, 30), cond = BITS(insn, 15, 12), nzcv = insn & 15;
        uint64_t b = BIT(insn, 11) ? (uint64_t)rm : X(rm);
        if (cond_holds(c, cond)) {
            uint32_t f;
            if (op) add_with_carry(X(rn), ~b, 1, sf, &f);
            else add_with_carry(X(rn), b, 0, sf, &f);
            c->nzcv = f;
        } else c->nzcv = (uint32_t)nzcv << 28;
        return;
    }
    if ((insn & 0x1FE00000) == 0x1A800000) { /* اختيار شرطي */
        int op = BIT(insn, 30), o2 = BITS(insn, 11, 10), cond = BITS(insn, 15, 12);
        uint64_t r;
        if (cond_holds(c, cond)) r = X(rn);
        else {
            r = X(rm);
            if (op == 0 && o2 == 1) r = r + 1;
            else if (op == 1 && o2 == 0) r = ~r;
            else if (op == 1 && o2 == 1) r = -r;
        }
        if (!sf) r &= 0xFFFFFFFF;
        set_xreg(c, rd, r);
        return;
    }
    if ((insn & 0x1F000000) == 0x1B000000) { /* ثلاثي المصادر */
        int op31 = BITS(insn, 23, 21), o0 = BIT(insn, 15), ra = BITS(insn, 14, 10);
        uint64_t a = X(rn), b = X(rm), acc = X(ra), r;
        switch (op31) {
        case 0: r = o0 ? acc - a * b : acc + a * b; if (!sf) r &= 0xFFFFFFFF; break;
        case 1: { int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b; r = o0 ? acc - p : acc + p; break; }
        case 5: { uint64_t p = (uint64_t)(uint32_t)a * (uint32_t)b; r = o0 ? acc - p : acc + p; break; }
        case 2: r = (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); break;
        case 6: r = (uint64_t)(((unsigned __int128)a * b) >> 64); break;
        default: raise_undef(c); return;
        }
        set_xreg(c, rd, r);
        return;
    }
    if ((insn & 0x5FE00000) == 0x1AC00000) { /* ثنائي المصادر */
        int opc = BITS(insn, 15, 10);
        uint64_t a = X(rn), b = X(rm), r;
        if (!sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
        switch (opc) {
        case 2: r = b ? a / b : 0; break;
        case 3:
            if (sf) r = b ? ((int64_t)a == INT64_MIN && (int64_t)b == -1 ? a : (uint64_t)((int64_t)a / (int64_t)b)) : 0;
            else r = b ? ((int32_t)a == INT32_MIN && (int32_t)b == -1 ? a : (uint64_t)(uint32_t)((int32_t)a / (int32_t)b)) : 0;
            break;
        case 8: r = shift_reg(a, 0, b, sf); break;
        case 9: r = shift_reg(a, 1, b, sf); break;
        case 10: r = shift_reg(a, 2, b, sf); break;
        case 11: r = shift_reg(a, 3, b, sf); break;
        default: raise_undef(c); return;
        }
        if (!sf) r &= 0xFFFFFFFF;
        set_xreg(c, rd, r);
        return;
    }
    if ((insn & 0x5FE00000) == 0x5AC00000) { /* أحادي المصدر */
        int opc = BITS(insn, 15, 10);
        if (BITS(insn, 20, 16) != 0) raise_undef(c);
        uint64_t a = X(rn), r = 0;
        if (!sf) a &= 0xFFFFFFFF;
        switch (opc) {
        case 0: { /* RBIT */
            r = 0;
            int n = sf ? 64 : 32;
            for (int i = 0; i < n; i++) if (BIT(a, i)) r |= 1ULL << (n - 1 - i);
            break;
        }
        case 1: /* REV16 */
            r = ((a & 0x00FF00FF00FF00FFULL) << 8) | ((a >> 8) & 0x00FF00FF00FF00FFULL);
            break;
        case 2: /* REV32 أو REV للـ32 بت */
            if (sf) r = ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32) | __builtin_bswap32((uint32_t)a);
            else r = __builtin_bswap32((uint32_t)a);
            break;
        case 3: if (!sf) raise_undef(c); r = __builtin_bswap64(a); break;
        case 4: r = sf ? (a ? __builtin_clzll(a) : 64) : (a ? __builtin_clz((uint32_t)a) : 32); break;
        case 5: { /* CLS */
            if (sf) { uint64_t t = a ^ ((uint64_t)((int64_t)a >> 63)); r = (t ? __builtin_clzll(t) : 64) - 1; }
            else { uint32_t t = (uint32_t)a ^ (uint32_t)((int32_t)a >> 31); r = (t ? __builtin_clz(t) : 32) - 1; }
            break;
        }
        default: raise_undef(c); return;
        }
        if (!sf) r &= 0xFFFFFFFF;
        set_xreg(c, rd, r);
        return;
    }
    raise_undef(c);
}

/* ---------------- الحلقة الرئيسية ---------------- */
void cpu_reset(CPU *c, Machine *m) {
    memset(c, 0, sizeof(*c));
    c->m = m;
    c->el = 1;
    c->spsel = 1;
    c->daif = 0xF << 6;
    c->sctlr = 0x30D00800;
    c->cpacr = 0;
}

/* ينفذ تعليمة واحدة (c->pc مضبوط على العنوان التالي مسبقًا) */
void cpu_exec_insn(CPU *c, uint32_t insn) {
    uint32_t op = BITS(insn, 28, 25);
    switch (op) {
    case 8: case 9: exec_dp_imm(c, insn); break;
    case 10: case 11: exec_branch_sys(c, insn); break;
    case 4: case 6: case 12: case 14: exec_loadstore(c, insn); break;
    case 5: case 13: exec_dp_reg(c, insn); break;
    case 7: case 15: if (!exec_simd(c, insn)) raise_undef(c); break;
    default: raise_undef(c);
    }
}

static inline void step(CPU *c) {
    c->cur_pc = c->pc;
    uint32_t insn = fetch(c);
    c->cur_insn = insn;
    c->pc += 4;
    cpu_exec_insn(c, insn);
}

/* عنوان صفحة الشيفرة على المضيف للمترجم الفوري (قد يرفع استثناء تعليمة) */
const uint8_t *cpu_code_ptr(CPU *c, uint64_t pc, uint64_t *pa) {
    if (pc & 3) sync_exception(c, (0x22ULL << 26) | (1ULL << 25), pc, 1);
    TLBEntry *e = translate(c, pc, ACC_X);
    if (!e->host) {
        fprintf(stderr, "[gtv] تنفيذ من خارج الذاكرة pc=%llx\n", (unsigned long long)pc);
        exit(3);
    }
    *pa = e->pa | (pc & 0xFFF);
    return e->host;
}

int cpu_run(CPU *c, uint64_t max_insns) {
    Machine *m = c->m;
    uint64_t end = max_insns ? c->icount + max_insns : ~0ULL;
    static uint64_t last_tick;
    last_tick = ~0ULL;
    setjmp(c->jb);
    if (c->jit_in) { /* عودة من استثناء أثناء تنفيذ شيفرة مترجمة */
        c->icount += (uint64_t)(c->jit_given - c->jit_budget);
        c->jit_in = 0;
    }
    while (!c->stop && c->icount < end) {
        /* فحص الأجهزة دوريًا (كل 1024 تعليمة) */
        uint64_t tick = c->icount >> 10;
        if (m->jit ? tick != last_tick : (c->icount & 0x3FF) == 0) {
            int poll = m->jit ? (tick & 63) == 0 || (tick >> 6) != (last_tick >> 6) : (c->icount & 0xFFFF) == 0;
            last_tick = tick;
            timer_update(m);
            if (poll) uart_poll(m);
            if (m->verbose >= 3 && (tick % 9766) == 0) fprintf(stderr, "[gtv] %lluM pc=%016llx EL%d\n", (unsigned long long)(c->icount / 1000000), (unsigned long long)c->pc, c->el);
        }
        if (c->irq_line && !(c->daif & (1 << 7))) {
            c->halted = 0;
            take_exception(c, EXC_IRQ, 0, 0, 0, c->pc);
        }
        if (c->halted && m->ncpu > 1) {
            /* مع تعدد المعالجات: نتخلى عن الدور، والحلقة العامة تنام إذا توقف الجميع */
            if (!c->irq_line) return 0;
            c->halted = 0;
        }
        if (c->halted) {
            /* انتظار مقاطعة: نام حتى موعد المؤقت التالي */
            uint64_t dl = timer_next_deadline(m);
            uint64_t now = cpu_counter(m);
            if (!c->irq_line) {
                if (dl > now) {
                    uint64_t wait_ns = (dl - now) * 16;
                    if (wait_ns > 10000000) wait_ns = 10000000;
                    struct timespec ts = { 0, (long)wait_ns };
                    if (!uart_has_input(m)) nanosleep(&ts, NULL);
                }
                uart_poll(m);
                timer_update(m);
            }
            if (c->irq_line) c->halted = 0;
            c->icount++;
            continue;
        }
        if (m->jit && !m->trace) {
            int64_t b = 1024 - (int64_t)(c->icount & 1023);
            if (end - c->icount < (uint64_t)b) b = (int64_t)(end - c->icount);
            c->jit_budget = b;
            c->jit_given = b;
            c->jit_in = 1;
            jit_exec(c);
            c->jit_in = 0;
            c->icount += (uint64_t)(b - c->jit_budget);
            continue;
        }
        if (m->trace) fprintf(stderr, "%016llx EL%d\n", (unsigned long long)c->pc, c->el);
        step(c);
        c->icount++;
    }
    return c->stop;
}

/* ---------------- تشغيل الآلة بعدة معالجات ----------------
 * تُنفَّذ المعالجات بالتناوب على خيط واحد من المضيف (شرائح من 16384 تعليمة)،
 * فيبقى النموذج حتميًا ولا يحتاج إلى أقفال. تُلغى مراقبة التعليمات الحصرية
 * (LDXR/STXR) للمعالج عند انتهاء شريحته، فيفشل STXR ويعيد المحاولة بدل كسر الذرّية. */
uint64_t machine_icount(Machine *m) {
    uint64_t t = 0;
    for (int i = 0; i < m->ncpu; i++) t += m->cpus[i].icount;
    return t;
}

int machine_run(Machine *m, uint64_t max_insns) {
    if (m->ncpu == 1) { m->cur = &m->cpus[0]; return cpu_run(&m->cpus[0], max_insns); }
    const uint64_t SLICE = 16384;
    for (;;) {
        int any_run = 0, stop = 0;
        for (int i = 0; i < m->ncpu; i++) {
            CPU *c = &m->cpus[i];
            if (!c->powered) continue;
            m->cur = c;
            cpu_run(c, SLICE);
            c->excl_valid = 0;
            if (c->stop) { stop = 1; m->cpus[0].exit_code = c->exit_code; break; }
            if (!c->halted || c->irq_line) any_run = 1;
        }
        if (stop) return 1;
        if (max_insns && machine_icount(m) >= max_insns) return 0;
        if (!any_run) {
            /* كل المعالجات في WFI: نام حتى أقرب موعد مؤقت أو وصول إدخال */
            uint64_t dl = timer_next_deadline(m), now = cpu_counter(m);
            if (dl > now) {
                uint64_t wait_ns = (dl - now) * 16;
                if (wait_ns > 10000000) wait_ns = 10000000;
                struct timespec ts = { 0, (long)wait_ns };
                if (!uart_has_input(m)) nanosleep(&ts, NULL);
            }
            uart_poll(m);
            timer_update(m);
            for (int i = 0; i < m->ncpu; i++) if (m->cpus[i].powered && m->cpus[i].halted) m->cpus[i].icount += 64;
        }
    }
}
