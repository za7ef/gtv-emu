/* gtv-emu — محاكي AArch64 لنظام Android TV
 * الترويسة المشتركة لنواة المحاكي.
 */
#ifndef GTV_EMU_H
#define GTV_EMU_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

/* ---------- خريطة الذاكرة الفيزيائية للآلة ---------- */
#define GIC_DIST_BASE   0x08000000ULL
#define GIC_DIST_SIZE   0x00010000ULL
#define GIC_REDIST_BASE 0x080A0000ULL
#define GIC_REDIST_SIZE 0x00020000ULL
#define UART_BASE       0x09000000ULL
#define UART_SIZE       0x00001000ULL
#define VIRTIO_BASE     0x0A000000ULL
#define VIRTIO_SIZE     0x00000200ULL
#define VIRTIO_COUNT    4   /* 0 قرص، 1 شبكة، 2 رسوميات، 3 لوحة مفاتيح */
#define RAM_BASE        0x40000000ULL

#define IRQ_UART   33   /* SPI 1  */
#define IRQ_VIRTIO 48   /* SPI 16 وما بعده */
#define IRQ_VTIMER 27   /* PPI 11 */
#define IRQ_PTIMER 30   /* PPI 14 */

#define TIMER_FREQ 62500000ULL

typedef struct { uint64_t lo, hi; } v128;

typedef struct {
    uint64_t tag;      /* (va >> 12) | 1 عند الصلاحية */
    uint8_t *host;     /* مؤشر صفحة الذاكرة على المضيف، أو NULL لأجهزة MMIO */
    uint64_t pa;       /* العنوان الفيزيائي للصفحة */
    uint8_t  perm;     /* بتات الصلاحيات */
    uint8_t  code;     /* الصفحة تحتوي شيفرة مترجمة: الكتابة تمر بالمسار البطيء */
} TLBEntry;

#define TLB_SIZE 4096
#define P_R0 1
#define P_W0 2
#define P_X0 4
#define P_R1 8
#define P_W1 16
#define P_X1 32

struct Machine;

typedef struct CPU {
    uint64_t x[32];          /* x[31] غير مستخدم (XZR/SP) */
    uint64_t sp_el0, sp_el1;
    uint64_t pc;
    uint32_t nzcv;           /* البتات 31..28 */
    uint32_t daif;           /* البتات 9..6 */
    int el;                  /* 0 أو 1 */
    int spsel;
    v128 v[32];
    uint32_t fpcr, fpsr;

    /* مسجلات النظام */
    uint64_t sctlr, ttbr0, ttbr1, tcr, mair, amair, vbar, esr, far, elr, spsr;
    uint64_t tpidr_el0, tpidrro_el0, tpidr_el1, contextidr, cpacr, par;
    uint64_t cntkctl, cntv_ctl, cntv_cval, cntp_ctl, cntp_cval, cntvoff;
    uint64_t mdscr, afsr0, afsr1, actlr;
    uint64_t sp_save;

    /* المراقب الحصري */
    uint64_t excl_addr;
    int excl_valid;

    /* حالة المترجم الفوري */
    int64_t jit_budget;       /* التعليمات المسموح بتنفيذها قبل العودة إلى الحلقة */
    int64_t jit_given;
    uint64_t jit_last_slot;   /* آخر قفزة خروج قابلة للربط */
    int jit_exit_req;
    int jit_in;

    /* ذاكرة الترجمة المؤقتة */
    TLBEntry tlb[TLB_SIZE];
    uint64_t fetch_tag;
    uint8_t *fetch_host;

    uint64_t icount;
    uint64_t cur_pc;          /* عنوان التعليمة الجارية */
    uint32_t cur_insn;
    jmp_buf jb;
    int halted;               /* WFI */
    int stop;                 /* طلب إيقاف */
    int exit_code;
    struct Machine *m;
} CPU;

typedef struct Machine {
    CPU cpu;
    uint8_t *ram;
    uint64_t ram_size;
    void *gic;
    void *uart;
    void *virtio[VIRTIO_COUNT];
    int irq_line;            /* خط المقاطعة إلى المعالج */
    uint64_t start_ns;
    int trace;
    int verbose;
    int quiet_unimpl;
    void *jit;               /* NULL عند العمل بالمفسّر وحده */
} Machine;

/* cpu.c */
void cpu_reset(CPU *c, Machine *m);
int  cpu_run(CPU *c, uint64_t max_insns);
void tlb_flush(CPU *c);
uint64_t cpu_counter(Machine *m);

/* simd.c — يعيد 1 إذا نُفّذت التعليمة */
int exec_simd(CPU *c, uint32_t insn);
int exec_simd_ldst(CPU *c, uint32_t insn);
void fp_check(CPU *c);

/* واجهة الذاكرة المستخدمة من simd.c */
uint64_t mem_read(CPU *c, uint64_t va, int size);
void mem_write(CPU *c, uint64_t va, int size, uint64_t val);
uint64_t xreg(CPU *c, int n);       /* XZR عند 31 */
uint64_t xreg_sp(CPU *c, int n);    /* SP عند 31 */
void set_xreg(CPU *c, int n, uint64_t v);
void set_xreg_sp(CPU *c, int n, uint64_t v);
void raise_undef(CPU *c);

/* devices.c */
void *gic_create(Machine *m);
uint64_t gic_mmio_read(Machine *m, uint64_t pa, int size);
void gic_mmio_write(Machine *m, uint64_t pa, int size, uint64_t v);
void gic_set_irq(Machine *m, int irq, int level);
int  gic_sysreg_read(Machine *m, uint32_t enc, uint64_t *v);
int  gic_sysreg_write(Machine *m, uint32_t enc, uint64_t v);
void gic_update(Machine *m);

void *uart_create(Machine *m);
uint64_t uart_read(Machine *m, uint64_t off, int size);
void uart_write(Machine *m, uint64_t off, int size, uint64_t v);
void uart_poll(Machine *m);
int  uart_has_input(Machine *m);
void uart_feed(Machine *m, const char *s);

/* virtio.c */
void virtio_create_all(Machine *m, const char *disk, const char *keys);
void virtio_poll(Machine *m);
int  virtio_has_work(Machine *m);
int  gpu_screenshot(Machine *m, const char *path);
void input_arm_keys(Machine *m);
uint64_t virtio_read(Machine *m, int idx, uint64_t off, int size);
void virtio_write(Machine *m, int idx, uint64_t off, int size, uint64_t v);

void timer_update(Machine *m);
uint64_t timer_next_deadline(Machine *m);

/* jit.c */
void *jit_create(Machine *m);
int  jit_page_is_code(Machine *m, uint64_t pa);
void jit_code_write(CPU *c, uint64_t pa);
void jit_dma_write(Machine *m, uint64_t pa, uint64_t len);
void jit_exec(CPU *c);
void jit_stats(Machine *m);

/* fdt.c */
size_t fdt_build(uint8_t *buf, size_t cap, uint64_t ram_size, const char *bootargs,
                 uint64_t initrd_start, uint64_t initrd_end, int nvirtio);

uint64_t host_ns(void);

#endif
