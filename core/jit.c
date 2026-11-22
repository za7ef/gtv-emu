/* gtv-emu — مترجم فوري (JIT) من AArch64 إلى AArch64.
 *
 * الفكرة: المعالج الضيف والمضيف كلاهما ARM64، فتُنسخ معظم التعليمات الحسابية كما هي
 * بعد إعادة ترقيم مسجلاتها إلى مسجلات مؤقتة في المضيف. يُحمَّل كل مسجل ضيف من بنية CPU
 * قبل التعليمة ويُعاد بعدها، فتبقى حالة الضيف كاملة في الذاكرة عند أي استدعاء أو استثناء.
 *
 * تخطيط مسجلات المضيف داخل الشيفرة المولّدة:
 *   x19 = مؤشر CPU،  x20 = مؤشر جدول TLB
 *   x1..x4 = مسجلات الضيف المعاد ترقيمها (Rd/Rt، Rn/Rt2، Rm/القاعدة، Ra/الفهرس)
 *   x5..x12 = عناوين ومؤقتات المسار السريع للذاكرة،  x16 = مؤقت عام
 *   q16/q17 = مسجلات SIMD مؤقتة لتحميل وتخزين مسجلات الفاصلة العائمة
 *
 * يُستخدم المفسّر احتياطيًا لكل تعليمة غير مترجمة، وللمسار البطيء للذاكرة (MMIO وأخطاء الصفحات).
 */
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && !defined(GTV_NO_JIT)

#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#endif

#define BIT(x, n) (((x) >> (n)) & 1)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1ULL << ((hi) - (lo) + 1)) - 1))
#define SEXT(v, bits) ((int64_t)((uint64_t)(v) << (64 - (bits))) >> (64 - (bits)))

_Static_assert(sizeof(TLBEntry) == 32, "TLBEntry يجب أن يكون 32 بايت");
#define CODE_SIZE   (64u << 20)
#define HASH_SIZE   (1u << 16)
#define MAX_BLOCK   192

typedef struct {
    uint64_t va, pa;
    uint32_t mode;
    uint32_t *code;  /* عنوان التنفيذ (RX) */
} Block;

typedef struct Jit {
    uint8_t *rw;           /* نافذة الكتابة */
    uint8_t *rx;           /* نافذة التنفيذ */
    size_t pos, tramp_end;
    Block *hash;
    uint8_t *codemap;      /* بت لكل صفحة فيزيائية تحتوي شيفرة مترجمة */
    uint64_t npages;
    int flush_pending;
    uint64_t inval[64]; int ninval;  /* صفحات فيزيائية تنتظر الإبطال */
    uint32_t used, tombs;            /* خانات الجدول المشغولة والمحذوفة */
    uint64_t gen;                    /* يزيد مع كل إبطال؛ يمنع ربط كتلة أُبطلت */
    uint64_t stats_inval;
    void (*enter)(CPU *, void *, void *);
    uint32_t *exit_rx;     /* الخاتمة المشتركة */
    uint64_t stats_blocks, stats_flushes, stats_chains;
} Jit;

/* ---------- الذاكرة القابلة للتنفيذ ---------- */
/* نتبع نموذج W^X: نافذتان للذاكرة نفسها، واحدة للكتابة وأخرى للتنفيذ، كما يتطلب iOS. */
static int jit_mode;             /* GTV_JIT_AUTO وأخواته من gtv.h */
static char jit_status_buf[200] = "لم تُجرَّب بعد";
void gtv_set_jit_mode(int mode) { jit_mode = mode; }
const char *gtv_jit_status(void) { return jit_status_buf; }
#define JIT_STATUS(...) snprintf(jit_status_buf, sizeof jit_status_buf, __VA_ARGS__)

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
extern int csops(int pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_DEBUGGED 0x10000000

/* هل يوجد مصحّح مرفق منح العملية صلاحية JIT (StikDebug أو Xcode)؟ */
static int cs_debugged(void) {
    uint32_t f = 0;
    if (csops(getpid(), 0, &f, sizeof f) != 0) return 0;
    return (f & CS_DEBUGGED) != 0;
}

static int os_major(void) {
    char b[32] = {0}; size_t n = sizeof b - 1;
    if (sysctlbyname("kern.osproductversion", b, &n, NULL, 0) != 0) return 0;
    return atoi(b);
}

/* هل يتتبّع مصحّح العملية الآن فعلًا؟ CS_DEBUGGED يبقى مضبوطًا بعد فصل المصحّح،
 * فلا يكفي وحده للحكم بأن نقطة التوقف ستُعالَج. */
static int ptraced_now(void) {
    struct kinfo_proc kp; size_t n = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    memset(&kp, 0, sizeof kp);
    if (sysctl(mib, 4, &kp, &n, NULL, 0) != 0) return 0;
    return (kp.kp_proc.p_flag & P_TRACED) != 0;
}

/* «استدعاء» بروتوكول StikDebug لـ TXM ‏(iOS 26 وما بعده)، مطابق لسكربت universal.js الحالي
 * (آخر تحديث 2026-03-29): يتوقف التنفيذ عند brk #0xf00d، فيقرأ السكربت رقم الأمر من x16
 * والوسائط من x0 وx1، ويكتب النتيجة في x0، ويتجاوز نقطة التوقف.
 * الأمر 0 = فصل المصحّح، والأمر 1 = PrepareRegion (مع x0 = 0 يحجز منطقة RX بحجم x1). */
__attribute__((noinline, naked)) static void *jit26_prepare_region(void *addr, size_t len) {
    __asm__ volatile("mov x16, #1\n"
                     "brk #0xf00d\n"
                     "ret");
}
__attribute__((noinline, naked)) static void jit26_detach(void) {
    __asm__ volatile("mov x16, #0\n"
                     "brk #0xf00d\n"
                     "ret");
}

/* شبكة أمان: إن نُفِّذت نقطة التوقف ولم يعالجها أي مصحّح (سكربت غير مفعّل أو سكربت قديم)
 * يصل SIGTRAP إلى العملية؛ نتجاوز التعليمة ونعيد 0 بدل أن ينهار التطبيق. */
#include <signal.h>
#include <sys/ucontext.h>
static volatile sig_atomic_t brk_unhandled;
static void brk_trap(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
    uint32_t insn = *(uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
    if ((insn & 0xFFE0001F) == 0xD4200000 && ((insn >> 5) & 0xFFFF) == 0xF00D) {
        uc->uc_mcontext->__ss.__pc += 4;
        uc->uc_mcontext->__ss.__x[0] = 0;
        brk_unhandled = 1;
    }
}
static void *guarded_call(int detach, size_t len) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = brk_trap; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, &old);
    brk_unhandled = 0;
    void *r = NULL;
    if (detach) jit26_detach(); else r = jit26_prepare_region(NULL, len);
    sigaction(SIGTRAP, &old, NULL);
    return brk_unhandled ? NULL : r;
}

