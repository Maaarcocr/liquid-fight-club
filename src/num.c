/*
 * num.c - numbers.
 *
 *  - Ruby-compatible Float#to_s formatting (shortest round-trip digits).
 *  - Arbitrary precision integers (Ruby Integer semantics) built on a small
 *    magnitude library, with a size cap so hostile input cannot trigger
 *    quadratic blowups.
 *  - A decimal type that emulates the BigDecimal arithmetic Liquid's math
 *    filters perform on floats (Utils.to_number converts floats and decimal
 *    strings to BigDecimal, and results are converted back with to_f).
 *  - Ruby String#to_i / Kernel#Integer parsing.
 */
#include "lq.h"
#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Magnitudes                                                          */
/* ------------------------------------------------------------------ */

void lq_mag_init(mag *m) {
    m->d = NULL;
    m->n = m->cap = 0;
}

void lq_mag_free(mag *m) {
    free(m->d);
    lq_mag_init(m);
}

static void lq_mag_reserve(mag *m, size_t n) {
    if (n <= m->cap) return;
    size_t nc = m->cap ? m->cap : 4;
    while (nc < n) nc *= 2;
    m->d = lq_xrealloc(m->d, nc * sizeof(uint32_t));
    m->cap = nc;
}

static void lq_mag_trim(mag *m) {
    while (m->n && m->d[m->n - 1] == 0) m->n--;
}

void lq_mag_set_u64(mag *m, uint64_t v) {
    lq_mag_reserve(m, 2);
    m->d[0] = (uint32_t)v;
    m->d[1] = (uint32_t)(v >> 32);
    m->n = 2;
    lq_mag_trim(m);
}

void lq_mag_copy(mag *dst, const mag *src) {
    lq_mag_reserve(dst, src->n);
    if (src->n) memcpy(dst->d, src->d, src->n * sizeof(uint32_t));
    dst->n = src->n;
}

int lq_mag_cmp(const mag *a, const mag *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (size_t i = a->n; i-- > 0;) {
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

/* r = a + b (r may alias a or b) */
static void lq_mag_add(mag *r, const mag *a, const mag *b) {
    size_t n = a->n > b->n ? a->n : b->n;
    uint32_t *tmp = lq_xmalloc((n + 1) * sizeof(uint32_t));
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t s = carry;
        if (i < a->n) s += a->d[i];
        if (i < b->n) s += b->d[i];
        tmp[i] = (uint32_t)s;
        carry = s >> 32;
    }
    tmp[n] = (uint32_t)carry;
    free(r->d);
    r->d = tmp;
    r->cap = n + 1;
    r->n = n + 1;
    lq_mag_trim(r);
}

/* r = a - b, requires a >= b (r may alias) */
static void lq_mag_sub(mag *r, const mag *a, const mag *b) {
    size_t n = a->n;
    uint32_t *tmp = lq_xmalloc((n ? n : 1) * sizeof(uint32_t));
    int64_t borrow = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t s = (int64_t)a->d[i] - borrow - (i < b->n ? (int64_t)b->d[i] : 0);
        borrow = 0;
        if (s < 0) {
            s += (int64_t)1 << 32;
            borrow = 1;
        }
        tmp[i] = (uint32_t)s;
    }
    free(r->d);
    r->d = tmp;
    r->cap = n ? n : 1;
    r->n = n;
    lq_mag_trim(r);
}

