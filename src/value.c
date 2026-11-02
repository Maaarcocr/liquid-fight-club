/*
 * value.c - allocation, buffers, the Liquid value model and the core
 * semantic questions (truthiness, output, inspect, equality, blank/empty).
 */
#include "lq.h"
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

static void oom(void) {
    fputs("liquid: out of memory\n", stderr);
    abort();
}

void *lq_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *lq_xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *lq_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *lq_xstrndup(const char *s, size_t n) {
    char *p = lq_xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

void lq_buf_init(lq_buf *b) {
    b->p = NULL;
    b->len = b->cap = 0;
}

void lq_buf_free(lq_buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void lq_buf_reserve(lq_buf *b, size_t extra) {
    if (extra > SIZE_MAX / 2 - b->len) oom();
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    b->p = lq_xrealloc(b->p, nc);
    b->cap = nc;
}

void lq_buf_put(lq_buf *b, const char *s, size_t n) {
    if (!n) return;
    lq_buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void lq_buf_putc(lq_buf *b, char c) {
    lq_buf_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void lq_buf_puts(lq_buf *b, const char *s) { lq_buf_put(b, s, strlen(s)); }

void lq_buf_printf(lq_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        lq_buf_put(b, tmp, (size_t)n);
        return;
    }
    lq_buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *lq_buf_detach(lq_buf *b, size_t *len) {
    if (!b->p) lq_buf_reserve(b, 0);
    b->p[b->len] = 0;
    char *p = b->p;
    if (len) *len = b->len;
    b->p = NULL;
    b->len = b->cap = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

size_t lq_utf8_decode(const unsigned char *p, size_t avail, int32_t *cp) {
    if (!avail) { *cp = -1; return 0; }
    unsigned c = p[0];
    if (c < 0x80) { *cp = (int32_t)c; return 1; }
    size_t n;
    int32_t v;
    int32_t min;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; v = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; v = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; v = c & 0x07; min = 0x10000; }
    else { *cp = -1; return 1; }
    if (avail < n) { *cp = -1; return 1; }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = -1; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = -1; return 1; }
    *cp = v;
    return n;
}

size_t lq_utf8_len(const unsigned char *p, size_t avail) {
    int32_t cp;
    size_t n = lq_utf8_decode(p, avail, &cp);
    return n ? n : 1;
}

bool lq_utf8_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        if (p[i] < 0x80) { i++; continue; }
        int32_t cp;
        size_t k = lq_utf8_decode(p + i, n - i, &cp);
        if (cp < 0) return false;
        i += k;
    }
    return true;
}

size_t lq_utf8_encode(char *out, int32_t cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool lq_ruby_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == 0;
}

bool lq_regex_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

static uint32_t g_hash_seed = 0x9747b28cu;

uint32_t lq_hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u ^ g_hash_seed;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    h ^= h >> 15;
    h *= 0x2c1b3c6dU;
    h ^= h >> 12;
    return h ? h : 1;
}

static struct {
    lq_str s;
    char nul;
} g_empty_str = {{{LQ_RC_STATIC}, 0, 0, 0}, 0};

lq_value lq_str_empty(void) { return lq_str_val(&g_empty_str.s); }

lq_value lq_str_new(const char *s, size_t n) {
    if (n == 0) return lq_str_empty();
    if (n > SIZE_MAX - sizeof(lq_str) - 1) oom();
    lq_str *str = lq_xmalloc(sizeof(lq_str) + n + 1);
    str->h.rc = 1;
    str->len = n;
    str->hash = 0;
    str->nchars = -1;
    memcpy(str->data, s, n);
    str->data[n] = 0;
    return lq_str_val(str);
}

lq_value lq_str_cstr(const char *s) { return lq_str_new(s, strlen(s)); }

lq_value lq_str_take_buf(lq_buf *b) {
    lq_value v = lq_str_new(b->p ? b->p : "", b->len);
    lq_buf_free(b);
    return v;
}

