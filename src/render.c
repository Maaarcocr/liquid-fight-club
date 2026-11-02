/*
 * render.c - the tree-walking renderer.
 *
 * Mirrors the reference Context/BlockBody/tag semantics: scope stack,
 * per-context registers (cycle, for offsets, ifchanged, for stack), interrupt
 * stack, error handling (inline vs raised, blank-tag suppression), partial
 * loading/caching and resource limits.
 *
 * Errors are propagated with a flag on the context (ctx->has_err); every
 * function that can fail checks it after calls and cleans up its own
 * references before returning.
 */
#include "lq.h"
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_SCOPE_DEPTH 100
#ifndef LQ_DEFAULT_MAX_OUTPUT
#define LQ_DEFAULT_MAX_OUTPUT ((int64_t)64 * 1024 * 1024)
#endif
#ifndef LQ_MAX_RENDER_DEPTH
#define LQ_MAX_RENDER_DEPTH 1000
#endif
#ifndef LQ_DEFAULT_MAX_STEPS
#define LQ_DEFAULT_MAX_STEPS ((int64_t)100 * 1000 * 1000)
#endif

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

void lq_raise(lq_ctx *ctx, lq_err_kind kind, const char *fmt, ...) {
    if (ctx->has_err) return;
    lq_error_info_clear(&ctx->err);
    lq_buf b;
    lq_buf_init(&b);
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < sizeof tmp) lq_buf_put(&b, tmp, (size_t)n);
    else if (n >= 0) {
        lq_buf_reserve(&b, (size_t)n);
        va_start(ap, fmt);
        vsnprintf(b.p, (size_t)n + 1, fmt, ap);
        va_end(ap);
        b.len = (size_t)n;
    }
    ctx->err.kind = kind;
    ctx->err.msg = lq_buf_detach(&b, NULL);
    ctx->has_err = true;
}

static void raise_memory(lq_ctx *ctx) { lq_raise(ctx, LQ_ERR_MEMORY, "Memory limits exceeded"); }

/* ------------------------------------------------------------------ */
/* Resource accounting                                                 */
/* ------------------------------------------------------------------ */

/* Safety budget, independent of Liquid's own resource limits: every node
   rendered and every loop iteration costs one step. */
static bool tick(lq_ctx *ctx, int64_t n) {
    lq_shared *sh = ctx->sh;
    sh->steps += n;
    if (sh->steps > sh->max_steps) {
        raise_memory(ctx);
        return false;
    }
    return true;
}

/* ResourceLimits#increment_render_score */
static bool add_render_score(lq_ctx *ctx, int64_t n) {
    lq_shared *sh = ctx->sh;
    sh->render_score += n;
    sh->cum_render_score += n;
    const liquid_render_options *o = sh->opts;
    if (o->render_score_limit && sh->render_score > o->render_score_limit) {
        raise_memory(ctx);
        return false;
    }
    if (o->cumulative_render_score_limit && sh->cum_render_score > o->cumulative_render_score_limit) {
        raise_memory(ctx);
        return false;
    }
    return tick(ctx, n);
}

/* ResourceLimits#increment_assign_score */
static bool add_assign_score(lq_ctx *ctx, int64_t n) {
    lq_shared *sh = ctx->sh;
    sh->assign_score += n;
    sh->cum_assign_score += n;
    const liquid_render_options *o = sh->opts;
    if (o->assign_score_limit && sh->assign_score > o->assign_score_limit) {
        raise_memory(ctx);
        return false;
    }
    if (o->cumulative_assign_score_limit && sh->cum_assign_score > o->cumulative_assign_score_limit) {
        raise_memory(ctx);
        return false;
    }
    return true;
}