static void lq_mag_mul(mag *r, const mag *a, const mag *b) {
    if (!a->n || !b->n) {
        r->n = 0;
        return;
    }
    size_t n = a->n + b->n;
    uint32_t *tmp = lq_xcalloc(n, sizeof(uint32_t));
    for (size_t i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b->n; j++) {
            uint64_t t = (uint64_t)a->d[i] * b->d[j] + tmp[i + j] + carry;
            tmp[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        size_t k = i + b->n;
        while (carry) {
            uint64_t t = (uint64_t)tmp[k] + carry;
            tmp[k] = (uint32_t)t;
            carry = t >> 32;
            k++;
        }
    }
    free(r->d);
    r->d = tmp;
    r->cap = n;
    r->n = n;
    lq_mag_trim(r);
}

static void lq_mag_mul_small(mag *m, uint32_t f) {
    uint64_t carry = 0;
    for (size_t i = 0; i < m->n; i++) {
        uint64_t t = (uint64_t)m->d[i] * f + carry;
        m->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        lq_mag_reserve(m, m->n + 1);
        m->d[m->n++] = (uint32_t)carry;
    }
}

static void lq_mag_add_small(mag *m, uint32_t s) {
    uint64_t carry = s;
    for (size_t i = 0; i < m->n && carry; i++) {
        uint64_t t = (uint64_t)m->d[i] + carry;
        m->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        lq_mag_reserve(m, m->n + 1);
        m->d[m->n++] = (uint32_t)carry;
    }
}

static uint32_t lq_mag_divmod_small(mag *m, uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = m->n; i-- > 0;) {
        uint64_t cur = (rem << 32) | m->d[i];
        m->d[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    lq_mag_trim(m);
    return (uint32_t)rem;
}

static size_t lq_mag_bits(const mag *m) {
    if (!m->n) return 0;
    uint32_t top = m->d[m->n - 1];
    size_t b = 0;
    while (top) {
        b++;
        top >>= 1;
    }
    return (m->n - 1) * 32 + b;
}

/* q = a / b, r = a % b (b != 0). q or r may be NULL. */
void lq_mag_divmod(mag *q, mag *r, const mag *a, const mag *b) {
    if (b->n == 1) {
        mag t;
        lq_mag_init(&t);
        lq_mag_copy(&t, a);
        uint32_t rem = lq_mag_divmod_small(&t, b->d[0]);
        if (q) { lq_mag_free(q); *q = t; } else lq_mag_free(&t);
        if (r) lq_mag_set_u64(r, rem);
        return;
    }
    mag qq, rr;
    lq_mag_init(&qq);
    lq_mag_init(&rr);
    size_t nb = lq_mag_bits(a);
    lq_mag_reserve(&qq, a->n ? a->n : 1);
    memset(qq.d, 0, (a->n ? a->n : 1) * sizeof(uint32_t));
    qq.n = a->n;
    for (size_t i = nb; i-- > 0;) {
        /* rr = rr << 1 | bit */
        uint32_t carry = (a->d[i / 32] >> (i % 32)) & 1;
        for (size_t k = 0; k < rr.n; k++) {
            uint32_t nc = rr.d[k] >> 31;
            rr.d[k] = (rr.d[k] << 1) | carry;
            carry = nc;
        }
        if (carry) {
            lq_mag_reserve(&rr, rr.n + 1);
            rr.d[rr.n++] = carry;
        }
        if (lq_mag_cmp(&rr, b) >= 0) {
            lq_mag_sub(&rr, &rr, b);
            qq.d[i / 32] |= (uint32_t)1 << (i % 32);
        }
    }
    lq_mag_trim(&qq);
    if (q) { lq_mag_free(q); *q = qq; } else lq_mag_free(&qq);
    if (r) { lq_mag_free(r); *r = rr; } else lq_mag_free(&rr);
}

/* append decimal digits of m */
void lq_mag_to_dec(lq_buf *b, const mag *m) {
    if (!m->n) {
        lq_buf_putc(b, '0');
        return;
    }
    mag t;
    lq_mag_init(&t);
    lq_mag_copy(&t, m);
    size_t cap = m->n * 10 + 1;
    uint32_t *chunks = lq_xmalloc(cap * sizeof(uint32_t));
    size_t nc = 0;
    while (t.n) chunks[nc++] = lq_mag_divmod_small(&t, 1000000000u);
    lq_buf_printf(b, "%u", chunks[nc - 1]);
    for (size_t i = nc - 1; i-- > 0;) lq_buf_printf(b, "%09u", chunks[i]);
    free(chunks);
    lq_mag_free(&t);
}

/* parse decimal digits (only '0'-'9') */
static void lq_mag_from_dec(mag *m, const char *s, size_t n) {
    m->n = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t chunk = 0, mul = 1;
        size_t k = 0;
        while (k < 9 && i < n) {
            chunk = chunk * 10 + (uint32_t)(s[i] - '0');
            mul *= 10;
            i++;
            k++;
        }
        lq_mag_mul_small(m, mul);
        lq_mag_add_small(m, chunk);
    }
}

static void lq_mag_pow10_mul(mag *m, int64_t k) {
    while (k >= 9) {
        lq_mag_mul_small(m, 1000000000u);
        k -= 9;
    }
    static const uint32_t p10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if (k > 0) lq_mag_mul_small(m, p10[k]);
}

static double lq_mag_to_double(const mag *m) {
    double r = 0;
    for (size_t i = m->n; i-- > 0;) r = r * 4294967296.0 + m->d[i];
    return r;
}

/* ------------------------------------------------------------------ */
/* Big integers as values                                              */
/* ------------------------------------------------------------------ */

/* Integers above this many limbs degrade to Float (documented limit). */
#define BIGINT_MAX_LIMBS 64

static lq_value bigint_from_mag(int neg, const mag *m) {
    if (m->n <= 2) {
        uint64_t v = m->n ? m->d[0] : 0;
        if (m->n == 2) v |= (uint64_t)m->d[1] << 32;
        if (!neg && v <= (uint64_t)INT64_MAX) return lq_int((int64_t)v);
        if (neg && v <= (uint64_t)INT64_MAX + 1) return lq_int(v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v);
    }
    if (m->n > BIGINT_MAX_LIMBS) {
        double d = lq_mag_to_double(m);
        return lq_float(neg ? -d : d);
    }
    lq_bigint *b = lq_xmalloc(sizeof(lq_bigint) + m->n * sizeof(uint32_t));
    b->h.rc = 1;
    b->neg = neg;
    b->n = m->n;
    memcpy(b->limb, m->d, m->n * sizeof(uint32_t));
    lq_value v;
    v.type = LQ_BIGINT;
    v.u.bi = b;
    return v;
}

lq_value lq_bigint_normalize(lq_bigint *b) {
    mag m = {b->limb, b->n, b->n};
    return bigint_from_mag(b->neg, &m);
}

/* integer value (INT/BIGINT) -> sign + magnitude (caller frees mag) */
static void int_to_mag(lq_value v, int *neg, mag *m) {
    lq_mag_init(m);
    if (v.type == LQ_INT) {
        *neg = v.u.i < 0;
        uint64_t u = v.u.i < 0 ? (uint64_t)0 - (uint64_t)v.u.i : (uint64_t)v.u.i;
        lq_mag_set_u64(m, u);
    } else {
        *neg = v.u.bi->neg;
        lq_mag_reserve(m, v.u.bi->n);
        memcpy(m->d, v.u.bi->limb, v.u.bi->n * sizeof(uint32_t));
        m->n = v.u.bi->n;
    }
}

void lq_bigint_fmt(lq_buf *b, const lq_bigint *x) {
    if (x->neg) lq_buf_putc(b, '-');
    mag m = {(uint32_t *)x->limb, x->n, x->n};
    lq_mag_to_dec(b, &m);
}

/* Integer#size for a bignum: bytes needed for the magnitude */
int64_t lq_bigint_size(const lq_bigint *x) {
    if (!x->n) return 0;
    uint32_t top = x->limb[x->n - 1];
    int64_t bits = (int64_t)(x->n - 1) * 32;
    while (top) {
        bits++;
        top >>= 1;
    }
    return (bits + 7) / 8;
}

double lq_bigint_to_double(const lq_bigint *x) {
    mag m = {(uint32_t *)x->limb, x->n, x->n};
    double d = lq_mag_to_double(&m);
    return x->neg ? -d : d;
}

bool lq_is_numeric(lq_value v) {
    return v.type == LQ_INT || v.type == LQ_FLOAT || v.type == LQ_BIGINT;
}

static int cmp_int_float(int64_t i, double f) {
    if (f != f) return 2;
    if (f >= 9223372036854775808.0) return -1;
    if (f < -9223372036854775808.0) return 1;
    double t = trunc(f);
    int64_t ti = (int64_t)t;
    if (i < ti) return -1;
    if (i > ti) return 1;
    double frac = f - t;
    if (frac > 0) return -1;
    if (frac < 0) return 1;
    return 0;
}

static int cmp_ints(lq_value a, lq_value b) {
    if (a.type == LQ_INT && b.type == LQ_INT) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i;
    int an, bn;
    mag am, bm;
    int_to_mag(a, &an, &am);
    int_to_mag(b, &bn, &bm);
    int r;
    if (an != bn) r = an ? -1 : 1;
    else {
        r = lq_mag_cmp(&am, &bm);
        if (an) r = -r;
    }
    lq_mag_free(&am);
    lq_mag_free(&bm);
    return r;
}

/* Returns -1/0/1, or 2 when unordered (NaN). */
int lq_num_cmp(lq_value a, lq_value b) {
    if (a.type == LQ_FLOAT && b.type == LQ_FLOAT) {
        if (a.u.f != a.u.f || b.u.f != b.u.f) return 2;
        return a.u.f < b.u.f ? -1 : a.u.f > b.u.f;
    }
    if (a.type == LQ_FLOAT || b.type == LQ_FLOAT) {
        bool swap = a.type == LQ_FLOAT;
        lq_value iv = swap ? b : a;
        double f = swap ? a.u.f : b.u.f;
        int r;
        if (iv.type == LQ_INT) r = cmp_int_float(iv.u.i, f);
        else {
            if (f != f) return 2;
            double d = lq_bigint_to_double(iv.u.bi);
            r = d < f ? -1 : d > f;
        }
        if (r == 2) return 2;
        return swap ? -r : r;
    }
    return cmp_ints(a, b);
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

void lq_fmt_int(lq_buf *b, int64_t i) {
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)i);
    lq_buf_put(b, tmp, (size_t)n);
}

