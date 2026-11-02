/*
 * filters.c - Liquid's standard filters (StandardFilters in the reference).
 *
 * Each filter receives the input value and its evaluated arguments (keyword
 * arguments arrive as a trailing hash, exactly like the reference's Ruby
 * calling convention) and returns a new reference.
 */
#include "lq.h"
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define MAX_STRING_BYTES ((size_t)256 * 1024 * 1024)
#define MAX_DEPTH 1100

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool size_ok(lq_ctx *ctx, size_t n) {
    size_t lim = MAX_STRING_BYTES;
    if (ctx->sh->max_output > 0 && (uint64_t)ctx->sh->max_output < lim) lim = (size_t)ctx->sh->max_output;
    if (n > lim) {
        lq_raise(ctx, LQ_ERR_MEMORY, "Memory limits exceeded");
        return false;
    }
    return true;
}

static lq_value S(lq_ctx *ctx, lq_value v) { return lq_to_str(v, ctx); }

static int64_t clamp_int(lq_value v) {
    if (v.type == LQ_INT) return v.u.i;
    if (v.type == LQ_BIGINT) return v.u.bi->neg ? INT64_MIN : INT64_MAX;
    return 0;
}

/* Utils.to_integer */
static bool to_integer(lq_ctx *ctx, lq_value v, int64_t *out) {
    if (v.type == LQ_INT || v.type == LQ_BIGINT) {
        *out = clamp_int(v);
        return true;
    }
    lq_value s = S(ctx, v);
    lq_value iv;
    bool ok = lq_ruby_Integer(s.u.s->data, s.u.s->len, &iv);
    lq_release(s);
    if (!ok || (iv.type != LQ_INT && iv.type != LQ_BIGINT)) {
        if (ok) lq_release(iv);
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid integer");
        return false;
    }
    *out = clamp_int(iv);
    lq_release(iv);
    return true;
}

/* byte offset of the char index `ci` in s (ci <= nchars) */
static size_t char_offset(const char *s, size_t len, size_t ci) {
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (ci && i < len) {
        i += p[i] < 0x80 ? 1 : lq_utf8_len(p + i, len - i);
        ci--;
    }
    return i;
}

static lq_value drop_to_liquid(lq_value v) {
    if (v.type == LQ_DROP && strcmp(v.u.d->vt->class_name, "StandardNilDrop") == 0) return lq_nil();
    return lq_retain(v);
}

/* InputIterator: flatten arrays, wrap hashes/scalars, expand enumerables. */
static void flatten_into(lq_arr *out, lq_value v, int depth) {
    if (v.type == LQ_ARR && depth < MAX_DEPTH) {
        for (size_t i = 0; i < v.u.a->len; i++) flatten_into(out, v.u.a->items[i], depth + 1);
    } else {
        lq_arr_push_take(out, drop_to_liquid(v));
    }
}

static lq_value input_iter(lq_ctx *ctx, lq_value input) {
    lq_value out;
    switch (input.type) {
    case LQ_ARR:
        out = lq_arr_new(input.u.a->len);
        flatten_into(out.u.a, input, 0);
        return out;
    case LQ_NIL:
        return lq_arr_new(0);
    case LQ_RANGE: {
        int64_t a = input.u.r->first, b = input.u.r->last;
        out = lq_arr_new(0);
        if (b >= a) {
            uint64_t n = (uint64_t)b - (uint64_t)a + 1;
            if (n == 0 || n > 10000000) {
                lq_raise(ctx, LQ_ERR_MEMORY, "Memory limits exceeded");
                return out;
            }
            for (int64_t i = a;; i++) {
                lq_arr_push_take(out.u.a, lq_int(i));
                if (i == b) break;
            }
        }
        return out;
    }
    case LQ_DROP:
        if (input.u.d->vt->iterate) {
            lq_value items = input.u.d->vt->iterate(input.u.d, ctx);
            if (items.type == LQ_ARR) {
                out = lq_arr_new(items.u.a->len);
                for (size_t i = 0; i < items.u.a->len; i++)
                    lq_arr_push_take(out.u.a, drop_to_liquid(items.u.a->items[i]));
                lq_release(items);
                return out;
            }
            lq_release(items);
        }
        /* fallthrough */
    default:
        out = lq_arr_new(1);
        lq_arr_push_take(out.u.a, drop_to_liquid(input));
        return out;
    }
}

/* Emulate Ruby's obj[key] for the collection filters.
   status: 0 ok, 1 TypeError, 2 NoMethodError (no [] at all) */
static lq_value ruby_index(lq_ctx *ctx, lq_value obj, lq_value key, int *status) {
    *status = 0;
    switch (obj.type) {
    case LQ_HASH: {
        lq_value v;
        if (key.type == LQ_STR && lq_hash_get_str(obj.u.h, key.u.s, &v)) return lq_retain(v);
        return lq_nil();
    }
    case LQ_ARR:
        if (key.type == LQ_INT) {
            int64_t i = key.u.i, n = (int64_t)obj.u.a->len;
            if (i < 0) i += n;
            if (i >= 0 && i < n) return lq_retain(obj.u.a->items[i]);
            return lq_nil();
        }
        if (key.type == LQ_FLOAT) {
            double d = key.u.f;
            if (d != d || isinf(d)) { *status = 1; return lq_nil(); }
            int64_t i = (int64_t)d, n = (int64_t)obj.u.a->len;
            if (i < 0) i += n;
            if (i >= 0 && i < n) return lq_retain(obj.u.a->items[i]);
            return lq_nil();
        }
        if (key.type == LQ_BIGINT) return lq_nil();
        *status = 1;
        return lq_nil();
    case LQ_STR:
        if (key.type == LQ_STR) {
            /* substring match */
            lq_str *h = obj.u.s, *n = key.u.s;
            if (n->len == 0) return lq_str_empty();
            if (lq_memfind(h->data, h->len, n->data, n->len) >= 0) return lq_retain(key);
            return lq_nil();
        }
        if (key.type == LQ_INT) {
            size_t nc = lq_str_nchars(obj.u.s);
            int64_t i = key.u.i;
            if (i < 0) i += (int64_t)nc;
            if (i < 0 || i >= (int64_t)nc) return lq_nil();
            size_t off = char_offset(obj.u.s->data, obj.u.s->len, (size_t)i);
            size_t k = lq_utf8_len((const unsigned char *)obj.u.s->data + off, obj.u.s->len - off);
            return lq_str_new(obj.u.s->data + off, k);
        }
        *status = 1;
        return lq_nil();
    case LQ_INT:
    case LQ_BIGINT:
        if (key.type == LQ_INT) {
            if (obj.type == LQ_INT) {
                int64_t b = key.u.i;
                if (b < 0) return lq_int(0);
                if (b >= 63) return lq_int(obj.u.i < 0 ? 1 : 0);
                return lq_int((obj.u.i >> b) & 1);
            }
            return lq_int(0);
        }
        *status = 1;
        return lq_nil();
    case LQ_DROP:
        if (obj.u.d->vt->get) {
            lq_value r = obj.u.d->vt->get(obj.u.d, key, ctx);
            lq_value t = drop_to_liquid(r);
            lq_release(r);
            return t;
        }
        return lq_nil();
    default:
        *status = 2;
        return lq_nil();
    }
}

static bool responds_to_index(lq_value v) {
    return v.type == LQ_HASH || v.type == LQ_ARR || v.type == LQ_STR || v.type == LQ_INT ||
           v.type == LQ_BIGINT || v.type == LQ_DROP;
}

static void raise_property_error(lq_ctx *ctx, lq_value property) {
    lq_value s = S(ctx, property);
    lq_raise(ctx, LQ_ERR_ARGUMENT, "cannot select the property '%.*s'", (int)s.u.s->len, s.u.s->data);
    lq_release(s);
}

/* ------------------------------------------------------------------ */
/* Case mapping (Unicode subset: Latin, Greek, Cyrillic, Armenian)     */
/* ------------------------------------------------------------------ */

static int32_t cp_upper(int32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0xB5) return 0x39C;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c == 0x17F) return 0x53;
    if (c == 0x131) return 0x49;
    if (c >= 0x1CD && c <= 0x1DC) return (c & 1) ? c : c - 1;
    if (c >= 0x1DE && c <= 0x1EF) return (c & 1) ? c - 1 : c;
    if (c >= 0x1F8 && c <= 0x21F) return (c & 1) ? c - 1 : c;
    if (c == 0x1C5 || c == 0x1C6) return 0x1C4;
    if (c == 0x1C8 || c == 0x1C9) return 0x1C7;
    if (c == 0x1CB || c == 0x1CC) return 0x1CA;
    if (c == 0x1F2 || c == 0x1F3) return 0x1F1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 37;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 63;
    if (c >= 0x3CA && c <= 0x3CB) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return (c & 1) ? c - 1 : c;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
    if (c == 0x4CF) return 0x4C0;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if (c >= 0x1E00 && c <= 0x1E95) return (c & 1) ? c - 1 : c;
    if (c >= 0x1EA0 && c <= 0x1EFF) return (c & 1) ? c - 1 : c;
    if (c >= 0x24D0 && c <= 0x24E9) return c - 26;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}

