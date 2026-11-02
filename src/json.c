/*
 * json.c - a small, strict JSON parser producing Liquid values, plus the
 * string escaper used by the JSON-RPC server.
 *
 * Special objects:
 *   {"_instantiate": "Name", "params": {...}}  built-in test drop
 *   {<host marker>: "id", "type": "Class"}     host drop (callbacks)
 *   {"_ruby_type": "Range"|"Symbol"|"Hash"...}  best-effort conversion
 */
#include "lq.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>

#define JSON_MAX_DEPTH 1000

typedef struct jp {
    const char *s;
    size_t n, i;
    int depth;
    const lq_json_opts *o;
    char err[128];
} jp;

static void ws(jp *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static bool fail(jp *p, const char *msg) {
    if (!p->err[0]) snprintf(p->err, sizeof p->err, "%s at offset %zu", msg, p->i);
    return false;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}

static bool parse_string(jp *p, lq_buf *b) {
    p->i++; /* opening quote */
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->i++;
            return true;
        }
        if (c < 0x20) return fail(p, "control character in string");
        if (c == '\\') {
            if (p->i + 1 >= p->n) return fail(p, "bad escape");
            char e = p->s[p->i + 1];
            p->i += 2;
            switch (e) {
            case '"': lq_buf_putc(b, '"'); break;
            case '\\': lq_buf_putc(b, '\\'); break;
            case '/': lq_buf_putc(b, '/'); break;
            case 'b': lq_buf_putc(b, '\b'); break;
            case 'f': lq_buf_putc(b, '\f'); break;
            case 'n': lq_buf_putc(b, '\n'); break;
            case 'r': lq_buf_putc(b, '\r'); break;
            case 't': lq_buf_putc(b, '\t'); break;
            case 'u': {
                if (p->i + 4 > p->n) return fail(p, "bad unicode escape");
                int cp = hex4(p->s + p->i);
                if (cp < 0) return fail(p, "bad unicode escape");
                p->i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                        int lo = hex4(p->s + p->i + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p->i += 6;
                        } else cp = 0xFFFD;
                    } else cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
                char enc[4];
                lq_buf_put(b, enc, lq_utf8_encode(enc, cp));
                break;
            }
            default:
                return fail(p, "bad escape");
            }
            continue;
        }
        /* copy a run of plain bytes */
        size_t st = p->i;
        while (p->i < p->n) {
            unsigned char d = (unsigned char)p->s[p->i];
            if (d == '"' || d == '\\' || d < 0x20) break;
            p->i++;
        }
        lq_buf_put(b, p->s + st, p->i - st);
    }
    return fail(p, "unterminated string");
}

static bool parse_value(jp *p, lq_value *out);

/* host drop */
typedef struct hostdrop {
    char *id;
    char *type;
    const liquid_drop_host *host;
} hostdrop;

static lq_value host_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    hostdrop *h = d->data;
    if (!h->host || !h->host->get) return lq_nil();
    lq_value ks = lq_to_str(key, ctx);
    size_t len = 0;
    char *json = h->host->get(h->host->userdata, h->id, ks.u.s->data, ks.u.s->len, &len);
    lq_release(ks);
    if (!json) return lq_nil();
    lq_json_opts o = {h->host};
    lq_value v;
    char *err = NULL;
    if (!lq_json_parse(json, len, &o, &v, &err)) {
        free(err);
        v = lq_nil();
    }
    free(json);
    return v;
}

static lq_value host_iterate(lq_drop *d, lq_ctx *ctx) {
    (void)ctx;
    hostdrop *h = d->data;
    if (!h->host || !h->host->iterate) return lq_nil();
    size_t len = 0;
    char *json = h->host->iterate(h->host->userdata, h->id, &len);
    if (!json) return lq_nil();
    lq_json_opts o = {h->host};
    lq_value v;
    char *err = NULL;
    if (!lq_json_parse(json, len, &o, &v, &err)) {
        free(err);
        v = lq_nil();
    }
    free(json);
    return v;
}

static void host_free(lq_drop *d) {
    hostdrop *h = d->data;
    free(h->id);
    free(h->type);
    free(h);
}

static lq_value host_to_s(lq_drop *d, lq_ctx *ctx) {
    (void)ctx;
    hostdrop *h = d->data;
    return lq_str_cstr(h->type ? h->type : "Liquid::Drop");
}

static const lq_drop_vt host_vt = {"Liquid::Drop", host_get, NULL, host_to_s, host_iterate, NULL, host_free};

