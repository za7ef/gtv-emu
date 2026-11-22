/* gtv-emu — تعليمات النقطة العائمة وAdvanced SIMD (NEON). */
#include "emu.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define BIT(x, n) (((x) >> (n)) & 1)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1ULL << ((hi) - (lo) + 1)) - 1))
#define SEXT(v, bits) ((int64_t)((uint64_t)(v) << (64 - (bits))) >> (64 - (bits)))
#define MASK(n) ((n) >= 64 ? ~0ULL : ((1ULL << (n)) - 1))

/* ---------- أدوات العناصر ---------- */
static inline uint64_t lane(const v128 *v, int esz, int i) {
    const uint8_t *b = (const uint8_t *)v;
    switch (esz) {
    case 1: return b[i];
    case 2: { uint16_t x; memcpy(&x, b + 2 * i, 2); return x; }
    case 4: { uint32_t x; memcpy(&x, b + 4 * i, 4); return x; }
    default: { uint64_t x; memcpy(&x, b + 8 * i, 8); return x; }
    }
}
static inline void set_lane(v128 *v, int esz, int i, uint64_t x) {
    uint8_t *b = (uint8_t *)v;
    switch (esz) {
    case 1: b[i] = x; break;
    case 2: { uint16_t y = x; memcpy(b + 2 * i, &y, 2); break; }
    case 4: { uint32_t y = x; memcpy(b + 4 * i, &y, 4); break; }
    default: memcpy(b + 8 * i, &x, 8); break;
    }
}
static inline int64_t slane(const v128 *v, int esz, int i) { return SEXT(lane(v, esz, i), esz * 8); }