static int32_t cp_lower(int32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : c + 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    if (c == 0x130) return 0x69;
    if (c >= 0x1CD && c <= 0x1DC) return (c & 1) ? c + 1 : c;
    if (c >= 0x1DE && c <= 0x1EF) return (c & 1) ? c : c + 1;
    if (c >= 0x1F8 && c <= 0x21F) return (c & 1) ? c : c + 1;
    if (c == 0x1C4 || c == 0x1C5) return 0x1C6;
    if (c == 0x1C7 || c == 0x1C8) return 0x1C9;
    if (c == 0x1CA || c == 0x1CB) return 0x1CC;
    if (c == 0x1F1 || c == 0x1F2) return 0x1F3;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x3AA && c <= 0x3AB) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return (c & 1) ? c : c + 1;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c + 1 : c;
    if (c == 0x4C0) return 0x4CF;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if (c >= 0x1E00 && c <= 0x1E95) return (c & 1) ? c : c + 1;
    if (c == 0x1E9E) return 0xDF;
    if (c >= 0x1EA0 && c <= 0x1EFF) return (c & 1) ? c : c + 1;
    if (c >= 0x24B6 && c <= 0x24CF) return c + 26;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}

/* mode: 0 upcase, 1 downcase, 2 capitalize */
static lq_value case_map(lq_ctx *ctx, lq_value input, int mode) {
    lq_value s = S(ctx, input);
    lq_buf b;
    lq_buf_init(&b);
    lq_buf_reserve(&b, s.u.s->len);
    const unsigned char *p = (const unsigned char *)s.u.s->data;
    size_t n = s.u.s->len, i = 0;
    bool first = true;
    while (i < n) {
        int32_t cp;
        size_t k = lq_utf8_decode(p + i, n - i, &cp);
        if (cp < 0) {
            lq_buf_putc(&b, (char)p[i]);
            i++;
            first = false;
            continue;
        }
        bool up = mode == 0 || (mode == 2 && first);
        char enc[8];
        if (up && cp == 0xDF) {
            lq_buf_put(&b, mode == 2 ? "Ss" : "SS", 2);
        } else if (up && cp == 0xFB00) {
            lq_buf_put(&b, mode == 2 ? "Ff" : "FF", 2);
        } else if (up && cp == 0xFB01) {
            lq_buf_put(&b, mode == 2 ? "Fi" : "FI", 2);
        } else if (up && cp == 0xFB02) {
            lq_buf_put(&b, mode == 2 ? "Fl" : "FL", 2);
        } else if (mode == 2 && first && (cp == 0x1C4 || cp == 0x1C5 || cp == 0x1C6)) {
            lq_buf_put(&b, enc, lq_utf8_encode(enc, 0x1C5));
        } else if (mode == 2 && first && (cp == 0x1C7 || cp == 0x1C8 || cp == 0x1C9)) {
            lq_buf_put(&b, enc, lq_utf8_encode(enc, 0x1C8));
        } else if (mode == 2 && first && (cp == 0x1CA || cp == 0x1CB || cp == 0x1CC)) {
            lq_buf_put(&b, enc, lq_utf8_encode(enc, 0x1CB));
        } else if (mode == 2 && first && (cp == 0x1F1 || cp == 0x1F2 || cp == 0x1F3)) {
            lq_buf_put(&b, enc, lq_utf8_encode(enc, 0x1F2));
        } else {
            int32_t m = up ? cp_upper(cp) : cp_lower(cp);
            if (m == cp) lq_buf_put(&b, (const char *)p + i, k);
            else lq_buf_put(&b, enc, lq_utf8_encode(enc, m));
        }
        first = false;
        i += k;
    }
    lq_release(s);
    return lq_str_take_buf(&b);
}

static lq_value f_downcase(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return case_map(c, in, 1); }
static lq_value f_upcase(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return case_map(c, in, 0); }
static lq_value f_capitalize(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return case_map(c, in, 2); }

/* ------------------------------------------------------------------ */
/* size, escapes, encodings                                            */
/* ------------------------------------------------------------------ */

static lq_value f_size(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    switch (in.type) {
    case LQ_STR: return lq_int((int64_t)lq_str_nchars(in.u.s));
    case LQ_ARR: return lq_int((int64_t)in.u.a->len);
    case LQ_HASH: return lq_int((int64_t)in.u.h->len);
    case LQ_INT: return lq_int(8);
    case LQ_BIGINT: return lq_int((int64_t)((in.u.bi->n * 4 + 7) / 8 * 8));
    case LQ_RANGE: {
        int64_t x = in.u.r->first, y = in.u.r->last;
        return lq_int(y < x ? 0 : (y - x == INT64_MAX ? INT64_MAX : y - x + 1));
    }
    case LQ_DROP: {
        if (in.u.d->vt->iterate) {
            lq_value items = in.u.d->vt->iterate(in.u.d, ctx);
            int64_t k = items.type == LQ_ARR ? (int64_t)items.u.a->len : 0;
            lq_release(items);
            return lq_int(k);
        }
        return lq_int(0);
    }
    default: return lq_int(0);
    }
}

static void html_escape(lq_buf *b, const char *s, size_t n, bool once) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
        case '&':
            if (once) {
                /* &(?!([a-zA-Z]+|(#\d+));) */
                size_t j = i + 1;
                bool ent = false;
                if (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) {
                    while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
                    ent = j < n && s[j] == ';';
                } else if (j < n && s[j] == '#') {
                    j++;
                    size_t d0 = j;
                    while (j < n && s[j] >= '0' && s[j] <= '9') j++;
                    ent = j > d0 && j < n && s[j] == ';';
                }
                if (ent) {
                    lq_buf_putc(b, '&');
                    break;
                }
            }
            lq_buf_put(b, "&amp;", 5);
            break;
        case '<': lq_buf_put(b, "&lt;", 4); break;
        case '>': lq_buf_put(b, "&gt;", 4); break;
        case '"': lq_buf_put(b, "&quot;", 6); break;
        case '\'': lq_buf_put(b, "&#39;", 5); break;
        default: lq_buf_putc(b, c);
        }
    }
}