static int64_t assign_score_of(lq_value v, int depth) {
    if (depth > 1100) return 1;
    switch (v.type) {
    case LQ_STR: return (int64_t)v.u.s->len;
    case LQ_ARR: {
        int64_t s = 1;
        for (size_t i = 0; i < v.u.a->len; i++) s += assign_score_of(v.u.a->items[i], depth + 1);
        return s;
    }
    case LQ_HASH: {
        int64_t s = 1;
        for (size_t i = 0; i < v.u.h->len; i++) {
            s += (int64_t)v.u.h->ents[i].key->len;
            s += assign_score_of(v.u.h->ents[i].val, depth + 1);
        }
        return s;
    }
    default: return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Scopes                                                              */
/* ------------------------------------------------------------------ */

static void ctx_push_scope(lq_ctx *ctx) {
    if (ctx->nscopes == ctx->capscopes) {
        ctx->capscopes = ctx->capscopes ? ctx->capscopes * 2 : 8;
        ctx->scopes = lq_xrealloc(ctx->scopes, ctx->capscopes * sizeof(lq_hash *));
    }
    lq_value h = lq_hash_new(0);
    ctx->scopes[ctx->nscopes++] = h.u.h;
}

static void ctx_pop_scope(lq_ctx *ctx) {
    lq_hash *h = ctx->scopes[--ctx->nscopes];
    lq_value v;
    v.type = LQ_HASH;
    v.u.h = h;
    lq_release(v);
}

/* Context#stack: push a scope; false (with error) on overflow. The scope is
   pushed either way and must be popped by the caller. */
static bool ctx_stack_push(lq_ctx *ctx) {
    ctx_push_scope(ctx);
    if (ctx->base_depth + (int)ctx->nscopes > MAX_SCOPE_DEPTH) {
        lq_raise(ctx, LQ_ERR_STACK, "Nesting too deep");
        return false;
    }
    return true;
}

static void ctx_set_local(lq_ctx *ctx, lq_str *key, lq_value v) {
    lq_hash_set(ctx->scopes[ctx->nscopes - 1], key, v);
}

static void ctx_set_local_cstr(lq_ctx *ctx, const char *key, lq_value v) {
    lq_hash_set_cstr(ctx->scopes[ctx->nscopes - 1], key, v);
}

static void ctx_init(lq_ctx *ctx, lq_shared *sh, const lq_hash *env, int base_depth) {
    memset(ctx, 0, sizeof *ctx);
    ctx->sh = sh;
    ctx->env = env;
    ctx->base_depth = base_depth;
    ctx->counters = lq_hash_new(0);
    ctx->reg.cycle = lq_nil();
    ctx->reg.for_offsets = lq_nil();
    ctx->reg.ifchanged = lq_nil();
    ctx->reg.for_stack = lq_nil();
    ctx_push_scope(ctx);
}

static void ctx_destroy(lq_ctx *ctx) {
    while (ctx->nscopes) ctx_pop_scope(ctx);
    free(ctx->scopes);
    lq_release(ctx->counters);
    lq_release(ctx->reg.cycle);
    lq_release(ctx->reg.for_offsets);
    lq_release(ctx->reg.ifchanged);
    lq_release(ctx->reg.for_stack);
    free(ctx->intr);
    lq_error_info_clear(&ctx->err);
}

static void push_interrupt(lq_ctx *ctx, uint8_t k) {
    if (ctx->nintr == ctx->capintr) {
        ctx->capintr = ctx->capintr ? ctx->capintr * 2 : 4;
        ctx->intr = lq_xrealloc(ctx->intr, ctx->capintr);
    }
    ctx->intr[ctx->nintr++] = k;
}

static uint8_t pop_interrupt(lq_ctx *ctx) { return ctx->nintr ? ctx->intr[--ctx->nintr] : 0; }

/* ------------------------------------------------------------------ */
/* Self drop                                                           */
/* ------------------------------------------------------------------ */

static bool variable_defined(lq_ctx *ctx, lq_value key) {
    if (key.type != LQ_STR) return false;
    for (size_t i = ctx->nscopes; i-- > 0;)
        if (lq_hash_get_str(ctx->scopes[i], key.u.s, NULL)) return true;
    if (lq_hash_get_str(ctx->counters.u.h, key.u.s, NULL)) return true;
    if (ctx->env && lq_hash_get_str(ctx->env, key.u.s, NULL)) return true;
    return false;
}

static lq_value self_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    return lq_find_variable((lq_ctx *)d->data, key);
}

static const lq_drop_vt self_vt = {"Liquid::SelfDrop", self_get, NULL, NULL, NULL, NULL, NULL, NULL};

/* ------------------------------------------------------------------ */
/* Variable lookup                                                     */
/* ------------------------------------------------------------------ */

lq_value lq_find_variable(lq_ctx *ctx, lq_value key) {
    lq_value v;
    if (key.type == LQ_STR) {
        for (size_t i = ctx->nscopes; i-- > 0;) {
            if (lq_hash_get_str(ctx->scopes[i], key.u.s, &v)) return lq_retain(v);
        }
        if (lq_hash_get_str(ctx->counters.u.h, key.u.s, &v) && v.type != LQ_NIL) return lq_retain(v);
        if (ctx->env && lq_hash_get_str(ctx->env, key.u.s, &v) && v.type != LQ_NIL) {
            /* NilDrop materializes to nil via to_liquid */
            if (v.type == LQ_DROP && strcmp(v.u.d->vt->class_name, "StandardNilDrop") == 0) return lq_nil();
            return lq_retain(v);
        }
        if (key.u.s->len == 4 && memcmp(key.u.s->data, "self", 4) == 0) return lq_drop_new(&self_vt, ctx);
    }
    return lq_nil();
}

static bool is_cmd(lq_value key, const char *name) {
    size_t n = strlen(name);
    return key.type == LQ_STR && key.u.s->len == n && memcmp(key.u.s->data, name, n) == 0;
}

/* Drop results go through to_liquid (NilDrop -> nil). */
static lq_value to_liquid(lq_value v) {
    if (v.type == LQ_DROP && strcmp(v.u.d->vt->class_name, "StandardNilDrop") == 0) {
        lq_release(v);
        return lq_nil();
    }
    return v;
}

static lq_value first_char(lq_str *s, bool last) {
    if (!s->len) return lq_str_empty();
    const unsigned char *p = (const unsigned char *)s->data;
    if (!last) return lq_str_new(s->data, lq_utf8_len(p, s->len));
    /* find start of last char */
    size_t i = s->len - 1;
    while (i > 0 && (p[i] & 0xC0) == 0x80 && s->len - i < 4) i--;
    size_t k = lq_utf8_len(p + i, s->len - i);
    if (i + k != s->len) i = s->len - 1, k = 1;
    return lq_str_new(s->data + i, k);
}

lq_value lq_lookup_step(lq_ctx *ctx, lq_value obj, lq_value key_in, bool command) {
    lq_value key = lq_liquid_value(key_in, ctx);
    lq_value r = lq_nil();
    switch (obj.type) {
    case LQ_HASH: {
        lq_value v;
        if (key.type == LQ_STR && lq_hash_get_str(obj.u.h, key.u.s, &v)) {
            r = lq_retain(v);
            break;
        }
        if (command && is_cmd(key, "size")) r = lq_int((int64_t)obj.u.h->len);
        else if (command && is_cmd(key, "first")) {
            if (obj.u.h->len) {
                lq_value pair = lq_arr_new(2);
                lq_arr_push(pair.u.a, lq_str_val(obj.u.h->ents[0].key));
                lq_arr_push(pair.u.a, obj.u.h->ents[0].val);
                r = pair;
            }
        }
        break;
    }
    case LQ_ARR: {
        if (key.type == LQ_INT) {
            int64_t i = key.u.i, n = (int64_t)obj.u.a->len;
            if (i < 0) i += n;
            if (i >= 0 && i < n) r = lq_retain(obj.u.a->items[i]);
            break;
        }
        if (key.type == LQ_BIGINT) break;
        if (command && is_cmd(key, "size")) r = lq_int((int64_t)obj.u.a->len);
        else if (command && is_cmd(key, "first")) {
            if (obj.u.a->len) r = lq_retain(obj.u.a->items[0]);
        } else if (command && is_cmd(key, "last")) {
            if (obj.u.a->len) r = lq_retain(obj.u.a->items[obj.u.a->len - 1]);
        }
        break;
    }
    case LQ_STR:
        if (command && is_cmd(key, "size")) r = lq_int((int64_t)lq_str_nchars(obj.u.s));
        else if (command && is_cmd(key, "first")) r = first_char(obj.u.s, false);
        else if (command && is_cmd(key, "last")) r = first_char(obj.u.s, true);
        break;
    case LQ_INT:
    case LQ_BIGINT:
        if (command && is_cmd(key, "size")) r = lq_int(obj.type == LQ_INT ? 8 : lq_bigint_size(obj.u.bi));
        break;
    case LQ_RANGE: {
        int64_t a = obj.u.r->first, b = obj.u.r->last;
        if (command && is_cmd(key, "size")) r = lq_int(b < a ? 0 : (b - a == INT64_MAX ? INT64_MAX : b - a + 1));
        else if (command && is_cmd(key, "first")) r = lq_int(a);
        else if (command && is_cmd(key, "last")) r = lq_int(b);
        break;
    }
    case LQ_DROP: {
        lq_drop *d = obj.u.d;
        if (d->vt == &self_vt) {
            lq_ctx *owner = (lq_ctx *)d->data;
            if (variable_defined(owner, key)) r = lq_find_variable(owner, key);
            break;
        }
        if (d->vt->get) r = to_liquid(d->vt->get(d, key, ctx));
        break;
    }
    default:
        break;
    }
    lq_release(key);
    return r;
}

static int64_t clamp_i64(lq_value v) {
    if (v.type == LQ_INT) return v.u.i;
    if (v.type == LQ_BIGINT) return v.u.bi->neg ? INT64_MIN : INT64_MAX;
    return 0;
}

/* Utils.to_integer: Integer passes; everything else goes through
   Integer(obj.to_s). */
static bool to_integer(lq_ctx *ctx, lq_value v, int64_t *out) {
    if (v.type == LQ_INT || v.type == LQ_BIGINT) {
        *out = clamp_i64(v);
        return true;
    }
    lq_value s = lq_to_str(v, ctx);
    lq_value iv;
    bool ok = lq_ruby_Integer(s.u.s->data, s.u.s->len, &iv);
    lq_release(s);
    if (!ok || (iv.type != LQ_INT && iv.type != LQ_BIGINT)) {
        if (ok) lq_release(iv);
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid integer");
        return false;
    }
    *out = clamp_i64(iv);
    lq_release(iv);
    return true;
}

/* RangeLookup#to_integer */
static bool range_endpoint(lq_ctx *ctx, lq_value v, int64_t *out) {
    switch (v.type) {
    case LQ_INT:
    case LQ_BIGINT:
        *out = clamp_i64(v);
        return true;
    case LQ_NIL:
        *out = 0;
        return true;
    case LQ_STR: {
        lq_value iv = lq_str_to_i(v.u.s->data, v.u.s->len);
        *out = clamp_i64(iv);
        lq_release(iv);
        return true;
    }
    default:
        return to_integer(ctx, v, out);
    }
}

lq_value lq_eval(lq_ctx *ctx, const lq_expr *e) {
    switch (e->kind) {
    case EX_LIT:
        return lq_retain(e->lit);
    case EX_RANGE: {
        lq_value a = lq_eval(ctx, e->rstart);
        if (ctx->has_err) { lq_release(a); return lq_nil(); }
        lq_value b = lq_eval(ctx, e->rend);
        if (ctx->has_err) { lq_release(a); lq_release(b); return lq_nil(); }
        int64_t x = 0, y = 0;
        bool ok = range_endpoint(ctx, a, &x) && range_endpoint(ctx, b, &y);
        lq_release(a);
        lq_release(b);
        if (!ok) return lq_nil();
        return lq_range_new(x, y);
    }
    case EX_VAR: {
        lq_value name = lq_eval(ctx, e->name_expr);
        if (ctx->has_err) { lq_release(name); return lq_nil(); }
        lq_value obj = to_liquid(lq_find_variable(ctx, name));
        lq_release(name);
        for (size_t i = 0; i < e->nlook; i++) {
            lq_value key = lq_eval(ctx, e->look[i].key);
            if (ctx->has_err) {
                lq_release(key);
                lq_release(obj);
                return lq_nil();
            }
            lq_value next = lq_lookup_step(ctx, obj, key, e->look[i].command);
            lq_release(key);
            lq_release(obj);
            obj = next;
            if (ctx->has_err) {
                lq_release(obj);
                return lq_nil();
            }
            if (obj.type == LQ_NIL) break;
        }
        return obj;
    }
    }
    return lq_nil();
}

/* ------------------------------------------------------------------ */
/* Filters                                                             */
/* ------------------------------------------------------------------ */

static lq_value apply_filter(lq_ctx *ctx, const lq_filter *f, lq_value input) {
    size_t total = f->nargs + (f->nkw ? 1 : 0);
    lq_value small[8];
    lq_value *args = total <= 8 ? small : lq_xmalloc(total * sizeof(lq_value));
    size_t got = 0;
    for (size_t i = 0; i < f->nargs; i++) {
        args[got] = lq_eval(ctx, f->args[i]);
        got++;
        if (ctx->has_err) goto fail;
    }
    if (f->nkw) {
        lq_value h = lq_hash_new(f->nkw);
        args[got++] = h;
        for (size_t i = 0; i < f->nkw; i++) {
            lq_value v = lq_eval(ctx, f->kwargs[i]);
            lq_hash_set_take(h.u.h, f->kwnames[i], v);
            if (ctx->has_err) goto fail;
        }
    }
    lq_value r;
    if (!f->def) {
        r = lq_retain(input);
    } else {
        const lq_filter_def *d = f->def;
        if ((int)total < d->min_args || (d->max_args >= 0 && (int)total > d->max_args)) {
            if (d->max_args < 0)
                lq_raise(ctx, LQ_ERR_ARGUMENT, "wrong number of arguments (given %zu, expected %d+)", total + 1,
                         d->min_args + 1);
            else if (d->min_args == d->max_args)
                lq_raise(ctx, LQ_ERR_ARGUMENT, "wrong number of arguments (given %zu, expected %d)", total + 1,
                         d->min_args + 1);
            else
                lq_raise(ctx, LQ_ERR_ARGUMENT, "wrong number of arguments (given %zu, expected %d..%d)", total + 1,
                         d->min_args + 1, d->max_args + 1);
            goto fail;
        }
        r = d->fn(ctx, input, args, total);
        r = to_liquid(r);
        /* charge the safety budget for the data produced, so repeated
           copying of growing values stays bounded */
        if (r.type == LQ_STR) tick(ctx, (int64_t)(r.u.s->len >> 5));
        else if (r.type == LQ_ARR) tick(ctx, (int64_t)r.u.a->len);
    }
    for (size_t i = 0; i < got; i++) lq_release(args[i]);
    if (args != small) free(args);
    return r;
fail:
    for (size_t i = 0; i < got; i++) lq_release(args[i]);
    if (args != small) free(args);
    return lq_nil();
}

lq_value lq_render_variable(lq_ctx *ctx, const lq_variable *v) {
    lq_value obj = v->expr ? lq_eval(ctx, v->expr) : lq_nil();
    if (ctx->has_err) {
        lq_release(obj);
        return lq_nil();
    }
    for (size_t i = 0; i < v->nfilters; i++) {
        lq_value next = apply_filter(ctx, &v->filters[i], obj);
        lq_release(obj);
        obj = next;
        if (ctx->has_err) {
            lq_release(obj);
            return lq_nil();
        }
    }
    return obj;
}

/* ------------------------------------------------------------------ */
/* Conditions                                                          */
/* ------------------------------------------------------------------ */

static bool responds_to_compare(lq_value v) {
    return v.type == LQ_INT || v.type == LQ_FLOAT || v.type == LQ_BIGINT || v.type == LQ_STR;
}

static void cmp_error(lq_ctx *ctx, lq_value l, lq_value r) {
    lq_buf b;
    lq_buf_init(&b);
    if (r.type == LQ_NIL || r.type == LQ_BOOL || r.type == LQ_INT || r.type == LQ_FLOAT) lq_inspect(&b, r, ctx);
    else lq_buf_puts(&b, lq_class_name(r));
    lq_raise(ctx, LQ_ERR_ARGUMENT, "comparison of %s with %s failed", lq_class_name(l), b.p ? b.p : "");
    lq_buf_free(&b);
}

static bool method_literal_check(lq_value lit, lq_value v) {
    if (lit.type == LQ_EMPTY) return lq_is_empty(v);
    return lq_is_blank(v);
}

static bool str_contains(const char *h, size_t hn, const char *n, size_t nn) {
    return lq_memfind(h, hn, n, nn) >= 0;
}

/* returns a new reference */
static lq_value interpret_condition(lq_ctx *ctx, const lq_cond *c) {
    if (c->op == OP_NONE) return lq_eval(ctx, c->left);
    lq_value l0 = lq_eval(ctx, c->left);
    if (ctx->has_err) { lq_release(l0); return lq_nil(); }
    lq_value r0 = lq_eval(ctx, c->right);
    if (ctx->has_err) { lq_release(l0); lq_release(r0); return lq_nil(); }
    lq_value l = lq_liquid_value(l0, ctx), r = lq_liquid_value(r0, ctx);
    lq_release(l0);
    lq_release(r0);
    lq_value res = lq_nil();
    switch (c->op) {
    case OP_EQ:
    case OP_NE: {
        bool eq;
        if (l.type == LQ_EMPTY || l.type == LQ_BLANK) eq = method_literal_check(l, r);
        else if (r.type == LQ_EMPTY || r.type == LQ_BLANK) eq = method_literal_check(r, l);
        else eq = lq_equal(l, r);
        res = lq_bool(c->op == OP_EQ ? eq : !eq);
        break;
    }
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE: {
        if (!responds_to_compare(l) || !responds_to_compare(r)) break;
        int cmp;
        if (l.type == LQ_STR && r.type == LQ_STR) {
            size_t n = l.u.s->len < r.u.s->len ? l.u.s->len : r.u.s->len;
            int m = memcmp(l.u.s->data, r.u.s->data, n);
            cmp = m < 0 ? -1 : m > 0 ? 1 : (l.u.s->len < r.u.s->len ? -1 : l.u.s->len > r.u.s->len);
        } else if (lq_is_numeric(l) && lq_is_numeric(r)) {
            cmp = lq_num_cmp(l, r);
            if (cmp == 2) {
                res = lq_bool(false);
                break;
            }
        } else {
            cmp_error(ctx, l, r);
            break;
        }
        bool b = c->op == OP_LT ? cmp < 0 : c->op == OP_GT ? cmp > 0 : c->op == OP_LE ? cmp <= 0 : cmp >= 0;
        res = lq_bool(b);
        break;
    }
    case OP_CONTAINS: {
        if (!lq_truthy(l) || !lq_truthy(r)) {
            res = lq_bool(false);
            break;
        }
        if (l.type == LQ_STR) {
            lq_value rs = lq_to_str(r, ctx);
            res = lq_bool(str_contains(l.u.s->data, l.u.s->len, rs.u.s->data, rs.u.s->len));
            lq_release(rs);
        } else if (l.type == LQ_ARR) {
            bool f = false;
            for (size_t i = 0; i < l.u.a->len && !f; i++) f = lq_equal(l.u.a->items[i], r);
            res = lq_bool(f);
        } else if (l.type == LQ_HASH) {
            res = lq_bool(r.type == LQ_STR && lq_hash_get_str(l.u.h, r.u.s, NULL));
        } else if (l.type == LQ_RANGE) {
            bool f = false;
            if (lq_is_numeric(r)) {
                f = lq_num_cmp(lq_int(l.u.r->first), r) <= 0 && lq_num_cmp(r, lq_int(l.u.r->last)) <= 0;
            }
            res = lq_bool(f);
        } else {
            res = lq_bool(false);
        }
        break;
    }
    }
    lq_release(l);
    lq_release(r);
    return res;
}

static bool eval_condition(lq_ctx *ctx, const lq_cond *c) {
    lq_value result = lq_nil();
    while (c) {
        lq_release(result);
        result = interpret_condition(ctx, c);
        if (ctx->has_err) {
            lq_release(result);
            return false;
        }
        if (c->rel == REL_OR) {
            lq_value lv = lq_liquid_value(result, ctx);
            bool t = lq_truthy(lv);
            lq_release(lv);
            if (t) break;
        } else if (c->rel == REL_AND) {
            lq_value lv = lq_liquid_value(result, ctx);
            bool t = lq_truthy(lv);
            lq_release(lv);
            if (!t) break;
        } else break;
        c = c->child;
    }
    lq_value lv = lq_liquid_value(result, ctx);
    bool t = lq_truthy(lv);
    lq_release(lv);
    lq_release(result);
    return t;
}

/* ------------------------------------------------------------------ */
/* Iteration helpers                                                   */
/* ------------------------------------------------------------------ */

/* A sequence: either a materialized array or a slice of an integer range. */
typedef struct seq {
    lq_value arr; /* LQ_ARR or nil */
    bool is_range;
    int64_t start; /* range: first value */
    int64_t len;
    bool reversed;
} seq;

static void seq_free(seq *s) { lq_release(s->arr); }

static lq_value seq_at(const seq *s, int64_t i) {
    int64_t k = s->reversed ? s->len - 1 - i : i;
    if (s->is_range) return lq_int(s->start + k);
    return lq_retain(s->arr.u.a->items[k]);
}

/* Utils.slice_collection_using_each(collection, from, to). to < 0 = none. */
static bool slice_collection(lq_ctx *ctx, lq_value coll, int64_t from, bool has_to, int64_t to, seq *out) {
    memset(out, 0, sizeof *out);
    out->arr = lq_nil();
    switch (coll.type) {
    case LQ_STR:
        if (coll.u.s->len) {
            out->arr = lq_arr_new(1);
            lq_arr_push(out->arr.u.a, coll);
            out->len = 1;
        }
        return true;
    case LQ_RANGE: {
        int64_t a = coll.u.r->first, b = coll.u.r->last;
        if (b < a) return true;
        /* index i in [lo, hi) where lo = max(from,0), hi = min(count, to) */
        uint64_t count = (uint64_t)b - (uint64_t)a + 1; /* may wrap for full range */
        if (count == 0) count = UINT64_MAX;
        int64_t lo = from < 0 ? 0 : from;
        uint64_t hi = count;
        if (has_to) {
            if (to <= 0) return true;
            if ((uint64_t)to < hi) hi = (uint64_t)to;
        }
        if ((uint64_t)lo >= hi) return true;
        out->is_range = true;
        out->start = a + lo;
        uint64_t n = hi - (uint64_t)lo;
        out->len = n > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)n;
        return true;
    }
    case LQ_ARR:
    case LQ_HASH: {
        size_t n = coll.type == LQ_ARR ? coll.u.a->len : coll.u.h->len;
        int64_t lo = from < 0 ? 0 : from;
        int64_t hi = (int64_t)n;
        if (has_to && to < hi) hi = to;
        if (lo >= hi) return true;
        if (coll.type == LQ_ARR && lo == 0 && hi == (int64_t)n) {
            out->arr = lq_retain(coll);
        } else {
            out->arr = lq_arr_new((size_t)(hi - lo));
            for (int64_t i = lo; i < hi; i++) {
                if (coll.type == LQ_ARR) lq_arr_push(out->arr.u.a, coll.u.a->items[i]);
                else {
                    lq_value pair = lq_arr_new(2);
                    lq_arr_push(pair.u.a, lq_str_val(coll.u.h->ents[i].key));
                    lq_arr_push(pair.u.a, coll.u.h->ents[i].val);
                    lq_arr_push_take(out->arr.u.a, pair);
                }
            }
        }
        out->len = hi - lo;
        return true;
    }
    case LQ_DROP: {
        if (!coll.u.d->vt->iterate) return true;
        lq_value items = coll.u.d->vt->iterate(coll.u.d, ctx);
        if (ctx->has_err) {
            lq_release(items);
            return false;
        }
        if (items.type != LQ_ARR) {
            lq_release(items);
            return true;
        }
        bool r = slice_collection(ctx, items, from, has_to, to, out);
        lq_release(items);
        return r;
    }
    default:
        return true;
    }
}