/* Shortest round-trip digits of |d| (d finite, nonzero). Writes digits to
   out (no dot) and returns the decimal exponent `decpt` such that
   value = 0.DIGITS * 10^decpt. */
static int shortest_digits(double d, char *out, size_t *ndig) {
    char tmp[40];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*e", prec - 1, d);
        if (strtod(tmp, NULL) == d || prec == 17) break;
    }
    /* tmp: D.DDDDe[+-]XX */
    size_t n = 0;
    const char *p = tmp;
    if (*p == '-') p++;
    while (*p && *p != 'e') {
        if (*p >= '0' && *p <= '9') out[n++] = *p;
        p++;
    }
    int exp10 = atoi(p + 1);
    while (n > 1 && out[n - 1] == '0') n--;
    out[n] = 0;
    *ndig = n;
    return exp10 + 1;
}

void lq_fmt_float(lq_buf *b, double d) {
    if (d != d) {
        lq_buf_puts(b, "NaN");
        return;
    }
    if (isinf(d)) {
        lq_buf_puts(b, d < 0 ? "-Infinity" : "Infinity");
        return;
    }
    if (d == 0) {
        lq_buf_puts(b, signbit(d) ? "-0.0" : "0.0");
        return;
    }
    char dig[40];
    size_t nd;
    int decpt = shortest_digits(d, dig, &nd);
    if (d < 0) lq_buf_putc(b, '-');
    /* Ruby prints fixed notation for decimal exponents in (-4, 16), and also
       at 16 when the value has a fractional part */
    if (decpt > -4 && (decpt < 16 || (decpt == 16 && nd > 16))) {
        if (decpt <= 0) {
            lq_buf_put(b, "0.", 2);
            for (int i = 0; i < -decpt; i++) lq_buf_putc(b, '0');
            lq_buf_put(b, dig, nd);
        } else if ((size_t)decpt >= nd) {
            lq_buf_put(b, dig, nd);
            for (size_t i = nd; i < (size_t)decpt; i++) lq_buf_putc(b, '0');
            lq_buf_put(b, ".0", 2);
        } else {
            lq_buf_put(b, dig, (size_t)decpt);
            lq_buf_putc(b, '.');
            lq_buf_put(b, dig + decpt, nd - (size_t)decpt);
        }
    } else {
        lq_buf_putc(b, dig[0]);
        lq_buf_putc(b, '.');
        if (nd > 1) lq_buf_put(b, dig + 1, nd - 1);
        else lq_buf_putc(b, '0');
        lq_buf_printf(b, "e%c%02d", decpt - 1 < 0 ? '-' : '+', abs(decpt - 1));
    }
}

