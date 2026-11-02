/*
 * drops.c - built-in drops: forloop, tablerowloop and the portable standard
 * test drops (BooleanDrop, NumberDrop, ...), plus host-backed drops.
 */
#include "lq.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>

static bool key_is(lq_value key, const char *s) {
    size_t n = strlen(s);
    return key.type == LQ_STR && key.u.s->len == n && memcmp(key.u.s->data, s, n) == 0;
}

/* ------------------------------------------------------------------ */
/* forloop                                                             */
/* ------------------------------------------------------------------ */

typedef struct forloop {
    lq_str *name;
    int64_t length;
    int64_t index;
    lq_value parent;
} forloop;

static lq_value forloop_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    forloop *f = d->data;
    if (key_is(key, "length")) return lq_int(f->length);
    if (key_is(key, "index")) return lq_int(f->index + 1);
    if (key_is(key, "index0")) return lq_int(f->index);
    if (key_is(key, "rindex")) return lq_int(f->length - f->index);
    if (key_is(key, "rindex0")) return lq_int(f->length - f->index - 1);
    if (key_is(key, "first")) return lq_bool(f->index == 0);
    if (key_is(key, "last")) return lq_bool(f->index == f->length - 1);
    if (key_is(key, "parentloop")) return lq_retain(f->parent);
    if (key_is(key, "name")) return lq_retain(lq_str_val(f->name));
    if (key_is(key, "to_liquid")) {
        lq_value v;
        v.type = LQ_DROP;
        v.u.d = d;
        return lq_retain(v);
    }
    return lq_nil();
}

static void forloop_free(lq_drop *d) {
    forloop *f = d->data;
    lq_release(lq_str_val(f->name));
    lq_release(f->parent);
    free(f);
}

static const lq_drop_vt forloop_vt = {"Liquid::ForloopDrop", forloop_get, NULL, NULL, NULL, NULL, forloop_free, NULL};

lq_value lq_forloop_new(lq_str *name, int64_t length, lq_value parent) {
    forloop *f = lq_xcalloc(1, sizeof(forloop));
    f->name = lq_retain(lq_str_val(name)).u.s;
    f->length = length;
    f->parent = lq_retain(parent);
    return lq_drop_new(&forloop_vt, f);
}

void lq_forloop_increment(lq_value drop) {
    if (drop.type == LQ_DROP && drop.u.d->vt == &forloop_vt) ((forloop *)drop.u.d->data)->index++;
}

/* ------------------------------------------------------------------ */
/* tablerowloop                                                        */
/* ------------------------------------------------------------------ */

typedef struct trloop {
    int64_t length, row, col, cols, index;
} trloop;

static lq_value trloop_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    trloop *t = d->data;
    if (key_is(key, "length")) return lq_int(t->length);
    if (key_is(key, "col")) return lq_int(t->col);
    if (key_is(key, "row")) return lq_int(t->row);
    if (key_is(key, "index")) return lq_int(t->index + 1);
    if (key_is(key, "index0")) return lq_int(t->index);
    if (key_is(key, "col0")) return lq_int(t->col - 1);
    if (key_is(key, "rindex")) return lq_int(t->length - t->index);
    if (key_is(key, "rindex0")) return lq_int(t->length - t->index - 1);
    if (key_is(key, "first")) return lq_bool(t->index == 0);
    if (key_is(key, "last")) return lq_bool(t->index == t->length - 1);
    if (key_is(key, "col_first")) return lq_bool(t->col == 1);
    if (key_is(key, "col_last")) return lq_bool(t->col == t->cols);
    if (key_is(key, "to_liquid")) {
        lq_value v;
        v.type = LQ_DROP;
        v.u.d = d;
        return lq_retain(v);
    }
    return lq_nil();
}

static void trloop_free(lq_drop *d) { free(d->data); }

static const lq_drop_vt trloop_vt = {"Liquid::TablerowloopDrop", trloop_get, NULL, NULL, NULL, NULL, trloop_free, NULL};

lq_value lq_tablerowloop_new(int64_t length, int64_t cols) {
    trloop *t = lq_xcalloc(1, sizeof(trloop));
    t->length = length;
    t->row = 1;
    t->col = 1;
    t->cols = cols;
    return lq_drop_new(&trloop_vt, t);
}

static trloop *TR(lq_value v) { return (trloop *)v.u.d->data; }

void lq_tablerowloop_increment(lq_value drop) {
    trloop *t = TR(drop);
    t->index++;
    if (t->col == t->cols) {
        t->col = 1;
        t->row++;
    } else {
        t->col++;
    }
}

int64_t lq_tablerowloop_col(lq_value drop) { return TR(drop)->col; }
int64_t lq_tablerowloop_row(lq_value drop) { return TR(drop)->row; }
bool lq_tablerowloop_col_last(lq_value drop) { return TR(drop)->col == TR(drop)->cols; }
bool lq_tablerowloop_last(lq_value drop) { return TR(drop)->index == TR(drop)->length - 1; }