/* ------------------------------------------------------------------ */
/* Node rendering                                                      */
/* ------------------------------------------------------------------ */

static void render_body(lq_ctx *ctx, const lq_body *b, lq_buf *out);

static bool check_output(lq_ctx *ctx, lq_buf *out) {
    lq_shared *sh = ctx->sh;
    const liquid_render_options *o = sh->opts;
    if (ctx->capture_depth == 0) {
        if (o->render_length_limit && (int64_t)out->len > o->render_length_limit) {
            raise_memory(ctx);
            return false;
        }
    }
    if ((int64_t)out->len > sh->max_output) {
        raise_memory(ctx);
        return false;
    }
    return true;
}

static void render_text(lq_buf *out, const char *s, size_t n) { lq_buf_put(out, s, n); }

static void render_if(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    for (size_t i = 0; i < n->u.cond.n; i++) {
        const lq_cblock *blk = &n->u.cond.blocks[i];
        bool t;
        if (!blk->cond) t = true;
        else {
            t = eval_condition(ctx, blk->cond);
            if (ctx->has_err) return;
            if (n->kind == N_UNLESS && i == 0) t = !t;
        }
        if (t) {
            render_body(ctx, blk->body, out);
            return;
        }
    }
}

static void render_case(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    bool run_else = true;
    for (size_t i = 0; i < n->u.cond.n; i++) {
        const lq_cblock *blk = &n->u.cond.blocks[i];
        if (!blk->cond) {
            if (run_else) {
                render_body(ctx, blk->body, out);
                if (ctx->has_err) return;
            }
            continue;
        }
        bool t = eval_condition(ctx, blk->cond);
        if (ctx->has_err) return;
        if (t) {
            run_else = false;
            render_body(ctx, blk->body, out);
            if (ctx->has_err) return;
        }
    }
}