/* ------------------------------------------------------------------ */
/* Integer parsing                                                     */
/* ------------------------------------------------------------------ */

/* digits are pure 0-9 */
static lq_value int_from_digits(int neg, const char *s, size_t n) {
    while (n > 1 && *s == '0') {
        s++;
        n--;
    }
    if (n <= 18) {
        int64_t v = 0;
        for (size_t i = 0; i < n; i++) v = v * 10 + (s[i] - '0');
        return lq_int(neg ? -v : v);
    }
    if (n > BIGINT_MAX_LIMBS * 9) {
        /* too large for exact handling: degrade to float */
        char *tmp = lq_xmalloc(n + 2);
        tmp[0] = neg ? '-' : '+';
        memcpy(tmp + 1, s, n);
        tmp[n + 1] = 0;
        double d = strtod(tmp, NULL);
        free(tmp);
        return lq_float(d);
    }
    mag m;
    lq_mag_init(&m);
    lq_mag_from_dec(&m, s, n);
    lq_value v = bigint_from_mag(neg, &m);
    lq_mag_free(&m);
    return v;
}

lq_value lq_parse_int_text(const char *s, size_t n) {
    int neg = 0;
    if (n && (*s == '-' || *s == '+')) {
        neg = *s == '-';
        s++;
        n--;
    }
    return int_from_digits(neg, s, n);
}

/* Collect digits allowing single underscores between digits. */
static size_t collect_digits(const char *s, size_t n, int base, lq_buf *out, bool *trailing_junk) {
    size_t i = 0;
    bool last_us = false;
    *trailing_junk = false;
    while (i < n) {
        char c = s[i];
        int dv = -1;
        if (c >= '0' && c <= '9') dv = c - '0';
        else if (c >= 'a' && c <= 'z') dv = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') dv = c - 'A' + 10;
        if (dv >= 0 && dv < base) {
            lq_buf_putc(out, c);
            last_us = false;
            i++;
            continue;
        }
        if (c == '_' && out->len > 0 && !last_us && i + 1 < n) {
            last_us = true;
            i++;
            continue;
        }
        break;
    }
    if (last_us) { i--; *trailing_junk = true; }
    return i;
}

lq_value lq_str_to_i(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && lq_regex_space((unsigned char)s[i])) i++;
    int neg = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        neg = s[i] == '-';
        i++;
    }
    lq_buf d;
    lq_buf_init(&d);
    bool junk;
    collect_digits(s + i, n - i, 10, &d, &junk);
    lq_value v = d.len ? int_from_digits(neg, d.p, d.len) : lq_int(0);
    lq_buf_free(&d);
    return v;
}

static lq_value int_from_base(int neg, const char *dig, size_t n, int base) {
    if (base == 10) return int_from_digits(neg, dig, n);
    mag m;
    lq_mag_init(&m);
    for (size_t i = 0; i < n; i++) {
        char c = dig[i];
        int dv = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        lq_mag_mul_small(&m, (uint32_t)base);
        lq_mag_add_small(&m, (uint32_t)dv);
        if (m.n > BIGINT_MAX_LIMBS + 1) break;
    }
    lq_value v = bigint_from_mag(neg, &m);
    lq_mag_free(&m);
    return v;
}

/* Kernel#Integer(str). Digits may be separated by single underscores.
   Ruby validates the remainder differently for "small" literals (up to 19
   decimal digits, no underscores: everything after the digits must be
   whitespace) and big/underscored ones (a NUL byte ends the string). */
