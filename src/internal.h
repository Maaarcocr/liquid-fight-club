/*
 * internal.h - AST, parser and renderer internals.
 */
#ifndef LQ_INTERNAL_H
#define LQ_INTERNAL_H

#include "lq.h"
#include <setjmp.h>

typedef struct lq_ctx lq_ctx;

/* ------------------------------------------------------------------ */
/* Big magnitudes and decimals (num.c)                                 */
/* ------------------------------------------------------------------ */

typedef struct mag {
    uint32_t *d;
    size_t n, cap;
} mag;

void lq_mag_init(mag *m);
void lq_mag_free(mag *m);
void lq_mag_set_u64(mag *m, uint64_t v);
void lq_mag_copy(mag *dst, const mag *src);
int lq_mag_cmp(const mag *a, const mag *b);
void lq_mag_divmod(mag *q, mag *r, const mag *a, const mag *b);
void lq_mag_to_dec(lq_buf *b, const mag *m);

enum { DEC_FINITE = 0, DEC_INF = 1, DEC_NAN = 2 };

typedef struct dec {
    int special;
    int neg;
    mag m;
    int64_t e;
} dec;

void lq_dec_init(dec *d);
void lq_dec_free(dec *d);
void lq_dec_from_int(dec *d, lq_value v);
void lq_dec_from_text(dec *d, const char *s, size_t n);
void lq_dec_from_double(dec *d, double f);
double lq_dec_to_double(const dec *d);
void lq_dec_add(dec *r, const dec *a, const dec *b, bool sub);
void lq_dec_mul(dec *r, const dec *a, const dec *b);
void lq_dec_div(dec *r, const dec *a, const dec *b);
bool lq_dec_mod(dec *r, const dec *a, const dec *b);
int lq_dec_cmp(const dec *a, const dec *b);
void lq_dec_round(dec *r, const dec *a, int64_t digits, int mode);
lq_value lq_dec_to_int(const dec *d);

lq_value lq_int_add(lq_value a, lq_value b, bool sub);
lq_value lq_int_mul(lq_value a, lq_value b);
void lq_int_divmod(lq_value a, lq_value b, lq_value *q, lq_value *r);
lq_value lq_pow2(int64_t k);

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct lq_arena_chunk {
    struct lq_arena_chunk *next;
    size_t used, cap;
    max_align_t data[];
} lq_arena_chunk;

typedef struct lq_arena {
    lq_arena_chunk *head;
    lq_value *vals; /* values released when the arena is freed */
    size_t nvals, capvals;
} lq_arena;