static inline float getf(CPU *c, int n) { float f; uint32_t u = (uint32_t)c->v[n].lo; memcpy(&f, &u, 4); return f; }
static inline double getd(CPU *c, int n) { double d; memcpy(&d, &c->v[n].lo, 8); return d; }
static inline void setf(CPU *c, int n, float f) { uint32_t u; memcpy(&u, &f, 4); c->v[n].lo = u; c->v[n].hi = 0; }
static inline void setd(CPU *c, int n, double d) { memcpy(&c->v[n].lo, &d, 8); c->v[n].hi = 0; }
static inline float u2f(uint64_t u) { float f; uint32_t x = u; memcpy(&f, &x, 4); return f; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t f2u(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
static inline uint64_t d2u(double d) { uint64_t x; memcpy(&x, &d, 8); return x; }

static int fcond(CPU *c, int cond) {
    uint32_t f = c->nzcv;
    int N = BIT(f, 31), Z = BIT(f, 30), C = BIT(f, 29), V = BIT(f, 28), r;
    switch (cond >> 1) {
    case 0: r = Z; break; case 1: r = C; break; case 2: r = N; break; case 3: r = V; break;
    case 4: r = C && !Z; break; case 5: r = N == V; break; case 6: r = (N == V) && !Z; break;
    default: r = 1;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static double round_mode(double x, int mode) {
    switch (mode) {
    case 0: return nearbyint(x);          /* أقرب زوجي */
    case 1: return ceil(x);
    case 2: return floor(x);
    case 3: return trunc(x);
    case 4: return round(x);              /* بعيدًا عن الصفر */
    }
    return x;
}

static uint64_t fp_to_int(double x, int mode, int is_unsigned, int bits) {
    if (isnan(x)) return 0;
    x = round_mode(x, mode);
    if (is_unsigned) {
        if (x <= 0) return 0;
        if (bits == 64) { if (x >= 18446744073709551616.0) return ~0ULL; return (uint64_t)x; }
        if (x >= 4294967295.0) return 0xFFFFFFFFULL; return (uint64_t)x;
    } else {
        if (bits == 64) {
            if (x >= 9223372036854775808.0) return 0x7FFFFFFFFFFFFFFFULL;
            if (x < -9223372036854775808.0) return 0x8000000000000000ULL;
            return (uint64_t)(int64_t)x;
        }
        if (x >= 2147483647.0) return 0x7FFFFFFF;
        if (x <= -2147483648.0) return 0x80000000;
        return (uint64_t)(uint32_t)(int32_t)x;
    }
}

static void fcmp_flags(CPU *c, double a, double b) {
    if (isnan(a) || isnan(b)) c->nzcv = 0x30000000;
    else if (a == b) c->nzcv = 0x60000000;
    else if (a < b) c->nzcv = 0x80000000;
    else c->nzcv = 0x20000000;
}

static uint64_t vfp_expand_imm(int imm8, int dbl) {
    int a = BIT(imm8, 7), b = BIT(imm8, 6), cd = BITS(imm8, 5, 4), efgh = imm8 & 15;
    if (dbl) {
        uint64_t exp = ((uint64_t)!b << 10) | (b ? 0x3FCULL : 0) | cd;
        return ((uint64_t)a << 63) | (exp << 52) | ((uint64_t)efgh << 48);
    } else {
        uint64_t exp = ((uint64_t)!b << 7) | (b ? 0x7CULL : 0) | cd;
        return ((uint64_t)a << 31) | (exp << 23) | ((uint64_t)efgh << 19);
    }
}

static double fmaxnm_(double a, double b, int mx) {
    if (isnan(a)) return b;
    if (isnan(b)) return a;
    return mx ? (a > b ? a : b) : (a < b ? a : b);
}

/* ---------- العمليات العائمة القياسية ---------- */
static int exec_fp_scalar(CPU *c, uint32_t insn) {
    int type = BITS(insn, 23, 22);
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16);
    int dbl = type == 1;
    if (type == 3 || type == 2) {
        /* نصف الدقة غير مدعوم إلا FMOV العلوي */
        if (!(BITS(insn, 15, 10) == 0 && type == 2)) return 0;
    }
    if (BITS(insn, 11, 10) == 0) {
        if (BITS(insn, 15, 10) == 0) { /* تحويل بين صحيح وعائم */
            int sf = BIT(insn, 31), rmode = BITS(insn, 20, 19), opc = BITS(insn, 18, 16);
            int bits = sf ? 64 : 32;
            if (opc == 6 || opc == 7) { /* FMOV */
                if (type == 2) { /* V.D[1] */
                    if (rmode != 1 || !sf) return 0;
                    if (opc == 6) set_xreg(c, rd, c->v[rn].hi);
                    else c->v[rd].hi = xreg(c, rn);
                    return 1;
                }
                if (opc == 6) set_xreg(c, rd, dbl ? c->v[rn].lo : (uint32_t)c->v[rn].lo);
                else { uint64_t x = xreg(c, rn); c->v[rd].lo = dbl ? x : (uint32_t)x; c->v[rd].hi = 0; }
                return 1;
            }
            if (opc == 2 || opc == 3) { /* SCVTF/UCVTF */
                if (rmode != 0) return 0;
                uint64_t x = xreg(c, rn);
                double v;
                if (opc == 2) v = sf ? (double)(int64_t)x : (double)(int32_t)x;
                else v = sf ? (double)x : (double)(uint32_t)x;
                if (dbl) {
                    if (opc == 2 && sf) v = (double)(int64_t)x;
                    setd(c, rd, v);
                } else {
                    float f = (opc == 2) ? (sf ? (float)(int64_t)x : (float)(int32_t)x) : (sf ? (float)x : (float)(uint32_t)x);
                    setf(c, rd, f);
                }
                return 1;
            }
            if (opc == 0 || opc == 1 || opc == 4 || opc == 5) {
                double v = dbl ? getd(c, rn) : getf(c, rn);
                int mode;
                if (opc >= 4) { if (rmode != 0) return 0; mode = 4; }
                else mode = rmode == 0 ? 0 : rmode == 1 ? 1 : rmode == 2 ? 2 : 3;
                uint64_t r = fp_to_int(v, mode, opc & 1, bits);
                set_xreg(c, rd, sf ? r : (r & 0xFFFFFFFF));
                return 1;
            }
            return 0;
        }
        if (BITS(insn, 14, 10) == 0x10) { /* أحادي المصدر */
            int opc = BITS(insn, 20, 15);
            double v = dbl ? getd(c, rn) : getf(c, rn);
            switch (opc) {
            case 0: c->v[rd].lo = dbl ? c->v[rn].lo : (uint32_t)c->v[rn].lo; c->v[rd].hi = 0; return 1;
            case 1: if (dbl) setd(c, rd, fabs(v)); else setf(c, rd, fabsf((float)v)); return 1;
            case 2: if (dbl) c->v[rd].lo = c->v[rn].lo ^ (1ULL << 63); else c->v[rd].lo = (uint32_t)c->v[rn].lo ^ 0x80000000u; c->v[rd].hi = 0; return 1;
            case 3: if (dbl) setd(c, rd, sqrt(v)); else setf(c, rd, sqrtf((float)v)); return 1;
            case 4: setf(c, rd, (float)v); return 1;
            case 5: setd(c, rd, v); return 1;
            case 8: case 9: case 10: case 11: case 12: case 14: case 15: {
                int mode = opc == 8 ? 0 : opc == 9 ? 1 : opc == 10 ? 2 : opc == 11 ? 3 : opc == 12 ? 4 : 0;
                double r = round_mode(v, mode);
                if (dbl) setd(c, rd, r); else setf(c, rd, (float)r);
                return 1;
            }
            }
            return 0;
        }
        if (BITS(insn, 13, 10) == 0x8) { /* FCMP */
            double a = dbl ? getd(c, rn) : getf(c, rn);
            double b = BIT(insn, 3) ? 0.0 : (dbl ? getd(c, rm) : getf(c, rm));
            fcmp_flags(c, a, b);
            return 1;
        }
        if (BITS(insn, 12, 10) == 0x4) { /* FMOV فوري */
            int imm8 = BITS(insn, 20, 13);
            c->v[rd].lo = vfp_expand_imm(imm8, dbl);
            c->v[rd].hi = 0;
            return 1;
        }
        return 0;
    }
    if (BITS(insn, 11, 10) == 1) { /* FCCMP */
        if (fcond(c, BITS(insn, 15, 12))) {
            double a = dbl ? getd(c, rn) : getf(c, rn), b = dbl ? getd(c, rm) : getf(c, rm);
            fcmp_flags(c, a, b);
        } else c->nzcv = (uint32_t)(insn & 15) << 28;
        return 1;
    }
    if (BITS(insn, 11, 10) == 3) { /* FCSEL */
        int s = fcond(c, BITS(insn, 15, 12)) ? rn : rm;
        c->v[rd].lo = dbl ? c->v[s].lo : (uint32_t)c->v[s].lo;
        c->v[rd].hi = 0;
        return 1;
    }
    /* ثنائي المصدر */
    int opc = BITS(insn, 15, 12);
    if (dbl) {
        double a = getd(c, rn), b = getd(c, rm), r;
        switch (opc) {
        case 0: r = a * b; break; case 1: r = a / b; break; case 2: r = a + b; break; case 3: r = a - b; break;
        case 4: r = (isnan(a) || isnan(b)) ? NAN : (a > b ? a : b); break;
        case 5: r = (isnan(a) || isnan(b)) ? NAN : (a < b ? a : b); break;
        case 6: r = fmaxnm_(a, b, 1); break; case 7: r = fmaxnm_(a, b, 0); break;
        case 8: r = -(a * b); break;
        default: return 0;
        }
        setd(c, rd, r);
    } else {
        float a = getf(c, rn), b = getf(c, rm), r;
        switch (opc) {
        case 0: r = a * b; break; case 1: r = a / b; break; case 2: r = a + b; break; case 3: r = a - b; break;
        case 4: r = (isnan(a) || isnan(b)) ? NAN : (a > b ? a : b); break;
        case 5: r = (isnan(a) || isnan(b)) ? NAN : (a < b ? a : b); break;
        case 6: r = (float)fmaxnm_(a, b, 1); break; case 7: r = (float)fmaxnm_(a, b, 0); break;
        case 8: r = -(a * b); break;
        default: return 0;
        }
        setf(c, rd, r);
    }
    return 1;
}

static int exec_fp_3src(CPU *c, uint32_t insn) {
    int type = BITS(insn, 23, 22), o1 = BIT(insn, 21), o0 = BIT(insn, 15);
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16), ra = BITS(insn, 14, 10);
    if (type == 1) {
        double n = getd(c, rn), m = getd(c, rm), a = getd(c, ra), r;
        if (!o1 && !o0) r = fma(n, m, a);
        else if (!o1 && o0) r = fma(-n, m, a);
        else if (o1 && !o0) r = fma(-n, m, -a);
        else r = fma(n, m, -a);
        setd(c, rd, r);
    } else if (type == 0) {
        float n = getf(c, rn), m = getf(c, rm), a = getf(c, ra), r;
        if (!o1 && !o0) r = fmaf(n, m, a);
        else if (!o1 && o0) r = fmaf(-n, m, a);
        else if (o1 && !o0) r = fmaf(-n, m, -a);
        else r = fmaf(n, m, -a);
        setf(c, rd, r);
    } else return 0;
    return 1;
}

/* ---------- Advanced SIMD ---------- */
static uint64_t simd_expand_imm(int op, int cmode, uint64_t imm8) {
    uint64_t imm = 0;
    switch (cmode >> 1) {
    case 0: imm = imm8 | (imm8 << 32); break;
    case 1: imm = (imm8 << 8) | (imm8 << 40); break;
    case 2: imm = (imm8 << 16) | (imm8 << 48); break;
    case 3: imm = (imm8 << 24) | (imm8 << 56); break;
    case 4: imm = imm8 | (imm8 << 16); imm |= imm << 32; break;
    case 5: imm = (imm8 << 8) | (imm8 << 24); imm |= imm << 32; break;
    case 6:
        if (cmode & 1) imm = (imm8 << 16) | 0xFFFF;
        else imm = (imm8 << 8) | 0xFF;
        imm |= imm << 32;
        break;
    case 7:
        if (!(cmode & 1) && !op) { for (int i = 0; i < 8; i++) imm |= imm8 << (8 * i); }
        else if (!(cmode & 1) && op) { for (int i = 0; i < 8; i++) if (BIT(imm8, i)) imm |= 0xFFULL << (8 * i); }
        else if ((cmode & 1) && !op) { uint64_t f = vfp_expand_imm(imm8, 0); imm = f | (f << 32); }
        else imm = vfp_expand_imm(imm8, 1);
        break;
    }
    return imm;
}

static uint64_t sat_s(int64_t v, int esz, int *q) {
    int64_t mx = (int64_t)(MASK(esz * 8 - 1)), mn = -mx - 1;
    if (v > mx) { *q = 1; return (uint64_t)mx; }
    if (v < mn) { *q = 1; return (uint64_t)mn; }
    return (uint64_t)v;
}
static uint64_t sat_u(int64_t v, int esz, int *q) {
    uint64_t mx = MASK(esz * 8);
    if (v < 0) { *q = 1; return 0; }
    if ((uint64_t)v > mx) { *q = 1; return mx; }
    return (uint64_t)v;
}

/* عملية ثلاثية متماثلة على عنصر واحد — تعيد 0 إن لم تكن مدعومة */
static int three_same_elem(int U, int opc, int esz, uint64_t a, uint64_t b, uint64_t d, uint64_t *r) {
    int bits = esz * 8;
    uint64_t m = MASK(bits);
    int64_t sa = SEXT(a, bits), sb = SEXT(b, bits);
    int q = 0;
    switch (opc) {
    case 0x00: *r = U ? ((a + b) >> 1) & m : (uint64_t)((sa + sb) >> 1) & m; return 1;
    case 0x01: *r = U ? sat_u((int64_t)(a + b), esz, &q) : sat_s(sa + sb, esz, &q); if (esz == 8 && U) *r = (a + b < a) ? ~0ULL : a + b; *r &= m; return 1;
    case 0x02: *r = U ? ((a + b + 1) >> 1) & m : (uint64_t)((sa + sb + 1) >> 1) & m; return 1;
    case 0x05: *r = U ? (a >= b ? a - b : 0) : sat_s(sa - sb, esz, &q); *r &= m; return 1;
    case 0x06: *r = (U ? a > b : sa > sb) ? m : 0; return 1;
    case 0x07: *r = (U ? a >= b : sa >= sb) ? m : 0; return 1;
    case 0x08: { /* SSHL/USHL */
        int sh = (int8_t)(b & 0xFF);
        if (sh >= 0) *r = sh >= bits ? 0 : (a << sh) & m;
        else { sh = -sh; if (U) *r = sh >= bits ? 0 : a >> sh; else *r = (uint64_t)(sh >= bits ? (sa < 0 ? -1 : 0) : sa >> sh) & m; }
        return 1;
    }
    case 0x0C: *r = U ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb) & m; return 1;
    case 0x0D: *r = U ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb) & m; return 1;
    case 0x0E: *r = U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa) & m; return 1;
    case 0x0F: *r = (d + (U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa))) & m; return 1;
    case 0x10: *r = (U ? a - b : a + b) & m; return 1;
    case 0x11: *r = U ? (a == b ? m : 0) : ((a & b) ? m : 0); return 1;
    case 0x12: *r = (U ? d - a * b : d + a * b) & m; return 1;
    case 0x13: {
        if (!U) { *r = (a * b) & m; return 1; }
        if (esz != 1) return 0;
        uint64_t p = 0; for (int i = 0; i < 8; i++) if (BIT(b, i)) p ^= a << i; *r = p & m; return 1;
    }
    }
    return 0;
}