static lq_value convert_special(jp *p, lq_value obj) {
    lq_hash *h = obj.u.h;
    lq_value v;
    if (lq_hash_get_cstr(h, "_instantiate", &v) && v.type == LQ_STR) {
        lq_value params = lq_nil();
        lq_hash_get_cstr(h, "params", &params);
        bool ok;
        lq_value d = lq_make_builtin_drop(v.u.s->data, params, &ok);
        if (ok) {
            lq_release(obj);
            return d;
        }
        return obj;
    }
    if (p->o && p->o->host && p->o->host->marker_key && lq_hash_get_cstr(h, p->o->host->marker_key, &v) &&
        v.type == LQ_STR) {
        hostdrop *hd = lq_xcalloc(1, sizeof(hostdrop));
        hd->id = lq_xstrndup(v.u.s->data, v.u.s->len);
        lq_value t;
        if (lq_hash_get_cstr(h, "type", &t) && t.type == LQ_STR) hd->type = lq_xstrndup(t.u.s->data, t.u.s->len);
        hd->host = p->o->host;
        lq_release(obj);
        return lq_drop_new(&host_vt, hd);
    }
    if (lq_hash_get_cstr(h, "_ruby_type", &v) && v.type == LQ_STR) {
        if (strcmp(v.u.s->data, "Range") == 0) {
            lq_value b, e;
            if (lq_hash_get_cstr(h, "begin", &b) && lq_hash_get_cstr(h, "end", &e) && b.type == LQ_INT && e.type == LQ_INT) {
                lq_value ex;
                int64_t last = e.u.i;
                if (lq_hash_get_cstr(h, "exclude_end", &ex) && lq_truthy(ex)) last--;
                lq_value r = lq_range_new(b.u.i, last);
                lq_release(obj);
                return r;
            }
        } else if (strcmp(v.u.s->data, "Symbol") == 0) {
            lq_value s;
            if (lq_hash_get_cstr(h, "value", &s)) {
                lq_retain(s);
                lq_release(obj);
                return s;
            }
        } else if (strcmp(v.u.s->data, "Hash") == 0) {
            lq_value d;
            if (lq_hash_get_cstr(h, "data", &d)) {
                lq_retain(d);
                lq_release(obj);
                return d;
            }
        }
    }
    return obj;
}

static bool parse_number(jp *p, lq_value *out) {
    size_t st = p->i;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    if (p->i >= p->n) return fail(p, "bad number");
    if (p->s[p->i] == '0') p->i++;
    else if (p->s[p->i] >= '1' && p->s[p->i] <= '9') {
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    } else return fail(p, "bad number");
    bool is_float = false;
    if (p->i < p->n && p->s[p->i] == '.') {
        is_float = true;
        p->i++;
        size_t d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == d) return fail(p, "bad number");
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        is_float = true;
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        size_t d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == d) return fail(p, "bad number");
    }
    if (is_float) {
        char tmp[64];
        size_t len = p->i - st;
        double d;
        if (len < sizeof tmp) {
            memcpy(tmp, p->s + st, len);
            tmp[len] = 0;
            d = strtod(tmp, NULL);
        } else {
            char *t = lq_xstrndup(p->s + st, len);
            d = strtod(t, NULL);
            free(t);
        }
        *out = lq_float(d);
    } else {
        *out = lq_parse_int_text(p->s + st, p->i - st);
    }
    return true;
}

static bool parse_value(jp *p, lq_value *out) {
    ws(p);
    if (p->i >= p->n) return fail(p, "unexpected end of input");
    char c = p->s[p->i];
    if (c == '{') {
        if (++p->depth > JSON_MAX_DEPTH) return fail(p, "nesting too deep");
        p->i++;
        lq_value obj = lq_hash_new(0);
        ws(p);
        if (p->i < p->n && p->s[p->i] == '}') {
            p->i++;
            p->depth--;
            *out = convert_special(p, obj);
            return true;
        }
        for (;;) {
            ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') {
                lq_release(obj);
                return fail(p, "expected string key");
            }
            lq_buf kb;
            lq_buf_init(&kb);
            if (!parse_string(p, &kb)) {
                lq_buf_free(&kb);
                lq_release(obj);
                return false;
            }
            lq_value key = lq_str_take_buf(&kb);
            ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') {
                lq_release(key);
                lq_release(obj);
                return fail(p, "expected ':'");
            }
            p->i++;
            lq_value v;
            if (!parse_value(p, &v)) {
                lq_release(key);
                lq_release(obj);
                return false;
            }
            lq_hash_set_take(obj.u.h, key.u.s, v);
            lq_release(key);
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') {
                p->i++;
                continue;
            }
            if (p->i < p->n && p->s[p->i] == '}') {
                p->i++;
                break;
            }
            lq_release(obj);
            return fail(p, "expected ',' or '}'");
        }
        p->depth--;
        *out = convert_special(p, obj);
        return true;
    }
    if (c == '[') {
        if (++p->depth > JSON_MAX_DEPTH) return fail(p, "nesting too deep");
        p->i++;
        lq_value arr = lq_arr_new(0);
        ws(p);
        if (p->i < p->n && p->s[p->i] == ']') {
            p->i++;
            p->depth--;
            *out = arr;
            return true;
        }
        for (;;) {
            lq_value v;
            if (!parse_value(p, &v)) {
                lq_release(arr);
                return false;
            }
            lq_arr_push_take(arr.u.a, v);
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') {
                p->i++;
                continue;
            }
            if (p->i < p->n && p->s[p->i] == ']') {
                p->i++;
                break;
            }
            lq_release(arr);
            return fail(p, "expected ',' or ']'");
        }
        p->depth--;
        *out = arr;
        return true;
    }
    if (c == '"') {
        lq_buf b;
        lq_buf_init(&b);
        if (!parse_string(p, &b)) {
            lq_buf_free(&b);
            return false;
        }
        *out = lq_str_take_buf(&b);
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p, out);
    if (p->n - p->i >= 4 && memcmp(p->s + p->i, "true", 4) == 0) {
        p->i += 4;
        *out = lq_bool(true);
        return true;
    }
    if (p->n - p->i >= 5 && memcmp(p->s + p->i, "false", 5) == 0) {
        p->i += 5;
        *out = lq_bool(false);
        return true;
    }
    if (p->n - p->i >= 4 && memcmp(p->s + p->i, "null", 4) == 0) {
        p->i += 4;
        *out = lq_nil();
        return true;
    }
    return fail(p, "unexpected character");
}