static lq_value f_escape(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    if (in.type == LQ_NIL) return lq_nil();
    lq_value s = S(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    html_escape(&b, s.u.s->data, s.u.s->len, false);
    lq_release(s);
    return lq_str_take_buf(&b);
}

static lq_value f_escape_once(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    lq_value s = S(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    html_escape(&b, s.u.s->data, s.u.s->len, true);
    lq_release(s);
    return lq_str_take_buf(&b);
}

static lq_value f_url_encode(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    if (in.type == LQ_NIL) return lq_nil();
    lq_value s = S(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    for (size_t i = 0; i < s.u.s->len; i++) {
        unsigned char c = (unsigned char)s.u.s->data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
            c == '-' || c == '~')
            lq_buf_putc(&b, (char)c);
        else if (c == ' ') lq_buf_putc(&b, '+');
        else lq_buf_printf(&b, "%%%02X", c);
    }
    lq_release(s);
    return lq_str_take_buf(&b);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static lq_value f_url_decode(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    if (in.type == LQ_NIL) return lq_nil();
    lq_value s = S(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '+') lq_buf_putc(&b, ' ');
        else if (p[i] == '%' && i + 2 < len + 0 && i + 2 <= len - 1 + 0 && hexval(p[i + 1]) >= 0 && hexval(p[i + 2]) >= 0) {
            lq_buf_putc(&b, (char)(hexval(p[i + 1]) * 16 + hexval(p[i + 2])));
            i += 2;
        } else lq_buf_putc(&b, p[i]);
    }
    lq_release(s);
    if (!lq_utf8_valid(b.p ? b.p : "", b.len)) {
        lq_buf_free(&b);
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid byte sequence in UTF-8");
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static lq_value b64_encode(lq_ctx *ctx, lq_value in, bool url) {
    lq_value s = S(ctx, in);
    const unsigned char *p = (const unsigned char *)s.u.s->data;
    size_t n = s.u.s->len;
    lq_buf b;
    lq_buf_init(&b);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        char out[4];
        for (int k = 0; k < 4; k++) {
            char c = B64[(v >> (18 - 6 * k)) & 63];
            if (url && c == '+') c = '-';
            if (url && c == '/') c = '_';
            out[k] = c;
        }
        if (i + 1 >= n) out[2] = '=';
        if (i + 2 >= n) out[3] = '=';
        lq_buf_put(&b, out, 4);
    }
    lq_release(s);
    return lq_str_take_buf(&b);
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* strict decode; returns false on invalid input */
static bool b64_strict_decode(const char *p, size_t n, lq_buf *out) {
    if (n % 4) return false;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = p[i + k];
            if (c == '=') {
                if (i + 4 != n || k < 2) return false;
                v[k] = 0;
                pad++;
            } else {
                if (pad) return false;
                v[k] = b64val(c);
                if (v[k] < 0) return false;
            }
        }
        uint32_t x = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        /* unused bits must be zero */
        if (pad == 2 && (x & 0xFFFF)) return false;
        if (pad == 1 && (x & 0xFF)) return false;
        lq_buf_putc(out, (char)(x >> 16));
        if (pad < 2) lq_buf_putc(out, (char)(x >> 8));
        if (pad < 1) lq_buf_putc(out, (char)x);
    }
    return true;
}

static lq_value f_base64_encode(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return b64_encode(c, in, false); }
static lq_value f_base64_url_safe_encode(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return b64_encode(c, in, true); }

static lq_value f_base64_decode(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    lq_value s = S(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    bool ok = b64_strict_decode(s.u.s->data, s.u.s->len, &b);
    lq_release(s);
    if (!ok) {
        lq_buf_free(&b);
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid base64 provided to base64_decode");
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

static lq_value f_base64_url_safe_decode(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    lq_value s = S(ctx, in);
    lq_buf t;
    lq_buf_init(&t);
    for (size_t i = 0; i < s.u.s->len; i++) {
        char c = s.u.s->data[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        lq_buf_putc(&t, c);
    }
    lq_release(s);
    /* urlsafe_decode64 adds missing padding when the string has no '=' */
    if (t.len % 4 && !(t.p && memchr(t.p, '=', t.len))) {
        size_t pad = 4 - t.len % 4;
        if (pad < 3)
            for (size_t i = 0; i < pad; i++) lq_buf_putc(&t, '=');
    }
    lq_buf b;
    lq_buf_init(&b);
    bool ok = b64_strict_decode(t.p ? t.p : "", t.len, &b);
    lq_buf_free(&t);
    if (!ok) {
        lq_buf_free(&b);
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid base64 provided to base64_url_safe_decode");
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

/* ------------------------------------------------------------------ */
/* String filters                                                      */
/* ------------------------------------------------------------------ */

static lq_value f_slice(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    int64_t off, len = 1;
    if (!to_integer(ctx, a[0], &off)) return lq_nil();
    if (n > 1 && lq_truthy(a[1]) && !to_integer(ctx, a[1], &len)) return lq_nil();
    if (in.type == LQ_ARR) {
        int64_t sz = (int64_t)in.u.a->len;
        if (off < 0) {
            if (off < -sz) return lq_arr_new(0);
            off += sz;
        }
        if (off > sz || len < 0) return lq_arr_new(0);
        int64_t end = len > sz - off ? sz : off + len;
        lq_value r = lq_arr_new((size_t)(end - off));
        for (int64_t i = off; i < end; i++) lq_arr_push(r.u.a, in.u.a->items[i]);
        return r;
    }
    lq_value s = S(ctx, in);
    int64_t sz = (int64_t)lq_str_nchars(s.u.s);
    lq_value r;
    if (off < 0) {
        if (off < -sz) {
            lq_release(s);
            return lq_str_empty();
        }
        off += sz;
    }
    if (off > sz || len < 0) r = lq_str_empty();
    else {
        int64_t end = len > sz - off ? sz : off + len;
        size_t bo = char_offset(s.u.s->data, s.u.s->len, (size_t)off);
        size_t be = bo + char_offset(s.u.s->data + bo, s.u.s->len - bo, (size_t)(end - off));
        r = lq_str_new(s.u.s->data + bo, be - bo);
    }
    lq_release(s);
    return r;
}

static lq_value f_truncate(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    if (in.type == LQ_NIL) return lq_nil();
    int64_t length = 50;
    if (n > 0 && !to_integer(ctx, a[0], &length)) return lq_nil();
    lq_value s = S(ctx, in);
    lq_value ts = n > 1 ? S(ctx, a[1]) : lq_str_cstr("...");
    int64_t tl = (int64_t)lq_str_nchars(ts.u.s);
    int64_t l;
    if (__builtin_sub_overflow(length, tl, &l)) l = 0;
    if (l < 0) l = 0;
    lq_value r;
    if ((int64_t)lq_str_nchars(s.u.s) > length) {
        size_t bo = char_offset(s.u.s->data, s.u.s->len, (size_t)l);
        lq_buf b;
        lq_buf_init(&b);
        lq_buf_put(&b, s.u.s->data, bo);
        lq_buf_put(&b, ts.u.s->data, ts.u.s->len);
        r = lq_str_take_buf(&b);
    } else r = lq_retain(s);
    lq_release(s);
    lq_release(ts);
    return r;
}

static bool awk_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == 0; }

static lq_value f_truncatewords(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    if (in.type == LQ_NIL) return lq_nil();
    int64_t words = 15;
    if (n > 0 && !to_integer(ctx, a[0], &words)) return lq_nil();
    if (words <= 0) words = 1;
    lq_value s = S(ctx, in);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len, i = 0;
    /* split(" ", words + 1): collect up to `words` words; see if more remain */
    int64_t count = 0;
    size_t starts[1] = {0};
    (void)starts;
    lq_buf b;
    lq_buf_init(&b);
    bool more = false;
    while (i < len) {
        if (count == words) {
            more = true; /* remaining text (even whitespace) forms another field */
            break;
        }
        while (i < len && awk_space((unsigned char)p[i])) i++;
        if (i >= len) break;
        size_t st = i;
        while (i < len && !awk_space((unsigned char)p[i])) i++;
        if (count) lq_buf_putc(&b, ' ');
        lq_buf_put(&b, p + st, i - st);
        count++;
    }
    lq_value r;
    if (!more) {
        lq_buf_free(&b);
        r = lq_retain(s);
    } else {
        lq_value ts = n > 1 ? S(ctx, a[1]) : lq_str_cstr("...");
        lq_buf_put(&b, ts.u.s->data, ts.u.s->len);
        lq_release(ts);
        r = lq_str_take_buf(&b);
    }
    lq_release(s);
    return r;
}

/* Ruby String#split(pattern) with limit 0 */
static lq_value ruby_split(lq_str *s, lq_str *pat) {
    lq_value out = lq_arr_new(0);
    const char *p = s->data;
    size_t len = s->len;
    if (pat->len == 1 && pat->data[0] == ' ') {
        size_t i = 0;
        while (i < len) {
            while (i < len && awk_space((unsigned char)p[i])) i++;
            if (i >= len) break;
            size_t st = i;
            while (i < len && !awk_space((unsigned char)p[i])) i++;
            lq_arr_push_take(out.u.a, lq_str_new(p + st, i - st));
        }
        return out;
    }
    if (pat->len == 0) {
        size_t i = 0;
        while (i < len) {
            size_t k = lq_utf8_len((const unsigned char *)p + i, len - i);
            lq_arr_push_take(out.u.a, lq_str_new(p + i, k));
            i += k;
        }
        return out;
    }
    size_t st = 0;
    lq_finder f;
    lq_finder_init(&f, pat->data, pat->len);
    for (;;) {
        long m = lq_finder_next(&f, p, len, st);
        if (m < 0) break;
        lq_arr_push_take(out.u.a, lq_str_new(p + st, (size_t)m - st));
        st = (size_t)m + pat->len;
    }
    lq_finder_free(&f);
    lq_arr_push_take(out.u.a, lq_str_new(p + st, len - st));
    /* drop trailing empty strings */
    lq_arr *a = out.u.a;
    while (a->len && a->items[a->len - 1].u.s->len == 0) lq_release(a->items[--a->len]);
    return out;
}

static lq_value f_split(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)n;
    lq_value pat = S(ctx, a[0]);
    lq_value s = S(ctx, in);
    lq_value r = ruby_split(s.u.s, pat.u.s);
    lq_release(pat);
    lq_release(s);
    return r;
}

typedef enum { STRIP_BOTH, STRIP_L, STRIP_R } strip_mode;

static lq_value do_strip(lq_ctx *ctx, lq_value in, strip_mode m) {
    lq_value s = S(ctx, in);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len, st = 0, en = len;
    if (m != STRIP_R)
        while (st < en && lq_ruby_space((unsigned char)p[st])) st++;
    if (m != STRIP_L)
        while (en > st && lq_ruby_space((unsigned char)p[en - 1])) en--;
    lq_value r = (st == 0 && en == len) ? lq_retain(s) : lq_str_new(p + st, en - st);
    lq_release(s);
    return r;
}

static lq_value f_strip(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return do_strip(c, in, STRIP_BOTH); }
static lq_value f_lstrip(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return do_strip(c, in, STRIP_L); }
static lq_value f_rstrip(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return do_strip(c, in, STRIP_R); }

static lq_value f_squish(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    if (in.type == LQ_NIL) return lq_nil();
    lq_value s = do_strip(ctx, in, STRIP_BOTH);
    lq_buf b;
    lq_buf_init(&b);
    const char *p = s.u.s->data;
    for (size_t i = 0; i < s.u.s->len;) {
        if (lq_regex_space((unsigned char)p[i])) {
            while (i < s.u.s->len && lq_regex_space((unsigned char)p[i])) i++;
            lq_buf_putc(&b, ' ');
        } else lq_buf_putc(&b, p[i++]);
    }
    lq_release(s);
    return lq_str_take_buf(&b);
}

static const char *memmem_c(const char *h, size_t hn, const char *nd, size_t nn) {
    long r = lq_memfind(h, hn, nd, nn);
    return r < 0 ? NULL : h + r;
}

static lq_value f_strip_html(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    lq_value s = S(ctx, in);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len;
    /* pass 1: remove script/comment/style blocks (leftmost alternative) */
    lq_buf b1;
    lq_buf_init(&b1);
    size_t i = 0;
    bool no_script = false, no_comment = false, no_style = false;
    while (i < len) {
        if (p[i] == '<') {
            const char *endp = NULL;
            size_t endlen = 0;
            if (!no_script && len - i >= 7 && memcmp(p + i, "<script", 7) == 0) {
                endp = memmem_c(p + i + 7, len - i - 7, "</script>", 9);
                endlen = 9;
                no_script = !endp;
            } else if (!no_comment && len - i >= 4 && memcmp(p + i, "<!--", 4) == 0) {
                endp = memmem_c(p + i + 4, len - i - 4, "-->", 3);
                endlen = 3;
                no_comment = !endp;
            } else if (!no_style && len - i >= 6 && memcmp(p + i, "<style", 6) == 0) {
                endp = memmem_c(p + i + 6, len - i - 6, "</style>", 8);
                endlen = 8;
                no_style = !endp;
            }
            if (endp) {
                i = (size_t)(endp - p) + endlen;
                continue;
            }
        }
        lq_buf_putc(&b1, p[i]);
        i++;
    }
    lq_release(s);
    /* pass 2: remove <.*?> */
    lq_buf b2;
    lq_buf_init(&b2);
    const char *q = b1.p ? b1.p : "";
    size_t ql = b1.len;
    i = 0;
    bool no_close = false;
    while (i < ql) {
        if (q[i] == '<' && !no_close) {
            const char *e = memchr(q + i + 1, '>', ql - i - 1);
            if (e) {
                i = (size_t)(e - q) + 1;
                continue;
            }
            no_close = true;
        }
        lq_buf_putc(&b2, q[i]);
        i++;
    }
    lq_buf_free(&b1);
    return lq_str_take_buf(&b2);
}

static lq_value nl_replace(lq_ctx *ctx, lq_value in, const char *rep) {
    lq_value s = S(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\r' && i + 1 < len && p[i + 1] == '\n') {
            lq_buf_puts(&b, rep);
            i++;
        } else if (p[i] == '\n') lq_buf_puts(&b, rep);
        else lq_buf_putc(&b, p[i]);
    }
    lq_release(s);
    if (!size_ok(ctx, b.len)) {
        lq_buf_free(&b);
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

static lq_value f_strip_newlines(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return nl_replace(c, in, ""); }
static lq_value f_newline_to_br(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return nl_replace(c, in, "<br />\n"); }

/* gsub/sub replacement string escapes */
static bool replacement_valid(const char *rep, size_t rn) {
    for (size_t i = 0; i + 1 < rn; i++) {
        if (rep[i] == '\\') {
            if (rep[i + 1] == 'k' && i + 2 < rn && rep[i + 2] == '<') return false;
            i++;
        }
    }
    return true;
}

static void put_replacement(lq_buf *b, const char *rep, size_t rn, const char *src, size_t slen, size_t ms, size_t ml) {
    for (size_t i = 0; i < rn; i++) {
        if (rep[i] == '\\' && i + 1 < rn) {
            char c = rep[i + 1];
            if (c == '+') { i++; continue; }
            if (c >= '0' && c <= '9') {
                if (c == '0') lq_buf_put(b, src + ms, ml);
                i++;
                continue;
            }
            if (c == '&') { lq_buf_put(b, src + ms, ml); i++; continue; }
            if (c == '`') { lq_buf_put(b, src, ms); i++; continue; }
            if (c == '\'') { lq_buf_put(b, src + ms + ml, slen - ms - ml); i++; continue; }
            if (c == '\\') { lq_buf_putc(b, '\\'); i++; continue; }
            lq_buf_putc(b, '\\');
            continue;
        }
        lq_buf_putc(b, rep[i]);
    }
}

static lq_value do_replace(lq_ctx *ctx, lq_value in, lq_value pat, lq_value rep, bool all) {
    lq_value ps = S(ctx, pat), rs = S(ctx, rep), s = S(ctx, in);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len, pl = ps.u.s->len;
    lq_buf b;
    lq_buf_init(&b);
    bool ok = true;
    bool any_match = pl == 0 || memmem_c(p, len, ps.u.s->data, pl) != NULL;
    if (any_match && !replacement_valid(rs.u.s->data, rs.u.s->len)) {
        lq_release(ps);
        lq_release(rs);
        lq_release(s);
        lq_raise(ctx, LQ_ERR_INTERNAL, "internal");
        return lq_nil();
    }
    if (pl == 0) {
        /* empty pattern matches at every char boundary */
        size_t i = 0;
        bool done = false;
        while (!done) {
            put_replacement(&b, rs.u.s->data, rs.u.s->len, p, len, i, 0);
            if (!all) {
                lq_buf_put(&b, p + i, len - i);
                break;
            }
            if (i >= len) break;
            size_t k = lq_utf8_len((const unsigned char *)p + i, len - i);
            lq_buf_put(&b, p + i, k);
            i += k;
            if (b.len > MAX_STRING_BYTES) { ok = false; break; }
        }
    } else {
        size_t st = 0;
        lq_finder f;
        lq_finder_init(&f, ps.u.s->data, pl);
        for (;;) {
            long m = lq_finder_next(&f, p, len, st);
            if (m < 0) break;
            lq_buf_put(&b, p + st, (size_t)m - st);
            put_replacement(&b, rs.u.s->data, rs.u.s->len, p, len, (size_t)m, pl);
            st = (size_t)m + pl;
            if (!all) break;
            if (b.len > MAX_STRING_BYTES) { ok = false; break; }
        }
        lq_finder_free(&f);
        lq_buf_put(&b, p + st, len - st);
    }
    lq_release(ps);
    lq_release(rs);
    lq_release(s);
    if (!ok || !size_ok(ctx, b.len)) {
        lq_buf_free(&b);
        if (ok) return lq_nil();
        lq_raise(ctx, LQ_ERR_MEMORY, "Memory limits exceeded");
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

static lq_value f_replace(lq_ctx *c, lq_value in, lq_value *a, size_t n) {
    return do_replace(c, in, a[0], n > 1 ? a[1] : lq_str_empty(), true);
}
static lq_value f_replace_first(lq_ctx *c, lq_value in, lq_value *a, size_t n) {
    return do_replace(c, in, a[0], n > 1 ? a[1] : lq_str_empty(), false);
}
static lq_value f_remove(lq_ctx *c, lq_value in, lq_value *a, size_t n) {
    (void)n;
    return do_replace(c, in, a[0], lq_str_empty(), true);
}
static lq_value f_remove_first(lq_ctx *c, lq_value in, lq_value *a, size_t n) {
    (void)n;
    return do_replace(c, in, a[0], lq_str_empty(), false);
}

static lq_value do_replace_last(lq_ctx *ctx, lq_value in, lq_value pat, lq_value rep) {
    lq_value s = S(ctx, in), ps = S(ctx, pat), rs = S(ctx, rep);
    const char *p = s.u.s->data;
    size_t len = s.u.s->len, pl = ps.u.s->len;
    lq_finder f;
    lq_finder_init(&f, ps.u.s->data, pl);
    long found = lq_finder_last(&f, p, len);
    lq_finder_free(&f);
    lq_value r;
    if (found < 0) r = lq_retain(s);
    else {
        lq_buf b;
        lq_buf_init(&b);
        lq_buf_put(&b, p, (size_t)found);
        lq_buf_put(&b, rs.u.s->data, rs.u.s->len);
        lq_buf_put(&b, p + found + pl, len - (size_t)found - pl);
        r = lq_str_take_buf(&b);
    }
    lq_release(s);
    lq_release(ps);
    lq_release(rs);
    return r;
}

static lq_value f_replace_last(lq_ctx *c, lq_value in, lq_value *a, size_t n) {
    (void)n;
    return do_replace_last(c, in, a[0], a[1]);
}
static lq_value f_remove_last(lq_ctx *c, lq_value in, lq_value *a, size_t n) {
    (void)n;
    return do_replace_last(c, in, a[0], lq_str_empty());
}

static lq_value concat_str(lq_ctx *ctx, lq_value x, lq_value y) {
    lq_value a = S(ctx, x), b = S(ctx, y);
    if (!size_ok(ctx, a.u.s->len + b.u.s->len)) {
        lq_release(a);
        lq_release(b);
        return lq_nil();
    }
    lq_value r;
    if (b.u.s->len == 0) r = lq_retain(a);
    else if (a.u.s->len == 0) r = lq_retain(b);
    else {
        lq_buf buf;
        lq_buf_init(&buf);
        lq_buf_reserve(&buf, a.u.s->len + b.u.s->len);
        lq_buf_put(&buf, a.u.s->data, a.u.s->len);
        lq_buf_put(&buf, b.u.s->data, b.u.s->len);
        r = lq_str_take_buf(&buf);
    }
    lq_release(a);
    lq_release(b);
    return r;
}

static lq_value f_append(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return concat_str(c, in, a[0]); }
static lq_value f_prepend(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return concat_str(c, a[0], in); }

/* ------------------------------------------------------------------ */
/* Array filters                                                       */
/* ------------------------------------------------------------------ */

static lq_value f_join(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    lq_value glue = n > 0 ? S(ctx, a[0]) : lq_str_cstr(" ");
    lq_value arr = input_iter(ctx, in);
    lq_buf b;
    lq_buf_init(&b);
    bool ok = true;
    for (size_t i = 0; i < arr.u.a->len; i++) {
        if (i) lq_buf_put(&b, glue.u.s->data, glue.u.s->len);
        lq_to_s(&b, arr.u.a->items[i], ctx);
        if (b.len > MAX_STRING_BYTES) { ok = false; break; }
    }
    lq_release(glue);
    lq_release(arr);
    if (!ok || !size_ok(ctx, b.len)) {
        lq_buf_free(&b);
        if (!ok) lq_raise(ctx, LQ_ERR_MEMORY, "Memory limits exceeded");
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

/* Ruby <=>: returns false when incomparable */
static bool spaceship(lq_value a, lq_value b, int *out, int depth) {
    if (lq_is_numeric(a) && lq_is_numeric(b)) {
        int c = lq_num_cmp(a, b);
        if (c == 2) return false;
        *out = c;
        return true;
    }
    if (a.type == LQ_STR && b.type == LQ_STR) {
        size_t n = a.u.s->len < b.u.s->len ? a.u.s->len : b.u.s->len;
        int m = memcmp(a.u.s->data, b.u.s->data, n);
        *out = m < 0 ? -1 : m > 0 ? 1 : (a.u.s->len < b.u.s->len ? -1 : a.u.s->len > b.u.s->len);
        return true;
    }
    if (a.type == LQ_ARR && b.type == LQ_ARR && depth < MAX_DEPTH) {
        size_t n = a.u.a->len < b.u.a->len ? a.u.a->len : b.u.a->len;
        for (size_t i = 0; i < n; i++) {
            int c;
            if (!spaceship(a.u.a->items[i], b.u.a->items[i], &c, depth + 1)) return false;
            if (c) {
                *out = c;
                return true;
            }
        }
        *out = a.u.a->len < b.u.a->len ? -1 : a.u.a->len > b.u.a->len;
        return true;
    }
    if (lq_equal(a, b)) {
        *out = 0;
        return true;
    }
    return false;
}

typedef struct sort_item {
    lq_value v;   /* element */
    lq_value key; /* comparison key */
} sort_item;

typedef struct sort_ctx {
    lq_ctx *ctx;
    bool natural;
    bool failed;
} sort_ctx;

static int nil_safe_compare(sort_ctx *sc, lq_value a, lq_value b) {
    int c;
    if (spaceship(a, b, &c, 0)) return c;
    if (a.type == LQ_NIL) return 1;
    if (b.type == LQ_NIL) return -1;
    if (!sc->failed) {
        sc->failed = true;
        lq_raise(sc->ctx, LQ_ERR_ARGUMENT, "cannot sort values of incompatible types");
    }
    return 0;
}

static int casecmp_values(sort_ctx *sc, lq_value a, lq_value b) {
    if (a.type != LQ_NIL && b.type != LQ_NIL) {
        lq_value x = S(sc->ctx, a), y = S(sc->ctx, b);
        size_t n = x.u.s->len < y.u.s->len ? x.u.s->len : y.u.s->len;
        int r = 0;
        for (size_t i = 0; i < n && !r; i++) {
            unsigned char p = (unsigned char)x.u.s->data[i], q = (unsigned char)y.u.s->data[i];
            if (p >= 'A' && p <= 'Z') p += 32;
            if (q >= 'A' && q <= 'Z') q += 32;
            if (p != q) r = p < q ? -1 : 1;
        }
        if (!r) r = x.u.s->len < y.u.s->len ? -1 : x.u.s->len > y.u.s->len;
        lq_release(x);
        lq_release(y);
        return r;
    }
    if (a.type == LQ_NIL && b.type == LQ_NIL) return 0;
    return a.type == LQ_NIL ? 1 : -1;
}

static int item_cmp(sort_ctx *sc, const sort_item *a, const sort_item *b) {
    if (sc->failed) return 0;
    return sc->natural ? casecmp_values(sc, a->key, b->key) : nil_safe_compare(sc, a->key, b->key);
}

static void merge_sort(sort_ctx *sc, sort_item *v, sort_item *tmp, size_t n) {
    if (n < 2) return;
    if (n <= 8) {
        for (size_t i = 1; i < n; i++) {
            sort_item x = v[i];
            size_t j = i;
            while (j > 0 && item_cmp(sc, &v[j - 1], &x) > 0) {
                v[j] = v[j - 1];
                j--;
            }
            v[j] = x;
        }
        return;
    }
    size_t m = n / 2;
    merge_sort(sc, v, tmp, m);
    merge_sort(sc, v + m, tmp, n - m);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (item_cmp(sc, &v[j], &v[i]) < 0) tmp[k++] = v[j++];
        else tmp[k++] = v[i++];
    }
    while (i < m) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(sort_item));
}

static lq_value do_sort(lq_ctx *ctx, lq_value in, lq_value *a, size_t n, bool natural) {
    lq_value arr = input_iter(ctx, in);
    if (ctx->has_err) { lq_release(arr); return lq_nil(); }
    size_t len = arr.u.a->len;
    if (len == 0) return arr;
    bool has_prop = n > 0 && a[0].type != LQ_NIL;
    if (has_prop) {
        for (size_t i = 0; i < len; i++)
            if (!responds_to_index(arr.u.a->items[i])) {
                lq_release(arr);
                return lq_nil();
            }
    }
    sort_item *items = lq_xmalloc(len * sizeof(sort_item));
    sort_item *tmp = lq_xmalloc(len * sizeof(sort_item));
    bool type_err = false;
    for (size_t i = 0; i < len; i++) {
        items[i].v = arr.u.a->items[i];
        if (has_prop) {
            int st;
            items[i].key = ruby_index(ctx, items[i].v, a[0], &st);
            if (st == 1) type_err = true;
        } else items[i].key = lq_retain(items[i].v);
    }
    lq_value result = lq_nil();
    if (type_err) {
        raise_property_error(ctx, a[0]);
    } else {
        sort_ctx sc = {ctx, natural, false};
        merge_sort(&sc, items, tmp, len);
        if (!sc.failed) {
            result = lq_arr_new(len);
            for (size_t i = 0; i < len; i++) lq_arr_push(result.u.a, items[i].v);
        }
    }
    for (size_t i = 0; i < len; i++) lq_release(items[i].key);
    free(items);
    free(tmp);
    lq_release(arr);
    return result;
}

static lq_value f_sort(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return do_sort(c, in, a, n, false); }
static lq_value f_sort_natural(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return do_sort(c, in, a, n, true); }

/* filter_array: mode 0 where, 1 reject, 2 has, 3 find, 4 find_index */
static lq_value filter_array(lq_ctx *ctx, lq_value in, lq_value *a, size_t n, int mode) {
    lq_value arr = input_iter(ctx, in);
    if (ctx->has_err) { lq_release(arr); return lq_nil(); }
    lq_value prop = a[0];
    lq_value target = n > 1 ? a[1] : lq_nil();
    size_t len = arr.u.a->len;
    if (len == 0) {
        lq_release(arr);
        if (mode == 2) return lq_bool(false);
        if (mode >= 3) return lq_nil();
        return lq_arr_new(0);
    }
    lq_value out = (mode <= 1) ? lq_arr_new(0) : lq_nil();
    lq_value result = lq_nil();
    bool done = false;
    for (size_t i = 0; i < len && !done; i++) {
        lq_value item = arr.u.a->items[i];
        int st;
        lq_value v = ruby_index(ctx, item, prop, &st);
        if (ctx->has_err) { lq_release(v); lq_release(out); lq_release(arr); return lq_nil(); }
        if (st == 1) {
            lq_release(v);
            lq_release(out);
            lq_release(arr);
            raise_property_error(ctx, prop);
            return lq_nil();
        }
        if (st == 2) {
            lq_release(v);
            lq_release(out);
            lq_release(arr);
            return lq_nil();
        }
        bool match;
        if (target.type == LQ_NIL) match = lq_truthy(v);
        else match = lq_equal(v, target);
        lq_release(v);
        switch (mode) {
        case 0: if (match) lq_arr_push(out.u.a, item); break;
        case 1: if (!match) lq_arr_push(out.u.a, item); break;
        case 2: if (match) { result = lq_bool(true); done = true; } break;
        case 3: if (match) { result = lq_retain(item); done = true; } break;
        case 4: if (match) { result = lq_int((int64_t)i); done = true; } break;
        }
    }
    lq_release(arr);
    if (mode <= 1) return out;
    if (mode == 2 && !done) return lq_bool(false);
    return result;
}

static lq_value f_where(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return filter_array(c, in, a, n, 0); }
static lq_value f_reject(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return filter_array(c, in, a, n, 1); }
static lq_value f_has(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return filter_array(c, in, a, n, 2); }
static lq_value f_find(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return filter_array(c, in, a, n, 3); }
static lq_value f_find_index(lq_ctx *c, lq_value in, lq_value *a, size_t n) { return filter_array(c, in, a, n, 4); }

/* Ruby eql? (used by uniq): like == but Integer and Float differ */
static bool eql_rec(lq_value a, lq_value b, int depth) {
    if (depth > MAX_DEPTH) return false;
    if ((a.type == LQ_FLOAT) != (b.type == LQ_FLOAT)) {
        if (lq_is_numeric(a) || lq_is_numeric(b)) return false;
    }
    if (a.type == LQ_ARR && b.type == LQ_ARR) {
        if (a.u.a->len != b.u.a->len) return false;
        for (size_t i = 0; i < a.u.a->len; i++)
            if (!eql_rec(a.u.a->items[i], b.u.a->items[i], depth + 1)) return false;
        return true;
    }
    if (a.type == LQ_HASH && b.type == LQ_HASH) {
        if (a.u.h->len != b.u.h->len) return false;
        for (size_t i = 0; i < a.u.h->len; i++) {
            lq_value o;
            if (!lq_hash_get_str(b.u.h, a.u.h->ents[i].key, &o)) return false;
            if (!eql_rec(a.u.h->ents[i].val, o, depth + 1)) return false;
        }
        return true;
    }
    return lq_equal(a, b);
}

static lq_value f_uniq(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    lq_value arr = input_iter(ctx, in);
    if (ctx->has_err) { lq_release(arr); return lq_nil(); }
    bool has_prop = n > 0 && a[0].type != LQ_NIL;
    size_t len = arr.u.a->len;
    if (has_prop && len == 0) return arr;
    lq_value out = lq_arr_new(0);
    lq_value keys = lq_arr_new(0);
    /* open addressing table of indices into keys (by eql?-consistent hash) */
    size_t tcap = 16;
    while (tcap < len * 2 + 2) tcap *= 2;
    size_t *table = lq_xcalloc(tcap, sizeof(size_t));
    uint64_t *hashes = lq_xmalloc((len ? len : 1) * sizeof(uint64_t));
    for (size_t i = 0; i < len; i++) {
        lq_value item = arr.u.a->items[i];
        lq_value key;
        if (has_prop) {
            int st;
            key = ruby_index(ctx, item, a[0], &st);
            if (st) {
                lq_release(key);
                lq_release(out);
                lq_release(keys);
                lq_release(arr);
                free(table);
                free(hashes);
                if (st == 1) raise_property_error(ctx, a[0]);
                return lq_nil();
            }
        } else key = lq_liquid_value(item, ctx);
        uint64_t hv = lq_value_hash(key, 0);
        size_t slot = (size_t)hv & (tcap - 1);
        bool seen = false;
        while (table[slot]) {
            size_t j = table[slot] - 1;
            if (hashes[j] == hv && eql_rec(keys.u.a->items[j], key, 0)) {
                seen = true;
                break;
            }
            slot = (slot + 1) & (tcap - 1);
        }
        if (!seen) {
            hashes[keys.u.a->len] = hv;
            table[slot] = keys.u.a->len + 1;
            lq_arr_push_take(keys.u.a, key);
            lq_arr_push(out.u.a, item);
        } else lq_release(key);
    }
    free(table);
    free(hashes);
    lq_release(keys);
    lq_release(arr);
    return out;
}

static lq_value f_reverse(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    lq_value arr = input_iter(ctx, in);
    lq_arr *x = arr.u.a;
    for (size_t i = 0, j = x->len; i + 1 < j; i++, j--) {
        lq_value t = x->items[i];
        x->items[i] = x->items[j - 1];
        x->items[j - 1] = t;
    }
    return arr;
}

static lq_value f_map(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)n;
    lq_value arr = input_iter(ctx, in);
    if (ctx->has_err) { lq_release(arr); return lq_nil(); }
    lq_value out = lq_arr_new(arr.u.a->len);
    bool to_liq = a[0].type == LQ_STR && a[0].u.s->len == 9 && memcmp(a[0].u.s->data, "to_liquid", 9) == 0;
    for (size_t i = 0; i < arr.u.a->len; i++) {
        lq_value e = arr.u.a->items[i];
        if (to_liq) {
            lq_arr_push(out.u.a, e);
            continue;
        }
        int st;
        lq_value v = ruby_index(ctx, e, a[0], &st);
        if (ctx->has_err) { lq_release(v); lq_release(out); lq_release(arr); return lq_nil(); }
        if (st == 1) {
            lq_release(v);
            lq_release(out);
            lq_release(arr);
            raise_property_error(ctx, a[0]);
            return lq_nil();
        }
        lq_arr_push_take(out.u.a, v);
    }
    lq_release(arr);
    return out;
}

static lq_value f_compact(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    lq_value arr = input_iter(ctx, in);
    if (ctx->has_err) { lq_release(arr); return lq_nil(); }
    bool has_prop = n > 0 && a[0].type != LQ_NIL;
    if (has_prop && arr.u.a->len == 0) return arr;
    lq_value out = lq_arr_new(arr.u.a->len);
    for (size_t i = 0; i < arr.u.a->len; i++) {
        lq_value item = arr.u.a->items[i];
        if (!has_prop) {
            if (item.type != LQ_NIL) lq_arr_push(out.u.a, item);
            continue;
        }
        int st;
        lq_value v = ruby_index(ctx, item, a[0], &st);
        if (st) {
            lq_release(v);
            lq_release(out);
            lq_release(arr);
            if (st == 1) raise_property_error(ctx, a[0]);
            return lq_nil();
        }
        if (v.type != LQ_NIL) lq_arr_push(out.u.a, item);
        lq_release(v);
    }
    lq_release(arr);
    return out;
}

static lq_value f_concat(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)n;
    if (a[0].type != LQ_ARR) {
        lq_raise(ctx, LQ_ERR_ARGUMENT, "concat filter requires an array argument");
        return lq_nil();
    }
    lq_value arr = input_iter(ctx, in);
    if (!size_ok(ctx, (arr.u.a->len + a[0].u.a->len) * 16)) {
        lq_release(arr);
        return lq_nil();
    }
    for (size_t i = 0; i < a[0].u.a->len; i++) lq_arr_push(arr.u.a, a[0].u.a->items[i]);
    return arr;
}

static lq_value f_first(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    (void)ctx;
    switch (in.type) {
    case LQ_STR: {
        if (!in.u.s->len) return lq_str_empty();
        size_t k = lq_utf8_len((const unsigned char *)in.u.s->data, in.u.s->len);
        return lq_str_new(in.u.s->data, k);
    }
    case LQ_ARR: return in.u.a->len ? lq_retain(in.u.a->items[0]) : lq_nil();
    case LQ_HASH:
        if (in.u.h->len) {
            lq_value pair = lq_arr_new(2);
            lq_arr_push(pair.u.a, lq_str_val(in.u.h->ents[0].key));
            lq_arr_push(pair.u.a, in.u.h->ents[0].val);
            return pair;
        }
        return lq_nil();
    case LQ_RANGE: return lq_int(in.u.r->first);
    case LQ_DROP: {
        if (!in.u.d->vt->iterate) return lq_nil();
        lq_value k = lq_str_cstr("first");
        lq_value r = lq_lookup_step(ctx, in, k, true);
        lq_release(k);
        return r;
    }
    default: return lq_nil();
    }
}

static lq_value f_last(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    switch (in.type) {
    case LQ_STR: {
        lq_str *s = in.u.s;
        if (!s->len) return lq_str_empty();
        const unsigned char *p = (const unsigned char *)s->data;
        size_t i = s->len - 1;
        while (i > 0 && (p[i] & 0xC0) == 0x80 && s->len - i < 4) i--;
        size_t k = lq_utf8_len(p + i, s->len - i);
        if (i + k != s->len) { i = s->len - 1; k = 1; }
        return lq_str_new(s->data + i, k);
    }
    case LQ_ARR: return in.u.a->len ? lq_retain(in.u.a->items[in.u.a->len - 1]) : lq_nil();
    case LQ_RANGE: return lq_int(in.u.r->last);
    case LQ_DROP: {
        if (!in.u.d->vt->iterate) return lq_nil();
        lq_value k = lq_str_cstr("last");
        lq_value r = lq_lookup_step(ctx, in, k, true);
        lq_release(k);
        return r;
    }
    default: return lq_nil();
    }
}

/* ------------------------------------------------------------------ */
/* Math                                                                */
/* ------------------------------------------------------------------ */

static bool decimal_string(const char *s, size_t n) {
    /* /\A-?\d+\.\d+\z/ on the stripped string */
    while (n && lq_ruby_space((unsigned char)*s)) { s++; n--; }
    while (n && lq_ruby_space((unsigned char)s[n - 1])) n--;
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    size_t d0 = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    if (i == d0 || i >= n || s[i] != '.') return false;
    i++;
    size_t d1 = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    return i > d1 && i == n;
}

void lq_to_number(lq_ctx *ctx, lq_value v, lq_num *out) {
    memset(out, 0, sizeof *out);
    out->i = lq_int(0);
    switch (v.type) {
    case LQ_INT:
    case LQ_BIGINT:
        out->i = lq_retain(v);
        return;
    case LQ_FLOAT:
        out->is_dec = true;
        dec_from_double(&out->d, v.u.f);
        return;
    case LQ_STR: {
        const char *s = v.u.s->data;
        size_t n = v.u.s->len;
        if (decimal_string(s, n)) {
            while (n && lq_ruby_space((unsigned char)*s)) { s++; n--; }
            out->is_dec = true;
            dec_from_text(&out->d, s, n);
        } else {
            out->i = lq_str_to_i(s, n);
            if (out->i.type == LQ_FLOAT) {
                double d = out->i.u.f;
                out->i = lq_int(0);
                out->is_dec = true;
                dec_from_double(&out->d, d);
            }
        }
        return;
    }
    case LQ_DROP:
        if (v.u.d->vt->to_number) {
            bool has = false;
            lq_value r = v.u.d->vt->to_number(v.u.d, ctx, &has);
            if (has) {
                lq_to_number(ctx, r, out);
                lq_release(r);
                return;
            }
            lq_release(r);
        }
        return;
    default:
        return;
    }
}

void lq_num_free(lq_num *n) {
    lq_release(n->i);
    if (n->is_dec) dec_free(&n->d);
}

static void num_to_dec(const lq_num *n, dec *d) {
    if (n->is_dec) {
        dec_init(d);
        d->special = n->d.special;
        d->neg = n->d.neg;
        d->e = n->d.e;
        mag_copy(&d->m, &n->d.m);
    } else dec_from_int(d, n->i);
}

static lq_value dec_result(const dec *d) { return lq_float(dec_to_double(d)); }

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD };

static lq_value arith(lq_ctx *ctx, lq_value x, lq_value y, int op) {
    lq_num a, b;
    lq_to_number(ctx, x, &a);
    lq_to_number(ctx, y, &b);
    lq_value r = lq_nil();
    if (!a.is_dec && !b.is_dec) {
        switch (op) {
        case OP_ADD: r = lq_int_add(a.i, b.i, false); break;
        case OP_SUB: r = lq_int_add(a.i, b.i, true); break;
        case OP_MUL: r = lq_int_mul(a.i, b.i); break;
        case OP_DIV:
        case OP_MOD:
            if (b.i.type == LQ_INT && b.i.u.i == 0) {
                lq_raise(ctx, LQ_ERR_ZERODIV, "divided by 0");
                break;
            }
            if (op == OP_DIV) lq_int_divmod(a.i, b.i, &r, NULL);
            else lq_int_divmod(a.i, b.i, NULL, &r);
            break;
        }
    } else {
        dec da, db, dr;
        num_to_dec(&a, &da);
        num_to_dec(&b, &db);
        bool ok = true;
        switch (op) {
        case OP_ADD: dec_add(&dr, &da, &db, false); break;
        case OP_SUB: dec_add(&dr, &da, &db, true); break;
        case OP_MUL: dec_mul(&dr, &da, &db); break;
        case OP_DIV: dec_div(&dr, &da, &db); break;
        case OP_MOD:
            if (!dec_mod(&dr, &da, &db)) {
                ok = false;
                dec_init(&dr);
            }
            break;
        }
        if (ok) r = dec_result(&dr);
        else lq_raise(ctx, LQ_ERR_ZERODIV, "divided by 0");
        dec_free(&da);
        dec_free(&db);
        dec_free(&dr);
    }
    lq_num_free(&a);
    lq_num_free(&b);
    return r;
}

static lq_value f_plus(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return arith(c, in, a[0], OP_ADD); }
static lq_value f_minus(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return arith(c, in, a[0], OP_SUB); }
static lq_value f_times(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return arith(c, in, a[0], OP_MUL); }
static lq_value f_divided_by(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return arith(c, in, a[0], OP_DIV); }
static lq_value f_modulo(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return arith(c, in, a[0], OP_MOD); }

static lq_value f_abs(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)a;
    (void)n;
    lq_num x;
    lq_to_number(ctx, in, &x);
    lq_value r;
    if (x.is_dec) {
        x.d.neg = 0;
        r = dec_result(&x.d);
    } else if (x.i.type == LQ_INT) {
        if (x.i.u.i == INT64_MIN) r = lq_int_mul(x.i, lq_int(-1));
        else r = lq_int(x.i.u.i < 0 ? -x.i.u.i : x.i.u.i);
    } else {
        r = x.i.u.bi->neg ? lq_int_mul(x.i, lq_int(-1)) : lq_retain(x.i);
    }
    lq_num_free(&x);
    return r;
}

static void float_domain(lq_ctx *ctx, const dec *d) {
    if (d->special == DEC_NAN) lq_raise(ctx, LQ_ERR_FLOATDOMAIN, "Computation results in 'NaN' (Not a Number)");
    else lq_raise(ctx, LQ_ERR_FLOATDOMAIN, "Computation results in '%sInfinity'", d->neg ? "-" : "");
}

static lq_value f_round(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    lq_value nv = n > 0 ? a[0] : lq_int(0);
    lq_num x, d;
    lq_to_number(ctx, in, &x);
    lq_to_number(ctx, nv, &d);
    int64_t digits = 0;
    if (d.is_dec) {
        lq_value di = dec_to_int(&d.d);
        digits = clamp_int(di);
        lq_release(di);
    } else digits = clamp_int(d.i);
    if (digits > INT32_MAX || digits < INT32_MIN) {
        /* Ruby raises RangeError (rendered as an internal error) */
        lq_num_free(&x);
        lq_num_free(&d);
        lq_raise(ctx, LQ_ERR_INTERNAL, "internal");
        return lq_nil();
    }
    if (digits > 100000) digits = 100000;
    if (digits < -100000) digits = -100000;
    lq_value r = lq_nil();
    bool n_is_zero = (nv.type == LQ_INT && nv.u.i == 0) || (nv.type == LQ_FLOAT && nv.u.f == 0);
    if (x.is_dec) {
        if (x.d.special != DEC_FINITE) {
            if (digits > 0) r = dec_result(&x.d);
            else float_domain(ctx, &x.d);
        } else {
            dec rr;
            dec_round(&rr, &x.d, digits, 0);
            if (digits > 0) r = dec_result(&rr);
            else r = dec_to_int(&rr);
            dec_free(&rr);
        }
    } else {
        if (digits >= 0) r = lq_retain(x.i);
        else {
            dec xd, rr;
            dec_from_int(&xd, x.i);
            dec_round(&rr, &xd, digits, 0);
            r = dec_to_int(&rr);
            dec_free(&xd);
            dec_free(&rr);
        }
    }
    if (!ctx->has_err && n_is_zero && r.type == LQ_FLOAT) {
        double f = r.u.f;
        if (f != f || isinf(f)) {
            lq_raise(ctx, LQ_ERR_FLOATDOMAIN, "%s", f != f ? "NaN" : f > 0 ? "Infinity" : "-Infinity");
            r = lq_nil();
        } else {
            dec t;
            dec_from_double(&t, f);
            r = dec_to_int(&t);
            dec_free(&t);
        }
    }
    lq_num_free(&x);
    lq_num_free(&d);
    return r;
}

static lq_value ceil_floor(lq_ctx *ctx, lq_value in, int mode) {
    lq_num x;
    lq_to_number(ctx, in, &x);
    lq_value r;
    if (!x.is_dec) r = lq_retain(x.i);
    else if (x.d.special != DEC_FINITE) {
        float_domain(ctx, &x.d);
        r = lq_nil();
    } else {
        dec rr;
        dec_round(&rr, &x.d, 0, mode);
        r = dec_to_int(&rr);
        dec_free(&rr);
    }
    lq_num_free(&x);
    return r;
}

static lq_value f_ceil(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return ceil_floor(c, in, 2); }
static lq_value f_floor(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)a; (void)n; return ceil_floor(c, in, 1); }

static int num_cmp(const lq_num *a, const lq_num *b) {
    if (!a->is_dec && !b->is_dec) return lq_num_cmp(a->i, b->i);
    dec x, y;
    num_to_dec(a, &x);
    num_to_dec(b, &y);
    int r = dec_cmp(&x, &y);
    dec_free(&x);
    dec_free(&y);
    return r;
}

static lq_value num_value(const lq_num *n) {
    if (n->is_dec) return dec_result(&n->d);
    return lq_retain(n->i);
}

static lq_value at_bound(lq_ctx *ctx, lq_value in, lq_value bound, bool least) {
    lq_num b, x;
    lq_to_number(ctx, bound, &b);
    lq_to_number(ctx, in, &x);
    int c = num_cmp(&b, &x);
    bool use_bound = least ? (c == 1) : (c == -1);
    lq_value r = use_bound ? num_value(&b) : num_value(&x);
    lq_num_free(&b);
    lq_num_free(&x);
    return r;
}

static lq_value f_at_least(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return at_bound(c, in, a[0], true); }
static lq_value f_at_most(lq_ctx *c, lq_value in, lq_value *a, size_t n) { (void)n; return at_bound(c, in, a[0], false); }

static lq_value f_sum(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    lq_value arr = input_iter(ctx, in);
    if (ctx->has_err) { lq_release(arr); return lq_nil(); }
    if (arr.u.a->len == 0) {
        lq_release(arr);
        return lq_int(0);
    }
    bool has_prop = n > 0 && a[0].type != LQ_NIL;
    lq_value vals = lq_arr_new(arr.u.a->len);
    for (size_t i = 0; i < arr.u.a->len; i++) {
        lq_value item = arr.u.a->items[i];
        if (!has_prop) {
            lq_arr_push(vals.u.a, item);
        } else if (responds_to_index(item)) {
            int st;
            lq_value v = ruby_index(ctx, item, a[0], &st);
            if (st == 1) {
                lq_release(v);
                lq_release(vals);
                lq_release(arr);
                raise_property_error(ctx, a[0]);
                return lq_nil();
            }
            lq_arr_push_take(vals.u.a, v);
        } else {
            lq_arr_push_take(vals.u.a, lq_int(0));
        }
    }
    lq_release(arr);
    lq_value flat = input_iter(ctx, vals);
    lq_release(vals);
    lq_value acc_i = lq_int(0);
    bool is_dec = false;
    dec acc_d;
    dec_init(&acc_d);
    for (size_t i = 0; i < flat.u.a->len; i++) {
        lq_num x;
        lq_to_number(ctx, flat.u.a->items[i], &x);
        if (!is_dec && !x.is_dec) {
            lq_value t = lq_int_add(acc_i, x.i, false);
            lq_release(acc_i);
            acc_i = t;
        } else {
            if (!is_dec) {
                dec_free(&acc_d);
                dec_from_int(&acc_d, acc_i);
                is_dec = true;
            }
            dec xd, t;
            num_to_dec(&x, &xd);
            dec_add(&t, &acc_d, &xd, false);
            dec_free(&acc_d);
            dec_free(&xd);
            acc_d = t;
        }
        lq_num_free(&x);
    }
    lq_release(flat);
    lq_value r;
    if (is_dec) {
        r = dec_result(&acc_d);
        lq_release(acc_i);
    } else r = acc_i;
    dec_free(&acc_d);
    return r;
}

static lq_value f_default(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    lq_value def = n > 0 ? a[0] : lq_str_empty();
    bool allow_false = false;
    if (n > 1 && a[1].type == LQ_HASH) {
        lq_value v;
        if (lq_hash_get_cstr(a[1].u.h, "allow_false", &v)) allow_false = lq_truthy(v);
    }
    bool false_check;
    if (allow_false) false_check = in.type == LQ_NIL;
    else {
        lq_value lv = lq_liquid_value(in, ctx);
        false_check = !lq_truthy(lv);
        lq_release(lv);
    }
    bool empty = (in.type == LQ_STR || in.type == LQ_ARR || in.type == LQ_HASH) && lq_is_empty(in);
    return lq_retain(false_check || empty ? def : in);
}

/* ------------------------------------------------------------------ */
/* Date                                                                */
/* ------------------------------------------------------------------ */

static lq_value f_date(lq_ctx *ctx, lq_value in, lq_value *a, size_t n) {
    (void)n;
    lq_value fmt = S(ctx, a[0]);
    if (fmt.u.s->len == 0) {
        lq_release(fmt);
        return lq_retain(in);
    }
    lq_time t;
    if (!lq_to_date(ctx, in, &t)) {
        lq_release(fmt);
        return lq_retain(in);
    }
    lq_buf b;
    lq_buf_init(&b);
    lq_strftime(&b, fmt.u.s->data, fmt.u.s->len, &t);
    lq_release(fmt);
    if (!size_ok(ctx, b.len)) {
        lq_buf_free(&b);
        return lq_nil();
    }
    return lq_str_take_buf(&b);
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static const lq_filter_def FILTERS[] = {
    {"abs", f_abs, 0, 0},
    {"append", f_append, 1, 1},
    {"at_least", f_at_least, 1, 1},
    {"at_most", f_at_most, 1, 1},
    {"base64_decode", f_base64_decode, 0, 0},
    {"base64_encode", f_base64_encode, 0, 0},
    {"base64_url_safe_decode", f_base64_url_safe_decode, 0, 0},
    {"base64_url_safe_encode", f_base64_url_safe_encode, 0, 0},
    {"capitalize", f_capitalize, 0, 0},
    {"ceil", f_ceil, 0, 0},
    {"compact", f_compact, 0, 1},
    {"concat", f_concat, 1, 1},
    {"date", f_date, 1, 1},
    {"default", f_default, 0, 2},
    {"divided_by", f_divided_by, 1, 1},
    {"downcase", f_downcase, 0, 0},
    {"escape", f_escape, 0, 0},
    {"escape_once", f_escape_once, 0, 0},
    {"find", f_find, 1, 2},
    {"find_index", f_find_index, 1, 2},
    {"first", f_first, 0, 0},
    {"floor", f_floor, 0, 0},
    {"h", f_escape, 0, 0},
    {"has", f_has, 1, 2},
    {"join", f_join, 0, 1},
    {"last", f_last, 0, 0},
    {"lstrip", f_lstrip, 0, 0},
    {"map", f_map, 1, 1},
    {"minus", f_minus, 1, 1},
    {"modulo", f_modulo, 1, 1},
    {"newline_to_br", f_newline_to_br, 0, 0},
    {"plus", f_plus, 1, 1},
    {"prepend", f_prepend, 1, 1},
    {"reject", f_reject, 1, 2},
    {"remove", f_remove, 1, 1},
    {"remove_first", f_remove_first, 1, 1},
    {"remove_last", f_remove_last, 1, 1},
    {"replace", f_replace, 1, 2},
    {"replace_first", f_replace_first, 1, 2},
    {"replace_last", f_replace_last, 2, 2},
    {"reverse", f_reverse, 0, 0},
    {"round", f_round, 0, 1},
    {"rstrip", f_rstrip, 0, 0},
    {"size", f_size, 0, 0},
    {"slice", f_slice, 1, 2},
    {"sort", f_sort, 0, 1},
    {"sort_natural", f_sort_natural, 0, 1},
    {"split", f_split, 1, 1},
    {"squish", f_squish, 0, 0},
    {"strip", f_strip, 0, 0},
    {"strip_html", f_strip_html, 0, 0},
    {"strip_newlines", f_strip_newlines, 0, 0},
    {"sum", f_sum, 0, 1},
    {"times", f_times, 1, 1},
    {"truncate", f_truncate, 0, 2},
    {"truncatewords", f_truncatewords, 0, 2},
    {"uniq", f_uniq, 0, 1},
    {"upcase", f_upcase, 0, 0},
    {"url_decode", f_url_decode, 0, 0},
    {"url_encode", f_url_encode, 0, 0},
    {"where", f_where, 1, 2},
};

const lq_filter_def *lq_filter_lookup(const char *name, size_t len) {
    size_t lo = 0, hi = sizeof FILTERS / sizeof FILTERS[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *fn = FILTERS[mid].name;
        size_t fl = strlen(fn);
        size_t m = fl < len ? fl : len;
        int c = memcmp(fn, name, m);
        if (c == 0) c = fl < len ? -1 : fl > len ? 1 : 0;
        if (c == 0) return &FILTERS[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