size_t lq_str_nchars(lq_str *s) {
    if (s->nchars >= 0) return (size_t)s->nchars;
    size_t n = 0, i = 0;
    const unsigned char *p = (const unsigned char *)s->data;
    while (i < s->len) {
        if (p[i] < 0x80) i++;
        else i += lq_utf8_len(p + i, s->len - i);
        n++;
    }
    if (n <= INT32_MAX) s->nchars = (int32_t)n;
    return n;
}

/* ------------------------------------------------------------------ */
/* Arrays                                                              */
/* ------------------------------------------------------------------ */

lq_value lq_arr_new(size_t cap) {
    lq_arr *a = lq_xmalloc(sizeof(lq_arr));
    a->h.rc = 1;
    a->len = 0;
    a->cap = cap;
    a->items = cap ? lq_xmalloc(cap * sizeof(lq_value)) : NULL;
    lq_value v;
    v.type = LQ_ARR;
    v.u.a = a;
    return v;
}

void lq_arr_push_take(lq_arr *a, lq_value v) {
    if (a->len == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 4;
        if (nc > SIZE_MAX / sizeof(lq_value)) oom();
        a->items = lq_xrealloc(a->items, nc * sizeof(lq_value));
        a->cap = nc;
    }
    a->items[a->len++] = v;
}

void lq_arr_push(lq_arr *a, lq_value v) { lq_arr_push_take(a, lq_retain(v)); }

/* ------------------------------------------------------------------ */
/* Hashes (insertion ordered)                                          */
/* ------------------------------------------------------------------ */

lq_value lq_hash_new(size_t cap) {
    lq_hash *h = lq_xcalloc(1, sizeof(lq_hash));
    h->h.rc = 1;
    h->cap = cap;
    h->ents = cap ? lq_xmalloc(cap * sizeof(lq_hent)) : NULL;
    lq_value v;
    v.type = LQ_HASH;
    v.u.h = h;
    return v;
}

#define HASH_LINEAR_MAX 8

static void hash_reindex(lq_hash *h) {
    size_t ic = 16;
    while (ic < h->len * 2 + 2) ic *= 2;
    free(h->index);
    h->index = lq_xcalloc(ic, sizeof(uint32_t));
    h->index_cap = ic;
    for (size_t i = 0; i < h->len; i++) {
        uint32_t hv = lq_str_hash(h->ents[i].key);
        size_t m = ic - 1, j = hv & m;
        while (h->index[j]) j = (j + 1) & m;
        h->index[j] = (uint32_t)(i + 1);
    }
}

static long hash_find(const lq_hash *h, const char *k, size_t klen, uint32_t hv) {
    if (!h->index_cap) {
        for (size_t i = 0; i < h->len; i++) {
            lq_str *key = h->ents[i].key;
            if (key->len == klen && memcmp(key->data, k, klen) == 0) return (long)i;
        }
        return -1;
    }
    size_t m = h->index_cap - 1, j = hv & m;
    for (;;) {
        uint32_t e = h->index[j];
        if (!e) return -1;
        lq_str *key = h->ents[e - 1].key;
        if (key->len == klen && lq_str_hash(key) == hv && memcmp(key->data, k, klen) == 0)
            return (long)(e - 1);
        j = (j + 1) & m;
    }
}

bool lq_hash_get(const lq_hash *h, const char *k, size_t klen, uint32_t hv, lq_value *out) {
    long i = hash_find(h, k, klen, hv);
    if (i < 0) return false;
    if (out) *out = h->ents[i].val;
    return true;
}

bool lq_hash_get_str(const lq_hash *h, lq_str *k, lq_value *out) {
    return lq_hash_get(h, k->data, k->len, h->index_cap ? lq_str_hash(k) : 0, out);
}

bool lq_hash_get_cstr(const lq_hash *h, const char *k, lq_value *out) {
    size_t n = strlen(k);
    return lq_hash_get(h, k, n, h->index_cap ? lq_hash_bytes(k, n) : 0, out);
}