static int jit_alloc_txm(Jit *j) {
    if (!ptraced_now()) {
        JIT_STATUS("TXM: المصحّح غير مرفق الآن (فُصل بعد التفعيل). في StikDebug اختر سكربت universal.js لهذا التطبيق");
        return 0;
    }
    void *rxp = guarded_call(0, CODE_SIZE);
    if (brk_unhandled) {
        JIT_STATUS("TXM: لم يعالج StikDebug الطلب؛ اختر سكربت universal.js لـ GTV Emu يدويًا ثم أعد التشغيل");
        return 0;
    }
    if (!rxp || (uintptr_t)rxp == 0xE0000069) {
        guarded_call(1, 0);
        JIT_STATUS("TXM: لم يُعِد المصحّح منطقة RX (السكربت المرفق ليس universal.js؟)");
        return 0;
    }
    vm_address_t rx = (vm_address_t)rxp, rw = 0;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &rw, CODE_SIZE, 0, VM_FLAGS_ANYWHERE, mach_task_self(), rx, FALSE,
                                &cur, &max, VM_INHERIT_NONE);
    if (kr == KERN_SUCCESS && mprotect((void *)rw, CODE_SIZE, PROT_READ | PROT_WRITE) != 0) kr = KERN_PROTECTION_FAILURE;
    /* المنطقة جاهزة؛ نفصل المصحّح حتى لا تمر إشارات المحاكي عبره ولا تتأثر العملية بانقطاعه */
    guarded_call(1, 0);
    if (kr != KERN_SUCCESS) { JIT_STATUS("TXM: فشل تجهيز نافذة الكتابة (%d)", kr); return 0; }
    /* فحص ذاتي: كتابة ret في نافذة الكتابة وتنفيذها من نافذة التنفيذ */
    *(volatile uint32_t *)rw = 0xD65F03C0;
    sys_icache_invalidate((void *)rx, 4);
    ((void (*)(void))(void *)rx)();
    j->rw = (uint8_t *)rw; j->rx = (uint8_t *)rx;
    JIT_STATUS("مفعّل: مسار TXM عبر StikDebug (iOS %d)", os_major());
    return 1;
}