bool lq_json_parse(const char *s, size_t n, const lq_json_opts *o, lq_value *out, char **errmsg) {
    jp p;
    memset(&p, 0, sizeof p);
    p.s = s ? s : "";
    p.n = s ? n : 0;
    p.o = o;
    lq_value v;
    if (!parse_value(&p, &v)) {
        if (errmsg) *errmsg = lq_xstrndup(p.err, strlen(p.err));
        return false;
    }
    ws(&p);
    if (p.i != p.n) {
        lq_release(v);
        fail(&p, "trailing characters");
        if (errmsg) *errmsg = lq_xstrndup(p.err, strlen(p.err));
        return false;
    }
    *out = v;
    return true;
}

void lq_json_write_str(lq_buf *b, const char *s, size_t n) {
    lq_buf_reserve(b, n + 2);
    lq_buf_putc(b, '"');
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        while (i < n && p[i] >= 0x20 && p[i] != '"' && p[i] != '\\' && p[i] < 0x80) i++;
        lq_buf_put(b, s + st, i - st);
        if (i >= n) break;
        unsigned char c = p[i];
        if (c >= 0x80) {
            int32_t cp;
            size_t k = lq_utf8_decode(p + i, n - i, &cp);
            if (cp < 0) {
                lq_buf_put(b, "\\ufffd", 6);
                i += 1;
            } else {
                lq_buf_put(b, s + i, k);
                i += k;
            }
            continue;
        }
        switch (c) {
        case '"': lq_buf_put(b, "\\\"", 2); break;
        case '\\': lq_buf_put(b, "\\\\", 2); break;
        case '\n': lq_buf_put(b, "\\n", 2); break;
        case '\r': lq_buf_put(b, "\\r", 2); break;
        case '\t': lq_buf_put(b, "\\t", 2); break;
        case '\b': lq_buf_put(b, "\\b", 2); break;
        case '\f': lq_buf_put(b, "\\f", 2); break;
        default: lq_buf_printf(b, "\\u%04x", c);
        }
        i++;
    }
    lq_buf_putc(b, '"');
}

void lq_json_write_value(lq_buf *b, lq_value v, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        lq_buf_puts(b, "null");
        return;
    }
    switch (v.type) {
    case LQ_NIL: lq_buf_puts(b, "null"); break;
    case LQ_BOOL: lq_buf_puts(b, v.u.b ? "true" : "false"); break;
    case LQ_INT: lq_fmt_int(b, v.u.i); break;
    case LQ_BIGINT: lq_bigint_fmt(b, v.u.bi); break;
    case LQ_FLOAT:
        if (v.u.f != v.u.f || v.u.f - v.u.f != 0) lq_buf_puts(b, "null");
        else lq_buf_printf(b, "%.17g", v.u.f);
        break;
    case LQ_STR: lq_json_write_str(b, v.u.s->data, v.u.s->len); break;
    case LQ_ARR:
        lq_buf_putc(b, '[');
        for (size_t i = 0; i < v.u.a->len; i++) {
            if (i) lq_buf_putc(b, ',');
            lq_json_write_value(b, v.u.a->items[i], depth + 1);
        }
        lq_buf_putc(b, ']');
        break;
    case LQ_HASH:
        lq_buf_putc(b, '{');
        for (size_t i = 0; i < v.u.h->len; i++) {
            if (i) lq_buf_putc(b, ',');
            lq_json_write_str(b, v.u.h->ents[i].key->data, v.u.h->ents[i].key->len);
            lq_buf_putc(b, ':');
            lq_json_write_value(b, v.u.h->ents[i].val, depth + 1);
        }
        lq_buf_putc(b, '}');
        break;
    default: lq_buf_puts(b, "null"); break;
    }
}