bool lq_ruby_Integer(const char *s, size_t n, lq_value *out) {
    size_t i = 0;
    while (i < n && lq_regex_space((unsigned char)s[i])) i++;
    if (i >= n) return false;
    int neg = 0;
    if (s[i] == '-' || s[i] == '+') {
        neg = s[i] == '-';
        i++;
    }
    int base = 10;
    if (i + 1 < n && s[i] == '0') {
        char p = s[i + 1];
        if (p == 'x' || p == 'X') { base = 16; i += 2; }
        else if (p == 'b' || p == 'B') { base = 2; i += 2; }
        else if (p == 'o' || p == 'O') { base = 8; i += 2; }
        else if (p == 'd' || p == 'D') { base = 10; i += 2; }
        else if (p == '_' || (p >= '0' && p <= '9')) { base = 8; i += 1; }
    }
    lq_buf d;
    lq_buf_init(&d);
    bool has_us = false, ok = true;
    while (i < n) {
        char c = s[i];
        int dv = -1;
        if (c >= '0' && c <= '9') dv = c - '0';
        else if (c >= 'a' && c <= 'z') dv = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') dv = c - 'A' + 10;
        if (dv >= 0 && dv < base) {
            lq_buf_putc(&d, c);
            i++;
            continue;
        }
        if (c == '_') {
            /* must sit between two digits */
            if (d.len == 0 || i + 1 >= n) { ok = false; break; }
            char nx = s[i + 1];
            int nv = -1;
            if (nx >= '0' && nx <= '9') nv = nx - '0';
            else if (nx >= 'a' && nx <= 'z') nv = nx - 'a' + 10;
            else if (nx >= 'A' && nx <= 'Z') nv = nx - 'A' + 10;
            if (nv < 0 || nv >= base) { ok = false; break; }
            has_us = true;
            i++;
            continue;
        }
        break;
    }
    if (ok && d.len == 0) {
        /* a lone "0" prefix (e.g. "0") is handled by the octal branch */
        ok = false;
        if (base == 8 && i >= 1 && s[i - 1] == '0') {
            lq_buf_putc(&d, '0');
            ok = true;
        }
    }
    if (ok) {
        /* "small" = fits in an unsigned 64-bit integer */
        bool fits = true;
        uint64_t acc = 0;
        for (size_t k = 0; k < d.len && fits; k++) {
            char c = d.p[k];
            uint64_t dv = (uint64_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            if (acc > (UINT64_MAX - dv) / (uint64_t)base) fits = false;
            else acc = acc * (uint64_t)base + dv;
        }
        bool big = has_us || !fits;
        while (i < n && lq_regex_space((unsigned char)s[i])) i++;
        if (i < n && !(big && s[i] == 0)) ok = false;
    }
    if (ok) *out = int_from_base(neg, d.p, d.len, base);
    lq_buf_free(&d);
    return ok;
}

double lq_str_to_f(const char *s, size_t n) {
    char *tmp = lq_xstrndup(s, n);
    double d = strtod(tmp, NULL);
    free(tmp);
    return d;
}

/* ------------------------------------------------------------------ */
/* Integer arithmetic (Ruby semantics, promotes to bigint)             */
/* ------------------------------------------------------------------ */

lq_value lq_int_add(lq_value a, lq_value b, bool sub) {
    if (a.type == LQ_INT && b.type == LQ_INT) {
        int64_t r;
        bool ovf = sub ? __builtin_sub_overflow(a.u.i, b.u.i, &r) : __builtin_add_overflow(a.u.i, b.u.i, &r);
        if (!ovf) return lq_int(r);
    }
    int an, bn;
    mag am, bm, rm;
    int_to_mag(a, &an, &am);
    int_to_mag(b, &bn, &bm);
    if (sub) bn = !bn;
    lq_mag_init(&rm);
    int rn;
    if (an == bn) {
        lq_mag_add(&rm, &am, &bm);
        rn = an;
    } else if (lq_mag_cmp(&am, &bm) >= 0) {
        lq_mag_sub(&rm, &am, &bm);
        rn = an;
    } else {
        lq_mag_sub(&rm, &bm, &am);
        rn = bn;
    }
    lq_value v = bigint_from_mag(rm.n ? rn : 0, &rm);
    lq_mag_free(&am);
    lq_mag_free(&bm);
    lq_mag_free(&rm);
    return v;
}

lq_value lq_int_mul(lq_value a, lq_value b) {
    if (a.type == LQ_INT && b.type == LQ_INT) {
        int64_t r;
        if (!__builtin_mul_overflow(a.u.i, b.u.i, &r)) return lq_int(r);
    }
    int an, bn;
    mag am, bm, rm;
    int_to_mag(a, &an, &am);
    int_to_mag(b, &bn, &bm);
    lq_mag_init(&rm);
    lq_mag_mul(&rm, &am, &bm);
    lq_value v = bigint_from_mag(rm.n ? (an != bn) : 0, &rm);
    lq_mag_free(&am);
    lq_mag_free(&bm);
    lq_mag_free(&rm);
    return v;
}

/* floor division / modulo; b must be nonzero */
void lq_int_divmod(lq_value a, lq_value b, lq_value *q, lq_value *r) {
    if (a.type == LQ_INT && b.type == LQ_INT && !(a.u.i == INT64_MIN && b.u.i == -1)) {
        int64_t qq = a.u.i / b.u.i, rr = a.u.i % b.u.i;
        if (rr != 0 && ((rr < 0) != (b.u.i < 0))) {
            qq -= 1;
            rr += b.u.i;
        }
        if (q) *q = lq_int(qq);
        if (r) *r = lq_int(rr);
        return;
    }
    int an, bn;
    mag am, bm, qm, rm;
    int_to_mag(a, &an, &am);
    int_to_mag(b, &bn, &bm);
    lq_mag_init(&qm);
    lq_mag_init(&rm);
    lq_mag_divmod(&qm, &rm, &am, &bm);
    int qneg = an != bn;
    if (qneg && rm.n) {
        /* floor: q = -(|a|/|b|) - 1, r = |b| - r with sign of b */
        lq_mag_add_small(&qm, 1);
        lq_mag_sub(&rm, &bm, &rm);
    }
    if (q) *q = bigint_from_mag(qm.n ? qneg : 0, &qm);
    if (r) *r = bigint_from_mag(rm.n ? bn : 0, &rm);
    lq_mag_free(&am);
    lq_mag_free(&bm);
    lq_mag_free(&qm);
    lq_mag_free(&rm);
}

/* ------------------------------------------------------------------ */
/* Decimals (BigDecimal emulation)                                     */
/* ------------------------------------------------------------------ */

/* Significant digits kept when importing decimal strings. */
#define DEC_MAX_DIGITS 100
/* Digits computed for quotients. */
#define DEC_DIV_DIGITS 40

void lq_dec_init(dec *d) {
    d->special = DEC_FINITE;
    d->neg = 0;
    lq_mag_init(&d->m);
    d->e = 0;
}

void lq_dec_free(dec *d) { lq_mag_free(&d->m); }

void lq_dec_from_int(dec *d, lq_value v) {
    lq_dec_init(d);
    lq_mag_free(&d->m);
    int_to_mag(v, &d->neg, &d->m);
    if (!d->m.n) d->neg = 0;
}

/* parse [-]digits[.digits][e[+-]digits] */
void lq_dec_from_text(dec *d, const char *s, size_t n) {
    lq_dec_init(d);
    size_t i = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        d->neg = s[i] == '-';
        i++;
    }
    lq_buf dig;
    lq_buf_init(&dig);
    int64_t exp = 0;
    bool seen_dot = false;
    size_t sig = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (dig.len == 0 && c == '0') {
                if (seen_dot) exp--;
                continue;
            }
            if (sig < DEC_MAX_DIGITS) {
                lq_buf_putc(&dig, c);
                sig++;
                if (seen_dot) exp--;
            } else if (!seen_dot) {
                exp++;
            }
        } else if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else if (c == 'e' || c == 'E') {
            long long ev = strtoll(s + i + 1, NULL, 10);
            if (ev > 100000) ev = 100000;
            if (ev < -100000) ev = -100000;
            exp += ev;
            break;
        } else if (c != '_') {
            break;
        }
    }
    lq_mag_from_dec(&d->m, dig.p ? dig.p : "", dig.len);
    d->e = exp;
    if (!d->m.n) d->e = 0; /* keep the sign: BigDecimal("-0.0") is -0 */
    lq_buf_free(&dig);
}