/* المسار القديم (قبل TXM، وعلى macOS): حجز RW ثم نسخة RX عبر vm_remap */
static int jit_alloc_legacy(Jit *j) {
    vm_address_t rw = 0, rx = 0;
    if (vm_allocate(mach_task_self(), &rw, CODE_SIZE, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { JIT_STATUS("فشل vm_allocate"); return 0; }
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rx, CODE_SIZE, 0, VM_FLAGS_ANYWHERE, mach_task_self(), rw, 0,
                 &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) { JIT_STATUS("فشل vm_remap"); return 0; }
    if (mprotect((void *)rx, CODE_SIZE, PROT_READ | PROT_EXEC) != 0) { JIT_STATUS("رفض النظام صفحات RX (JIT غير مفعّل)"); return 0; }
    j->rw = (uint8_t *)rw; j->rx = (uint8_t *)rx;
    JIT_STATUS("مفعّل: المسار القديم (vm_remap)");
    return 1;
}
#endif

/* المنطقة التنفيذية تُحجز مرة واحدة لكل عملية (على TXM لا يمكن طلبها ثانية بعد فصل المصحّح)،
 * وتُعاد للآلات اللاحقة في الجلسة نفسها. */
static uint8_t *cached_rw, *cached_rx;
static char cached_status[200];
static int jit_alloc_once(Jit *j);
static int jit_alloc(Jit *j) {
    if (jit_mode == 3) { JIT_STATUS("معطّل من الإعدادات"); return 0; }
    if (cached_rx) { j->rw = cached_rw; j->rx = cached_rx; JIT_STATUS("%s", cached_status); return 1; }
    if (!jit_alloc_once(j)) return 0;
    cached_rw = j->rw; cached_rx = j->rx;
    snprintf(cached_status, sizeof cached_status, "%s", jit_status_buf);
    return 1;
}
static int jit_alloc_once(Jit *j) {
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
    if (!cs_debugged()) { JIT_STATUS("غير مفعّل: افتح التطبيق من StikDebug لتفعيل JIT"); return 0; }
    if (jit_mode == 1) return jit_alloc_txm(j);
    if (jit_mode == 2) return jit_alloc_legacy(j);
    /* تلقائي: TXM في iOS 26 فما فوق، ثم المسار القديم */
    if (os_major() >= 26) return jit_alloc_txm(j);
    return jit_alloc_legacy(j);
#else
    return jit_alloc_legacy(j);
#endif
#else
#ifdef __linux__
    extern int memfd_create(const char *, unsigned int);
    int fd = memfd_create("gtv-jit", 0);
    if (fd >= 0 && ftruncate(fd, CODE_SIZE) == 0) {
        void *rw = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void *rx = mmap(NULL, CODE_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        close(fd);
        if (rw != MAP_FAILED && rx != MAP_FAILED) { j->rw = rw; j->rx = rx; JIT_STATUS("مفعّل: memfd بنافذتين"); return 1; }
    }
#endif
    void *p = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { JIT_STATUS("رفض النظام ذاكرة قابلة للتنفيذ"); return 0; }
    j->rw = j->rx = p;
    JIT_STATUS("مفعّل: RWX");
    return 1;
#endif
}

static void icache_sync(Jit *j, void *rx_start, size_t len) {
    (void)j;
#if defined(__APPLE__)
    sys_icache_invalidate(rx_start, len);
#else
    /* المسح يكون على نافذة التنفيذ؛ ونافذة الكتابة تشارك الصفحات الفيزيائية نفسها */
    __builtin___clear_cache((char *)rx_start, (char *)rx_start + len);
    __builtin___clear_cache((char *)j->rw + ((uint8_t *)rx_start - j->rx),
                            (char *)j->rw + ((uint8_t *)rx_start - j->rx) + len);
#endif
}

/* ---------- المُصدِر ---------- */
typedef struct {
    Jit *j;
    uint32_t *buf;     /* مؤشر الكتابة لبداية الكتلة */
    size_t n, cap;
    uint32_t *rxbase;  /* عنوان التنفيذ المقابل */
    int overflow;
} Emit;

static inline void E(Emit *e, uint32_t w) {
    if (e->n >= e->cap) { e->overflow = 1; return; }
    e->buf[e->n++] = w;
}
static inline uint32_t *rx_at(Emit *e, size_t idx) { return e->rxbase + idx; }

static void mov64(Emit *e, int rd, uint64_t v) {
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        uint32_t h = (v >> (16 * hw)) & 0xFFFF;
        if (h == 0 && !(first && hw == 3)) continue;
        if (first) { E(e, 0xD2800000 | (hw << 21) | (h << 5) | rd); first = 0; }
        else E(e, 0xF2800000 | (hw << 21) | (h << 5) | rd);
    }
    if (first) E(e, 0xD2800000 | rd);
}

static void ldr_x(Emit *e, int rt, size_t off) { E(e, 0xF9400000 | ((off / 8) << 10) | (19 << 5) | rt); }
static void str_x(Emit *e, int rt, size_t off) { E(e, 0xF9000000 | ((off / 8) << 10) | (19 << 5) | rt); }
static void ldr_w(Emit *e, int rt, size_t off) { E(e, 0xB9400000 | ((off / 4) << 10) | (19 << 5) | rt); }
static void str_w(Emit *e, int rt, size_t off) { E(e, 0xB9000000 | ((off / 4) << 10) | (19 << 5) | rt); }

/* add/sub بعدد صحيح ثابت */
static void add_imm(Emit *e, int rd, int rn, int64_t imm) {
    uint32_t op = imm < 0 ? 0xD1000000 : 0x91000000;
    uint64_t a = imm < 0 ? (uint64_t)(-imm) : (uint64_t)imm;
    if (a == 0) { if (rd != rn) E(e, 0x91000000 | (rn << 5) | rd); return; }
    int src = rn;
    if (a & 0xFFF) { E(e, op | ((a & 0xFFF) << 10) | (src << 5) | rd); src = rd; }
    if (a >> 12) {
        if (a >> 24) { /* نادر: عبر مسجل */
            mov64(e, 16, a);
            E(e, (imm < 0 ? 0xCB000000 : 0x8B000000) | (16 << 16) | (src << 5) | rd);
            return;
        }
        E(e, op | (1 << 22) | (((a >> 12) & 0xFFF) << 10) | (src << 5) | rd);
    }
}

static void b_to(Emit *e, uint32_t *target_rx) {
    int64_t off = target_rx - rx_at(e, e->n);
    E(e, 0x14000000 | ((uint32_t)off & 0x3FFFFFF));
}

/* تسميات أمامية بسيطة */
typedef struct { size_t at; int kind; } Fix; /* kind: 0 b.cond/cbz (imm19), 1 tbz (imm14), 2 b (imm26) */
static void patch_to_here(Emit *e, Fix f) {
    int64_t off = (int64_t)e->n - (int64_t)f.at;
    uint32_t w = e->buf[f.at];
    if (f.kind == 0) w |= ((uint32_t)off & 0x7FFFF) << 5;
    else if (f.kind == 1) w |= ((uint32_t)off & 0x3FFF) << 5;
    else w |= (uint32_t)off & 0x3FFFFFF;
    e->buf[f.at] = w;
}
static Fix fix(Emit *e, uint32_t w, int kind) { Fix f = { e->n, kind }; E(e, w); return f; }

/* ---------- حالة الترجمة ---------- */
typedef struct {
    Jit *j;
    CPU *c;
    Emit e;
    uint64_t start, page;
    int el, fpok;
    size_t sp_off;
    int pr, pw;        /* بت صلاحية القراءة/الكتابة في TLB حسب مستوى الامتياز */
} Tr;

#define OFF(f) offsetof(CPU, f)
static size_t xoff(int r) { return OFF(x) + 8 * r; }

static void exit_epilogue(Tr *t) { b_to(&t->e, t->j->exit_rx); }

/* خروج إلى عنوان ثابت؛ إن كان في الصفحة نفسها فهو قابل للربط المباشر */
static void exit_const(Tr *t, uint64_t pc) {
    Emit *e = &t->e;
    int chain = (pc >> 12) == (t->page >> 12);
    size_t slot = e->n;
    if (chain) E(e, 0x14000001); /* b .+4 — يُرقّع لاحقًا إلى الكتلة الهدف */
    mov64(e, 16, pc);
    str_x(e, 16, OFF(pc));
    if (chain) {
        mov64(e, 16, (uint64_t)(uintptr_t)rx_at(e, slot));
        str_x(e, 16, OFF(jit_last_slot));
    }
    exit_epilogue(t);
}

/* استدعاء المفسّر لتعليمة واحدة */
uint64_t jit_helper(CPU *c, uint64_t pc, uint32_t insn);

static void call_helper(Tr *t, uint64_t pc, uint32_t insn, int force_exit) {
    Emit *e = &t->e;
    E(e, 0xAA1303E0);              /* mov x0, x19 */
    mov64(e, 1, pc);
    mov64(e, 2, insn);
    mov64(e, 16, (uint64_t)(uintptr_t)jit_helper);
    E(e, 0xD63F0200);              /* blr x16 */
    if (force_exit) { exit_epilogue(t); return; }
    Fix f = fix(e, 0xB4000000 | 0, 0); /* cbz x0, متابعة */
    exit_epilogue(t);
    patch_to_here(e, f);
}

/* ---------- المسجلات ---------- */
enum { SEM_NONE, SEM_ZR, SEM_SP };

/* يحمّل مسجل الضيف g في مسجل المضيف h ويعيد رقم المسجل المستخدم في التعليمة المنسوخة */
static int load_g(Tr *t, int h, int g, int sem) {
    if (g == 31) {
        if (sem == SEM_ZR) return 31;
        ldr_x(&t->e, h, t->sp_off);
        return h;
    }
    ldr_x(&t->e, h, xoff(g));
    return h;
}
static void store_g(Tr *t, int h, int g, int sem) {
    if (g == 31) {
        if (sem == SEM_ZR) return;
        str_x(&t->e, h, t->sp_off);
        return;
    }
    str_x(&t->e, h, xoff(g));
}
static void flags_in(Tr *t) { ldr_w(&t->e, 16, OFF(nzcv)); E(&t->e, 0xD51B4200 | 16); }
static void flags_out(Tr *t) { E(&t->e, 0xD53B4200 | 16); str_w(&t->e, 16, OFF(nzcv)); }

/* ---------- التعليمات الحسابية: نسخ مع إعادة ترقيم ---------- */
typedef struct { int rd, rd_read, rn, rm, ra, fr, fw; } AluDesc;

static int alu_desc(uint32_t insn, AluDesc *d) {
    memset(d, 0, sizeof(*d));
    int sf = BIT(insn, 31);
    if (BITS(insn, 28, 26) == 4) { /* معالجة بيانات بقيمة فورية */
        switch (BITS(insn, 25, 23)) {
        case 2: { int S = BIT(insn, 29); d->rd = S ? SEM_ZR : SEM_SP; d->rn = SEM_SP; d->fw = S; return 1; }
        case 4: { int opc = BITS(insn, 30, 29);
            if (!sf && BIT(insn, 22)) return 0;
            d->rd = opc == 3 ? SEM_ZR : SEM_SP; d->rn = SEM_ZR; d->fw = opc == 3; return 1; }
        case 5: { int opc = BITS(insn, 30, 29);
            if (opc == 1 || (!sf && BIT(insn, 22))) return 0;
            d->rd = SEM_ZR; d->rd_read = opc == 3; return 1; }
        case 6: { int opc = BITS(insn, 30, 29);
            if (opc == 3 || BIT(insn, 22) != sf) return 0;
            if (!sf && (BIT(insn, 21) || BIT(insn, 15))) return 0;
            d->rd = SEM_ZR; d->rn = SEM_ZR; d->rd_read = opc == 1; return 1; }
        case 7: {
            if (BITS(insn, 30, 29) || BIT(insn, 21) || BIT(insn, 22) != sf || (!sf && BIT(insn, 15))) return 0;
            d->rd = d->rn = d->rm = SEM_ZR; return 1; }
        }
        return 0;
    }
    if (BITS(insn, 27, 25) != 5) return 0;
    if (!BIT(insn, 28)) {
        if (!BIT(insn, 24)) { /* منطقي بإزاحة */
            if (!sf && BIT(insn, 15)) return 0;
            d->rd = d->rn = d->rm = SEM_ZR; d->fw = BITS(insn, 30, 29) == 3; return 1;
        }
        if (!BIT(insn, 21)) { /* جمع/طرح بإزاحة */
            if (BITS(insn, 23, 22) == 3 || (!sf && BIT(insn, 15))) return 0;
            d->rd = d->rn = d->rm = SEM_ZR; d->fw = BIT(insn, 29); return 1;
        }
        /* جمع/طرح بتمديد */
        if (BITS(insn, 23, 22) || BITS(insn, 12, 10) > 4) return 0;
        d->rd = BIT(insn, 29) ? SEM_ZR : SEM_SP; d->rn = SEM_SP; d->rm = SEM_ZR; d->fw = BIT(insn, 29);
        return 1;
    }
    int op = BITS(insn, 24, 21);
    if (op == 0) { /* ADC/SBC */
        if (BITS(insn, 15, 10)) return 0;
        d->rd = d->rn = d->rm = SEM_ZR; d->fr = 1; d->fw = BIT(insn, 29); return 1;
    }
    if (op == 2) { /* CCMP/CCMN */
        if (!BIT(insn, 29) || BIT(insn, 10) || BIT(insn, 4)) return 0;
        d->rn = SEM_ZR; if (!BIT(insn, 11)) d->rm = SEM_ZR;
        d->fr = d->fw = 1; return 1;
    }
    if (op == 4) { /* CSEL */
        if (BIT(insn, 29) || BIT(insn, 11)) return 0;
        d->rd = d->rn = d->rm = SEM_ZR; d->fr = 1; return 1;
    }
    if (op == 6) {
        if (BIT(insn, 29)) return 0;
        int opcode = BITS(insn, 15, 10);
        if (!BIT(insn, 30)) { /* مصدران */
            if (!(opcode == 2 || opcode == 3 || (opcode >= 8 && opcode <= 11))) return 0;
            d->rd = d->rn = d->rm = SEM_ZR; return 1;
        }
        if (BITS(insn, 20, 16) || opcode > 5) return 0;
        if (!sf && opcode == 3) return 0;
        d->rd = d->rn = SEM_ZR; return 1;
    }
    if (op & 8) { /* ثلاثة مصادر */
        if (BITS(insn, 30, 29)) return 0;
        int op31 = BITS(insn, 23, 21), o0 = BIT(insn, 15);
        if (op31 == 0) { }
        else if (!sf) return 0;
        else if (op31 == 1 || op31 == 5) { }
        else if ((op31 == 2 || op31 == 6) && !o0) { }
        else return 0;
        d->rd = d->rn = d->rm = d->ra = SEM_ZR; return 1;
    }
    return 0;
}

static void tr_alu(Tr *t, uint32_t insn, const AluDesc *d) {
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16), ra = BITS(insn, 14, 10);
    uint32_t w = insn;
    if (d->rn) { int h = load_g(t, 2, rn, d->rn); w = (w & ~(31u << 5)) | (h << 5); }
    if (d->rm) { int h = load_g(t, 3, rm, d->rm); w = (w & ~(31u << 16)) | (h << 16); }
    if (d->ra) { int h = load_g(t, 4, ra, d->ra); w = (w & ~(31u << 10)) | (h << 10); }
    int hd = 31;
    if (d->rd) {
        if (rd == 31 && d->rd == SEM_ZR) hd = 31;
        else {
            hd = 1;
            if (d->rd_read) load_g(t, 1, rd, d->rd);
        }
        w = (w & ~31u) | hd;
    }
    if (d->fr || d->fw) flags_in(t);
    E(&t->e, w);
    if (d->fw) flags_out(t);
    if (d->rd && hd != 31) store_g(t, 1, rd, d->rd);
}