static lq_value *reg_hash(lq_value *slot) {
    if (slot->type != LQ_HASH) {
        lq_release(*slot);
        *slot = lq_hash_new(0);
    }
    return slot;
}

static void render_for(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    int64_t from = 0;
    lq_value *offs = reg_hash(&ctx->reg.for_offsets);
    if (n->u.loop.offset_continue) {
        lq_value v;
        if (lq_hash_get_str(offs->u.h, n->u.loop.name, &v) && v.type == LQ_INT) from = v.u.i;
    } else if (n->u.loop.offset) {
        lq_value fv = lq_eval(ctx, n->u.loop.offset);
        if (ctx->has_err) { lq_release(fv); return; }
        if (fv.type != LQ_NIL) {
            bool ok = to_integer(ctx, fv, &from);
            lq_release(fv);
            if (!ok) return;
        }
    }
    lq_value coll = lq_eval(ctx, n->u.loop.coll);
    if (ctx->has_err) { lq_release(coll); return; }
    bool has_to = false;
    int64_t to = 0;
    if (n->u.loop.limit) {
        lq_value lv = lq_eval(ctx, n->u.loop.limit);
        if (ctx->has_err) { lq_release(lv); lq_release(coll); return; }
        if (lv.type != LQ_NIL) {
            int64_t lim;
            bool ok = to_integer(ctx, lv, &lim);
            lq_release(lv);
            if (!ok) { lq_release(coll); return; }
            has_to = true;
            if (__builtin_add_overflow(lim, from, &to)) to = lim > 0 ? INT64_MAX : INT64_MIN;
        }
    }
    seq s;
    bool ok = slice_collection(ctx, coll, from, has_to, to, &s);
    lq_release(coll);
    if (!ok) return;
    s.reversed = n->u.loop.reversed;
    {
        int64_t next;
        if (__builtin_add_overflow(from, s.len, &next)) next = INT64_MAX;
        offs = reg_hash(&ctx->reg.for_offsets);
        lq_hash_set_take(offs->u.h, n->u.loop.name, lq_int(next));
    }
    if (s.len == 0) {
        seq_free(&s);
        if (n->u.loop.else_body) render_body(ctx, n->u.loop.else_body, out);
        return;
    }
    if (!ctx_stack_push(ctx)) {
        ctx_pop_scope(ctx);
        seq_free(&s);
        return;
    }
    if (ctx->reg.for_stack.type != LQ_ARR) {
        lq_release(ctx->reg.for_stack);
        ctx->reg.for_stack = lq_arr_new(4);
    }
    lq_arr *fs = ctx->reg.for_stack.u.a;
    lq_value parent = fs->len ? fs->items[fs->len - 1] : lq_nil();
    lq_value loop = lq_forloop_new(n->u.loop.name, s.len, parent);
    lq_arr_push(fs, loop);
    ctx_set_local_cstr(ctx, "forloop", loop);
    for (int64_t i = 0; i < s.len; i++) {
        lq_value item = seq_at(&s, i);
        ctx_set_local(ctx, n->u.loop.var, item);
        lq_release(item);
        render_body(ctx, n->u.loop.body, out);
        lq_forloop_increment(loop);
        if (ctx->has_err) break;
        if (ctx->nintr) {
            uint8_t k = pop_interrupt(ctx);
            if (k == INTR_BREAK) break;
        }
        if (!tick(ctx, 1)) break;
    }
    /* for_stack.pop */
    fs = ctx->reg.for_stack.u.a;
    if (fs->len) lq_release(fs->items[--fs->len]);
    lq_release(loop);
    ctx_pop_scope(ctx);
    seq_free(&s);
}