void lq_arena_init(lq_arena *a);
void lq_arena_free(lq_arena *a);
void *lq_arena_alloc(lq_arena *a, size_t n); /* zeroed */
lq_value lq_arena_keep(lq_arena *a, lq_value v); /* takes ownership */
char *lq_arena_strndup(lq_arena *a, const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

typedef enum lq_expr_kind { EX_LIT, EX_VAR, EX_RANGE } lq_expr_kind;

typedef struct lq_lookup {
    struct lq_expr *key;
    bool command; /* dotted size/first/last */
} lq_lookup;

typedef struct lq_expr {
    lq_expr_kind kind;
    lq_value lit;              /* EX_LIT (owned by arena) */
    struct lq_expr *name_expr; /* EX_VAR: root name expression */
    size_t nlook;
    lq_lookup *look;
    struct lq_expr *rstart, *rend; /* EX_RANGE */
} lq_expr;

struct lq_filter_def;

typedef struct lq_filter {
    const struct lq_filter_def *def; /* NULL: unknown filter (no-op) */
    size_t nargs;
    lq_expr **args;
    size_t nkw;
    lq_str **kwnames;
    lq_expr **kwargs;
} lq_filter;

typedef struct lq_variable {
    lq_expr *expr; /* NULL for empty {{ }} */
    size_t nfilters;
    lq_filter *filters;
    int line;
} lq_variable;

enum { OP_NONE = 0, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_CONTAINS };
enum { REL_NONE = 0, REL_AND, REL_OR };

typedef struct lq_cond {
    lq_expr *left;
    int op;
    lq_expr *right;
    int rel;
    struct lq_cond *child;
} lq_cond;

typedef enum lq_node_kind {
    N_TEXT,
    N_VAR,
    N_IF,
    N_UNLESS,
    N_CASE,
    N_FOR,
    N_TABLEROW,
    N_ASSIGN,
    N_CAPTURE,
    N_CYCLE,
    N_INCREMENT,
    N_DECREMENT,
    N_ECHO,
    N_RAW,
    N_NOOP, /* comment, doc, inline comment */
    N_BREAK,
    N_CONTINUE,
    N_IFCHANGED,
    N_INCLUDE,
    N_RENDER
} lq_node_kind;

typedef struct lq_node lq_node;

typedef struct lq_body {
    size_t n, cap;
    lq_node **nodes;
    bool blank;
} lq_body;

typedef struct lq_cblock {
    lq_cond *cond; /* NULL = else */
    lq_body *body;
} lq_cblock;

typedef struct lq_attr {
    lq_str *name;
    lq_expr *expr;
} lq_attr;

struct lq_node {
    lq_node_kind kind;
    int line;
    bool blank;
    union {
        struct { const char *s; size_t len; } text;       /* N_TEXT, N_RAW */
        lq_variable *var;                                  /* N_VAR, N_ECHO */
        struct { size_t n; lq_cblock *blocks; } cond;      /* N_IF, N_UNLESS, N_CASE */
        struct {
            lq_str *var;
            lq_expr *coll;
            lq_str *name; /* "var-collection" */
            bool reversed;
            bool offset_continue;
            lq_expr *limit, *offset;
            lq_body *body, *else_body;
        } loop;                                            /* N_FOR */
        struct {
            lq_str *var;
            lq_expr *coll;
            size_t nattrs;
            lq_attr *attrs;
            lq_body *body;
        } trow;                                            /* N_TABLEROW */
        struct { lq_str *to; lq_variable *from; } assign;  /* N_ASSIGN */
        struct { lq_str *to; lq_body *body; } capture;     /* N_CAPTURE */
        struct {
            lq_expr *name;   /* named cycle group expression, or NULL */
            lq_str *key;     /* register key for unnamed cycles */
            size_t n;
            lq_expr **vals;
        } cycle;                                           /* N_CYCLE */
        struct { lq_str *name; } counter;                  /* N_INCREMENT/N_DECREMENT */
        lq_body *body;                                     /* N_IFCHANGED */
        struct {
            lq_expr *tname;     /* include: expression; render: literal */
            lq_expr *var;       /* with/for expression or NULL */
            lq_str *alias;
            bool is_for;
            size_t nattrs;
            lq_attr *attrs;
        } partial;                                         /* N_INCLUDE, N_RENDER */
    } u;
};

struct lq_template {
    lq_arena arena;
    lq_body *root;
};

typedef struct lq_template lq_template;

/* Parse a template. Returns NULL with *err filled on failure. */
lq_template *lq_template_parse(const char *src, size_t len, bool line_numbers, lq_error_info *err);
void lq_template_free(lq_template *t);

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

typedef struct lq_partial_entry {
    char *name;
    lq_template *tpl; /* NULL if failed (error cached) */
    lq_error_info err;
} lq_partial_entry;

/* State shared by a render and all of its isolated subcontexts. */
typedef struct lq_shared {
    const liquid_render_options *opts;
    const lq_hash *fs;   /* file system data or NULL */
    bool strict;         /* raise render errors */
    bool line_numbers;
    /* resource accounting */
    int64_t render_score, assign_score;
    int64_t cum_render_score, cum_assign_score;
    int64_t steps;
    int64_t max_steps;
    int64_t max_output;
    size_t output_total; /* bytes written to the top-level output */
    /* partial cache */
    lq_partial_entry *partials;
    size_t npartials, cappartials;
    lq_value partial_index; /* hash: name -> index into partials */
    lq_value fs_index;      /* hash: normalized name -> source */
    int depth;              /* render_body recursion depth */
    /* current time */
    bool has_now;
    int64_t now;
    int32_t now_off;
    /* errors collected in inline mode (informational) */
    size_t nerrors;
} lq_shared;

typedef struct lq_registers {
    lq_value cycle;    /* hash: key -> int */
    lq_value for_offsets; /* hash: loop name -> int */
    lq_value ifchanged;   /* last ifchanged output string or nil */
    lq_value for_stack;   /* array of forloop drops */
} lq_registers;

struct lq_ctx {
    lq_shared *sh;
    lq_hash **scopes;   /* scopes[0] is the outermost (assign target) */
    size_t nscopes, capscopes;
    const lq_hash *env;     /* render environment (immutable) */
    lq_value counters;      /* hash overlaying env for increment/decrement */
    int base_depth;
    lq_registers reg;
    /* error state */
    lq_error_info err;
    bool has_err;
    /* interrupts */
    uint8_t *intr;
    size_t nintr, capintr;
    const char *template_name;
    bool partial;
    int include_disabled; /* >0 inside render */
    int capture_depth;
};

enum { INTR_BREAK = 1, INTR_CONTINUE = 2 };

/* raise a render error (first error wins) */
void lq_raise(lq_ctx *ctx, lq_err_kind kind, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
static inline bool lq_failed(const lq_ctx *ctx) { return ctx->has_err; }

lq_value lq_eval(lq_ctx *ctx, const lq_expr *e);
lq_value lq_find_variable(lq_ctx *ctx, lq_value key);
/* obj[key] with Liquid lookup semantics (VariableLookup step) */
lq_value lq_lookup_step(lq_ctx *ctx, lq_value obj, lq_value key, bool command);
lq_value lq_render_variable(lq_ctx *ctx, const lq_variable *v);

int lq_render_template(const lq_template *t, const lq_hash *env, const liquid_render_options *opts,
                       lq_buf *out, lq_error_info *err);

/* ------------------------------------------------------------------ */
/* Filters (filters.c)                                                 */
/* ------------------------------------------------------------------ */

typedef lq_value (*lq_filter_fn)(lq_ctx *ctx, lq_value input, lq_value *args, size_t nargs);

typedef struct lq_filter_def {
    const char *name;
    lq_filter_fn fn;
    int min_args; /* excluding input */
    int max_args; /* -1 = variadic */
} lq_filter_def;

const lq_filter_def *lq_filter_lookup(const char *name, size_t len);

/* number coercion used by math filters */
typedef struct lq_num {
    bool is_dec;
    lq_value i; /* INT/BIGINT when !is_dec */
    dec d;
} lq_num;
void lq_to_number(lq_ctx *ctx, lq_value v, lq_num *out);
void lq_num_free(lq_num *n);

/* ------------------------------------------------------------------ */
/* Dates (date.c)                                                      */
/* ------------------------------------------------------------------ */

typedef struct lq_time {
    int64_t sec;   /* seconds since epoch (UTC), plus day_offset days */
    int64_t day_offset; /* whole days beyond `sec` (huge Unix timestamps) */
    int64_t nsec;  /* fractional nanoseconds */
    int32_t off;   /* UTC offset in seconds */
    bool utc;      /* zone is "UTC" */
} lq_time;

/* Utils.to_date: false if not convertible */
bool lq_to_date(lq_ctx *ctx, lq_value v, lq_time *t);
bool lq_strftime(lq_buf *b, const char *fmt, size_t flen, const lq_time *t, size_t max_len);
bool lq_parse_time_text(const char *s, size_t n, lq_time *t);

/* ------------------------------------------------------------------ */
/* Drops (drops.c) and JSON (json.c)                                   */
/* ------------------------------------------------------------------ */

lq_value lq_make_builtin_drop(const char *name, lq_value params, bool *ok);
lq_value lq_forloop_new(lq_str *name, int64_t length, lq_value parent);
void lq_forloop_increment(lq_value drop);
lq_value lq_tablerowloop_new(int64_t length, int64_t cols);
void lq_tablerowloop_increment(lq_value drop);
int64_t lq_tablerowloop_col(lq_value drop);
int64_t lq_tablerowloop_row(lq_value drop);
bool lq_tablerowloop_col_last(lq_value drop);
bool lq_tablerowloop_last(lq_value drop);

typedef struct lq_json_opts {
    const liquid_drop_host *host;
} lq_json_opts;

/* Parse JSON into a value. Returns false with message on error. */
bool lq_json_parse(const char *s, size_t n, const lq_json_opts *o, lq_value *out, char **errmsg);
void lq_json_write_str(lq_buf *b, const char *s, size_t n);
void lq_json_write_value(lq_buf *b, lq_value v, int depth);

/* public data handle */
struct liquid_data {
    lq_value v;
};
liquid_data *lq_data_wrap(lq_value v); /* retains v */

#endif