void lq_dec_from_double(dec *d, double f) {
    if (f != f) {
        lq_dec_init(d);
        d->special = DEC_NAN;
        return;
    }
    if (isinf(f)) {
        lq_dec_init(d);
        d->special = DEC_INF;
        d->neg = f < 0;
        return;
    }
    lq_buf b;
    lq_buf_init(&b);
    lq_fmt_float(&b, f);
    lq_dec_from_text(d, b.p, b.len);
    if (f == 0 && signbit(f)) d->neg = 1;
    lq_buf_free(&b);
}

static size_t lq_mag_digits(const mag *m) {
    if (!m->n) return 0;
    lq_buf b;
    lq_buf_init(&b);
    lq_mag_to_dec(&b, m);
    size_t n = b.len;
    lq_buf_free(&b);
    return n;
}

double lq_dec_to_double(const dec *d) {
    if (d->special == DEC_NAN) return NAN;
    if (d->special == DEC_INF) return d->neg ? -INFINITY : INFINITY;
    if (!d->m.n) return d->neg ? -0.0 : 0.0;
    lq_buf b;
    lq_buf_init(&b);
    if (d->neg) lq_buf_putc(&b, '-');
    lq_mag_to_dec(&b, &d->m);
    lq_buf_printf(&b, "e%lld", (long long)d->e);
    double r = strtod(b.p, NULL);
    lq_buf_free(&b);
    return r;
}

/* align exponents: returns copies scaled to common exponent */
static bool dec_align(const dec *a, const dec *b, mag *am, mag *bm, int64_t *e) {
    lq_mag_init(am);
    lq_mag_init(bm);
    lq_mag_copy(am, &a->m);
    lq_mag_copy(bm, &b->m);
    int64_t ea = a->e, eb = b->e;
    if (a->m.n == 0) ea = eb;
    if (b->m.n == 0) eb = ea;
    if (ea - eb > 2000 || eb - ea > 2000) return false;
    if (ea > eb) {
        lq_mag_pow10_mul(am, ea - eb);
        *e = eb;
    } else {
        lq_mag_pow10_mul(bm, eb - ea);
        *e = ea;
    }
    return true;
}