void lq_hash_set_take(lq_hash *h, lq_str *key, lq_value v) {
    uint32_t hv = lq_str_hash(key);
    long i = hash_find(h, key->data, key->len, hv);
    if (i >= 0) {
        lq_release(h->ents[i].val);
        h->ents[i].val = v;
        return;
    }
    if (h->len >= UINT32_MAX - 1) oom();
    if (h->len == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 4;
        h->ents = lq_xrealloc(h->ents, nc * sizeof(lq_hent));
        h->cap = nc;
    }
    key->h.rc < LQ_RC_STATIC ? (void)key->h.rc++ : (void)0;
    h->ents[h->len].key = key;
    h->ents[h->len].val = v;
    h->len++;
    if (h->index_cap) {
        if (h->len * 2 > h->index_cap) hash_reindex(h);
        else {
            size_t m = h->index_cap - 1, j = hv & m;
            while (h->index[j]) j = (j + 1) & m;
            h->index[j] = (uint32_t)h->len;
        }
    } else if (h->len > HASH_LINEAR_MAX) {
        hash_reindex(h);
    }
}

void lq_hash_set(lq_hash *h, lq_str *key, lq_value v) { lq_hash_set_take(h, key, lq_retain(v)); }

void lq_hash_set_cstr(lq_hash *h, const char *key, lq_value v) {
    lq_value k = lq_str_cstr(key);
    lq_hash_set(h, k.u.s, v);
    lq_release(k);
}

/* ------------------------------------------------------------------ */
/* Ranges, drops                                                       */
/* ------------------------------------------------------------------ */

lq_value lq_range_new(int64_t a, int64_t b) {
    lq_range *r = lq_xmalloc(sizeof(lq_range));
    r->h.rc = 1;
    r->first = a;
    r->last = b;
    lq_value v;
    v.type = LQ_RANGE;
    v.u.r = r;
    return v;
}

lq_value lq_drop_new(const lq_drop_vt *vt, void *data) {
    lq_drop *d = lq_xmalloc(sizeof(lq_drop));
    d->h.rc = 1;
    d->vt = vt;
    d->data = data;
    lq_value v;
    v.type = LQ_DROP;
    v.u.d = d;
    return v;
}

/* ------------------------------------------------------------------ */
/* Freeing (iterative for containers to avoid deep recursion)          */
/* ------------------------------------------------------------------ */

void lq_obj_free(lq_value v) {
    /* An explicit work stack keeps freeing deeply nested data safe. */
    lq_value stack_small[64];
    lq_value *stack = stack_small;
    size_t sp = 0, scap = 64;
    stack[sp++] = v;
    while (sp) {
        lq_value x = stack[--sp];
        switch (x.type) {
        case LQ_STR:
        case LQ_RANGE:
        case LQ_BIGINT:
            free(x.u.o);
            break;
        case LQ_DROP:
            if (x.u.d->vt && x.u.d->vt->free) x.u.d->vt->free(x.u.d);
            free(x.u.o);
            break;
        case LQ_ARR: {
            lq_arr *a = x.u.a;
            for (size_t i = 0; i < a->len; i++) {
                lq_value c = a->items[i];
                if (lq_is_heap(c) && c.u.o->rc < LQ_RC_STATIC && --c.u.o->rc == 0) {
                    if (sp == scap) {
                        scap *= 2;
                        if (stack == stack_small) {
                            stack = lq_xmalloc(scap * sizeof(lq_value));
                            memcpy(stack, stack_small, sizeof stack_small);
                        } else stack = lq_xrealloc(stack, scap * sizeof(lq_value));
                    }
                    stack[sp++] = c;
                }
            }
            free(a->items);
            free(a);
            break;
        }
        case LQ_HASH: {
            lq_hash *h = x.u.h;
            for (size_t i = 0; i < h->len; i++) {
                lq_release(lq_str_val(h->ents[i].key));
                lq_value c = h->ents[i].val;
                if (lq_is_heap(c) && c.u.o->rc < LQ_RC_STATIC && --c.u.o->rc == 0) {
                    if (sp == scap) {
                        scap *= 2;
                        if (stack == stack_small) {
                            stack = lq_xmalloc(scap * sizeof(lq_value));
                            memcpy(stack, stack_small, sizeof stack_small);
                        } else stack = lq_xrealloc(stack, scap * sizeof(lq_value));
                    }
                    stack[sp++] = c;
                }
            }
            free(h->ents);
            free(h->index);
            free(h);
            break;
        }
        default:
            break;
        }
    }
    if (stack != stack_small) free(stack);
}

