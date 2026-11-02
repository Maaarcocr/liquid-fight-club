/*
 * lq.h - internal definitions shared by every part of the Liquid engine.
 *
 * Value model
 * -----------
 * Every Liquid value is an `lq_value`: a small tagged struct passed by
 * value. Scalars (nil, booleans, integers, floats) live inline; strings,
 * arrays, hashes, ranges, big integers and drops are reference-counted heap
 * objects.
 *
 * Ownership convention: a function that returns an lq_value returns a NEW
 * reference (the caller must lq_release it). Arguments are BORROWED unless
 * the function name says otherwise (e.g. lq_arr_push_take). Heap objects are
 * immutable once shared; containers are only mutated while being built.
 */
#ifndef LQ_H
#define LQ_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../include/liquid.h"

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

void *lq_xmalloc(size_t n);
void *lq_xcalloc(size_t n, size_t sz);
void *lq_xrealloc(void *p, size_t n);
char *lq_xstrndup(const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct lq_buf {
    char *p;
    size_t len, cap;
} lq_buf;

void lq_buf_init(lq_buf *b);
void lq_buf_free(lq_buf *b);
void lq_buf_reserve(lq_buf *b, size_t extra);
void lq_buf_put(lq_buf *b, const char *s, size_t n);
void lq_buf_putc(lq_buf *b, char c);
void lq_buf_puts(lq_buf *b, const char *s);
void lq_buf_printf(lq_buf *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Detach the contents as a NUL-terminated malloc'd string. */
char *lq_buf_detach(lq_buf *b, size_t *len);

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

typedef enum lq_type {
    LQ_NIL = 0,
    LQ_BOOL,
    LQ_INT,
    LQ_FLOAT,
    LQ_STR,
    LQ_ARR,
    LQ_HASH,
    LQ_RANGE,
    LQ_BIGINT,
    LQ_DROP,
    LQ_EMPTY, /* the `empty` method literal (conditions only) */
    LQ_BLANK  /* the `blank` method literal (conditions only) */
} lq_type;

typedef struct lq_obj {
    size_t rc; /* reference count; LQ_RC_STATIC means immortal */
} lq_obj;

#define LQ_RC_STATIC ((size_t)1 << (sizeof(size_t) * 8 - 2))
/* Pinned objects (parsed templates, render data) are never touched by
   retain/release, so rendering never writes to them. The real count is kept
   in the low bits and restored by lq_unpin. */
#define LQ_RC_PINNED ((size_t)1 << (sizeof(size_t) * 8 - 3))

typedef struct lq_str {
    lq_obj h;
    size_t len;
    uint32_t hash;   /* 0 = not computed yet */
    int32_t nchars;  /* cached UTF-8 char count, -1 = unknown */
    char *data;      /* NUL terminated; points into the same allocation */
} lq_str;

typedef struct lq_value {
    uint32_t type;
    union {
        bool b;
        int64_t i;
        double f;
        lq_obj *o;
        lq_str *s;
        struct lq_arr *a;
        struct lq_hash *h;
        struct lq_range *r;
        struct lq_bigint *bi;
        struct lq_drop *d;
    } u;
} lq_value;

typedef struct lq_arr {
    lq_obj h;
    size_t len, cap;
    lq_value *items;
} lq_arr;

typedef struct lq_hent {
    lq_str *key;
    lq_value val;
} lq_hent;

typedef struct lq_hash {
    lq_obj h;
    size_t len, cap;    /* entries */
    lq_hent *ents;
    uint32_t *index;    /* open addressing: entry index + 1, 0 = empty */
    size_t index_cap;   /* power of two, 0 = no index (linear scan) */
} lq_hash;

typedef struct lq_range {
    lq_obj h;
    int64_t first, last;
} lq_range;

/* Arbitrary precision integer (magnitude in base 2^32 limbs, little endian). */
typedef struct lq_bigint {
    lq_obj h;
    int neg;
    size_t n;
    uint32_t limb[];
} lq_bigint;

struct lq_ctx;

/* Drop protocol: objects whose properties are computed on access. */
typedef struct lq_drop_vt {
    const char *class_name;
    /* property / key lookup; returns a new reference, nil if absent */
    lq_value (*get)(struct lq_drop *d, lq_value key, struct lq_ctx *ctx);
    /* true if the drop implements to_liquid_value */
    lq_value (*to_liquid_value)(struct lq_drop *d, struct lq_ctx *ctx, bool *has);
    /* string form for output; NULL = class name */
    lq_value (*to_s)(struct lq_drop *d, struct lq_ctx *ctx);
    /* iterate: returns new array reference, or nil if not enumerable */
    lq_value (*iterate)(struct lq_drop *d, struct lq_ctx *ctx);
    /* to_number for numeric filters; has=false if not supported */
    lq_value (*to_number)(struct lq_drop *d, struct lq_ctx *ctx, bool *has);
    void (*free)(struct lq_drop *d);
    /* internal value owned by the drop (for pinning), or NULL */
    lq_value (*child)(struct lq_drop *d);
} lq_drop_vt;

typedef struct lq_drop {
    lq_obj h;
    const lq_drop_vt *vt;
    void *data;
} lq_drop;

/* constructors */
static inline lq_value lq_nil(void) { lq_value v; v.type = LQ_NIL; v.u.i = 0; return v; }
static inline lq_value lq_bool(bool b) { lq_value v; v.type = LQ_BOOL; v.u.i = 0; v.u.b = b; return v; }
static inline lq_value lq_int(int64_t i) { lq_value v; v.type = LQ_INT; v.u.i = i; return v; }
static inline lq_value lq_float(double f) { lq_value v; v.type = LQ_FLOAT; v.u.f = f; return v; }
static inline lq_value lq_special(lq_type t) { lq_value v; v.type = t; v.u.i = 0; return v; }

static inline bool lq_is_heap(lq_value v) {
    return v.type == LQ_STR || v.type == LQ_ARR || v.type == LQ_HASH ||
           v.type == LQ_RANGE || v.type == LQ_BIGINT || v.type == LQ_DROP;
}

void lq_obj_free(lq_value v);
/* Pin / unpin a value graph exclusively owned by the caller. */
void lq_pin(lq_value v);
void lq_unpin(lq_value v);

static inline lq_value lq_retain(lq_value v) {
    if (lq_is_heap(v) && v.u.o->rc < LQ_RC_PINNED) v.u.o->rc++;
    return v;
}
static inline void lq_release(lq_value v) {
    if (lq_is_heap(v) && v.u.o->rc < LQ_RC_PINNED) {
        if (--v.u.o->rc == 0) lq_obj_free(v);
    }
}

/* strings */
lq_value lq_str_new(const char *s, size_t n);
lq_value lq_str_cstr(const char *s);
lq_value lq_str_take_buf(lq_buf *b); /* consumes buffer contents */
lq_value lq_str_empty(void);
static inline lq_value lq_str_val(lq_str *s) { lq_value v; v.type = LQ_STR; v.u.s = s; return v; }
uint32_t lq_hash_bytes(const char *s, size_t n);
static inline uint32_t lq_str_hash(lq_str *s) {
    if (!s->hash) {
        uint32_t h = lq_hash_bytes(s->data, s->len);
        if (s->h.rc >= LQ_RC_STATIC) return h; /* never write immortal objects */
        s->hash = h;
    }
    return s->hash;
}
size_t lq_str_nchars(lq_str *s);

/* arrays */
lq_value lq_arr_new(size_t cap);
static inline lq_arr *lq_arr_of(lq_value v) { return v.u.a; }
void lq_arr_push(lq_arr *a, lq_value v);      /* retains v */
void lq_arr_push_take(lq_arr *a, lq_value v); /* steals v */

/* hashes */
lq_value lq_hash_new(size_t cap);
/* borrowed lookup; returns true when found */
bool lq_hash_get(const lq_hash *h, const char *k, size_t klen, uint32_t hash, lq_value *out);
bool lq_hash_get_str(const lq_hash *h, lq_str *k, lq_value *out);
bool lq_hash_get_cstr(const lq_hash *h, const char *k, lq_value *out);
/* insert or overwrite (keeps original position). Retains key and value. */
void lq_hash_set(lq_hash *h, lq_str *key, lq_value v);
void lq_hash_set_take(lq_hash *h, lq_str *key, lq_value v); /* steals v */
void lq_hash_set_cstr(lq_hash *h, const char *key, lq_value v);

/* ranges */
lq_value lq_range_new(int64_t a, int64_t b);

/* drops */
lq_value lq_drop_new(const lq_drop_vt *vt, void *data);

/* ------------------------------------------------------------------ */
/* Value semantics (value.c)                                           */
/* ------------------------------------------------------------------ */

static inline bool lq_truthy(lq_value v) {
    return !(v.type == LQ_NIL || (v.type == LQ_BOOL && !v.u.b));
}
/* Append Liquid's Utils.to_s form (strings raw, arrays/hashes inspected). */
void lq_to_s(lq_buf *b, lq_value v, struct lq_ctx *ctx);
/* New string value of Utils.to_s(v). Returns v retained if already string. */
lq_value lq_to_str(lq_value v, struct lq_ctx *ctx);
/* Append the Ruby #inspect form. */
void lq_inspect(lq_buf *b, lq_value v, struct lq_ctx *ctx);
void lq_inspect_str(lq_buf *b, const char *s, size_t n);
/* Output form used by {{ }}: arrays flattened, nil = nothing. */
void lq_output(lq_buf *b, lq_value v, struct lq_ctx *ctx);
/* Ruby == */
bool lq_equal(lq_value a, lq_value b);
/* to_liquid_value: materialize drops that implement it. new reference */
lq_value lq_liquid_value(lq_value v, struct lq_ctx *ctx);
bool lq_is_blank(lq_value v);
bool lq_is_empty(lq_value v);
const char *lq_class_name(lq_value v);
/* Substring search with linear worst-case time */
typedef struct lq_finder {
    const char *pat;
    size_t n;
    size_t *fail; /* KMP table for long patterns */
} lq_finder;
void lq_finder_init(lq_finder *f, const char *pat, size_t n);
void lq_finder_free(lq_finder *f);
long lq_finder_next(const lq_finder *f, const char *h, size_t hn, size_t start);
long lq_finder_last(const lq_finder *f, const char *h, size_t hn);
long lq_memfind(const char *h, size_t hn, const char *nd, size_t nn);
uint64_t lq_value_hash(lq_value v, int depth);
/* Ruby String predicates */
bool lq_ruby_space(unsigned char c); /* strip whitespace: \0 \t \n \v \f \r space */
bool lq_regex_space(unsigned char c); /* \s : \t \n \v \f \r space */

/* ------------------------------------------------------------------ */
/* Numbers (num.c)                                                     */
/* ------------------------------------------------------------------ */

void lq_fmt_float(lq_buf *b, double d);
void lq_fmt_int(lq_buf *b, int64_t i);
/* Parse JSON/Liquid integer literal text; may produce a bigint. */
lq_value lq_parse_int_text(const char *s, size_t n);

/* Big integer helpers */
lq_value lq_bigint_normalize(lq_bigint *b); /* returns INT if it fits */
void lq_bigint_fmt(lq_buf *b, const lq_bigint *x);
double lq_bigint_to_double(const lq_bigint *x);
int lq_num_cmp(lq_value a, lq_value b); /* both numeric: INT/FLOAT/BIGINT */
bool lq_is_numeric(lq_value v);

/* Ruby String#to_i (base 10, leading whitespace, optional sign, underscores) */
lq_value lq_str_to_i(const char *s, size_t n);
/* Ruby Kernel#Integer(str): strict; returns false when invalid */
bool lq_ruby_Integer(const char *s, size_t n, lq_value *out);
/* Ruby String#to_f */
double lq_str_to_f(const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

typedef enum lq_err_kind {
    LQ_ERR_NONE = 0,
    LQ_ERR_SYNTAX,
    LQ_ERR_ARGUMENT,
    LQ_ERR_STACK,       /* StackLevelError: Nesting too deep */
    LQ_ERR_MEMORY,      /* MemoryError: Memory limits exceeded */
    LQ_ERR_FILESYSTEM,
    LQ_ERR_ZERODIV,
    LQ_ERR_FLOATDOMAIN,
    LQ_ERR_DISABLED,
    LQ_ERR_INTERNAL,
    LQ_ERR_STANDARD,    /* Liquid::StandardError / other runtime errors */
    LQ_ERR_ENCODING
} lq_err_kind;

typedef struct lq_error_info {
    lq_err_kind kind;
    char *msg;           /* core message (malloc'd) */
    char *markup_ctx;    /* e.g. in "{{ x }}" or NULL */
    char *template_name; /* partial name or NULL */
    int line;
    bool has_line;
} lq_error_info;

void lq_error_info_clear(lq_error_info *e);
/* Format "Liquid syntax error (line N): msg in \"...\"" */
char *lq_error_format(const lq_error_info *e);

/* ------------------------------------------------------------------ */
/* Misc helpers                                                        */
/* ------------------------------------------------------------------ */

/* UTF-8: length of the sequence starting at p (1 for invalid bytes). */
size_t lq_utf8_len(const unsigned char *p, size_t avail);
bool lq_utf8_valid(const char *s, size_t n);
/* decode one code point; returns bytes consumed (>=1); cp=-1 if invalid */
size_t lq_utf8_decode(const unsigned char *p, size_t avail, int32_t *cp);
size_t lq_utf8_encode(char *out, int32_t cp);

#endif