/* ------------------------------------------------------------------ */
/* Standard test drops                                                 */
/* ------------------------------------------------------------------ */

typedef struct valdrop {
    lq_value value;
} valdrop;

static void valdrop_free(lq_drop *d) {
    valdrop *v = d->data;
    lq_release(v->value);
    free(v);
}

static lq_value self_ref(lq_drop *d) {
    lq_value v;
    v.type = LQ_DROP;
    v.u.d = d;
    return lq_retain(v);
}

static lq_value valdrop_child(lq_drop *d) { return ((valdrop *)d->data)->value; }

static lq_value value_tlv(lq_drop *d, lq_ctx *ctx, bool *has) {
    (void)ctx;
    *has = true;
    return lq_retain(((valdrop *)d->data)->value);
}

static lq_value value_to_s(lq_drop *d, lq_ctx *ctx) {
    return lq_to_str(((valdrop *)d->data)->value, ctx);
}

static lq_value bool_to_s(lq_drop *d, lq_ctx *ctx) {
    (void)ctx;
    return lq_str_cstr(lq_truthy(((valdrop *)d->data)->value) ? "true" : "false");
}

static lq_value bool_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    if (key_is(key, "to_liquid_value")) return lq_retain(((valdrop *)d->data)->value);
    if (key_is(key, "to_liquid")) return self_ref(d);
    return lq_nil();
}

static lq_value number_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    if (key_is(key, "to_liquid_value") || key_is(key, "to_number")) return lq_retain(((valdrop *)d->data)->value);
    if (key_is(key, "to_liquid")) return self_ref(d);
    return lq_nil();
}

static lq_value number_to_number(lq_drop *d, lq_ctx *ctx, bool *has) {
    (void)ctx;
    *has = true;
    return lq_retain(((valdrop *)d->data)->value);
}

static const lq_drop_vt boolean_vt = {"StandardBooleanDrop", bool_get, value_tlv, bool_to_s, NULL, NULL, valdrop_free, valdrop_child};
static const lq_drop_vt number_vt = {"StandardNumberDrop", number_get, value_tlv, value_to_s, NULL, number_to_number, valdrop_free, valdrop_child};
static const lq_drop_vt string_vt = {"StandardStringDrop", bool_get, value_tlv, value_to_s, NULL, NULL, valdrop_free, valdrop_child};

/* MethodDrop: echo_N / square_N / double_N */
static lq_value method_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    if (key.type == LQ_STR) {
        const char *s = key.u.s->data;
        size_t n = key.u.s->len;
        int op = -1;
        size_t pl = 0;
        if (n > 5 && memcmp(s, "echo_", 5) == 0) { op = 0; pl = 5; }
        else if (n > 7 && memcmp(s, "square_", 7) == 0) { op = 1; pl = 7; }
        else if (n > 7 && memcmp(s, "double_", 7) == 0) { op = 2; pl = 7; }
        if (op >= 0) {
            bool digits = true;
            for (size_t i = pl; i < n; i++)
                if (s[i] < '0' || s[i] > '9') digits = false;
            if (digits) {
                lq_value nv = lq_parse_int_text(s + pl, n - pl);
                lq_value r;
                if (op == 0) r = lq_retain(nv);
                else if (op == 1) r = lq_int_mul(nv, nv);
                else r = lq_int_mul(nv, lq_int(2));
                lq_release(nv);
                lq_value str = lq_to_str(r, ctx);
                lq_release(r);
                return str;
            }
        }
        if (key_is(key, "to_liquid")) return self_ref(d);
    }
    return lq_nil();
}

static const lq_drop_vt method_vt = {"StandardMethodDrop", method_get, NULL, NULL, NULL, NULL, NULL, NULL};

static const char *NUMBER_WORDS[] = {"zero", "one", "two", "three", "four", "five"};

static lq_value index_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)d;
    (void)ctx;
    if (key.type == LQ_INT) {
        if (key.u.i >= 0 && key.u.i < 6) return lq_str_cstr(NUMBER_WORDS[key.u.i]);
        if (key.u.i < 0 && key.u.i >= -6) return lq_str_cstr(NUMBER_WORDS[6 + key.u.i]);
        return lq_str_cstr("unknown");
    }
    if (key.type == LQ_STR) return lq_retain(key);
    return lq_str_cstr("unknown");
}

static const lq_drop_vt index_vt = {"StandardIndexDrop", index_get, NULL, NULL, NULL, NULL, NULL, NULL};

static lq_value seq_items(void) {
    lq_value a = lq_arr_new(3);
    lq_arr_push_take(a.u.a, lq_str_cstr("first"));
    lq_arr_push_take(a.u.a, lq_str_cstr("second"));
    lq_arr_push_take(a.u.a, lq_str_cstr("third"));
    return a;
}