/* ------------------------------------------------------------------ */
/* Semantics                                                           */
/* ------------------------------------------------------------------ */

const char *lq_class_name(lq_value v) {
    switch (v.type) {
    case LQ_NIL: return "NilClass";
    case LQ_BOOL: return v.u.b ? "TrueClass" : "FalseClass";
    case LQ_INT: case LQ_BIGINT: return "Integer";
    case LQ_FLOAT: return "Float";
    case LQ_STR: return "String";
    case LQ_ARR: return "Array";
    case LQ_HASH: return "Hash";
    case LQ_RANGE: return "Range";
    case LQ_DROP: return v.u.d->vt->class_name;
    case LQ_EMPTY: case LQ_BLANK: return "Liquid::Condition::MethodLiteral";
    }
    return "Object";
}

static bool is_noncharacter(int32_t cp) {
    return (cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE;
}

void lq_inspect_str(lq_buf *b, const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    lq_buf_reserve(b, n + 2);
    lq_buf_putc(b, '"');
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        if (c < 0x80) {
            switch (c) {
            case '"': lq_buf_put(b, "\\\"", 2); break;
            case '\\': lq_buf_put(b, "\\\\", 2); break;
            case '\n': lq_buf_put(b, "\\n", 2); break;
            case '\t': lq_buf_put(b, "\\t", 2); break;
            case '\r': lq_buf_put(b, "\\r", 2); break;
            case '\f': lq_buf_put(b, "\\f", 2); break;
            case '\v': lq_buf_put(b, "\\v", 2); break;
            case '\b': lq_buf_put(b, "\\b", 2); break;
            case '\a': lq_buf_put(b, "\\a", 2); break;
            case 0x1b: lq_buf_put(b, "\\e", 2); break;
            case '#':
                if (i + 1 < n && (p[i + 1] == '{' || p[i + 1] == '$' || p[i + 1] == '@'))
                    lq_buf_put(b, "\\#", 2);
                else lq_buf_putc(b, '#');
                break;
            default:
                if (c < 0x20 || c == 0x7f) lq_buf_printf(b, "\\u%04X", c);
                else lq_buf_putc(b, (char)c);
            }
            i++;
            continue;
        }
        int32_t cp;
        size_t k = lq_utf8_decode(p + i, n - i, &cp);
        if (cp < 0) {
            lq_buf_printf(b, "\\x%02X", c);
            i++;
            continue;
        }
        if ((cp >= 0x80 && cp < 0xA0) || cp == 0x2028 || cp == 0x2029 || is_noncharacter(cp)) {
            if (cp > 0xFFFF) lq_buf_printf(b, "\\u{%X}", (unsigned)cp);
            else lq_buf_printf(b, "\\u%04X", (unsigned)cp);
        } else {
            lq_buf_put(b, s + i, k);
        }
        i += k;
    }
    lq_buf_putc(b, '"');
}

/* Inspect with cycle protection is unnecessary: values are acyclic. Depth is
   bounded by an explicit limit to avoid stack exhaustion on deep data. */
#define INSPECT_MAX_DEPTH 512

static void inspect_rec(lq_buf *b, lq_value v, lq_ctx *ctx, int depth);

static void drop_to_s(lq_buf *b, lq_value v, lq_ctx *ctx) {
    lq_drop *d = v.u.d;
    if (d->vt->to_s) {
        lq_value s = d->vt->to_s(d, ctx);
        if (s.type == LQ_STR) lq_buf_put(b, s.u.s->data, s.u.s->len);
        else lq_to_s(b, s, ctx);
        lq_release(s);
    } else {
        lq_buf_puts(b, d->vt->class_name);
    }
}