/* TableRow#to_integer: value.to_i, NoMethodError -> invalid integer */
static bool tablerow_int(lq_ctx *ctx, lq_value v, int64_t *out) {
    switch (v.type) {
    case LQ_NIL: *out = 0; return true;
    case LQ_INT: case LQ_BIGINT: *out = clamp_i64(v); return true;
    case LQ_FLOAT: {
        double d = v.u.f;
        if (d != d || d > 9.2e18 || d < -9.2e18) {
            lq_raise(ctx, LQ_ERR_FLOATDOMAIN, "%s", d != d ? "NaN" : d > 0 ? "Infinity" : "-Infinity");
            return false;
        }
        *out = (int64_t)d;
        return true;
    }
    case LQ_STR: {
        lq_value iv = lq_str_to_i(v.u.s->data, v.u.s->len);
        *out = clamp_i64(iv);
        lq_release(iv);
        return true;
    }
    case LQ_DROP: {
        bool has = false;
        if (v.u.d->vt->to_number) {
            lq_value nv = v.u.d->vt->to_number(v.u.d, ctx, &has);
            if (has && (nv.type == LQ_INT || nv.type == LQ_BIGINT)) {
                *out = clamp_i64(nv);
                lq_release(nv);
                return true;
            }
            lq_release(nv);
        }
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid integer");
        return false;
    }
    default:
        lq_raise(ctx, LQ_ERR_ARGUMENT, "invalid integer");
        return false;
    }
}

static const lq_expr *trow_attr(const lq_node *n, const char *name) {
    const lq_expr *r = NULL;
    size_t len = strlen(name);
    for (size_t i = 0; i < n->u.trow.nattrs; i++) {
        lq_str *k = n->u.trow.attrs[i].name;
        if (k->len == len && memcmp(k->data, name, len) == 0) r = n->u.trow.attrs[i].expr;
    }
    return r;
}

static bool eval_trow_int(lq_ctx *ctx, const lq_expr *e, int64_t *out) {
    lq_value v = lq_eval(ctx, e);
    if (ctx->has_err) { lq_release(v); return false; }
    bool ok = tablerow_int(ctx, v, out);
    lq_release(v);
    return ok;
}

