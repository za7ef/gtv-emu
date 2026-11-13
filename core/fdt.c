/* gtv-emu — بناء شجرة الأجهزة (FDT) للآلة الافتراضية. */
#include "emu.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t *st; size_t st_len, st_cap;
    char strs[2048]; size_t s_len;
} FDT;

static void put32(FDT *f, uint32_t v) {
    if (f->st_len + 4 > f->st_cap) return;
    f->st[f->st_len++] = v >> 24; f->st[f->st_len++] = v >> 16;
    f->st[f->st_len++] = v >> 8;  f->st[f->st_len++] = v;
}
static void putbytes(FDT *f, const void *d, size_t n) {
    if (f->st_len + n + 4 > f->st_cap) return;
    memcpy(f->st + f->st_len, d, n);
    f->st_len += n;
    while (f->st_len & 3) f->st[f->st_len++] = 0;
}
static uint32_t stroff(FDT *f, const char *s) {
    size_t i = 0;
    while (i < f->s_len) {
        if (!strcmp(f->strs + i, s)) return i;
        i += strlen(f->strs + i) + 1;
    }
    size_t n = strlen(s) + 1;
    memcpy(f->strs + f->s_len, s, n);
    f->s_len += n;
    return i;
}
static void begin(FDT *f, const char *name) { put32(f, 1); putbytes(f, name, strlen(name) + 1); }
static void end(FDT *f) { put32(f, 2); }
static void prop(FDT *f, const char *name, const void *d, size_t n) {
    put32(f, 3); put32(f, n); put32(f, stroff(f, name)); putbytes(f, d, n);
}
static void prop_str(FDT *f, const char *name, const char *s) { prop(f, name, s, strlen(s) + 1); }
static void prop_u32s(FDT *f, const char *name, const uint32_t *v, int n) {
    uint8_t b[256];
    for (int i = 0; i < n; i++) {
        b[4*i] = v[i] >> 24; b[4*i+1] = v[i] >> 16; b[4*i+2] = v[i] >> 8; b[4*i+3] = v[i];
    }
    prop(f, name, b, 4 * n);
}
static void prop_u32(FDT *f, const char *name, uint32_t v) { prop_u32s(f, name, &v, 1); }
static void prop_u64(FDT *f, const char *name, uint64_t v) {
    uint32_t a[2] = { (uint32_t)(v >> 32), (uint32_t)v };
    prop_u32s(f, name, a, 2);
}
static void prop_strlist(FDT *f, const char *name, const char *a, const char *b) {
    char buf[256]; size_t la = strlen(a) + 1, lb = strlen(b) + 1;
    memcpy(buf, a, la); memcpy(buf + la, b, lb);
    prop(f, name, buf, la + lb);
}