static void inspect_rec(lq_buf *b, lq_value v, lq_ctx *ctx, int depth) {
    if (depth > INSPECT_MAX_DEPTH) {
        lq_buf_put(b, "...", 3);
        return;
    }
    switch (v.type) {
    case LQ_NIL: lq_buf_put(b, "nil", 3); break;
    case LQ_STR: lq_inspect_str(b, v.u.s->data, v.u.s->len); break;
    case LQ_ARR: {
        lq_buf_putc(b, '[');
        for (size_t i = 0; i < v.u.a->len; i++) {
            if (i) lq_buf_put(b, ", ", 2);
            inspect_rec(b, v.u.a->items[i], ctx, depth + 1);
        }
        lq_buf_putc(b, ']');
        break;
    }
    case LQ_HASH: {
        lq_buf_putc(b, '{');
        for (size_t i = 0; i < v.u.h->len; i++) {
            if (i) lq_buf_put(b, ", ", 2);
            lq_inspect_str(b, v.u.h->ents[i].key->data, v.u.h->ents[i].key->len);
            lq_buf_put(b, "=>", 2);
            inspect_rec(b, v.u.h->ents[i].val, ctx, depth + 1);
        }
        lq_buf_putc(b, '}');
        break;
    }
    case LQ_DROP:
        lq_buf_puts(b, v.u.d->vt->class_name);
        break;
    case LQ_EMPTY: case LQ_BLANK:
        lq_buf_puts(b, "#<Liquid::Condition::MethodLiteral>");
        break;
    default:
        lq_to_s(b, v, ctx);
    }
}

void lq_inspect(lq_buf *b, lq_value v, lq_ctx *ctx) { inspect_rec(b, v, ctx, 0); }

void lq_to_s(lq_buf *b, lq_value v, lq_ctx *ctx) {
    switch (v.type) {
    case LQ_NIL: break;
    case LQ_BOOL: if (v.u.b) lq_buf_put(b, "true", 4); else lq_buf_put(b, "false", 5); break;
    case LQ_INT: lq_fmt_int(b, v.u.i); break;
    case LQ_BIGINT: lq_bigint_fmt(b, v.u.bi); break;
    case LQ_FLOAT: lq_fmt_float(b, v.u.f); break;
    case LQ_STR: lq_buf_put(b, v.u.s->data, v.u.s->len); break;
    case LQ_ARR: case LQ_HASH: inspect_rec(b, v, ctx, 0); break;
    case LQ_RANGE:
        lq_fmt_int(b, v.u.r->first);
        lq_buf_put(b, "..", 2);
        lq_fmt_int(b, v.u.r->last);
        break;
    case LQ_DROP: drop_to_s(b, v, ctx); break;
    case LQ_EMPTY: case LQ_BLANK: break; /* MethodLiteral#to_s is '' */
    }
}

lq_value lq_to_str(lq_value v, lq_ctx *ctx) {
    if (v.type == LQ_STR) return lq_retain(v);
    if (v.type == LQ_NIL) return lq_str_empty();
    lq_buf b;
    lq_buf_init(&b);
    lq_to_s(&b, v, ctx);
    return lq_str_take_buf(&b);
}

static void output_rec(lq_buf *b, lq_value v, lq_ctx *ctx, int depth) {
    if (v.type == LQ_NIL) return;
    if (v.type == LQ_ARR) {
        if (depth > INSPECT_MAX_DEPTH) return;
        for (size_t i = 0; i < v.u.a->len; i++) output_rec(b, v.u.a->items[i], ctx, depth + 1);
        return;
    }
    lq_to_s(b, v, ctx);
}

void lq_output(lq_buf *b, lq_value v, lq_ctx *ctx) { output_rec(b, v, ctx, 0); }