/* ---------- الذاكرة ---------- */
/* يولّد المسار السريع: x5 = العنوان الافتراضي، والنتيجة x9 = عنوان المضيف.
 * يعيد قائمة قفزات إلى المسار البطيء. */
typedef struct { Fix f[8]; int n; } FixList;

static void fast_path(Tr *t, int total, int write, FixList *fl) {
    Emit *e = &t->e;
    E(e, 0xD3400000 | (12 << 16) | (23 << 10) | (5 << 5) | 7); /* ubfx x7, x5, #12, #12 */
    E(e, 0x8B000000 | (7 << 16) | (5 << 10) | (20 << 5) | 8); /* add x8, x20, x7, lsl #5 */
    E(e, 0xF9400000 | (8 << 5) | 9);                            /* ldr x9, [x8] */
    E(e, 0xD3400000 | (12 << 16) | (63 << 10) | (5 << 5) | 6); /* lsr x6, x5, #12 */
    E(e, 0xCA000000 | (6 << 16) | (1 << 10) | (9 << 5) | 10); /* eor x10, x9, x6, lsl #1 */
    E(e, 0xF100041F | (10 << 5));                               /* cmp x10, #1 */
    fl->f[fl->n++] = fix(e, 0x54000001, 0);                     /* b.ne بطيء */
    E(e, 0x39400000 | (offsetof(TLBEntry, perm) << 10) | (8 << 5) | 9); /* ldrb w9, [x8, #perm] */
    fl->f[fl->n++] = fix(e, 0x36000000 | ((write ? t->pw : t->pr) << 19) | 9, 1); /* tbz */
    if (write) {
        E(e, 0x39400000 | (offsetof(TLBEntry, code) << 10) | (8 << 5) | 9);
        fl->f[fl->n++] = fix(e, 0x35000000 | 9, 0);             /* cbnz w9, بطيء */
    }
    E(e, 0xF9400000 | ((offsetof(TLBEntry, host) / 8) << 10) | (8 << 5) | 9); /* ldr x9, [x8, #host] */
    fl->f[fl->n++] = fix(e, 0xB4000000 | 9, 0);                 /* cbz x9, بطيء */
    E(e, 0x92402C00 | (5 << 5) | 11);                           /* and x11, x5, #0xfff */
    if (total > 1) {
        E(e, 0xF100001F | ((uint32_t)(4096 - total) << 10) | (11 << 5)); /* cmp x11, #(4096-total) */
        fl->f[fl->n++] = fix(e, 0x54000008, 0);                 /* b.hi بطيء */
    }
    E(e, 0x8B000000 | (11 << 16) | (9 << 5) | 9);               /* add x9, x9, x11 */
}