static void render_tablerow(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    lq_value coll = lq_eval(ctx, n->u.trow.coll);
    if (ctx->has_err) { lq_release(coll); return; }
    if (!lq_truthy(coll)) { lq_release(coll); return; }
    int64_t from = 0, lim = 0, cols = 0;
    bool has_to = false;
    const lq_expr *e;
    if ((e = trow_attr(n, "offset")) && !eval_trow_int(ctx, e, &from)) { lq_release(coll); return; }
    if ((e = trow_attr(n, "limit"))) {
        if (!eval_trow_int(ctx, e, &lim)) { lq_release(coll); return; }
        has_to = true;
        if (__builtin_add_overflow(from, lim, &lim)) lim = INT64_MAX;
    }
    seq s;
    bool ok = slice_collection(ctx, coll, from, has_to, lim, &s);
    lq_release(coll);
    if (!ok) return;
    if ((e = trow_attr(n, "cols"))) {
        if (!eval_trow_int(ctx, e, &cols)) { seq_free(&s); return; }
    } else cols = s.len;
    lq_buf_puts(out, "<tr class=\"row1\">\n");
    if (!ctx_stack_push(ctx)) {
        ctx_pop_scope(ctx);
        seq_free(&s);
        return;
    }
    lq_value loop = lq_tablerowloop_new(s.len, cols);
    ctx_set_local_cstr(ctx, "tablerowloop", loop);
    for (int64_t i = 0; i < s.len; i++) {
        lq_value item = seq_at(&s, i);
        ctx_set_local(ctx, n->u.trow.var, item);
        lq_release(item);
        lq_buf_printf(out, "<td class=\"col%lld\">", (long long)lq_tablerowloop_col(loop));
        render_body(ctx, n->u.trow.body, out);
        lq_buf_puts(out, "</td>");
        if (ctx->has_err) break;
        if (ctx->nintr) {
            uint8_t k = pop_interrupt(ctx);
            if (k == INTR_BREAK) break;
        }
        if (lq_tablerowloop_col_last(loop) && !lq_tablerowloop_last(loop))
            lq_buf_printf(out, "</tr>\n<tr class=\"row%lld\">", (long long)lq_tablerowloop_row(loop) + 1);
        lq_tablerowloop_increment(loop);
        if (!tick(ctx, 1)) break;
    }
    lq_release(loop);
    ctx_pop_scope(ctx);
    seq_free(&s);
    if (!ctx->has_err) lq_buf_puts(out, "</tr>\n");
}

static void render_cycle(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    lq_value keyv;
    lq_buf kb;
    lq_buf_init(&kb);
    if (n->u.cycle.name) {
        lq_value nv = lq_eval(ctx, n->u.cycle.name);
        if (ctx->has_err) { lq_release(nv); lq_buf_free(&kb); return; }
        lq_buf_putc(&kb, 'N');
        lq_inspect(&kb, nv, ctx);
        lq_release(nv);
    } else {
        lq_buf_putc(&kb, 'U');
        lq_buf_put(&kb, n->u.cycle.key->data, n->u.cycle.key->len);
    }
    keyv = lq_str_take_buf(&kb);
    lq_value *reg = reg_hash(&ctx->reg.cycle);
    int64_t it = 0;
    lq_value cur;
    if (lq_hash_get_str(reg->u.h, keyv.u.s, &cur) && cur.type == LQ_INT) it = cur.u.i;
    if (it < 0 || (size_t)it >= n->u.cycle.n) it = 0;
    lq_value val = lq_eval(ctx, n->u.cycle.vals[it]);
    if (ctx->has_err) {
        lq_release(val);
        lq_release(keyv);
        return;
    }
    if (val.type == LQ_ARR) {
        /* Array#join */
        for (size_t i = 0; i < val.u.a->len; i++) {
            lq_value e = val.u.a->items[i];
            if (e.type == LQ_ARR) lq_output(out, e, ctx);
            else lq_to_s(out, e, ctx);
        }
    } else {
        lq_to_s(out, val, ctx);
    }
    lq_release(val);
    it++;
    if ((size_t)it >= n->u.cycle.n) it = 0;
    reg = reg_hash(&ctx->reg.cycle);
    lq_hash_set_take(reg->u.h, keyv.u.s, lq_int(it));
    lq_release(keyv);
}

static void render_counter(lq_ctx *ctx, const lq_node *n, lq_buf *out, bool inc) {
    lq_hash *env = ctx->counters.u.h;
    lq_value v;
    bool found = lq_hash_get_str(env, n->u.counter.name, &v);
    lq_value cur = found ? v : lq_nil();
    lq_value next;
    if (cur.type == LQ_NIL || (cur.type == LQ_BOOL && !cur.u.b)) cur = lq_int(0);
    if (cur.type == LQ_INT || cur.type == LQ_BIGINT) {
        next = lq_int_add(cur, lq_int(1), !inc);
    } else if (cur.type == LQ_FLOAT) {
        next = lq_float(cur.u.f + (inc ? 1 : -1));
    } else {
        lq_raise(ctx, LQ_ERR_INTERNAL, "internal");
        return;
    }
    if (inc) lq_to_s(out, cur, ctx);
    else lq_to_s(out, next, ctx);
    lq_hash_set_take(env, n->u.counter.name, next);
}

static void render_ifchanged(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    lq_buf tmp;
    lq_buf_init(&tmp);
    render_body(ctx, n->u.body, &tmp);
    if (ctx->has_err) {
        lq_buf_free(&tmp);
        return;
    }
    lq_value prev = ctx->reg.ifchanged;
    bool same = prev.type == LQ_STR && prev.u.s->len == tmp.len && (tmp.len == 0 || memcmp(prev.u.s->data, tmp.p, tmp.len) == 0);
    if (!same) {
        lq_buf_put(out, tmp.p ? tmp.p : "", tmp.len);
        lq_release(ctx->reg.ifchanged);
        ctx->reg.ifchanged = lq_str_take_buf(&tmp);
    } else {
        lq_buf_free(&tmp);
    }
}

/* ---- partials ---- */

/* File system key normalization: ASCII-lowercased, ".liquid" appended. */
static lq_value normalize_fs_name(const char *s, size_t n) {
    lq_buf b;
    lq_buf_init(&b);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        lq_buf_putc(&b, c);
    }
    if (!(b.len >= 7 && memcmp(b.p + b.len - 7, ".liquid", 7) == 0)) lq_buf_puts(&b, ".liquid");
    return lq_str_take_buf(&b);
}

static const lq_str *fs_lookup(lq_shared *sh, lq_str *name) {
    if (!sh->fs) return NULL;
    if (sh->fs_index.type != LQ_HASH) {
        sh->fs_index = lq_hash_new(sh->fs->len);
        for (size_t i = 0; i < sh->fs->len; i++) {
            lq_hent *h = &sh->fs->ents[i];
            if (h->val.type != LQ_STR) continue;
            lq_value k = normalize_fs_name(h->key->data, h->key->len);
            lq_hash_set(sh->fs_index.u.h, k.u.s, h->val);
            lq_release(k);
        }
    }
    lq_value k = normalize_fs_name(name->data, name->len);
    lq_value v;
    const lq_str *r = NULL;
    if (lq_hash_get_str(sh->fs_index.u.h, k.u.s, &v)) r = v.u.s;
    lq_release(k);
    return r;
}

static const lq_partial_entry *load_partial(lq_ctx *ctx, lq_str *name) {
    lq_shared *sh = ctx->sh;
    if (sh->partial_index.type != LQ_HASH) sh->partial_index = lq_hash_new(0);
    lq_value idx;
    if (lq_hash_get_str(sh->partial_index.u.h, name, &idx)) return &sh->partials[idx.u.i];
    lq_hash_set_take(sh->partial_index.u.h, name, lq_int((int64_t)sh->npartials));
    if (sh->npartials == sh->cappartials) {
        sh->cappartials = sh->cappartials ? sh->cappartials * 2 : 4;
        sh->partials = lq_xrealloc(sh->partials, sh->cappartials * sizeof(lq_partial_entry));
    }
    lq_partial_entry *e = &sh->partials[sh->npartials++];
    memset(e, 0, sizeof *e);
    e->name = lq_xstrndup(name->data, name->len);
    const lq_str *src = fs_lookup(sh, name);
    if (!src) {
        e->err.kind = LQ_ERR_FILESYSTEM;
        if (!sh->fs && sh->opts->no_file_system_message) {
            e->err.msg = lq_xstrndup(sh->opts->no_file_system_message, strlen(sh->opts->no_file_system_message));
        } else {
            lq_buf b;
            lq_buf_init(&b);
            lq_buf_puts(&b, "Could not find asset ");
            lq_buf_put(&b, name->data, name->len);
            e->err.msg = lq_buf_detach(&b, NULL);
        }
        return e;
    }
    e->tpl = lq_template_parse(src->data, src->len, sh->line_numbers, &e->err);
    if (!e->tpl) {
        free(e->err.template_name);
        e->err.template_name = lq_xstrndup(name->data, name->len);
    }
    return e;
}