static bool equal_rec(lq_value a, lq_value b, int depth) {
    if (depth > INSPECT_MAX_DEPTH) return false;
    if (lq_is_numeric(a) && lq_is_numeric(b)) return lq_num_cmp(a, b) == 0 &&
        !((a.type == LQ_FLOAT && a.u.f != a.u.f) || (b.type == LQ_FLOAT && b.u.f != b.u.f));
    if (a.type != b.type) return false;
    switch (a.type) {
    case LQ_NIL: return true;
    case LQ_BOOL: return a.u.b == b.u.b;
    case LQ_STR:
        return a.u.s == b.u.s ||
               (a.u.s->len == b.u.s->len && memcmp(a.u.s->data, b.u.s->data, a.u.s->len) == 0);
    case LQ_ARR:
        if (a.u.a == b.u.a) return true;
        if (a.u.a->len != b.u.a->len) return false;
        for (size_t i = 0; i < a.u.a->len; i++)
            if (!equal_rec(a.u.a->items[i], b.u.a->items[i], depth + 1)) return false;
        return true;
    case LQ_HASH:
        if (a.u.h == b.u.h) return true;
        if (a.u.h->len != b.u.h->len) return false;
        for (size_t i = 0; i < a.u.h->len; i++) {
            lq_value o;
            if (!lq_hash_get_str(b.u.h, a.u.h->ents[i].key, &o)) return false;
            if (!equal_rec(a.u.h->ents[i].val, o, depth + 1)) return false;
        }
        return true;
    case LQ_RANGE:
        return a.u.r->first == b.u.r->first && a.u.r->last == b.u.r->last;
    case LQ_DROP:
        if (a.u.d->vt == b.u.d->vt && strcmp(a.u.d->vt->class_name, "Liquid::SelfDrop") == 0)
            return a.u.d->data == b.u.d->data;
        return a.u.d == b.u.d;
    case LQ_EMPTY: case LQ_BLANK:
        return true;
    }
    return false;
}

bool lq_equal(lq_value a, lq_value b) { return equal_rec(a, b, 0); }

lq_value lq_liquid_value(lq_value v, lq_ctx *ctx) {
    if (v.type == LQ_DROP && v.u.d->vt->to_liquid_value) {
        bool has = false;
        lq_value r = v.u.d->vt->to_liquid_value(v.u.d, ctx, &has);
        if (has) return r;
        lq_release(r);
    }
    return lq_retain(v);
}

bool lq_is_empty(lq_value v) {
    switch (v.type) {
    case LQ_STR: return v.u.s->len == 0;
    case LQ_ARR: return v.u.a->len == 0;
    case LQ_HASH: return v.u.h->len == 0;
    default: return false;
    }
}

bool lq_is_blank(lq_value v) {
    switch (v.type) {
    case LQ_NIL: return true;
    case LQ_BOOL: return !v.u.b;
    case LQ_STR:
        for (size_t i = 0; i < v.u.s->len; i++)
            if (!lq_regex_space((unsigned char)v.u.s->data[i])) return false;
        return true;
    case LQ_ARR: return v.u.a->len == 0;
    case LQ_HASH: return v.u.h->len == 0;
    default: return false;
    }
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

void lq_error_info_clear(lq_error_info *e) {
    free(e->msg);
    free(e->markup_ctx);
    free(e->template_name);
    memset(e, 0, sizeof *e);
}

char *lq_error_format(const lq_error_info *e) {
    lq_buf b;
    lq_buf_init(&b);
    lq_buf_puts(&b, e->kind == LQ_ERR_SYNTAX ? "Liquid syntax error" : "Liquid error");
    if (e->line > 0) {
        lq_buf_puts(&b, " (");
        if (e->template_name) {
            lq_buf_puts(&b, e->template_name);
            lq_buf_putc(&b, ' ');
        }
        lq_buf_printf(&b, "line %d)", e->line);
    }
    lq_buf_puts(&b, ": ");
    if (e->msg) lq_buf_puts(&b, e->msg);
    if (e->markup_ctx) {
        lq_buf_putc(&b, ' ');
        lq_buf_puts(&b, e->markup_ctx);
    }
    return lq_buf_detach(&b, NULL);
}