/* يترجم تعليمة تحميل/تخزين؛ يعيد 0 إذا يجب تسليمها للمفسّر */
static int tr_ldst(Tr *t, uint64_t pc, uint32_t insn) {
    Emit *e = &t->e;
    int rt = insn & 31, rn = BITS(insn, 9, 5);
    int V = BIT(insn, 26);
    int pair = 0, L, total, wb = 0, post = 0, rt2 = 31, literal = 0;
    int64_t imm = 0;
    int regoff = 0, rm = 0, opt = 0, sh = 0;
    uint32_t hostop;

    if (V && !t->fpok) return 0;

    if (BITS(insn, 29, 27) == 7) { /* مسجل واحد */
        int size = BITS(insn, 31, 30), opc = BITS(insn, 23, 22);
        int mode24 = BITS(insn, 25, 24);
        if (mode24 == 1) imm = (int64_t)BITS(insn, 21, 10);
        else if (mode24 == 0 && !BIT(insn, 21)) {
            int k = BITS(insn, 11, 10);
            if (k == 2) return 0;
            imm = SEXT(BITS(insn, 20, 12), 9);
            if (k == 1) { wb = 1; post = 1; } else if (k == 3) wb = 1;
        } else if (mode24 == 0 && BITS(insn, 11, 10) == 2) {
            regoff = 1; rm = BITS(insn, 20, 16); opt = BITS(insn, 15, 13);
            if (!(opt & 2)) return 0;
        } else return 0;
        int shift;
        if (V) {
            int sz = size | ((opc & 2) << 1);
            if (sz > 4) return 0;
            total = 1 << sz; shift = sz; L = opc & 1;
        } else {
            total = 1 << size; shift = size;
            if (size == 3 && opc >= 2) {
                if (opc == 2 && !wb) return -1; /* PRFM: لا شيء */
                return 0;
            }
            if (size == 2 && opc == 3) return 0;
            L = opc != 0;
        }
        if (mode24 == 1) imm <<= shift;
        if (regoff) sh = BIT(insn, 12) ? shift : 0;
        hostop = (insn & 0xC4C00000) | 0x39000000; /* الصيغة ذات الإزاحة الموجبة بقيمة صفر */
    } else if (BITS(insn, 29, 27) == 5 && !BIT(insn, 25)) { /* زوج */
        int opc = BITS(insn, 31, 30), m = BITS(insn, 24, 23);
        L = BIT(insn, 22);
        rt2 = BITS(insn, 14, 10);
        int scale;
        if (V) { if (opc == 3) return 0; scale = 4 << opc; }
        else {
            if (opc == 3 || (opc == 1 && !L)) return 0;
            scale = opc == 2 ? 8 : 4;
        }
        total = 2 * scale;
        imm = SEXT(BITS(insn, 21, 15), 7) * scale;
        if (m == 1) { wb = 1; post = 1; } else if (m == 3) wb = 1;
        if (L && rt == rt2) return 0;
        pair = 1;
        hostop = (insn & 0xC4400000) | 0x29000000;
    } else if (BITS(insn, 29, 27) == 3 && BITS(insn, 25, 24) == 0) { /* حرفي */
        int opc = BITS(insn, 31, 30);
        if (V) { if (opc == 3) return 0; total = 4 << opc; hostop = 0x3D400000 | ((opc == 2 ? 0 : opc + 2) << 30) | ((opc == 2) << 23); }
        else {
            if (opc == 3) return -1; /* PRFM حرفي */
            total = opc == 1 ? 8 : 4;
            hostop = opc == 2 ? 0xB9800000 : (opc == 1 ? 0xF9400000 : 0xB9400000);
        }
        L = 1; literal = 1;
        imm = SEXT(BITS(insn, 23, 5), 19) << 2;
    } else return 0;

    if (wb && rn != 31 && (rn == rt || (pair && rn == rt2)) && !V) return 0;

    /* العنوان */
    if (literal) mov64(e, 5, pc + imm);
    else {
        int hb = load_g(t, 3, rn, SEM_SP);
        if (regoff) {
            int hm = load_g(t, 4, rm, SEM_ZR);
            E(e, 0x8B200000 | (hm << 16) | (opt << 13) | (sh << 10) | (hb << 5) | 5); /* add x5, xb, xm, ext #sh */
        } else if (post) {
            add_imm(e, 5, hb, 0);
        } else add_imm(e, 5, hb, imm);
        if (wb) add_imm(e, 12, hb, imm);
    }

    /* قيم التخزين */
    int ht = 31, ht2 = 31;
    if (V) {
        ht = 16; ht2 = 17;
        if (!L) {
            add_imm(e, 6, 19, OFF(v) + 16 * rt);
            E(e, 0x3DC00000 | (6 << 5) | 16);  /* ldr q16, [x6] */
            if (pair) { add_imm(e, 6, 19, OFF(v) + 16 * rt2); E(e, 0x3DC00000 | (6 << 5) | 17); }
        }
    } else {
        if (rt != 31) ht = 1;
        if (pair && rt2 != 31) ht2 = 2;
        if (!L) {
            if (ht != 31) load_g(t, 1, rt, SEM_ZR);
            if (pair && ht2 != 31) load_g(t, 2, rt2, SEM_ZR);
        }
    }

    FixList fl = { .n = 0 };
    fast_path(t, total, !L, &fl);
    uint32_t w = hostop | (9 << 5) | ht;
    if (pair) w |= ht2 << 10;
    E(e, w);
    /* نتائج التحميل */
    if (L) {
        if (V) {
            add_imm(e, 6, 19, OFF(v) + 16 * rt);
            E(e, 0x3D800000 | (6 << 5) | 16);  /* str q16, [x6] */
            if (pair) { add_imm(e, 6, 19, OFF(v) + 16 * rt2); E(e, 0x3D800000 | (6 << 5) | 17); }
        } else {
            if (ht != 31) store_g(t, 1, rt, SEM_ZR);
            if (pair && ht2 != 31) store_g(t, 2, rt2, SEM_ZR);
        }
    }
    if (wb) store_g(t, 12, rn, SEM_SP);
    Fix done = fix(e, 0x14000000, 2);
    for (int i = 0; i < fl.n; i++) patch_to_here(e, fl.f[i]);
    call_helper(t, pc, insn, 0);
    patch_to_here(e, done);
    return 1;
}