static int exec_adv_simd(CPU *c, uint32_t insn) {
    int Q = BIT(insn, 30), U = BIT(insn, 29);
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16);
    int nbytes = Q ? 16 : 8;
    v128 *vd = &c->v[rd], *vn = &c->v[rn], *vm = &c->v[rm];
    v128 res = {0, 0};

    /* نسخ: DUP/INS/UMOV/SMOV */
    if ((insn & 0x9FE08400) == 0x0E000400) {
        int imm5 = BITS(insn, 20, 16), imm4 = BITS(insn, 14, 11);
        if (!(imm5 & 15)) return 0;
        int sz = __builtin_ctz(imm5);
        int esz = 1 << sz;
        int idx = imm5 >> (sz + 1);
        if (U) { /* INS (element) */
            if (!Q) return 0;
            int idx2 = imm4 >> sz;
            set_lane(vd, esz, idx, lane(vn, esz, idx2));
            return 1;
        }
        switch (imm4) {
        case 0: { uint64_t x = lane(vn, esz, idx); for (int i = 0; i < nbytes / esz; i++) set_lane(&res, esz, i, x); *vd = res; return 1; }
        case 1: { uint64_t x = xreg(c, rn); for (int i = 0; i < nbytes / esz; i++) set_lane(&res, esz, i, x); *vd = res; return 1; }
        case 3: set_lane(vd, esz, idx, xreg(c, rn)); return 1;
        case 5: { int64_t x = slane(vn, esz, idx); set_xreg(c, rd, Q ? (uint64_t)x : (uint64_t)(uint32_t)x); return 1; }
        case 7: set_xreg(c, rd, lane(vn, esz, idx)); return 1;
        }
        return 0;
    }
    /* قيمة فورية معدلة */
    if ((insn & 0x9FF80400) == 0x0F000400) {
        int cmode = BITS(insn, 15, 12), op = U;
        uint64_t imm8 = (BITS(insn, 18, 16) << 5) | BITS(insn, 9, 5);
        uint64_t imm = simd_expand_imm(op, cmode, imm8);
        int is_orr_bic = (cmode < 12) && (cmode & 1);
        if (cmode == 15 && op && !Q) return 0;
        if (is_orr_bic) {
            if (op) { vd->lo &= ~imm; vd->hi = Q ? vd->hi & ~imm : 0; }
            else { vd->lo |= imm; vd->hi = Q ? vd->hi | imm : 0; }
            return 1;
        }
        if (op && cmode != 14 && cmode != 15) imm = ~imm;
        vd->lo = imm;
        vd->hi = Q ? imm : 0;
        return 1;
    }
    /* ثلاثي متماثل */
    if ((insn & 0x9F200400) == 0x0E200400) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 15, 11);
        int esz = 1 << size;
        if (opc == 0x03) { /* منطقي */
            uint64_t a[2] = { vn->lo, vn->hi }, b[2] = { vm->lo, vm->hi }, d[2] = { vd->lo, vd->hi }, r[2];
            for (int i = 0; i < 2; i++) {
                if (!U) {
                    switch (size) { case 0: r[i] = a[i] & b[i]; break; case 1: r[i] = a[i] & ~b[i]; break;
                                    case 2: r[i] = a[i] | b[i]; break; default: r[i] = a[i] | ~b[i]; }
                } else {
                    switch (size) { case 0: r[i] = a[i] ^ b[i]; break;
                                    case 1: r[i] = (d[i] & a[i]) | (~d[i] & b[i]); break;
                                    case 2: r[i] = (b[i] & a[i]) | (~b[i] & d[i]); break;
                                    default: r[i] = (~b[i] & a[i]) | (b[i] & d[i]); }
                }
            }
            vd->lo = r[0]; vd->hi = Q ? r[1] : 0;
            return 1;
        }
        if (opc >= 0x18) { /* عائم متجهي */
            int dbl = BIT(insn, 22);
            int fesz = dbl ? 8 : 4, n = nbytes / fesz;
            int hi = BIT(insn, 23);
            if (dbl && !Q) return 0;
            for (int i = 0; i < n; i++) {
                double a = dbl ? u2d(lane(vn, 8, i)) : u2f(lane(vn, 4, i));
                double b = dbl ? u2d(lane(vm, 8, i)) : u2f(lane(vm, 4, i));
                double d = dbl ? u2d(lane(vd, 8, i)) : u2f(lane(vd, 4, i));
                double r; uint64_t mask = 0; int is_mask = 0;
                int key = (U << 6) | (hi << 5) | (opc & 0x1F);
                switch (key) {
                case (0 << 6) | (0 << 5) | 0x1A: r = a + b; break;           /* FADD */
                case (0 << 6) | (1 << 5) | 0x1A: r = a - b; break;           /* FSUB */
                case (1 << 6) | (0 << 5) | 0x1B: r = a * b; break;           /* FMUL */
                case (1 << 6) | (0 << 5) | 0x1F: r = a / b; break;           /* FDIV */
                case (0 << 6) | (0 << 5) | 0x19: r = fma(a, b, d); break;    /* FMLA */
                case (0 << 6) | (1 << 5) | 0x19: r = fma(-a, b, d); break;   /* FMLS */
                case (0 << 6) | (0 << 5) | 0x1E: r = fmaxnm_(a, b, 1); break; /* FMAX تقريبًا */
                case (0 << 6) | (1 << 5) | 0x1E: r = fmaxnm_(a, b, 0); break;
                case (0 << 6) | (0 << 5) | 0x18: r = fmaxnm_(a, b, 1); break;
                case (0 << 6) | (1 << 5) | 0x18: r = fmaxnm_(a, b, 0); break;
                case (1 << 6) | (1 << 5) | 0x1A: r = fabs(a - b); break;     /* FABD */
                case (0 << 6) | (0 << 5) | 0x1C: is_mask = 1; mask = a == b; break; /* FCMEQ */
                case (1 << 6) | (0 << 5) | 0x1C: is_mask = 1; mask = a >= b; break; /* FCMGE */
                case (1 << 6) | (1 << 5) | 0x1C: is_mask = 1; mask = a > b; break;  /* FCMGT */
                default: return 0;
                }
                if (is_mask) set_lane(&res, fesz, i, mask ? MASK(fesz * 8) : 0);
                else set_lane(&res, fesz, i, dbl ? d2u(r) : f2u((float)r));
            }
            if (U && !hi && (opc & 0x1F) == 0x1A) { /* FADDP */
                v128 cat[2] = { *vn, *vm };
                for (int i = 0; i < n; i++) {
                    int src = (2 * i) / n ? 1 : 0;
                    int j = (2 * i) % n;
                    double a = dbl ? u2d(lane(&cat[src], 8, j)) : u2f(lane(&cat[src], 4, j));
                    double b = dbl ? u2d(lane(&cat[src], 8, j + 1)) : u2f(lane(&cat[src], 4, j + 1));
                    set_lane(&res, fesz, i, dbl ? d2u(a + b) : f2u((float)(a + b)));
                }
            }
            *vd = res;
            return 1;
        }
        int n = nbytes / esz;
        if (opc == 0x14 || opc == 0x15 || (opc == 0x17 && !U)) { /* زوجي */
            v128 cat[2] = { *vn, *vm };
            for (int i = 0; i < n; i++) {
                int src = (2 * i) >= n;
                int j = (2 * i) % n;
                uint64_t a = lane(&cat[src], esz, j), b = lane(&cat[src], esz, j + 1), r;
                int64_t sa = slane(&cat[src], esz, j), sb = slane(&cat[src], esz, j + 1);
                if (opc == 0x17) r = a + b;
                else if (opc == 0x14) r = U ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb);
                else r = U ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb);
                set_lane(&res, esz, i, r);
            }
            *vd = res;
            return 1;
        }
        for (int i = 0; i < n; i++) {
            uint64_t r;
            if (!three_same_elem(U, opc, esz, lane(vn, esz, i), lane(vm, esz, i), lane(vd, esz, i), &r)) return 0;
            set_lane(&res, esz, i, r);
        }
        *vd = res;
        return 1;
    }
    /* ثلاثي مختلف */
    if ((insn & 0x9F200C00) == 0x0E200000) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 15, 12);
        int esz = 1 << size, wsz = esz * 2;
        if (size == 3) return 0;
        int n = 8 / esz, base = Q ? n : 0;
        if (opc == 4 || opc == 6) { /* ADDHN/SUBHN */
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, wsz, i), b = lane(vm, wsz, i);
                uint64_t r = opc == 4 ? a + b : a - b;
                if (U) r += 1ULL << (esz * 8 - 1);      /* RADDHN/RSUBHN: تقريب */
                r >>= esz * 8;
                set_lane(&res, esz, i, r);
            }
            if (Q) { vd->hi = res.lo; } else { vd->lo = res.lo; vd->hi = 0; }
            return 1;
        }
        if (opc == 1 || opc == 3) { /* SADDW/UADDW وSSUBW/USUBW: المصدر الأول عريض */
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, wsz, i);
                uint64_t b = U ? lane(vm, esz, base + i) : (uint64_t)slane(vm, esz, base + i);
                set_lane(&res, wsz, i, opc == 1 ? a + b : a - b);
            }
            *vd = res;
            return 1;
        }
        if (!U && (opc == 9 || opc == 11 || opc == 13)) { /* SQDMLAL/SQDMLSL/SQDMULL */
            if (size == 0) return 0;
            int bits = wsz * 8;
            int64_t mx = (int64_t)(MASK(bits - 1)), mn = -mx - 1;
            for (int i = 0; i < n; i++) {
                __int128 p = (__int128)slane(vn, esz, base + i) * slane(vm, esz, base + i) * 2;
                if (p > mx) { p = mx; c->fpsr |= 1u << 27; }
                if (p < mn) { p = mn; c->fpsr |= 1u << 27; }
                if (opc != 13) {
                    __int128 d = slane(vd, wsz, i);
                    p = opc == 9 ? d + p : d - p;
                    if (p > mx) { p = mx; c->fpsr |= 1u << 27; }
                    if (p < mn) { p = mn; c->fpsr |= 1u << 27; }
                }
                set_lane(&res, wsz, i, (uint64_t)(int64_t)p);
            }
            *vd = res;
            return 1;
        }
        for (int i = 0; i < n; i++) {
            uint64_t a = lane(vn, esz, base + i), b = lane(vm, esz, base + i);
            int64_t sa = slane(vn, esz, base + i), sb = slane(vm, esz, base + i);
            uint64_t d = lane(vd, wsz, i), r;
            switch (opc) {
            case 0: r = U ? a + b : (uint64_t)(sa + sb); break;
            case 2: r = U ? a - b : (uint64_t)(sa - sb); break;
            case 5: r = d + (U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa)); break;
            case 7: r = U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa); break;
            case 8: r = d + (U ? a * b : (uint64_t)(sa * sb)); break;
            case 10: r = d - (U ? a * b : (uint64_t)(sa * sb)); break;
            case 12: r = U ? a * b : (uint64_t)(sa * sb); break;
            case 14: { if (U || size != 0) return 0; uint64_t p = 0; for (int k = 0; k < 8; k++) if (BIT(b, k)) p ^= a << k; r = p; break; }
            default: return 0;
            }
            set_lane(&res, wsz, i, r);
        }
        *vd = res;
        return 1;
    }
    /* عبر المسارات */
    if ((insn & 0x9F3E0C00) == 0x0E300800) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 16, 12);
        int esz = 1 << size, n = nbytes / esz;
        if (opc == 0x0C || opc == 0x0F) { /* FMAXNMV/FMAXV (عائم 32) */
            double r = u2f(lane(vn, 4, 0));
            int mn = BIT(insn, 23);
            for (int i = 1; i < nbytes / 4; i++) r = fmaxnm_(r, u2f(lane(vn, 4, i)), !mn);
            setf(c, rd, (float)r);
            return 1;
        }
        uint64_t r = lane(vn, esz, 0);
        int64_t sr = slane(vn, esz, 0);
        for (int i = 1; i < n; i++) {
            uint64_t a = lane(vn, esz, i); int64_t sa = slane(vn, esz, i);
            switch (opc) {
            case 0x03: if (U) r += a; else sr += sa; break;
            case 0x0A: if (U) { if (a > r) r = a; } else { if (sa > sr) sr = sa; } break;
            case 0x1A: if (U) { if (a < r) r = a; } else { if (sa < sr) sr = sa; } break;
            case 0x1B: r += a; break;
            default: return 0;
            }
        }
        if (opc == 0x03 && !U) r = (uint64_t)sr;
        if ((opc == 0x0A || opc == 0x1A) && !U) r = (uint64_t)sr & MASK(esz * 8);
        if (opc == 0x1B) r &= MASK(esz * 8);
        int osz = opc == 0x03 ? esz * 2 : esz;
        c->v[rd].lo = r & MASK(osz * 8);
        c->v[rd].hi = 0;
        return 1;
    }
    /* ثنائي متنوع */
    if ((insn & 0x9F3E0C00) == 0x0E200800) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 16, 12);
        int esz = 1 << size, n = nbytes / esz;
        switch (opc) {
        case 0x00: case 0x01: { /* REV64 / REV16 / REV32 */
            if (opc == 1 && U) return 0;
            int cont = opc == 0 ? (U ? 4 : 8) : 2;  /* REV64 / REV32 / REV16 */
            if (esz >= cont) return 0;
            int per = cont / esz;
            for (int i = 0; i < n; i++) {
                int g = i / per, k = i % per;
                set_lane(&res, esz, i, lane(vn, esz, g * per + (per - 1 - k)));
            }
            *vd = res; return 1;
        }
        case 0x02: case 0x06: { /* SADDLP/UADDLP و SADALP/UADALP */
            int wsz = esz * 2, m = nbytes / wsz;
            for (int i = 0; i < m; i++) {
                uint64_t r = U ? lane(vn, esz, 2 * i) + lane(vn, esz, 2 * i + 1)
                               : (uint64_t)(slane(vn, esz, 2 * i) + slane(vn, esz, 2 * i + 1));
                if (opc == 0x06) r += lane(vd, wsz, i);
                set_lane(&res, wsz, i, r);
            }
            *vd = res; return 1;
        }
        case 0x04: { /* CLS / CLZ */
            if (esz > 4) return 0;
            int bits = esz * 8;
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, esz, i); int cnt = 0;
                if (U) { for (int k = bits - 1; k >= 0 && !BIT(a, k); k--) cnt++; }
                else { int s = BIT(a, bits - 1); for (int k = bits - 2; k >= 0 && BIT(a, k) == s; k--) cnt++; }
                set_lane(&res, esz, i, cnt);
            }
            *vd = res; return 1;
        }
        case 0x05:
            if (!U && size == 0) { for (int i = 0; i < nbytes; i++) set_lane(&res, 1, i, __builtin_popcount(lane(vn, 1, i))); }
            else if (U && size == 0) { res.lo = ~vn->lo; res.hi = ~vn->hi; }
            else if (U && size == 1) {
                for (int i = 0; i < nbytes; i++) { uint8_t b = lane(vn, 1, i), r = 0; for (int k = 0; k < 8; k++) if (BIT(b, k)) r |= 1 << (7 - k); set_lane(&res, 1, i, r); }
            } else return 0;
            if (!Q) res.hi = 0;
            *vd = res; return 1;
        case 0x08: case 0x09: case 0x0A: case 0x0B: {
            uint64_t m = MASK(esz * 8);
            for (int i = 0; i < n; i++) {
                int64_t a = slane(vn, esz, i); uint64_t r;
                if (opc == 0x08) r = (U ? a >= 0 : a > 0) ? m : 0;
                else if (opc == 0x09) r = (U ? a <= 0 : a == 0) ? m : 0;
                else if (opc == 0x0A) { if (U) return 0; r = a < 0 ? m : 0; }
                else r = (U ? (uint64_t)(-a) : (uint64_t)(a < 0 ? -a : a)) & m;
                set_lane(&res, esz, i, r);
            }
            *vd = res; return 1;
        }
        case 0x13: { /* SHLL / SHLL2: توسيع ثم إزاحة بعرض العنصر */
            if (!U || esz > 4) return 0;
            int m = 8 / esz;
            for (int i = 0; i < m; i++)
                set_lane(&res, esz * 2, i, lane(vn, esz, (Q ? m : 0) + i) << (esz * 8));
            *vd = res; return 1;
        }
        case 0x12: case 0x14: { /* XTN / SQXTN / UQXTN / SQXTUN */
            int wsz = esz * 2, m = 8 / esz;
            v128 out = Q ? *vd : (v128){0, 0};
            for (int i = 0; i < m; i++) {
                uint64_t a = lane(vn, wsz, i); int64_t sa = slane(vn, wsz, i); int qq = 0; uint64_t r;
                if (opc == 0x12 && !U) r = a;
                else if (opc == 0x12 && U) r = sat_u(sa, esz, &qq);
                else if (!U) r = sat_s(sa, esz, &qq);
                else r = a > MASK(esz * 8) ? MASK(esz * 8) : a;
                set_lane(&out, esz, (Q ? m : 0) + i, r);
            }
            *vd = out; return 1;
        }
        case 0x1D: case 0x1B: case 0x1A: case 0x1C: case 0x18: case 0x19: case 0x0F: case 0x0C: case 0x0D: case 0x0E: case 0x1F: {
            /* عائم متجهي: تحويلات ومقارنات بالصفر */
            int dbl = BIT(insn, 22), fesz = dbl ? 8 : 4, m = nbytes / fesz;
            int hi = BIT(insn, 23);
            for (int i = 0; i < m; i++) {
                uint64_t a = lane(vn, fesz, i);
                double f = dbl ? u2d(a) : u2f(a);
                uint64_t r;
                if (opc == 0x1D && !hi) { /* SCVTF/UCVTF */
                    double v = U ? (dbl ? (double)a : (double)(uint32_t)a) : (dbl ? (double)(int64_t)a : (double)(int32_t)a);
                    r = dbl ? d2u(v) : f2u((float)v);
                } else if (opc == 0x1B && hi) r = fp_to_int(f, 3, U, fesz * 8);     /* FCVTZS/ZU */
                else if (opc == 0x1A && !hi) r = fp_to_int(f, 0, U, fesz * 8);     /* FCVTNS */
                else if (opc == 0x1A && hi) r = fp_to_int(f, 1, U, fesz * 8);      /* FCVTPS */
                else if (opc == 0x1B && !hi) r = fp_to_int(f, 2, U, fesz * 8);     /* FCVTMS */
                else if (opc == 0x1C && !hi) r = fp_to_int(f, 4, U, fesz * 8);     /* FCVTAS */
                else if (opc == 0x0F && hi) r = dbl ? d2u(U ? -f : fabs(f)) : f2u(U ? -(float)f : fabsf((float)f));
                else if (opc == 0x1F && hi) r = dbl ? d2u(sqrt(f)) : f2u(sqrtf((float)f));
                else if (opc == 0x0C && hi) r = (U ? f >= 0 : f > 0) ? MASK(fesz * 8) : 0;
                else if (opc == 0x0D && hi) r = (U ? f <= 0 : f == 0) ? MASK(fesz * 8) : 0;
                else if (opc == 0x0E && hi && !U) r = f < 0 ? MASK(fesz * 8) : 0;
                else if ((opc == 0x18 || opc == 0x19) && !U) {
                    double rr = round_mode(f, opc == 0x18 ? (hi ? 1 : 0) : (hi ? 3 : 2));
                    r = dbl ? d2u(rr) : f2u((float)rr);
                } else if (opc == 0x18 && U && !hi) { /* FRINTA */
                    double rr = round_mode(f, 4);
                    r = dbl ? d2u(rr) : f2u((float)rr);
                } else if (opc == 0x19 && U) { /* FRINTX / FRINTI: وضع التقريب الافتراضي (الأقرب) */
                    double rr = round_mode(f, 0);
                    r = dbl ? d2u(rr) : f2u((float)rr);
                } else if (opc == 0x1D && hi) { /* FRECPE / FRSQRTE: قيمة دقيقة بدل التقدير */
                    double rr = U ? 1.0 / sqrt(f) : 1.0 / f;
                    r = dbl ? d2u(rr) : f2u((float)rr);
                } else return 0;
                set_lane(&res, fesz, i, r);
            }
            *vd = res; return 1;
        }
        case 0x16: /* FCVTN / FCVTL */
        case 0x17: {
            if (opc == 0x16 && !U) { /* FCVTN: double->single */
                v128 out = Q ? *vd : (v128){0, 0};
                if (!BIT(insn, 22)) return 0;
                for (int i = 0; i < 2; i++) set_lane(&out, 4, (Q ? 2 : 0) + i, f2u((float)u2d(lane(vn, 8, i))));
                *vd = out; return 1;
            }
            if (opc == 0x17 && !U) { /* FCVTL */
                if (!BIT(insn, 22)) return 0;
                for (int i = 0; i < 2; i++) set_lane(&res, 8, i, d2u((double)u2f(lane(vn, 4, (Q ? 2 : 0) + i))));
                *vd = res; return 1;
            }
            return 0;
        }
        }
        return 0;
    }
    /* إزاحة بقيمة فورية */
    if ((insn & 0x9F800400) == 0x0F000400) {
        int immh = BITS(insn, 22, 19), immb = BITS(insn, 18, 16), opc = BITS(insn, 15, 11);
        if (!immh) return 0;
        int hsb = 31 - __builtin_clz(immh);
        int esz = 1 << hsb, bits = esz * 8;
        int immhb = (immh << 3) | immb;
        int rsh = 2 * bits - immhb, lsh = immhb - bits;
        int n = nbytes / esz;
        uint64_t m = MASK(bits);
        switch (opc) {
        case 0x00: case 0x02: case 0x04: case 0x06:
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, esz, i); int64_t sa = slane(vn, esz, i); uint64_t r;
                int rnd = opc == 0x04 || opc == 0x06;
                if (U) r = rsh >= bits ? 0 : ((a + (rnd ? (1ULL << (rsh - 1)) : 0)) >> rsh);
                else r = (uint64_t)((sa + (rnd ? (int64_t)(1ULL << (rsh - 1)) : 0)) >> (rsh >= 64 ? 63 : rsh));
                if (opc == 0x02 || opc == 0x06) r += lane(vd, esz, i);
                set_lane(&res, esz, i, r & m);
            }
            *vd = res; return 1;
        case 0x0A: /* SHL / SLI */
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, esz, i), r = (a << lsh) & m;
                if (U) r |= lane(vd, esz, i) & MASK(lsh);
                set_lane(&res, esz, i, r);
            }
            *vd = res; return 1;
        case 0x08: /* SRI */
            if (!U) return 0;
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, esz, i), d = lane(vd, esz, i);
                uint64_t keep = rsh >= bits ? m : (~(m >> rsh)) & m;
                uint64_t r = (rsh >= bits ? 0 : a >> rsh) | (d & keep);
                set_lane(&res, esz, i, r);
            }
            *vd = res; return 1;
        case 0x10: case 0x11: case 0x12: case 0x13: { /* SHRN/RSHRN وما شابه (المبسط) */
            if (hsb == 3) return 0;
            int nesz = esz, wsz = esz * 2, wbits = bits * 2;
            int rs = 2 * bits - immhb; (void)wbits;
            v128 out = Q ? *vd : (v128){0, 0};
            int cnt = 8 / nesz;
            for (int i = 0; i < cnt; i++) {
                uint64_t a = lane(vn, wsz, i); int64_t sa = slane(vn, wsz, i);
                int rnd = opc & 1; int qq = 0; uint64_t r;
                uint64_t add = rnd ? (1ULL << (rs - 1)) : 0;
                if (opc <= 0x11 && !U) r = (a + add) >> rs;                 /* SHRN/RSHRN */
                else if (opc <= 0x11 && U) r = sat_u((sa + (int64_t)add) >> rs, nesz, &qq); /* SQSHRUN */
                else if (!U) r = sat_s((sa + (int64_t)add) >> rs, nesz, &qq); /* SQSHRN */
                else { uint64_t t = (a + add) >> rs; r = t > MASK(bits) ? MASK(bits) : t; } /* UQSHRN */
                set_lane(&out, nesz, (Q ? cnt : 0) + i, r);
            }
            *vd = out; return 1;
        }
        case 0x14: { /* SSHLL/USHLL */
            if (hsb == 3) return 0;
            int wsz = esz * 2, cnt = 8 / esz, base = Q ? cnt : 0;
            for (int i = 0; i < cnt; i++) {
                uint64_t a = U ? lane(vn, esz, base + i) : (uint64_t)slane(vn, esz, base + i);
                set_lane(&res, wsz, i, a << lsh);
            }
            *vd = res; return 1;
        }
        case 0x1C: case 0x1F: { /* SCVTF/FCVTZS بنقطة ثابتة */
            if (hsb < 2) return 0;
            int fb = rsh;
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, esz, i), r;
                if (opc == 0x1C) {
                    double v = U ? (esz == 8 ? (double)a : (double)(uint32_t)a) : (esz == 8 ? (double)(int64_t)a : (double)(int32_t)a);
                    v = ldexp(v, -fb);
                    r = esz == 8 ? d2u(v) : f2u((float)v);
                } else {
                    double f = esz == 8 ? u2d(a) : u2f(a);
                    r = fp_to_int(ldexp(f, fb), 3, U, bits);
                }
                set_lane(&res, esz, i, r);
            }
            *vd = res; return 1;
        }
        }
        return 0;
    }
    /* EXT */
    if ((insn & 0xBFE08400) == 0x2E000000) {
        int pos = BITS(insn, 14, 11);
        uint8_t cat[32];
        memcpy(cat, vn, 16);
        if (Q) memcpy(cat + 16, vm, 16);
        else { memcpy(cat + 8, vm, 8); }
        for (int i = 0; i < nbytes; i++) set_lane(&res, 1, i, cat[pos + i]);
        *vd = res; return 1;
    }
    /* TBL/TBX */
    if ((insn & 0xBF209C00) == 0x0E000000 || (insn & 0xBF209C00) == 0x0E001000) {
        int len = BITS(insn, 14, 13) + 1, op = BIT(insn, 12);
        uint8_t tbl[64];
        for (int k = 0; k < len; k++) memcpy(tbl + 16 * k, &c->v[(rn + k) & 31], 16);
        v128 idx = *vm, old = *vd;
        for (int i = 0; i < nbytes; i++) {
            uint8_t ix = lane(&idx, 1, i);
            set_lane(&res, 1, i, ix < 16 * len ? tbl[ix] : (op ? lane(&old, 1, i) : 0));
        }
        *vd = res; return 1;
    }
    /* ZIP/UZP/TRN */
    if ((insn & 0xBF208C00) == 0x0E000800) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 14, 12);
        int esz = 1 << size, n = nbytes / esz, part = opc >> 2;
        for (int i = 0; i < n; i++) {
            uint64_t r;
            switch (opc & 3) {
            case 1: { int j = 2 * i + part; r = j < n ? lane(vn, esz, j) : lane(vm, esz, j - n); break; }
            case 2: { int p = i / 2 * 2 + part; r = (i & 1) ? lane(vm, esz, p) : lane(vn, esz, p); break; }
            case 3: { int p = part * n / 2 + i / 2; r = (i & 1) ? lane(vm, esz, p) : lane(vn, esz, p); break; }
            default: return 0;
            }
            set_lane(&res, esz, i, r);
        }
        *vd = res; return 1;
    }
    /* عنصر بالفهرس (MUL/MLA/FMUL by element) */
    if ((insn & 0x9F000400) == 0x0F000000) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 15, 12);
        int H = BIT(insn, 11), L = BIT(insn, 21), M = BIT(insn, 20);
        int rmi = BITS(insn, 19, 16);
        if (opc == 0x1 || opc == 0x5 || opc == 0x9) { /* FMLA/FMLS/FMUL بالعنصر */
            int dbl = BIT(insn, 22);
            int idx = dbl ? H : (H << 1) | L;
            int mreg = (M << 4) | rmi;
            int fesz = dbl ? 8 : 4, n = nbytes / fesz;
            double b = dbl ? u2d(lane(&c->v[mreg], 8, idx)) : u2f(lane(&c->v[mreg], 4, idx));
            for (int i = 0; i < n; i++) {
                double a = dbl ? u2d(lane(vn, 8, i)) : u2f(lane(vn, 4, i));
                double d = dbl ? u2d(lane(vd, 8, i)) : u2f(lane(vd, 4, i));
                double r = opc == 0x9 ? a * b : (opc == 0x1 ? fma(a, b, d) : fma(-a, b, d));
                if (U && opc == 0x9) return 0;
                set_lane(&res, fesz, i, dbl ? d2u(r) : f2u((float)r));
            }
            *vd = res; return 1;
        }
        if (opc == 0x8 || opc == 0x0 || opc == 0x4) { /* MUL/MLA/MLS بالعنصر */
            int esz = 1 << size, idx, mreg;
            if (size == 1) { idx = (H << 2) | (L << 1) | M; mreg = rmi; }
            else if (size == 2) { idx = (H << 1) | L; mreg = (M << 4) | rmi; }
            else return 0;
            uint64_t b = lane(&c->v[mreg], esz, idx);
            int n = nbytes / esz;
            if (opc != 0x8 && !U) return 0;
            for (int i = 0; i < n; i++) {
                uint64_t a = lane(vn, esz, i), d = lane(vd, esz, i), r;
                r = opc == 0x8 ? a * b : (opc == 0x0 ? d + a * b : d - a * b);
                set_lane(&res, esz, i, r);
            }
            if (opc == 0x8 && U) return 0;
            *vd = res; return 1;
        }
        return 0;
    }
    return 0;
}