static int64_t dec_adjexp(const dec *d) { return d->e + (int64_t)lq_mag_digits(&d->m); }

void lq_dec_add(dec *r, const dec *a, const dec *b, bool sub) {
    int bneg = sub ? !b->neg : b->neg;
    lq_dec_init(r);
    if (a->special == DEC_NAN || b->special == DEC_NAN) { r->special = DEC_NAN; return; }
    if (a->special == DEC_INF || b->special == DEC_INF) {
        if (a->special == DEC_INF && b->special == DEC_INF && a->neg != bneg) { r->special = DEC_NAN; return; }
        r->special = DEC_INF;
        r->neg = a->special == DEC_INF ? a->neg : bneg;
        return;
    }
    if (!a->m.n && !b->m.n) {
        /* signed zeros: negative only when both are negative */
        r->neg = a->neg && bneg;
        return;
    }
    mag am, bm;
    int64_t e;
    if (!dec_align(a, b, &am, &bm, &e)) {
        /* magnitudes too far apart: the smaller is negligible */
        bool a_big = a->m.n && (!b->m.n || dec_adjexp(a) > dec_adjexp(b));
        const dec *big = a_big ? a : b;
        lq_mag_copy(&r->m, &big->m);
        r->e = big->e;
        r->neg = a_big ? a->neg : bneg;
        lq_mag_free(&am);
        lq_mag_free(&bm);
        return;
    }
    r->e = e;
    if (a->neg == bneg) {
        lq_mag_add(&r->m, &am, &bm);
        r->neg = a->neg;
    } else if (lq_mag_cmp(&am, &bm) >= 0) {
        lq_mag_sub(&r->m, &am, &bm);
        r->neg = a->neg;
    } else {
        lq_mag_sub(&r->m, &bm, &am);
        r->neg = bneg;
    }
    if (!r->m.n) r->neg = 0;
    lq_mag_free(&am);
    lq_mag_free(&bm);
}

void lq_dec_mul(dec *r, const dec *a, const dec *b) {
    lq_dec_init(r);
    if (a->special == DEC_NAN || b->special == DEC_NAN) { r->special = DEC_NAN; return; }
    if (a->special == DEC_INF || b->special == DEC_INF) {
        if ((a->special == DEC_FINITE && !a->m.n) || (b->special == DEC_FINITE && !b->m.n)) {
            r->special = DEC_NAN;
            return;
        }
        r->special = DEC_INF;
        r->neg = a->neg != b->neg;
        return;
    }
    lq_mag_mul(&r->m, &a->m, &b->m);
    r->e = a->e + b->e;
    r->neg = a->neg != b->neg; /* BigDecimal keeps signed zeros */
}

/* r = a / b with DEC_DIV_DIGITS significant digits (truncated). */
void lq_dec_div(dec *r, const dec *a, const dec *b) {
    lq_dec_init(r);
    if (a->special == DEC_NAN || b->special == DEC_NAN) { r->special = DEC_NAN; return; }
    if (a->special == DEC_INF) {
        if (b->special == DEC_INF) { r->special = DEC_NAN; return; }
        r->special = DEC_INF;
        r->neg = a->neg != b->neg;
        return;
    }
    if (b->special == DEC_INF) return; /* zero */
    if (!b->m.n) {
        if (!a->m.n) r->special = DEC_NAN;
        else {
            r->special = DEC_INF;
            r->neg = a->neg;
        }
        return;
    }
    if (!a->m.n) {
        r->neg = a->neg != b->neg;
        return;
    }
    int64_t da = (int64_t)lq_mag_digits(&a->m), db = (int64_t)lq_mag_digits(&b->m);
    int64_t k = DEC_DIV_DIGITS - (da - db);
    if (k < 0) k = 0;
    mag num;
    lq_mag_init(&num);
    lq_mag_copy(&num, &a->m);
    lq_mag_pow10_mul(&num, k);
    lq_mag_divmod(&r->m, NULL, &num, &b->m);
    lq_mag_free(&num);
    r->e = a->e - b->e - k;
    r->neg = r->m.n ? a->neg != b->neg : 0;
}

/* floor(a / b) as decimal integer and remainder a - b*q (exact). */
bool lq_dec_mod(dec *r, const dec *a, const dec *b) {
    lq_dec_init(r);
    if (a->special != DEC_FINITE || b->special != DEC_FINITE) {
        r->special = DEC_NAN;
        return true;
    }
    if (!b->m.n) return false; /* ZeroDivisionError */
    mag am, bm, qm, rm;
    int64_t e;
    if (!dec_align(a, b, &am, &bm, &e)) {
        lq_mag_free(&am);
        lq_mag_free(&bm);
        r->special = DEC_NAN;
        return true;
    }
    lq_mag_init(&qm);
    lq_mag_init(&rm);
    lq_mag_divmod(&qm, &rm, &am, &bm);
    if (a->neg != b->neg && rm.n) lq_mag_sub(&rm, &bm, &rm);
    lq_mag_copy(&r->m, &rm);
    r->e = e;
    r->neg = rm.n ? b->neg : 0;
    lq_mag_free(&am);
    lq_mag_free(&bm);
    lq_mag_free(&qm);
    lq_mag_free(&rm);
    return true;
}