/* ---------- الفروع ---------- */
static int tr_branch(Tr *t, uint64_t pc, uint32_t insn) {
    Emit *e = &t->e;
    if ((insn & 0x7C000000) == 0x14000000) { /* B/BL */
        uint64_t tgt = pc + (SEXT(insn & 0x3FFFFFF, 26) << 2);
        if (BIT(insn, 31)) { mov64(e, 16, pc + 4); str_x(e, 16, xoff(30)); }
        exit_const(t, tgt);
        return 1;
    }
    Fix taken;
    uint64_t tgt;
    if ((insn & 0xFF000010) == 0x54000000) { /* B.cond */
        tgt = pc + (SEXT(BITS(insn, 23, 5), 19) << 2);
        flags_in(t);
        taken = fix(e, 0x54000000 | (insn & 15), 0);
    } else if ((insn & 0x7E000000) == 0x34000000) { /* CBZ/CBNZ */
        tgt = pc + (SEXT(BITS(insn, 23, 5), 19) << 2);
        load_g(t, 1, insn & 31, SEM_ZR);
        if ((insn & 31) == 31) mov64(e, 1, 0);
        taken = fix(e, (insn & 0xFF000000) | 1, 0);
    } else if ((insn & 0x7E000000) == 0x36000000) { /* TBZ/TBNZ */
        tgt = pc + (SEXT(BITS(insn, 18, 5), 14) << 2);
        load_g(t, 1, insn & 31, SEM_ZR);
        if ((insn & 31) == 31) mov64(e, 1, 0);
        taken = fix(e, (insn & 0xFFF80000) | 1, 1);
    } else if ((insn & 0xFE1FFC1F) == 0xD61F0000 && BITS(insn, 24, 21) <= 2) { /* BR/BLR/RET */
        int opc = BITS(insn, 24, 21);
        load_g(t, 1, BITS(insn, 9, 5), SEM_ZR);
        if (BITS(insn, 9, 5) == 31) mov64(e, 1, 0);
        if (opc == 1) { mov64(e, 16, pc + 4); str_x(e, 16, xoff(30)); }
        str_x(e, 1, OFF(pc));
        exit_epilogue(t);
        return 1;
    } else return 0;
    exit_const(t, pc + 4);
    patch_to_here(e, taken);
    exit_const(t, tgt);
    return 1;
}

/* ---------- ترجمة كتلة ---------- */
static uint32_t cur_mode(CPU *c) {
    int fpen = (c->cpacr >> 20) & 3;
    int fpok = fpen == 3 || (fpen == 1 && c->el == 1);
    return (uint32_t)c->el | ((uint32_t)c->spsel << 1) | ((uint32_t)fpok << 2);
}

static void mark_code_page(Jit *j, CPU *c, uint64_t pa) {
    if (pa < RAM_BASE) return;
    uint64_t pg = (pa - RAM_BASE) >> 12;
    if (pg >= j->npages || (j->codemap[pg >> 3] & (1 << (pg & 7)))) return;
    j->codemap[pg >> 3] |= 1 << (pg & 7);
    uint64_t base = pa & ~0xFFFULL;
    for (int i = 0; i < TLB_SIZE; i++)
        if (c->tlb[i].tag && c->tlb[i].pa == base) c->tlb[i].code = 1;
}

int jit_page_is_code(Machine *m, uint64_t pa) {
    Jit *j = m->jit;
    if (!j || pa < RAM_BASE) return 0;
    uint64_t pg = (pa - RAM_BASE) >> 12;
    return pg < j->npages && (j->codemap[pg >> 3] & (1 << (pg & 7)));
}

#define TOMB ((uint32_t *)(uintptr_t)1)

static void jit_flush(Jit *j, CPU *c) {
    j->pos = j->tramp_end;
    memset(j->hash, 0, sizeof(Block) * HASH_SIZE);
    memset(j->codemap, 0, (j->npages + 7) / 8);
    for (int k = 0; k < c->m->ncpu; k++)
        for (int i = 0; i < TLB_SIZE; i++) c->m->cpus[k].tlb[i].code = 0;
    j->flush_pending = 0;
    j->ninval = 0;
    j->used = j->tombs = 0;
    j->gen++;
    j->stats_flushes++;
}