/* ---------- Advanced SIMD القياسي ---------- */
static int exec_adv_simd_scalar(CPU *c, uint32_t insn) {
    int U = BIT(insn, 29);
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16);
    v128 *vn = &c->v[rn], *vm = &c->v[rm];
    /* DUP قياسي (MOV من عنصر) */
    if ((insn & 0xFFE0FC00) == 0x5E000400) {
        int imm5 = BITS(insn, 20, 16);
        if (!(imm5 & 15)) return 0;
        int sz = __builtin_ctz(imm5), esz = 1 << sz, idx = imm5 >> (sz + 1);
        uint64_t x = lane(vn, esz, idx);
        c->v[rd].lo = x; c->v[rd].hi = 0;
        return 1;
    }
    /* عائم قياسي بالعنصر: FMLA/FMLS/FMUL/FMULX (Vd, Vn, Vm.T[i]) */
    if ((insn & 0xDF000400) == 0x5F000000 && BIT(insn, 23)) {
        int opc = BITS(insn, 15, 12), H = BIT(insn, 11), L = BIT(insn, 21), M = BIT(insn, 20);
        if (opc != 0x1 && opc != 0x5 && opc != 0x9) return 0;
        if (U && opc != 0x9) return 0;
        int dbl = BIT(insn, 22);
        if (dbl && L) return 0;
        int idx = dbl ? H : (H << 1) | L, mreg = (M << 4) | BITS(insn, 19, 16);
        double b = dbl ? u2d(lane(&c->v[mreg], 8, idx)) : u2f(lane(&c->v[mreg], 4, idx));
        double a = dbl ? u2d(lane(vn, 8, 0)) : u2f(lane(vn, 4, 0));
        double d = dbl ? u2d(lane(&c->v[rd], 8, 0)) : u2f(lane(&c->v[rd], 4, 0));
        double r;
        if (dbl) r = opc == 0x9 ? a * b : (opc == 0x1 ? fma(a, b, d) : fma(-a, b, d));
        else r = opc == 0x9 ? (float)a * (float)b : (opc == 0x1 ? fmaf((float)a, (float)b, (float)d) : fmaf(-(float)a, (float)b, (float)d));
        c->v[rd].lo = dbl ? d2u(r) : f2u((float)r); c->v[rd].hi = 0;
        return 1;
    }
    /* ثلاثي متماثل قياسي */
    if ((insn & 0xDF200400) == 0x5E200400) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 15, 11);
        int esz = 1 << size;
        if (opc >= 0x18) {
            int dbl = BIT(insn, 22), hi = BIT(insn, 23);
            double a = dbl ? getd(c, rn) : getf(c, rn), b = dbl ? getd(c, rm) : getf(c, rm);
            uint64_t r; int fesz = dbl ? 8 : 4;
            int key = (U << 6) | (hi << 5) | (opc & 0x1F);
            switch (key) {
            case (0 << 6) | (0 << 5) | 0x1C: r = a == b ? MASK(fesz * 8) : 0; break;
            case (1 << 6) | (0 << 5) | 0x1C: r = a >= b ? MASK(fesz * 8) : 0; break;
            case (1 << 6) | (1 << 5) | 0x1C: r = a > b ? MASK(fesz * 8) : 0; break;
            case (1 << 6) | (1 << 5) | 0x1A: { double d = fabs(a - b); r = dbl ? d2u(d) : f2u((float)d); break; }
            default: return 0;
            }
            c->v[rd].lo = r; c->v[rd].hi = 0;
            return 1;
        }
        uint64_t r;
        if (!three_same_elem(U, opc, esz, lane(vn, esz, 0), lane(vm, esz, 0), lane(&c->v[rd], esz, 0), &r)) return 0;
        c->v[rd].lo = r; c->v[rd].hi = 0;
        return 1;
    }
    /* إزاحة قياسية بقيمة فورية */
    if ((insn & 0xDF800400) == 0x5F000400) {
        int immh = BITS(insn, 22, 19), immb = BITS(insn, 18, 16), opc = BITS(insn, 15, 11);
        if (!immh) return 0;
        int hsb = 31 - __builtin_clz(immh), esz = 1 << hsb, bits = esz * 8;
        int immhb = (immh << 3) | immb, rsh = 2 * bits - immhb, lsh = immhb - bits;
        uint64_t a = lane(vn, esz, 0), d = lane(&c->v[rd], esz, 0), r, m = MASK(bits);
        int64_t sa = slane(vn, esz, 0);
        switch (opc) {
        case 0x00: r = U ? (rsh >= bits ? 0 : a >> rsh) : (uint64_t)(sa >> (rsh >= 64 ? 63 : rsh)); break;
        case 0x02: r = d + (U ? (rsh >= bits ? 0 : a >> rsh) : (uint64_t)(sa >> (rsh >= 64 ? 63 : rsh))); break;
        case 0x0A: r = (a << lsh) | (U ? d & MASK(lsh) : 0); break;
        case 0x08: if (!U) return 0; r = (rsh >= bits ? 0 : a >> rsh) | (d & (rsh >= bits ? m : ~(m >> rsh))); break;
        case 0x1C: case 0x1F: {
            if (hsb < 2) return 0;
            if (opc == 0x1C) {
                double v = U ? (esz == 8 ? (double)a : (double)(uint32_t)a) : (esz == 8 ? (double)(int64_t)a : (double)(int32_t)a);
                v = ldexp(v, -rsh);
                r = esz == 8 ? d2u(v) : f2u((float)v);
            } else {
                double f = esz == 8 ? u2d(a) : u2f(a);
                r = fp_to_int(ldexp(f, rsh), 3, U, bits);
            }
            break;
        }
        default: return 0;
        }
        c->v[rd].lo = r & m; c->v[rd].hi = 0;
        return 1;
    }
    /* زوجي قياسي */
    if ((insn & 0xDF3E0C00) == 0x5E300800) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 16, 12);
        if (opc == 0x1B && !U && size == 3) { c->v[rd].lo = vn->lo + vn->hi; c->v[rd].hi = 0; return 1; }
        if (opc == 0x0D && U) { /* FADDP */
            if (BIT(insn, 22)) setd(c, rd, u2d(vn->lo) + u2d(vn->hi));
            else setf(c, rd, u2f(lane(vn, 4, 0)) + u2f(lane(vn, 4, 1)));
            return 1;
        }
        return 0;
    }
    /* ثنائي متنوع قياسي */
    if ((insn & 0xDF3E0C00) == 0x5E200800) {
        int size = BITS(insn, 23, 22), opc = BITS(insn, 16, 12);
        int esz = 1 << size;
        uint64_t a = lane(vn, esz, 0), r, m = MASK(esz * 8);
        int64_t sa = slane(vn, esz, 0);
        switch (opc) {
        case 0x08: r = (U ? sa >= 0 : sa > 0) ? m : 0; break;
        case 0x09: r = (U ? sa <= 0 : sa == 0) ? m : 0; break;
        case 0x0A: if (U) return 0; r = sa < 0 ? m : 0; break;
        case 0x0B: r = (U ? (uint64_t)(-sa) : (uint64_t)(sa < 0 ? -sa : sa)) & m; break;
        case 0x1D: case 0x1B: case 0x1A: case 0x1C: {
            int dbl = BIT(insn, 22), hi = BIT(insn, 23), fesz = dbl ? 8 : 4;
            uint64_t x = lane(vn, fesz, 0);
            double f = dbl ? u2d(x) : u2f(x);
            if (opc == 0x1D && !hi) {
                double v = U ? (dbl ? (double)x : (double)(uint32_t)x) : (dbl ? (double)(int64_t)x : (double)(int32_t)x);
                r = dbl ? d2u(v) : f2u((float)v);
            } else if (opc == 0x1B && hi) r = fp_to_int(f, 3, U, fesz * 8);
            else if (opc == 0x1A && !hi) r = fp_to_int(f, 0, U, fesz * 8);
            else if (opc == 0x1B && !hi) r = fp_to_int(f, 2, U, fesz * 8);
            else if (opc == 0x1A && hi) r = fp_to_int(f, 1, U, fesz * 8);
            else if (opc == 0x1C && !hi) r = fp_to_int(f, 4, U, fesz * 8);
            else return 0;
            c->v[rd].lo = r; c->v[rd].hi = 0;
            return 1;
        }
        default: return 0;
        }
        c->v[rd].lo = r; c->v[rd].hi = 0;
        return 1;
    }
    return 0;
}

