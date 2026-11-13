/* اختبارات وحدة لدوال المعالج الأساسية: فك أقنعة البتات، والجمع مع الحمل، والشروط، والإزاحة. */
#include "../core/cpu.c"
#include <assert.h>

static int fails;
#define EXPECT(cond, msg) do { if (!(cond)) { printf("فشل: %s\n", msg); fails++; } else printf("نجح: %s\n", msg); } while (0)

int main(void) {
    uint64_t m;
    EXPECT(decode_bitmask(1, 1, 0, 0, &m) && m == 1, "قناع 64 بت لبت واحد");
    EXPECT(decode_bitmask(1, 0, 0, 0x3C, &m) && m == 0x5555555555555555ULL, "قناع متكرر 0x55..");
    EXPECT(decode_bitmask(0, 0, 0, 0x27, &m) && m == 0x00FF00FFULL, "قناع 32 بت 0x00FF00FF");
    EXPECT(!decode_bitmask(1, 1, 0, 0x3F, &m), "رفض قناع غير صالح");
    uint32_t f;
    EXPECT(add_with_carry(0xFFFFFFFFFFFFFFFFULL, 1, 0, 1, &f) == 0 && f == 0x60000000, "حمل وصفر في الجمع 64 بت");
    EXPECT(add_with_carry(0x7FFFFFFF, 1, 0, 0, &f) == 0x80000000ULL && f == 0x90000000, "فيضان موقّع 32 بت");
    EXPECT(add_with_carry(5, ~5ULL, 1, 1, &f) == 0 && f == 0x60000000, "طرح يساوي صفرًا (CMP)");
    EXPECT(shift_reg(0x80000000ULL, 2, 4, 0) == 0xF8000000ULL, "إزاحة حسابية 32 بت");
    EXPECT(shift_reg(1, 3, 1, 1) == 0x8000000000000000ULL, "تدوير 64 بت");
    EXPECT(extend_reg(0x80, 4, 0) == 0xFFFFFFFFFFFFFF80ULL, "تمديد SXTB");
    CPU c; memset(&c, 0, sizeof c);
    c.nzcv = 0x60000000;
    EXPECT(cond_holds(&c, 0) && !cond_holds(&c, 1) && cond_holds(&c, 2) && cond_holds(&c, 9), "تقييم الشروط EQ/NE/CS/LS");
    uint8_t dtb[8192];
    size_t n = fdt_build(dtb, sizeof dtb, 512ULL << 20, "console=ttyAMA0", 0, 0, 1);
    EXPECT(n > 64 && dtb[0] == 0xd0 && dtb[1] == 0x0d && dtb[2] == 0xfe && dtb[3] == 0xed, "بناء شجرة أجهزة صالحة");
    printf(fails ? "\nبعض الاختبارات فشلت\n" : "\nكل الاختبارات نجحت\n");
    return fails != 0;
}