/* إبطال صفحة واحدة: تُحذف كتلها من الجدول فقط. لا يعبر الربط المباشر حدود الصفحة،
 * فلا توجد قفزات من صفحات أخرى إلى الشيفرة المحذوفة. تبقى مساحتها مهدرة حتى التفريغ الكامل. */
static void jit_invalidate_pages(Jit *j, CPU *c) {
    for (int k = 0; k < j->ninval; k++) {
        uint64_t pg = j->inval[k];
        for (uint32_t i = 0; i < HASH_SIZE; i++) {
            Block *b = &j->hash[i];
            if (b->code && b->code != TOMB && (b->pa >> 12) == pg) {
                b->code = TOMB; b->va = b->pa = ~0ULL; b->mode = ~0u;
                j->tombs++;
            }
        }
        uint64_t idx = pg - (RAM_BASE >> 12);
        if (idx < j->npages) j->codemap[idx >> 3] &= ~(1 << (idx & 7));
        for (int n = 0; n < c->m->ncpu; n++)
            for (int i = 0; i < TLB_SIZE; i++)
                if (c->m->cpus[n].tlb[i].pa >> 12 == pg) c->m->cpus[n].tlb[i].code = 0;
        j->stats_inval++;
    }
    j->ninval = 0;
    j->gen++;
}

void jit_code_write(CPU *c, uint64_t pa) {
    Jit *j = c->m->jit;
    if (!j || !jit_page_is_code(c->m, pa)) return;
    uint64_t pg = pa >> 12;
    int seen = 0;
    for (int k = 0; k < j->ninval; k++) if (j->inval[k] == pg) seen = 1;
    if (!seen) {
        if (j->ninval < 64) j->inval[j->ninval++] = pg;
        else j->flush_pending = 1;
    }
    c->jit_exit_req = 1;
    c->jit_budget = 0;
}

void jit_dma_write(Machine *m, uint64_t pa, uint64_t len) {
    if (!m->jit) return;
    for (uint64_t a = pa & ~0xFFFULL; a < pa + len; a += 0x1000)
        if (jit_page_is_code(m, a)) jit_code_write(m->cur, a);
}

static inline uint32_t hkey(uint64_t va, uint64_t pa, uint32_t mode) {
    uint64_t h = (va >> 2) * 0x9E3779B97F4A7C15ULL ^ (pa >> 12) * 0xC2B2AE3D27D4EB4FULL ^ mode;
    return (uint32_t)(h >> 40) & (HASH_SIZE - 1);
}

static Block *lookup(Jit *j, uint64_t va, uint64_t pa, uint32_t mode) {
    uint32_t i = hkey(va, pa, mode);
    for (;;) {
        Block *b = &j->hash[i];
        if (!b->code) return b; /* خانة فارغة */
        if (b->va == va && b->pa == pa && b->mode == mode) return b;
        i = (i + 1) & (HASH_SIZE - 1);
    }
}

static uint32_t *translate_block(Jit *j, CPU *c, uint64_t va, uint64_t pa, const uint8_t *host, uint32_t mode, int *ok) {
    Tr t;
    memset(&t, 0, sizeof(t));
    t.j = j; t.c = c; t.start = va; t.page = va & ~0xFFFULL;
    t.el = mode & 1; t.fpok = (mode >> 2) & 1;
    t.sp_off = (t.el == 0 || !((mode >> 1) & 1)) ? OFF(sp_el0) : OFF(sp_el1);
    t.pr = t.el ? 3 : 0; t.pw = t.el ? 4 : 1;
    t.e.j = j;
    t.e.buf = (uint32_t *)(j->rw + j->pos);
    t.e.rxbase = (uint32_t *)(j->rx + j->pos);
    t.e.cap = (CODE_SIZE - j->pos) / 4;
    if (t.e.cap > 65536) t.e.cap = 65536;
    if (t.e.cap < 4096) { *ok = 0; return NULL; }
    Emit *e = &t.e;

    /* المقدمة: فحص الميزانية */
    ldr_x(e, 16, OFF(jit_budget));
    E(e, 0xF100001F | (16 << 5));          /* cmp x16, #0 */
    Fix ok_f = fix(e, 0x5400000C, 0);       /* b.gt متابعة */
    mov64(e, 16, va); str_x(e, 16, OFF(pc)); exit_epilogue(&t);
    patch_to_here(e, ok_f);
    size_t sub_at = e->n;
    E(e, 0xD1000000 | (16 << 5) | 16);      /* sub x16, x16, #n */
    str_x(e, 16, OFF(jit_budget));

    int n = 0;
    uint64_t pc = va;
    for (;;) {
        if (n > 0 && ((pc & 0xFFF) == 0 || n >= MAX_BLOCK)) { exit_const(&t, pc); break; }
        uint32_t insn;
        memcpy(&insn, host + (pc & 0xFFF), 4);
        n++;
        uint32_t op = BITS(insn, 28, 25);
        int done = 0, handled = 0;
        AluDesc d;
        if ((op & 0xE) == 8 || (op & 7) == 5) {
            if ((op & 0xE) == 8 && BITS(insn, 25, 24) == 0) { /* ADR/ADRP */
                int64_t immv = (int64_t)((SEXT(BITS(insn, 23, 5), 19) << 2) | BITS(insn, 30, 29));
                uint64_t val = BIT(insn, 31) ? (pc & ~0xFFFULL) + (immv << 12) : pc + immv;
                if ((insn & 31) != 31) { mov64(e, 1, val); str_x(e, 1, xoff(insn & 31)); }
                handled = 1;
            } else if (alu_desc(insn, &d)) { tr_alu(&t, insn, &d); handled = 1; }
        } else if ((op & 5) == 4) {
            int r = tr_ldst(&t, pc, insn);
            if (r) handled = 1;
        } else if ((op & 0xE) == 0xA) {
            if (tr_branch(&t, pc, insn)) { handled = 1; done = 1; }
            else if ((insn & 0xFFFFF01F) == 0xD503201F) { /* تلميحات */
                int h = BITS(insn, 11, 5);
                if (h != 3) handled = 1;     /* WFI يحتاج المفسّر */
            } else if ((insn & 0xFFFFF09F) == 0xD503309F && BITS(insn, 7, 5) >= 4) {
                handled = 1;                 /* DSB/DMB/ISB: لا شيء في معالج واحد */
            } else if ((insn & 0xFFF00000) == 0xD5300000) {
                call_helper(&t, pc, insn, 0); /* MRS */
                handled = 1;
            }
        }
        if (!handled) {
            int sys = (op & 0xE) == 0xA; /* تعليمات النظام والاستثناءات: اخرج دائمًا */
            call_helper(&t, pc, insn, sys);
            if (sys) {
                done = 1;
            }
        }
        pc += 4;
        if (done) break;
        if (e->overflow) break;
    }
    if (e->overflow) { *ok = 0; return NULL; }
    e->buf[sub_at] |= (uint32_t)(n > 4095 ? 4095 : n) << 10;
    uint32_t *rx = (uint32_t *)(j->rx + j->pos);
    icache_sync(j, rx, e->n * 4);
    j->pos += (e->n * 4 + 63) & ~63ULL;
    j->stats_blocks++;
    mark_code_page(j, c, pa);
    *ok = 1;
    return rx;
}