/* ---------- امتدادات التشفير: AES وSHA1 وSHA256 وPMULL ذو 64 بت ---------- */
static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void v_get32(const v128 *v, uint32_t w[4]) { for (int i = 0; i < 4; i++) w[i] = lane(v, 4, i); }
static void v_set32(v128 *v, const uint32_t w[4]) { for (int i = 0; i < 4; i++) set_lane(v, 4, i, w[i]); }

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static uint8_t aes_inv_sbox[256];
static inline uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }

static int exec_crypto(CPU *c, uint32_t insn) {
    if ((insn & 0xFF000000) != 0x4E000000 && (insn & 0xFF000000) != 0x5E000000 && (insn & 0xBFE0FC00) != 0x0EE0E000) return -1;
    int rd = insn & 31, rn = BITS(insn, 9, 5), rm = BITS(insn, 20, 16);
    uint32_t top = insn & 0xFFFFFC00;
    if (top == 0x4E284800 || top == 0x4E285800) {             /* AESE / AESD */
        if (!aes_inv_sbox[0x63] && !aes_inv_sbox[0]) for (int i = 0; i < 256; i++) aes_inv_sbox[aes_sbox[i]] = i;
        uint8_t in[16], out[16];
        v128 x = { c->v[rd].lo ^ c->v[rn].lo, c->v[rd].hi ^ c->v[rn].hi };
        memcpy(in, &x, 16);
        int dec = top == 0x4E285800;
        for (int col = 0; col < 4; col++)
            for (int r = 0; r < 4; r++) {
                int src = dec ? r + 4 * ((col - r + 4) & 3) : r + 4 * ((col + r) & 3);
                out[r + 4 * col] = dec ? aes_inv_sbox[in[src]] : aes_sbox[in[src]];
            }
        memcpy(&c->v[rd], out, 16);
        return 1;
    }
    if (top == 0x4E286800 || top == 0x4E287800) {             /* AESMC / AESIMC */
        uint8_t in[16], out[16];
        memcpy(in, &c->v[rn], 16);
        int inv = top == 0x4E287800;
        for (int col = 0; col < 4; col++) {
            uint8_t *a = in + 4 * col, *o = out + 4 * col;
            if (!inv) {
                o[0] = gmul(a[0], 2) ^ gmul(a[1], 3) ^ a[2] ^ a[3];
                o[1] = a[0] ^ gmul(a[1], 2) ^ gmul(a[2], 3) ^ a[3];
                o[2] = a[0] ^ a[1] ^ gmul(a[2], 2) ^ gmul(a[3], 3);
                o[3] = gmul(a[0], 3) ^ a[1] ^ a[2] ^ gmul(a[3], 2);
            } else {
                o[0] = gmul(a[0], 14) ^ gmul(a[1], 11) ^ gmul(a[2], 13) ^ gmul(a[3], 9);
                o[1] = gmul(a[0], 9) ^ gmul(a[1], 14) ^ gmul(a[2], 11) ^ gmul(a[3], 13);
                o[2] = gmul(a[0], 13) ^ gmul(a[1], 9) ^ gmul(a[2], 14) ^ gmul(a[3], 11);
                o[3] = gmul(a[0], 11) ^ gmul(a[1], 13) ^ gmul(a[2], 9) ^ gmul(a[3], 14);
            }
        }
        memcpy(&c->v[rd], out, 16);
        return 1;
    }
    if ((insn & 0xFFE08C00) == 0x5E000000) {                  /* SHA ثلاثية المسجلات */
        int op = BITS(insn, 14, 12);
        uint32_t d[4], n[4], m[4], r[4];
        v_get32(&c->v[rd], d); v_get32(&c->v[rn], n); v_get32(&c->v[rm], m);
        if (op <= 2) {                                         /* SHA1C / SHA1P / SHA1M */
            uint32_t X[4] = { d[0], d[1], d[2], d[3] }, Y = n[0];
            for (int e = 0; e < 4; e++) {
                uint32_t t;
                if (op == 0) t = ((X[2] ^ X[3]) & X[1]) ^ X[3];
                else if (op == 1) t = X[1] ^ X[2] ^ X[3];
                else t = (X[1] & X[2]) | ((X[1] | X[2]) & X[3]);
                Y = Y + rol32(X[0], 5) + t + m[e];
                X[1] = rol32(X[1], 30);
                uint32_t ny = X[3];
                X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = Y; Y = ny;
            }
            v_set32(&c->v[rd], X);
            return 1;
        }
        if (op == 3) {                                         /* SHA1SU0 */
            r[0] = d[2] ^ d[0] ^ m[0]; r[1] = d[3] ^ d[1] ^ m[1];
            r[2] = n[0] ^ d[2] ^ m[2]; r[3] = n[1] ^ d[3] ^ m[3];
            v_set32(&c->v[rd], r);
            return 1;
        }
        if (op == 4 || op == 5) {                              /* SHA256H / SHA256H2 */
            uint32_t X[4], Y[4];
            if (op == 4) { memcpy(X, d, 16); memcpy(Y, n, 16); }
            else { memcpy(X, n, 16); memcpy(Y, d, 16); }
            for (int e = 0; e < 4; e++) {
                uint32_t chs = ((Y[1] ^ Y[2]) & Y[0]) ^ Y[2];
                uint32_t maj = (X[0] & X[1]) | ((X[0] | X[1]) & X[2]);
                uint32_t t = Y[3] + (ror32(Y[0], 6) ^ ror32(Y[0], 11) ^ ror32(Y[0], 25)) + chs + m[e];
                X[3] = t + X[3];
                Y[3] = t + (ror32(X[0], 2) ^ ror32(X[0], 13) ^ ror32(X[0], 22)) + maj;
                uint32_t y3 = Y[3], x3 = X[3];
                Y[3] = Y[2]; Y[2] = Y[1]; Y[1] = Y[0]; Y[0] = x3;
                X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = y3;
            }
            v_set32(&c->v[rd], op == 4 ? X : Y);
            return 1;
        }
        if (op == 6) {                                         /* SHA256SU1 */
            uint32_t T0[4] = { n[1], n[2], n[3], m[0] };
            for (int e = 0; e < 4; e++) {
                uint32_t x = e < 2 ? m[2 + e] : r[e - 2];
                x = ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10);
                r[e] = x + d[e] + T0[e];
            }
            v_set32(&c->v[rd], r);
            return 1;
        }
        return 0;
    }
    if (top == 0x5E280800) {                                   /* SHA1H */
        c->v[rd].lo = rol32((uint32_t)c->v[rn].lo, 30); c->v[rd].hi = 0;
        return 1;
    }
    if (top == 0x5E281800 || top == 0x5E282800) {             /* SHA1SU1 / SHA256SU0 */
        uint32_t d[4], n[4], r[4];
        v_get32(&c->v[rd], d); v_get32(&c->v[rn], n);
        if (top == 0x5E281800) {
            uint32_t T[4] = { d[0] ^ n[1], d[1] ^ n[2], d[2] ^ n[3], d[3] };
            r[0] = rol32(T[0], 1); r[1] = rol32(T[1], 1); r[2] = rol32(T[2], 1);
            r[3] = rol32(T[3], 1) ^ rol32(T[0], 2);
        } else {
            uint32_t T[4] = { d[1], d[2], d[3], n[0] };
            for (int e = 0; e < 4; e++) r[e] = (ror32(T[e], 7) ^ ror32(T[e], 18) ^ (T[e] >> 3)) + d[e];
        }
        v_set32(&c->v[rd], r);
        return 1;
    }
    if ((insn & 0xBFE0FC00) == 0x0EE0E000) {                  /* PMULL/PMULL2 بعناصر 64 بت */
        int q = BIT(insn, 30);
        uint64_t a = q ? c->v[rn].hi : c->v[rn].lo, b = q ? c->v[rm].hi : c->v[rm].lo, lo = 0, hi = 0;
        for (int i = 0; i < 64; i++)
            if (BIT(b, i)) { lo ^= a << i; if (i) hi ^= a >> (64 - i); }
        c->v[rd].lo = lo; c->v[rd].hi = hi;
        return 1;
    }
    return -1;
}