size_t fdt_build(uint8_t *buf, size_t cap, uint64_t ram_size, const char *bootargs,
                 uint64_t initrd_start, uint64_t initrd_end, int nvirtio) {
    static uint8_t st[16384];
    FDT f = { st, 0, sizeof(st), {0}, 0 };
    char nb[64];

    begin(&f, "");
    prop_u32(&f, "#address-cells", 2);
    prop_u32(&f, "#size-cells", 2);
    prop_str(&f, "compatible", "linux,dummy-virt");
    prop_str(&f, "model", "gtv-emu");
    prop_u32(&f, "interrupt-parent", 1);

    begin(&f, "chosen");
    prop_str(&f, "bootargs", bootargs);
    prop_str(&f, "stdout-path", "/pl011@9000000");
    if (initrd_end > initrd_start) {
        prop_u64(&f, "linux,initrd-start", initrd_start);
        prop_u64(&f, "linux,initrd-end", initrd_end);
    }
    end(&f);

    snprintf(nb, sizeof nb, "memory@%llx", (unsigned long long)RAM_BASE);
    begin(&f, nb);
    prop_str(&f, "device_type", "memory");
    { uint32_t r[4] = { 0, (uint32_t)RAM_BASE, (uint32_t)(ram_size >> 32), (uint32_t)ram_size }; prop_u32s(&f, "reg", r, 4); }
    end(&f);

    begin(&f, "cpus");
    prop_u32(&f, "#address-cells", 1);
    prop_u32(&f, "#size-cells", 0);
    begin(&f, "cpu@0");
    prop_str(&f, "device_type", "cpu");
    prop_str(&f, "compatible", "arm,cortex-a72");
    prop_u32(&f, "reg", 0);
    end(&f);
    end(&f);

    begin(&f, "psci");
    prop_strlist(&f, "compatible", "arm,psci-1.0", "arm,psci-0.2");
    prop_str(&f, "method", "hvc");
    end(&f);

    begin(&f, "timer");
    prop_str(&f, "compatible", "arm,armv8-timer");
    { uint32_t ir[12] = { 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4 }; prop_u32s(&f, "interrupts", ir, 12); }
    prop(&f, "always-on", NULL, 0);
    end(&f);

    begin(&f, "intc@8000000");
    prop_str(&f, "compatible", "arm,gic-v3");
    prop_u32(&f, "#interrupt-cells", 3);
    prop(&f, "interrupt-controller", NULL, 0);
    prop_u32(&f, "#redistributor-regions", 1);
    { uint32_t r[8] = { 0, (uint32_t)GIC_DIST_BASE, 0, (uint32_t)GIC_DIST_SIZE,
                        0, (uint32_t)GIC_REDIST_BASE, 0, (uint32_t)GIC_REDIST_SIZE }; prop_u32s(&f, "reg", r, 8); }
    prop_u32(&f, "phandle", 1);
    end(&f);

    begin(&f, "apb-pclk");
    prop_str(&f, "compatible", "fixed-clock");
    prop_u32(&f, "#clock-cells", 0);
    prop_u32(&f, "clock-frequency", 24000000);
    prop_str(&f, "clock-output-names", "clk24mhz");
    prop_u32(&f, "phandle", 2);
    end(&f);

    begin(&f, "pl011@9000000");
    prop_strlist(&f, "compatible", "arm,pl011", "arm,primecell");
    { uint32_t r[4] = { 0, (uint32_t)UART_BASE, 0, (uint32_t)UART_SIZE }; prop_u32s(&f, "reg", r, 4); }
    { uint32_t ir[3] = { 0, IRQ_UART - 32, 4 }; prop_u32s(&f, "interrupts", ir, 3); }
    { uint32_t ck[2] = { 2, 2 }; prop_u32s(&f, "clocks", ck, 2); }
    prop_strlist(&f, "clock-names", "uartclk", "apb_pclk");
    end(&f);

    for (int i = 0; i < nvirtio; i++) {
        uint64_t base = VIRTIO_BASE + i * VIRTIO_SIZE;
        snprintf(nb, sizeof nb, "virtio_mmio@%llx", (unsigned long long)base);
        begin(&f, nb);
        prop_str(&f, "compatible", "virtio,mmio");
        { uint32_t r[4] = { 0, (uint32_t)base, 0, (uint32_t)VIRTIO_SIZE }; prop_u32s(&f, "reg", r, 4); }
        { uint32_t ir[3] = { 0, (uint32_t)(IRQ_VIRTIO - 32 + i), 4 }; prop_u32s(&f, "interrupts", ir, 3); }
        prop(&f, "dma-coherent", NULL, 0);
        end(&f);
    }

    end(&f);
    put32(&f, 9);

    /* الترويسة */
    size_t hdr = 40, rsv = 16;
    size_t off_struct = hdr + rsv;
    size_t off_strings = off_struct + f.st_len;
    size_t total = off_strings + f.s_len;
    if (total > cap) return 0;
    memset(buf, 0, total);
    uint32_t h[10] = { 0xd00dfeed, (uint32_t)total, (uint32_t)off_struct, (uint32_t)off_strings, (uint32_t)hdr,
                       17, 16, 0, (uint32_t)f.s_len, (uint32_t)f.st_len };
    for (int i = 0; i < 10; i++) {
        buf[4*i] = h[i] >> 24; buf[4*i+1] = h[i] >> 16; buf[4*i+2] = h[i] >> 8; buf[4*i+3] = h[i];
    }
    memcpy(buf + off_struct, f.st, f.st_len);
    memcpy(buf + off_strings, f.strs, f.s_len);
    return total;
}