/* ---------- الإنشاء والتشغيل ---------- */
void *jit_create(Machine *m) {
    Jit *j = calloc(1, sizeof(Jit));
    if (!jit_alloc(j)) { free(j); return NULL; }
    j->hash = calloc(HASH_SIZE, sizeof(Block));
    j->npages = m->ram_size >> 12;
    j->codemap = calloc((j->npages + 7) / 8, 1);
    uint32_t *w = (uint32_t *)j->rw;
    int k = 0;
    /* enter(cpu, tlb, code) */
    w[k++] = 0xA9BE7BFD; /* stp x29, x30, [sp, #-32]! */
    w[k++] = 0xA90153F3; /* stp x19, x20, [sp, #16] */
    w[k++] = 0xAA0003F3; /* mov x19, x0 */
    w[k++] = 0xAA0103F4; /* mov x20, x1 */
    w[k++] = 0xD61F0040; /* br x2 */
    j->exit_rx = (uint32_t *)j->rx + k;
    w[k++] = 0xA94153F3; /* ldp x19, x20, [sp, #16] */
    w[k++] = 0xA8C27BFD; /* ldp x29, x30, [sp], #32 */
    w[k++] = 0xD65F03C0; /* ret */
    icache_sync(j, j->rx, k * 4);
    j->enter = (void (*)(CPU *, void *, void *))(void *)j->rx;
    j->tramp_end = 64;
    j->pos = j->tramp_end;
    m->jit = j;
    return j;
}

/* يُعرَّف في cpu.c */
const uint8_t *cpu_code_ptr(CPU *c, uint64_t pc, uint64_t *pa);
void cpu_exec_insn(CPU *c, uint32_t insn);

uint64_t jit_helper(CPU *c, uint64_t pc, uint32_t insn) {
    uint32_t m0 = cur_mode(c);
    c->cur_pc = pc;
    c->cur_insn = insn;
    c->pc = pc + 4;
    cpu_exec_insn(c, insn);
    return c->pc != pc + 4 || c->stop || c->halted || c->jit_exit_req || cur_mode(c) != m0;
}

void jit_exec(CPU *c) {
    Jit *j = c->m->jit;
    uint32_t *pending_slot = NULL;
    c->jit_exit_req = 0;
    while (c->jit_budget > 0 && !c->jit_exit_req && !c->stop && !c->halted) {
        if (j->flush_pending) { jit_flush(j, c); pending_slot = NULL; }
        else if (j->ninval) { jit_invalidate_pages(j, c); pending_slot = NULL; }
        if (j->used + j->tombs > HASH_SIZE * 3 / 4) { jit_flush(j, c); pending_slot = NULL; }
        uint64_t va = c->pc, pa;
        c->cur_pc = va;
        const uint8_t *host = cpu_code_ptr(c, va, &pa);
        uint32_t mode = cur_mode(c);
        Block *b = lookup(j, va, pa, mode);
        if (!b->code) {
            int ok;
            uint32_t *code = translate_block(j, c, va, pa, host, mode, &ok);
            if (!ok) { jit_flush(j, c); pending_slot = NULL; b = lookup(j, va, pa, mode);
                       code = translate_block(j, c, va, pa, host, mode, &ok);
                       if (!ok) { fprintf(stderr, "[gtv] فشل JIT\n"); exit(4); } }
            b->va = va; b->pa = pa; b->mode = mode; b->code = code;
            j->used++;
        }
        if (pending_slot) {
            /* ربط مباشر: رقّع قفزة الخروج لتذهب إلى الكتلة الهدف */
            uint32_t *rwp = (uint32_t *)(j->rw + ((uint8_t *)pending_slot - j->rx));
            int64_t off = b->code - pending_slot;
            *rwp = 0x14000000 | ((uint32_t)off & 0x3FFFFFF);
            icache_sync(j, pending_slot, 4);
            j->stats_chains++;
            pending_slot = NULL;
        }
        c->jit_last_slot = 0;
        j->enter(c, c->tlb, b->code);
        if (c->jit_last_slot && !j->flush_pending && !j->ninval) pending_slot = (uint32_t *)(uintptr_t)c->jit_last_slot;
    }
}

void jit_stats(Machine *m) {
    Jit *j = m->jit;
    if (!j) return;
    fprintf(stderr, "[gtv] JIT: %llu كتلة، %llu ربط مباشر، %llu تفريغ كامل، %llu إبطال صفحة\n",
            (unsigned long long)j->stats_blocks, (unsigned long long)j->stats_chains,
            (unsigned long long)j->stats_flushes, (unsigned long long)j->stats_inval);
}

#else /* لا يوجد مضيف ARM64: يعمل المفسّر وحده */

void gtv_set_jit_mode(int mode) { (void)mode; }
const char *gtv_jit_status(void) { return "غير متاح: المضيف ليس ARM64"; }

void *jit_create(Machine *m) { (void)m; return NULL; }
int jit_page_is_code(Machine *m, uint64_t pa) { (void)m; (void)pa; return 0; }
void jit_code_write(CPU *c, uint64_t pa) { (void)c; (void)pa; }
void jit_dma_write(Machine *m, uint64_t pa, uint64_t len) { (void)m; (void)pa; (void)len; }
void jit_exec(CPU *c) { (void)c; }
void jit_stats(Machine *m) { (void)m; }

#endif