int exec_simd(CPU *c, uint32_t insn) {
    fp_check(c);
    {
        int r = exec_crypto(c, insn);
        if (r >= 0) return r;
    }
    if (!BIT(insn, 28)) {
        if (BIT(insn, 31)) return 0;
        return exec_adv_simd(c, insn);
    }
    if (BIT(insn, 30)) {
        if (BIT(insn, 31)) return 0;
        return exec_adv_simd_scalar(c, insn);
    }
    if (BITS(insn, 28, 24) == 0x1F) return exec_fp_3src(c, insn);
    if (BIT(insn, 29)) return 0;
    if (BIT(insn, 21)) return exec_fp_scalar(c, insn);
    { /* تحويل بنقطة ثابتة */
        int sf = BIT(insn, 31), type = BITS(insn, 23, 22), rmode = BITS(insn, 20, 19), opc = BITS(insn, 18, 16);
        int scale = 64 - BITS(insn, 15, 10);
        int rd = insn & 31, rn = BITS(insn, 9, 5);
        int dbl = type == 1;
        if (type > 1) return 0;
        if (rmode == 0 && (opc == 2 || opc == 3)) {
            uint64_t x = xreg(c, rn);
            double v = opc == 2 ? (sf ? (double)(int64_t)x : (double)(int32_t)x) : (sf ? (double)x : (double)(uint32_t)x);
            v = ldexp(v, -scale);
            if (dbl) setd(c, rd, v); else setf(c, rd, (float)v);
            return 1;
        }
        if (rmode == 3 && (opc == 0 || opc == 1)) {
            double f = dbl ? getd(c, rn) : getf(c, rn);
            uint64_t r = fp_to_int(ldexp(f, scale), 3, opc, sf ? 64 : 32);
            set_xreg(c, rd, sf ? r : r & 0xFFFFFFFF);
            return 1;
        }
        return 0;
    }
}