static lq_value seq_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    if (key_is(key, "size") || key_is(key, "count")) return lq_int(3);
    if (key_is(key, "first") || key_is(key, "min")) return lq_str_cstr("first");
    if (key_is(key, "last") || key_is(key, "max")) return lq_str_cstr("third");
    if (key_is(key, "sort")) return seq_items();
    if (key_is(key, "to_liquid")) return self_ref(d);
    return lq_nil();
}

static lq_value seq_iterate(lq_drop *d, lq_ctx *ctx) {
    (void)d;
    (void)ctx;
    return seq_items();
}

static const lq_drop_vt sequence_vt = {"StandardSequenceDrop", seq_get, NULL, NULL, seq_iterate, NULL, NULL, NULL};

static lq_value plain_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    if (key_is(key, "to_liquid")) return self_ref(d);
    return lq_nil();
}

static const lq_drop_vt nil_vt = {"StandardNilDrop", plain_get, NULL, NULL, NULL, NULL, NULL, NULL};

static lq_value opaque_to_s(lq_drop *d, lq_ctx *ctx) {
    (void)d;
    (void)ctx;
    return lq_str_cstr("opaque");
}

static const lq_drop_vt opaque_vt = {"StandardOpaqueDrop", plain_get, NULL, opaque_to_s, NULL, NULL, NULL, NULL};

static lq_value error_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)d;
    (void)key;
    lq_raise(ctx, LQ_ERR_STANDARD, "ErrorDrop: access triggered an error");
    return lq_nil();
}

static const lq_drop_vt error_vt = {"StandardErrorDrop", error_get, NULL, NULL, NULL, NULL, NULL, NULL};

static lq_value nested_new(lq_value v);

static lq_value nested_get(lq_drop *d, lq_value key, lq_ctx *ctx) {
    (void)ctx;
    valdrop *v = d->data;
    if (key_is(key, "to_liquid_value")) return lq_retain(v->value);
    if (key_is(key, "nested") || key_is(key, "to_liquid")) return self_ref(d);
    if (key_is(key, "square")) {
        lq_value sq = (v->value.type == LQ_INT || v->value.type == LQ_BIGINT) ? lq_int_mul(v->value, v->value)
                      : v->value.type == LQ_FLOAT ? lq_float(v->value.u.f * v->value.u.f)
                                                  : lq_nil();
        lq_value r = nested_new(sq);
        lq_release(sq);
        return r;
    }
    return lq_nil();
}

static const lq_drop_vt nestedvt = {"StandardNestedDrop", nested_get, value_tlv, value_to_s, NULL, NULL, valdrop_free, valdrop_child};

static lq_value valdrop_new(const lq_drop_vt *vt, lq_value v) {
    valdrop *vd = lq_xcalloc(1, sizeof(valdrop));
    vd->value = lq_retain(v);
    return lq_drop_new(vt, vd);
}

static lq_value nested_new(lq_value v) { return valdrop_new(&nestedvt, v); }

lq_value lq_make_builtin_drop(const char *name, lq_value params, bool *ok) {
    *ok = true;
    lq_value value = lq_nil();
    bool have = false;
    if (params.type == LQ_HASH) {
        lq_value v;
        if (lq_hash_get_cstr(params.u.h, "value", &v)) {
            value = v;
            have = true;
        }
    }
    /* Ruby `params["value"] || default`: nil and false fall back */
    bool falsy = !have || !lq_truthy(value);
    if (strcmp(name, "BooleanDrop") == 0) return valdrop_new(&boolean_vt, falsy ? lq_bool(false) : value);
    if (strcmp(name, "NumberDrop") == 0) return valdrop_new(&number_vt, falsy ? lq_int(0) : value);
    if (strcmp(name, "StringDrop") == 0) return valdrop_new(&string_vt, falsy ? lq_nil() : value);
    if (strcmp(name, "NestedDrop") == 0) return valdrop_new(&nestedvt, falsy ? lq_int(3) : value);
    if (strcmp(name, "MethodDrop") == 0) return lq_drop_new(&method_vt, NULL);
    if (strcmp(name, "IndexDrop") == 0) return lq_drop_new(&index_vt, NULL);
    if (strcmp(name, "SequenceDrop") == 0) return lq_drop_new(&sequence_vt, NULL);
    if (strcmp(name, "NilDrop") == 0) return lq_drop_new(&nil_vt, NULL);
    if (strcmp(name, "OpaqueDrop") == 0) return lq_drop_new(&opaque_vt, NULL);
    if (strcmp(name, "ErrorDrop") == 0) return lq_drop_new(&error_vt, NULL);
    *ok = false;
    return lq_nil();
}