static void raise_copy(lq_ctx *ctx, const lq_error_info *src) {
    if (ctx->has_err) return;
    lq_error_info_clear(&ctx->err);
    ctx->err.kind = src->kind;
    ctx->err.msg = src->msg ? lq_xstrndup(src->msg, strlen(src->msg)) : NULL;
    ctx->err.markup_ctx = src->markup_ctx ? lq_xstrndup(src->markup_ctx, strlen(src->markup_ctx)) : NULL;
    ctx->err.template_name = src->template_name ? lq_xstrndup(src->template_name, strlen(src->template_name)) : NULL;
    ctx->err.line = src->line;
    ctx->err.has_line = src->has_line;
    ctx->has_err = true;
}

static const char *basename_of(lq_str *s, size_t *len) {
    const char *p = s->data;
    size_t n = s->len;
    const char *slash = NULL;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '/') slash = p + i;
    if (!slash) {
        *len = n;
        return p;
    }
    /* Ruby split('/').last: trailing empty fields are dropped */
    const char *start = slash + 1;
    if ((size_t)(start - p) == n) {
        /* name ends with '/': find previous non-empty field */
        size_t end = n;
        while (end > 0 && p[end - 1] == '/') end--;
        size_t b = end;
        while (b > 0 && p[b - 1] != '/') b--;
        *len = end - b;
        return p + b;
    }
    *len = n - (size_t)(start - p);
    return start;
}

/* Render a partial's body: Template#render resets per-render scores and
   swallows MemoryError in inline mode. */
static void render_partial_body(lq_ctx *ctx, const lq_template *t, lq_buf *out) {
    ctx->sh->render_score = 0;
    ctx->sh->assign_score = 0;
    render_body(ctx, t->root, out);
    if (ctx->has_err && ctx->err.kind == LQ_ERR_MEMORY && !ctx->sh->strict) {
        ctx->sh->nerrors++;
        lq_error_info_clear(&ctx->err);
        ctx->has_err = false;
    }
}

static void render_include(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    if (ctx->include_disabled > 0) {
        lq_raise(ctx, LQ_ERR_DISABLED, "include usage is not allowed in this context");
        return;
    }
    lq_value tname = lq_eval(ctx, n->u.partial.tname);
    if (ctx->has_err) { lq_release(tname); return; }
    if (tname.type != LQ_STR) {
        lq_release(tname);
        lq_raise(ctx, LQ_ERR_ARGUMENT, "Argument error in tag 'include' - Illegal template name");
        return;
    }
    const lq_partial_entry *pe = load_partial(ctx, tname.u.s);
    if (!pe->tpl) {
        raise_copy(ctx, &pe->err);
        lq_release(tname);
        return;
    }
    size_t vlen;
    const char *vname = n->u.partial.alias ? n->u.partial.alias->data : basename_of(tname.u.s, &vlen);
    if (n->u.partial.alias) vlen = n->u.partial.alias->len;
    lq_value varname = lq_str_new(vname, vlen);
    lq_value variable;
    if (n->u.partial.var) {
        variable = lq_eval(ctx, n->u.partial.var);
        if (ctx->has_err) { lq_release(variable); lq_release(varname); lq_release(tname); return; }
    } else {
        variable = to_liquid(lq_find_variable(ctx, tname));
    }
    const char *old_name = ctx->template_name;
    bool old_partial = ctx->partial;
    ctx->template_name = pe->name;
    ctx->partial = true;
    if (ctx_stack_push(ctx)) {
        for (size_t i = 0; i < n->u.partial.nattrs; i++) {
            lq_value v = lq_eval(ctx, n->u.partial.attrs[i].expr);
            if (ctx->has_err) { lq_release(v); break; }
            ctx_set_local(ctx, n->u.partial.attrs[i].name, v);
            lq_release(v);
        }
        if (!ctx->has_err) {
            if (variable.type == LQ_ARR) {
                for (size_t i = 0; i < variable.u.a->len; i++) {
                    ctx_set_local(ctx, varname.u.s, variable.u.a->items[i]);
                    render_partial_body(ctx, pe->tpl, out);
                    if (ctx->has_err) break;
                }
            } else {
                ctx_set_local(ctx, varname.u.s, variable);
                render_partial_body(ctx, pe->tpl, out);
            }
        }
    }
    ctx_pop_scope(ctx);
    ctx->template_name = old_name;
    ctx->partial = old_partial;
    lq_release(variable);
    lq_release(varname);
    lq_release(tname);
}

static void render_one_isolated(lq_ctx *ctx, const lq_node *n, const lq_partial_entry *pe, lq_str *varname,
                                lq_value var, lq_value forloop, lq_buf *out) {
    if (ctx->base_depth + (int)ctx->nscopes > MAX_SCOPE_DEPTH) {
        lq_raise(ctx, LQ_ERR_STACK, "Nesting too deep");
        return;
    }
    lq_ctx inner;
    ctx_init(&inner, ctx->sh, ctx->env, ctx->base_depth + 1);
    inner.template_name = pe->name;
    inner.partial = true;
    inner.include_disabled = ctx->include_disabled;
    inner.capture_depth = ctx->capture_depth;
    if (forloop.type != LQ_NIL) ctx_set_local_cstr(&inner, "forloop", forloop);
    for (size_t i = 0; i < n->u.partial.nattrs; i++) {
        lq_value v = lq_eval(ctx, n->u.partial.attrs[i].expr);
        if (ctx->has_err) {
            lq_release(v);
            ctx_destroy(&inner);
            return;
        }
        ctx_set_local(&inner, n->u.partial.attrs[i].name, v);
        lq_release(v);
    }
    if (var.type != LQ_NIL) ctx_set_local(&inner, varname, var);
    render_partial_body(&inner, pe->tpl, out);
    if (inner.has_err) raise_copy(ctx, &inner.err);
    ctx_destroy(&inner);
}

static void render_render(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    lq_value tname = n->u.partial.tname->lit;
    if (tname.type != LQ_STR) return;
    const lq_partial_entry *pe = load_partial(ctx, tname.u.s);
    if (!pe->tpl) {
        raise_copy(ctx, &pe->err);
        return;
    }
    size_t vlen;
    const char *vname = n->u.partial.alias ? n->u.partial.alias->data : basename_of(tname.u.s, &vlen);
    if (n->u.partial.alias) vlen = n->u.partial.alias->len;
    lq_value varname = lq_str_new(vname, vlen);
    lq_value variable = lq_nil();
    if (n->u.partial.var) {
        variable = lq_eval(ctx, n->u.partial.var);
        if (ctx->has_err) { lq_release(variable); lq_release(varname); return; }
    }
    ctx->include_disabled++;
    bool iterable = variable.type == LQ_ARR || variable.type == LQ_HASH || variable.type == LQ_RANGE ||
                    (variable.type == LQ_DROP && variable.u.d->vt->iterate);
    if (n->u.partial.is_for && iterable) {
        seq s;
        if (slice_collection(ctx, variable, 0, false, 0, &s)) {
            lq_value loop = lq_forloop_new(tname.u.s, s.len, lq_nil());
            for (int64_t i = 0; i < s.len; i++) {
                lq_value item = seq_at(&s, i);
                render_one_isolated(ctx, n, pe, varname.u.s, item, loop, out);
                lq_release(item);
                lq_forloop_increment(loop);
                if (ctx->has_err) break;
                if (!tick(ctx, 1)) break;
            }
            lq_release(loop);
            seq_free(&s);
        }
    } else {
        render_one_isolated(ctx, n, pe, varname.u.s, variable, lq_nil(), out);
    }
    ctx->include_disabled--;
    lq_release(variable);
    lq_release(varname);
}