/* ---------- تحميل/تخزين البنى ---------- */
int exec_simd_ldst(CPU *c, uint32_t insn) {
    int Q = BIT(insn, 30), L = BIT(insn, 22);
    int rn = BITS(insn, 9, 5), rt = insn & 31, rm = BITS(insn, 20, 16);
    int post = BIT(insn, 23);
    if ((insn & 0xBF000000) == 0x0C000000) { /* بنى متعددة */
        if (!post && rm != 0) return 0;
        fp_check(c);
        int opc = BITS(insn, 15, 12), size = BITS(insn, 11, 10);
        int esz = 1 << size, nbytes = Q ? 16 : 8, ne = nbytes / esz;
        int rpt, selem;
        switch (opc) {
        case 0: rpt = 1; selem = 4; break;
        case 2: rpt = 4; selem = 1; break;
        case 4: rpt = 1; selem = 3; break;
        case 6: rpt = 3; selem = 1; break;
        case 7: rpt = 1; selem = 1; break;
        case 8: rpt = 1; selem = 2; break;
        case 10: rpt = 2; selem = 1; break;
        default: return 0;
        }
        uint64_t addr = xreg_sp(c, rn), a = addr;
        v128 tmp[4];
        for (int k = 0; k < 4; k++) tmp[k] = c->v[(rt + k) & 31];
        if (L) for (int k = 0; k < rpt * selem; k++) { if (!Q) tmp[k].hi = 0; }
        for (int r = 0; r < rpt; r++)
            for (int e = 0; e < ne; e++)
                for (int s = 0; s < selem; s++) {
                    int t = r + s;
                    if (L) set_lane(&tmp[t], esz, e, mem_read(c, a, esz));
                    else mem_write(c, a, esz, lane(&tmp[t], esz, e));
                    a += esz;
                }
        if (L) for (int k = 0; k < rpt * selem; k++) { if (!Q) tmp[k].hi = 0; c->v[(rt + k) & 31] = tmp[k]; }
        if (post) set_xreg_sp(c, rn, rm == 31 ? a : addr + xreg(c, rm));
        return 1;
    }
    if ((insn & 0xBF000000) == 0x0D000000) { /* بنية مفردة */
        if (!post && rm != 0) return 0;
        fp_check(c);
        int R = BIT(insn, 21), opc = BITS(insn, 15, 13), S = BIT(insn, 12), size = BITS(insn, 11, 10);
        int selem = ((opc & 1) << 1 | R) + 1;
        uint64_t addr = xreg_sp(c, rn), a = addr;
        int scale = opc >> 1;
        if (scale == 3) { /* استنساخ LDnR */
            if (!L) return 0;
            int esz = 1 << size, nbytes = Q ? 16 : 8;
            for (int s = 0; s < selem; s++) {
                uint64_t x = mem_read(c, a, esz);
                v128 r = {0, 0};
                for (int i = 0; i < nbytes / esz; i++) set_lane(&r, esz, i, x);
                c->v[(rt + s) & 31] = r;
                a += esz;
            }
        } else {
            int index, esz;
            if (scale == 0) { esz = 1; index = (Q << 3) | (S << 2) | size; }
            else if (scale == 1) { esz = 2; index = (Q << 2) | (S << 1) | (size >> 1); }
            else if (size == 0) { esz = 4; index = (Q << 1) | S; }
            else { esz = 8; index = Q; }
            for (int s = 0; s < selem; s++) {
                v128 *v = &c->v[(rt + s) & 31];
                if (L) set_lane(v, esz, index, mem_read(c, a, esz));
                else mem_write(c, a, esz, lane(v, esz, index));
                a += esz;
            }
        }
        if (post) set_xreg_sp(c, rn, rm == 31 ? a : addr + xreg(c, rm));
        return 1;
    }
    return 0;
}