int lq_dec_cmp(const dec *a, const dec *b) {
    if (a->special == DEC_NAN || b->special == DEC_NAN) return 2;
    int sa = a->special == DEC_INF ? (a->neg ? -2 : 2) : (a->m.n ? (a->neg ? -1 : 1) : 0);
    int sb = b->special == DEC_INF ? (b->neg ? -2 : 2) : (b->m.n ? (b->neg ? -1 : 1) : 0);
    if (sa != sb || sa == 2 || sa == -2 || sa == 0) return sa < sb ? -1 : sa > sb;
    int64_t xa = dec_adjexp(a), xb = dec_adjexp(b);
    int r;
    if (xa != xb) r = xa < xb ? -1 : 1;
    else {
        mag am, bm;
        int64_t e;
        dec_align(a, b, &am, &bm, &e);
        r = lq_mag_cmp(&am, &bm);
        lq_mag_free(&am);
        lq_mag_free(&bm);
    }
    return sa < 0 ? -r : r;
}

/* Round to `digits` decimal places using ROUND_HALF_UP (away from zero on
   ties) or floor/ceil. mode: 0 half-up, 1 floor, 2 ceil. */
void lq_dec_round(dec *r, const dec *a, int64_t digits, int mode) {
    lq_dec_init(r);
    if (a->special != DEC_FINITE) {
        r->special = a->special;
        r->neg = a->neg;
        return;
    }
    int64_t drop = -digits - a->e; /* number of trailing digits to remove */
    if (drop <= 0) {
        lq_mag_copy(&r->m, &a->m);
        r->e = a->e;
        r->neg = a->neg;
        return;
    }
    int64_t nd = (int64_t)lq_mag_digits(&a->m);
    mag q, rem, p;
    lq_mag_init(&q);
    lq_mag_init(&rem);
    lq_mag_init(&p);
    bool round_up;
    if (drop > nd + 1) {
        /* everything is dropped */
        lq_mag_set_u64(&q, 0);
        bool nonzero = a->m.n != 0;
        round_up = mode == 1 ? (a->neg && nonzero) : mode == 2 ? (!a->neg && nonzero) : false;
    } else {
        lq_mag_set_u64(&p, 1);
        lq_mag_pow10_mul(&p, drop);
        lq_mag_divmod(&q, &rem, &a->m, &p);
        if (mode == 0) {
            mag twice;
            lq_mag_init(&twice);
            lq_mag_add(&twice, &rem, &rem);
            round_up = lq_mag_cmp(&twice, &p) >= 0;
            lq_mag_free(&twice);
        } else if (mode == 1) {
            round_up = a->neg && rem.n;
        } else {
            round_up = !a->neg && rem.n;
        }
    }
    if (round_up) lq_mag_add_small(&q, 1);
    lq_mag_copy(&r->m, &q);
    r->e = a->e + drop;
    r->neg = r->m.n ? a->neg : 0;
    lq_mag_free(&q);
    lq_mag_free(&rem);
    lq_mag_free(&p);
}

/* integer value of a finite decimal with e >= 0 (or truncated) */
lq_value lq_dec_to_int(const dec *d) {
    if (d->special != DEC_FINITE) return lq_int(0);
    mag m;
    lq_mag_init(&m);
    lq_mag_copy(&m, &d->m);
    if (d->e > 0) {
        if (d->e > BIGINT_MAX_LIMBS * 10) {
            lq_mag_free(&m);
            return lq_float(d->neg ? -INFINITY : INFINITY);
        }
        lq_mag_pow10_mul(&m, d->e);
    } else if (d->e < 0) {
        mag p;
        lq_mag_init(&p);
        lq_mag_set_u64(&p, 1);
        lq_mag_pow10_mul(&p, -d->e);
        mag q;
        lq_mag_init(&q);
        lq_mag_divmod(&q, NULL, &m, &p);
        lq_mag_free(&m);
        m = q;
        lq_mag_free(&p);
    }
    lq_value v = bigint_from_mag(m.n ? d->neg : 0, &m);
    lq_mag_free(&m);
    return v;
}

/* 2^k as an integer value (k <= 4096) */
lq_value lq_pow2(int64_t k) {
    mag m;
    lq_mag_init(&m);
    size_t limbs = (size_t)(k / 32) + 1;
    lq_mag_reserve(&m, limbs);
    memset(m.d, 0, limbs * sizeof(uint32_t));
    m.d[k / 32] = (uint32_t)1 << (k % 32);
    m.n = limbs;
    lq_value v = bigint_from_mag(0, &m);
    lq_mag_free(&m);
    return v;
}