static void render_node(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    switch (n->kind) {
    case N_TEXT:
    case N_RAW:
        render_text(out, n->u.text.s, n->u.text.len);
        break;
    case N_VAR:
    case N_ECHO: {
        lq_value v = lq_render_variable(ctx, n->u.var);
        if (!ctx->has_err) lq_output(out, v, ctx);
        lq_release(v);
        break;
    }
    case N_IF:
    case N_UNLESS:
        render_if(ctx, n, out);
        break;
    case N_CASE:
        render_case(ctx, n, out);
        break;
    case N_FOR:
        render_for(ctx, n, out);
        break;
    case N_TABLEROW:
        render_tablerow(ctx, n, out);
        break;
    case N_ASSIGN: {
        lq_value v = lq_render_variable(ctx, n->u.assign.from);
        if (ctx->has_err) {
            lq_release(v);
            break;
        }
        lq_hash_set_take(ctx->scopes[0], n->u.assign.to, v);
        add_assign_score(ctx, assign_score_of(v, 0));
        break;
    }
    case N_CAPTURE: {
        lq_buf tmp;
        lq_buf_init(&tmp);
        ctx->capture_depth++;
        render_body(ctx, n->u.capture.body, &tmp);
        ctx->capture_depth--;
        if (ctx->has_err) {
            lq_buf_free(&tmp);
            break;
        }
        add_assign_score(ctx, (int64_t)tmp.len);
        tick(ctx, (int64_t)(tmp.len >> 5));
        lq_hash_set_take(ctx->scopes[0], n->u.capture.to, lq_str_take_buf(&tmp));
        break;
    }
    case N_CYCLE:
        render_cycle(ctx, n, out);
        break;
    case N_INCREMENT:
        render_counter(ctx, n, out, true);
        break;
    case N_DECREMENT:
        render_counter(ctx, n, out, false);
        break;
    case N_NOOP:
        break;
    case N_BREAK:
        push_interrupt(ctx, INTR_BREAK);
        break;
    case N_CONTINUE:
        push_interrupt(ctx, INTR_CONTINUE);
        break;
    case N_IFCHANGED:
        render_ifchanged(ctx, n, out);
        break;
    case N_INCLUDE:
        render_include(ctx, n, out);
        break;
    case N_RENDER:
        render_render(ctx, n, out);
        break;
    }
}

/* BlockBody.render_node error handling */
static void handle_node_error(lq_ctx *ctx, const lq_node *n, lq_buf *out) {
    if (ctx->err.kind == LQ_ERR_MEMORY) return; /* re-raised as-is */
    if (!ctx->err.template_name && ctx->template_name)
        ctx->err.template_name = lq_xstrndup(ctx->template_name, strlen(ctx->template_name));
    if (!ctx->err.has_line && ctx->sh->line_numbers) {
        ctx->err.line = n->line;
        ctx->err.has_line = true;
    }
    if (ctx->sh->strict) return; /* propagate */
    bool blank_tag = n->kind != N_VAR && n->blank;
    ctx->sh->nerrors++;
    if (!blank_tag) {
        char *msg = lq_error_format(&ctx->err);
        lq_buf_puts(out, msg);
        free(msg);
    }
    lq_error_info_clear(&ctx->err);
    ctx->has_err = false;
}

static void render_body_inner(lq_ctx *ctx, const lq_body *b, lq_buf *out);

/* Bounds total render recursion (nested blocks across nested partials) so
   hostile templates cannot exhaust the C stack. */
static void render_body(lq_ctx *ctx, const lq_body *b, lq_buf *out) {
    lq_shared *sh = ctx->sh;
    if (sh->depth >= LQ_MAX_RENDER_DEPTH) {
        lq_raise(ctx, LQ_ERR_STACK, "Nesting too deep");
        return;
    }
    sh->depth++;
    render_body_inner(ctx, b, out);
    sh->depth--;
}

static void render_body_inner(lq_ctx *ctx, const lq_body *b, lq_buf *out) {
    if (!add_render_score(ctx, (int64_t)b->n)) return;
    for (size_t i = 0; i < b->n; i++) {
        const lq_node *n = b->nodes[i];
        if (n->kind == N_TEXT) {
            render_text(out, n->u.text.s, n->u.text.len);
        } else {
            render_node(ctx, n, out);
            if (ctx->has_err) {
                handle_node_error(ctx, n, out);
                if (ctx->has_err) return;
            }
            if (ctx->nintr) break;
        }
        if (!check_output(ctx, out)) return;
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int lq_render_template(const lq_template *t, const lq_hash *env, const liquid_render_options *opts,
                       lq_buf *out, lq_error_info *err) {
    lq_shared sh;
    memset(&sh, 0, sizeof sh);
    sh.opts = opts;
    sh.strict = opts->strict_errors != 0;
    sh.line_numbers = true;
    sh.max_steps = opts->max_steps > 0 ? opts->max_steps : LQ_DEFAULT_MAX_STEPS;
    sh.max_output = opts->max_output_bytes > 0 ? opts->max_output_bytes : LQ_DEFAULT_MAX_OUTPUT;
    if (opts->file_system) {
        const lq_value *fsv = (const lq_value *)opts->file_system;
        if (fsv->type == LQ_HASH) sh.fs = fsv->u.h;
    }
    sh.fs_index = lq_nil();
    sh.partial_index = lq_nil();
    sh.has_now = opts->has_current_time != 0;
    sh.now = opts->current_time;
    sh.now_off = opts->current_time_offset;

    lq_ctx ctx;
    ctx_init(&ctx, &sh, env, 0);
    render_body(&ctx, t->root, out);
    int rc = 0;
    if (ctx.has_err) {
        if (ctx.err.kind == LQ_ERR_MEMORY && !sh.strict) {
            /* Template#render rescues MemoryError: the output is replaced by
               the error message */
            char *msg = lq_error_format(&ctx.err);
            out->len = 0;
            lq_buf_puts(out, msg);
            free(msg);
        } else {
            lq_error_info_clear(err);
            *err = ctx.err;
            memset(&ctx.err, 0, sizeof ctx.err);
            rc = 1;
        }
    }
    ctx_destroy(&ctx);
    /* containers may reference values owned (pinned) by partial templates:
       release them before the templates */
    lq_release(sh.fs_index);
    lq_release(sh.partial_index);
    for (size_t i = 0; i < sh.npartials; i++) {
        free(sh.partials[i].name);
        lq_template_free(sh.partials[i].tpl);
        lq_error_info_clear(&sh.partials[i].err);
    }
    free(sh.partials);
    return rc;
}
