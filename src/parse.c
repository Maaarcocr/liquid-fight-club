/*
 * parse.c - template tokenizer, strict2 expression parser and tag parsers.
 *
 * The structure deliberately mirrors the reference implementation so that
 * observable behavior (accepted syntax, error messages, line numbers,
 * whitespace control, blank-body handling) matches exactly:
 *
 *   tokenizer      -> raw text / {% tag %} / {{ variable }} tokens
 *   block body     -> nodes, dispatching tags; unknown tags end the body
 *   strict2 parser -> expression lexer + recursive descent; an expression is
 *                     first rendered back to markup text and then turned into
 *                     an AST with Expression.parse semantics
 *
 * Parse errors unwind with longjmp. Everything allocated while parsing lives
 * in the template arena (or the parser's scratch arena), so unwinding never
 * leaks.
 */
#include "lq.h"
#include "internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_BLOCK_DEPTH 100
#define MAX_EXPR_DEPTH 200

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

void lq_arena_init(lq_arena *a) { memset(a, 0, sizeof *a); }

void lq_arena_free(lq_arena *a) {
    lq_arena_chunk *c = a->head;
    while (c) {
        lq_arena_chunk *n = c->next;
        free(c);
        c = n;
    }
    for (size_t i = 0; i < a->nvals; i++) {
        lq_unpin(a->vals[i]);
        lq_release(a->vals[i]);
    }
    free(a->vals);
    memset(a, 0, sizeof *a);
}

void *lq_arena_alloc(lq_arena *a, size_t n) {
    n = (n + sizeof(max_align_t) - 1) / sizeof(max_align_t) * sizeof(max_align_t);
    if (!n) n = sizeof(max_align_t);
    lq_arena_chunk *c = a->head;
    if (!c || c->cap - c->used < n) {
        size_t cap = 8192;
        if (n > cap / 2) cap = n;
        c = lq_xmalloc(sizeof(lq_arena_chunk) + cap);
        c->cap = cap;
        c->used = 0;
        if (a->head && n == cap) {
            /* big block: keep the current head for small allocations */
            c->next = a->head->next;
            a->head->next = c;
        } else {
            c->next = a->head;
            a->head = c;
        }
    }
    void *p = (char *)c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

lq_value lq_arena_keep(lq_arena *a, lq_value v) {
    if (!lq_is_heap(v)) return v;
    if (a->nvals == a->capvals) {
        a->capvals = a->capvals ? a->capvals * 2 : 16;
        a->vals = lq_xrealloc(a->vals, a->capvals * sizeof(lq_value));
    }
    a->vals[a->nvals++] = v;
    return v;
}

char *lq_arena_strndup(lq_arena *a, const char *s, size_t n) {
    char *p = lq_arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

typedef struct tokenizer {
    const char *src;
    size_t len, pos;
    bool liquid; /* {% liquid %} line mode */
    int line;
} tokenizer;

typedef enum tok_type {
    T_EOS,
    T_ID,
    T_NUMBER,
    T_STRING,
    T_COMPARISON,
    T_PIPE,
    T_DOT,
    T_DOTDOT,
    T_COLON,
    T_COMMA,
    T_OPEN_SQ,
    T_CLOSE_SQ,
    T_OPEN_ROUND,
    T_CLOSE_ROUND,
    T_QUESTION,
    T_DASH
} tok_type;

static const char *tok_names[] = {"end_of_string", "id", "number", "string", "comparison",
                                  "pipe", "dot", "dotdot", "colon", "comma", "open_square",
                                  "close_square", "open_round", "close_round", "question", "dash"};

typedef struct etok {
    tok_type type;
    const char *s;
    size_t n;
} etok;

typedef struct P {
    lq_arena *A;      /* template arena */
    lq_arena tmp;     /* scratch (freed after parse) */
    lq_error_info *err;
    jmp_buf jb;
    int line;         /* parse_context.line_number */
    bool trim;        /* parse_context.trim_whitespace */
    int depth;        /* block nesting */
    bool line_numbers;
    /* markup error context for the tag being parsed */
    const char *mctx;
    int mline;
    bool in_markup;
    /* expression parser */
    etok *toks;
    size_t ntoks, captoks, tp;
    int edepth;
} P;

static void set_error(P *p, lq_err_kind kind, int line, const char *ctx, const char *msg, size_t mlen) {
    lq_error_info_clear(p->err);
    p->err->kind = kind;
    p->err->msg = lq_xstrndup(msg, mlen);
    p->err->markup_ctx = ctx ? lq_xstrndup(ctx, strlen(ctx)) : NULL;
    p->err->line = line;
    p->err->has_line = p->line_numbers && kind == LQ_ERR_SYNTAX;
}

/* Raise a syntax error. Inside tag markup parsing the tag's line and markup
   context are attached (strict2_parse_with_error_context). */
#if defined(__GNUC__)
__attribute__((noreturn, format(printf, 2, 3)))
#endif
static void perr(P *p, const char *fmt, ...) {
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
    if (p->in_markup) set_error(p, LQ_ERR_SYNTAX, p->mline, p->mctx, b.p ? b.p : "", b.len);
    else set_error(p, LQ_ERR_SYNTAX, p->line, NULL, b.p ? b.p : "", b.len);
    lq_buf_free(&b);
    longjmp(p->jb, 1);
}

/* error with an explicit message buffer (may contain any bytes) */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void perr_buf(P *p, lq_err_kind kind, lq_buf *b) {
    if (p->in_markup && kind == LQ_ERR_SYNTAX) set_error(p, kind, p->mline, p->mctx, b->p ? b->p : "", b->len);
    else set_error(p, kind, kind == LQ_ERR_SYNTAX ? p->line : 0, NULL, b->p ? b->p : "", b->len);
    lq_buf_free(b);
    longjmp(p->jb, 1);
}

/* ------------------------------------------------------------------ */
/* Small string helpers                                                */
/* ------------------------------------------------------------------ */

static bool is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool sv_eq(const char *s, size_t n, const char *lit) {
    size_t m = strlen(lit);
    return n == m && memcmp(s, lit, m) == 0;
}

static void rb_strip(const char **s, size_t *n) {
    while (*n && lq_ruby_space((unsigned char)(*s)[0])) {
        (*s)++;
        (*n)--;
    }
    while (*n && lq_ruby_space((unsigned char)(*s)[*n - 1])) (*n)--;
}

static bool all_regex_space(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!lq_regex_space((unsigned char)s[i])) return false;
    return true;
}

static int count_nl(const char *s, size_t n) {
    int c = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n' && c < INT_MAX) c++;
    return c;
}

/* saturating line arithmetic: huge templates must not overflow */
static int line_add(int line, int n) {
    if (n > 0 && line > INT_MAX - n) return INT_MAX;
    if (n < 0 && line < INT_MIN - n) return INT_MIN;
    return line + n;
}

static const char *find2(const char *s, size_t n, char a, char b) {
    if (n < 2) return NULL;
    for (size_t i = 0; i + 1 < n; i++) {
        const char *q = memchr(s + i, a, n - 1 - i);
        if (!q) return NULL;
        i = (size_t)(q - s);
        if (s[i + 1] == b) return s + i;
    }
    return NULL;
}

static lq_str *mkstr(P *p, const char *s, size_t n) {
    lq_value v = lq_arena_keep(p->A, lq_str_new(s, n));
    return v.u.s;
}

/* "in \"...\"" context string for a tag markup (stripped) */
static const char *markup_context(P *p, const char *m, size_t n, bool variable) {
    lq_buf b;
    lq_buf_init(&b);
    if (variable) {
        lq_buf_puts(&b, "in \"{{");
        lq_buf_put(&b, m, n);
        lq_buf_puts(&b, "}}\"");
    } else {
        rb_strip(&m, &n);
        lq_buf_puts(&b, "in \"");
        lq_buf_put(&b, m, n);
        lq_buf_putc(&b, '"');
    }
    char *r = lq_arena_strndup(&p->tmp, b.p, b.len);
    lq_buf_free(&b);
    return r;
}

/* ------------------------------------------------------------------ */
/* Template tokenizer                                                  */
/* ------------------------------------------------------------------ */

static const char LIT_VAR_START[] = "{{";
static const char LIT_TAG_START[] = "{%";

static void tok_init_doc(tokenizer *t, const char *s, size_t n, int line) {
    t->src = s;
    t->len = n;
    t->pos = 0;
    t->liquid = false;
    t->line = line;
}

static void tok_init_liquid(tokenizer *t, const char *s, size_t n, int line) {
    /* Ruby's String#split("\n") drops trailing empty fields. */
    while (n && s[n - 1] == '\n') n--;
    t->src = s;
    t->len = n;
    t->pos = 0;
    t->liquid = true;
    t->line = line;
}

static bool tok_shift(tokenizer *t, const char **out, size_t *olen) {
    if (t->liquid) {
        if (t->pos > t->len || t->len == 0) return false;
        if (t->pos == t->len) {
            /* the field after a final separator was already consumed */
            return false;
        }
        const char *s = t->src + t->pos;
        size_t rem = t->len - t->pos;
        const char *nl = memchr(s, '\n', rem);
        size_t n = nl ? (size_t)(nl - s) : rem;
        *out = s;
        *olen = n;
        t->pos += n + (nl ? 1 : 0);
        if (!nl) t->pos = t->len + 1;
        t->line = line_add(t->line, 1);
        return true;
    }
    if (t->pos >= t->len) return false;
    const char *src = t->src;
    size_t len = t->len, pos = t->pos, start = pos;
    const char *tok = NULL;
    size_t tlen = 0;
    if (src[pos] == '{' && pos + 1 < len && src[pos + 1] == '%') {
        pos += 2;
        const char *e = find2(src + pos, len - pos, '%', '}');
        if (e) {
            pos = (size_t)(e - src) + 2;
            tok = src + start;
            tlen = pos - start;
        } else {
            tok = LIT_TAG_START;
            tlen = 2;
        }
    } else if (src[pos] == '{' && pos + 1 < len && src[pos + 1] == '{') {
        pos += 2;
        int a, b;
        if (pos < len) a = b = (unsigned char)src[pos++];
        else a = b = -1;
        bool done = false;
        while (b >= 0) {
            while (a >= 0 && a != '}' && a != '{') a = pos < len ? (unsigned char)src[pos++] : -1;
            if (a < 0) break;
            if (pos >= len) {
                if (a == '}') {
                    tok = src + start;
                    tlen = pos - start;
                } else {
                    tok = LIT_VAR_START;
                    tlen = 2;
                }
                done = true;
                break;
            }
            b = (unsigned char)src[pos++];
            if (a == '}') {
                if (b != '}') pos--;
                tok = src + start;
                tlen = pos - start;
                done = true;
                break;
            } else if (a == '{' && b == '%') {
                const char *e = find2(src + pos, len - pos, '%', '}');
                if (e) pos = (size_t)(e - src) + 2;
                tok = src + start;
                tlen = pos - start;
                done = true;
                break;
            }
            a = b;
        }
        if (!done) {
            tok = LIT_VAR_START;
            tlen = 2;
        }
    } else {
        /* text up to the next "{{" or "{%" */
        size_t i = pos;
        const char *found = NULL;
        while (i + 1 < len) {
            const char *q = memchr(src + i, '{', len - 1 - i);
            if (!q) break;
            i = (size_t)(q - src);
            if (src[i + 1] == '{' || src[i + 1] == '%') {
                found = q;
                break;
            }
            i++;
        }
        if (found) pos = (size_t)(found - src);
        else pos = len;
        tok = src + start;
        tlen = pos - start;
    }
    t->pos = pos;
    t->line = line_add(t->line, count_nl(tok, tlen));
    *out = tok;
    *olen = tlen;
    return true;
}

/* ------------------------------------------------------------------ */
/* Expression lexer                                                    */
/* ------------------------------------------------------------------ */

static void push_tok(P *p, tok_type t, const char *s, size_t n) {
    if (p->ntoks == p->captoks) {
        p->captoks = p->captoks ? p->captoks * 2 : 32;
        p->toks = lq_xrealloc(p->toks, p->captoks * sizeof(etok));
    }
    p->toks[p->ntoks].type = t;
    p->toks[p->ntoks].s = s;
    p->toks[p->ntoks].n = n;
    p->ntoks++;
}

static size_t scan_number(const char *s, size_t n, size_t i) {
    /* -?\d+(\.\d+)? starting at i; returns end */
    size_t j = i;
    if (j < n && s[j] == '-') j++;
    size_t d0 = j;
    while (j < n && s[j] >= '0' && s[j] <= '9') j++;
    if (j == d0) return i;
    if (j + 1 < n && s[j] == '.' && s[j + 1] >= '0' && s[j + 1] <= '9') {
        j++;
        while (j < n && s[j] >= '0' && s[j] <= '9') j++;
    }
    return j;
}

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void lex_unexpected(P *p, const char *s, size_t n, size_t i) {
    size_t k = lq_utf8_len((const unsigned char *)s + i, n - i);
    lq_buf b;
    lq_buf_init(&b);
    lq_buf_puts(&b, "Unexpected character ");
    lq_buf_put(&b, s + i, k);
    perr_buf(p, LQ_ERR_SYNTAX, &b);
}

static void lex(P *p, const char *s, size_t n) {
    p->ntoks = 0;
    p->tp = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && lq_regex_space((unsigned char)s[i])) i++;
        if (i >= n) break;
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '|': push_tok(p, T_PIPE, s + i, 1); i++; continue;
        case ':': push_tok(p, T_COLON, s + i, 1); i++; continue;
        case ',': push_tok(p, T_COMMA, s + i, 1); i++; continue;
        case '[': push_tok(p, T_OPEN_SQ, s + i, 1); i++; continue;
        case ']': push_tok(p, T_CLOSE_SQ, s + i, 1); i++; continue;
        case '(': push_tok(p, T_OPEN_ROUND, s + i, 1); i++; continue;
        case ')': push_tok(p, T_CLOSE_ROUND, s + i, 1); i++; continue;
        case '?': push_tok(p, T_QUESTION, s + i, 1); i++; continue;
        case '.':
            if (i + 1 < n && s[i + 1] == '.') {
                push_tok(p, T_DOTDOT, s + i, 2);
                i += 2;
            } else {
                push_tok(p, T_DOT, s + i, 1);
                i++;
            }
            continue;
        case '-':
            if (i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9') {
                size_t e = scan_number(s, n, i);
                push_tok(p, T_NUMBER, s + i, e - i);
                i = e;
            } else {
                push_tok(p, T_DASH, s + i, 1);
                i++;
            }
            continue;
        case '=':
        case '!':
            if (i + 1 < n && s[i + 1] == '=') {
                push_tok(p, T_COMPARISON, s + i, 2);
                i += 2;
                continue;
            }
            lex_unexpected(p, s, n, i);
        case '<':
            if (i + 1 < n && (s[i + 1] == '=' || s[i + 1] == '>')) {
                push_tok(p, T_COMPARISON, s + i, 2);
                i += 2;
            } else {
                push_tok(p, T_COMPARISON, s + i, 1);
                i++;
            }
            continue;
        case '>':
            if (i + 1 < n && s[i + 1] == '=') {
                push_tok(p, T_COMPARISON, s + i, 2);
                i += 2;
            } else {
                push_tok(p, T_COMPARISON, s + i, 1);
                i++;
            }
            continue;
        case '\'':
        case '"': {
            const char *q = memchr(s + i + 1, c, n - i - 1);
            if (!q) lex_unexpected(p, s, n, i);
            size_t e = (size_t)(q - s) + 1;
            push_tok(p, T_STRING, s + i, e - i);
            i = e;
            continue;
        }
        default:
            break;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            size_t j = i + 1;
            while (j < n && (is_word((unsigned char)s[j]) || s[j] == '-')) j++;
            if (j < n && s[j] == '?') j++;
            if (sv_eq(s + i, j - i, "contains") && !(p->ntoks && p->toks[p->ntoks - 1].type == T_DOT))
                push_tok(p, T_COMPARISON, s + i, j - i);
            else push_tok(p, T_ID, s + i, j - i);
            i = j;
            continue;
        }
        if (c >= '0' && c <= '9') {
            size_t e = scan_number(s, n, i);
            push_tok(p, T_NUMBER, s + i, e - i);
            i = e;
            continue;
        }
        lex_unexpected(p, s, n, i);
    }
    push_tok(p, T_EOS, s + n, 0);
}

/* ------------------------------------------------------------------ */
/* Parser primitives                                                   */
/* ------------------------------------------------------------------ */

static etok *cur(P *p) { return &p->toks[p->tp]; }

static bool look(P *p, tok_type t, size_t ahead) {
    size_t i = p->tp + ahead;
    if (i >= p->ntoks) return false;
    return p->toks[i].type == t;
}

static etok *consume(P *p, tok_type t) {
    etok *k = cur(p);
    if (k->type != t) perr(p, "Expected %s but found %s", tok_names[t], tok_names[k->type]);
    if (p->tp + 1 < p->ntoks) p->tp++;
    return k;
}

static etok *consume_any(P *p) {
    etok *k = cur(p);
    if (p->tp + 1 < p->ntoks) p->tp++;
    return k;
}

static bool accept(P *p, tok_type t) {
    if (cur(p)->type != t) return false;
    if (p->tp + 1 < p->ntoks) p->tp++;
    return true;
}

static bool accept_id(P *p, const char *word) {
    etok *k = cur(p);
    if (k->type != T_ID || !sv_eq(k->s, k->n, word)) return false;
    if (p->tp + 1 < p->ntoks) p->tp++;
    return true;
}

/* "#{token} is not a valid expression" with Ruby Array#inspect of the token */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void invalid_expression(P *p, etok *k) {
    lq_buf b;
    lq_buf_init(&b);
    lq_buf_puts(&b, "[:");
    lq_buf_puts(&b, tok_names[k->type]);
    if (k->type != T_EOS) {
        lq_buf_puts(&b, ", ");
        lq_inspect_str(&b, k->s, k->n);
    }
    lq_buf_puts(&b, "] is not a valid expression");
    perr_buf(p, LQ_ERR_SYNTAX, &b);
}

/* Parser#expression: returns the expression re-rendered as markup text. */
typedef struct sv {
    const char *s;
    size_t n;
} sv;

static sv sv_concat(P *p, sv *parts, size_t np) {
    size_t total = 0;
    for (size_t i = 0; i < np; i++) total += parts[i].n;
    char *r = lq_arena_alloc(&p->tmp, total + 1);
    size_t o = 0;
    for (size_t i = 0; i < np; i++) {
        memcpy(r + o, parts[i].s, parts[i].n);
        o += parts[i].n;
    }
    sv out = {r, total};
    return out;
}

static sv parse_expression_text(P *p);

static sv variable_lookups_text(P *p) {
    lq_buf b;
    lq_buf_init(&b);
    /* collect pieces; use scratch arena to avoid leaking on error */
    sv acc = {"", 0};
    for (;;) {
        if (look(p, T_OPEN_SQ, 0)) {
            consume_any(p);
            sv inner = parse_expression_text(p);
            consume(p, T_CLOSE_SQ);
            sv parts[4] = {acc, {"[", 1}, inner, {"]", 1}};
            acc = sv_concat(p, parts, 4);
        } else if (look(p, T_DOT, 0)) {
            consume_any(p);
            etok *id = consume(p, T_ID);
            sv parts[3] = {acc, {".", 1}, {id->s, id->n}};
            acc = sv_concat(p, parts, 3);
        } else break;
    }
    lq_buf_free(&b);
    return acc;
}

static sv parse_expression_text(P *p) {
    if (++p->edepth > MAX_EXPR_DEPTH) perr(p, "Expression too deeply nested");
    etok *k = cur(p);
    sv r;
    switch (k->type) {
    case T_ID: {
        consume_any(p);
        sv rest = variable_lookups_text(p);
        sv parts[2] = {{k->s, k->n}, rest};
        r = sv_concat(p, parts, 2);
        break;
    }
    case T_OPEN_SQ:
        perr(p, "Bare bracket access is not allowed. Use self['...'] instead");
    case T_STRING:
    case T_NUMBER:
        consume_any(p);
        r.s = k->s;
        r.n = k->n;
        break;
    case T_OPEN_ROUND: {
        consume_any(p);
        sv first = parse_expression_text(p);
        consume(p, T_DOTDOT);
        sv last = parse_expression_text(p);
        consume(p, T_CLOSE_ROUND);
        sv parts[5] = {{"(", 1}, first, {"..", 2}, last, {")", 1}};
        r = sv_concat(p, parts, 5);
        break;
    }
    default:
        invalid_expression(p, k);
    }
    p->edepth--;
    return r;
}

/* ------------------------------------------------------------------ */
/* Expression.parse (markup -> AST)                                    */
/* ------------------------------------------------------------------ */

static lq_expr *new_expr(P *p, lq_expr_kind k) {
    lq_expr *e = lq_arena_alloc(p->A, sizeof(lq_expr));
    e->kind = k;
    return e;
}

static lq_expr *lit_expr(P *p, lq_value v) {
    lq_expr *e = new_expr(p, EX_LIT);
    e->lit = lq_arena_keep(p->A, v);
    return e;
}

static lq_expr *expr_from_markup(P *p, const char *s, size_t n);

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* parse_number: returns true and sets *out when markup is numeric */
static bool parse_number_markup(const char *s, size_t n, lq_value *out) {
    if (!n) return false;
    size_t i = 0;
    if (s[0] == '-') i = 1;
    if (i >= n || !is_digit(s[i])) return false;
    size_t j = i;
    while (j < n && is_digit(s[j])) j++;
    if (j == n) {
        *out = lq_parse_int_text(s, n);
        return true;
    }
    /* remaining bytes must be digits or dots */
    for (size_t k = j; k < n; k++)
        if (!is_digit(s[k]) && s[k] != '.') return false;
    /* String#to_f of the whole markup: longest valid float prefix */
    size_t e = j;
    if (e < n && s[e] == '.' && e + 1 < n && is_digit(s[e + 1])) {
        e++;
        while (e < n && is_digit(s[e])) e++;
    }
    char *tmp = lq_xstrndup(s, e);
    *out = lq_float(strtod(tmp, NULL));
    free(tmp);
    return true;
}

/* Find the end of a balanced [...] group starting at s[i] == '['. */
static long bracket_end(const char *s, size_t n, size_t i) {
    long depth = 0;
    for (size_t j = i; j < n; j++) {
        if (s[j] == '[') depth++;
        else if (s[j] == ']') {
            depth--;
            if (depth == 0) return (long)(j + 1);
        }
    }
    return -1;
}

/* VariableParser scan: /\[(?>[^\[\]]+|\g<0>)*\]|[\w\-]+\??/ */
static bool next_var_part(const char *s, size_t n, size_t *i, size_t *ps, size_t *pe) {
    while (*i < n) {
        size_t k = *i;
        if (s[k] == '[') {
            long e = bracket_end(s, n, k);
            if (e > 0) {
                *ps = k;
                *pe = (size_t)e;
                *i = (size_t)e;
                return true;
            }
        } else if (is_word((unsigned char)s[k]) || s[k] == '-') {
            size_t j = k;
            while (j < n && (is_word((unsigned char)s[j]) || s[j] == '-')) j++;
            if (j < n && s[j] == '?') j++;
            *ps = k;
            *pe = j;
            *i = j;
            return true;
        }
        (*i)++;
    }
    return false;
}

static lq_expr *variable_lookup_from_markup(P *p, const char *s, size_t n) {
    lq_expr *e = new_expr(p, EX_VAR);
    size_t i = 0, ps, pe;
    size_t cap = 0;
    bool have_name = false;
    while (next_var_part(s, n, &i, &ps, &pe)) {
        const char *part = s + ps;
        size_t plen = pe - ps;
        bool bracket = plen >= 2 && part[0] == '[' && part[plen - 1] == ']';
        if (!have_name) {
            have_name = true;
            if (bracket) e->name_expr = expr_from_markup(p, part + 1, plen - 2);
            else e->name_expr = lit_expr(p, lq_str_new(part, plen));
            continue;
        }
        if (e->nlook == cap) {
            size_t nc = cap ? cap * 2 : 4;
            lq_lookup *nl = lq_arena_alloc(p->A, nc * sizeof(lq_lookup));
            if (e->nlook) memcpy(nl, e->look, e->nlook * sizeof(lq_lookup));
            e->look = nl;
            cap = nc;
        }
        lq_lookup *lk = &e->look[e->nlook++];
        if (bracket) {
            lk->key = expr_from_markup(p, part + 1, plen - 2);
            lk->command = false;
        } else {
            lk->key = lit_expr(p, lq_str_new(part, plen));
            lk->command = sv_eq(part, plen, "size") || sv_eq(part, plen, "first") || sv_eq(part, plen, "last");
        }
    }
    if (!have_name) e->name_expr = lit_expr(p, lq_nil());
    return e;
}

/* RANGES_REGEX = /\A\(\s*(?>(\S+)\s*\.\.)\s*(\S+)\s*\)\z/ */
static bool match_range(const char *s, size_t n, sv *m1, sv *m2) {
    if (n < 2 || s[0] != '(' || s[n - 1] != ')') return false;
    size_t i = 1;
    while (i < n && lq_regex_space((unsigned char)s[i])) i++;
    size_t j = i;
    while (j < n && !lq_regex_space((unsigned char)s[j])) j++;
    if (j == i) return false;
    /* largest k in (i, j] such that s[k..] matches \s*\.\. */
    size_t after = 0;
    bool ok = false;
    for (size_t k = j; k > i; k--) {
        size_t q = k;
        if (k == j)
            while (q < n && lq_regex_space((unsigned char)s[q])) q++;
        if (q + 1 < n && s[q] == '.' && s[q + 1] == '.') {
            m1->s = s + i;
            m1->n = k - i;
            after = q + 2;
            ok = true;
            break;
        }
    }
    if (!ok) return false;
    size_t a = after;
    while (a < n && lq_regex_space((unsigned char)s[a])) a++;
    size_t b = a;
    while (b < n && !lq_regex_space((unsigned char)s[b])) b++;
    if (b == a) return false;
    if (b == n) {
        /* run reaches the end: it must end with ')' and have >= 1 char before */
        if (b - a < 2) return false;
        m2->s = s + a;
        m2->n = b - a - 1;
        return true;
    }
    size_t c = b;
    while (c < n && lq_regex_space((unsigned char)s[c])) c++;
    if (c == n - 1 && s[c] == ')') {
        m2->s = s + a;
        m2->n = b - a;
        return true;
    }
    return false;
}

/* constant range endpoint .to_i; returns false if no to_i */
static bool const_to_i(lq_value v, int64_t *out) {
    switch (v.type) {
    case LQ_NIL: *out = 0; return true;
    case LQ_INT: *out = v.u.i; return true;
    case LQ_BIGINT: *out = v.u.bi->neg ? INT64_MIN : INT64_MAX; return true;
    case LQ_FLOAT: {
        double d = v.u.f;
        if (d != d) d = 0;
        if (d >= 9.2e18) *out = INT64_MAX;
        else if (d <= -9.2e18) *out = INT64_MIN;
        else *out = (int64_t)d;
        return true;
    }
    case LQ_STR: {
        lq_value iv = lq_str_to_i(v.u.s->data, v.u.s->len);
        if (iv.type == LQ_INT) *out = iv.u.i;
        else if (iv.type == LQ_BIGINT) *out = iv.u.bi->neg ? INT64_MIN : INT64_MAX;
        else *out = 0;
        lq_release(iv);
        return true;
    }
    default:
        return false;
    }
}

static lq_expr *range_from_markups(P *p, sv m1, sv m2) {
    lq_expr *a = expr_from_markup(p, m1.s, m1.n);
    lq_expr *b = expr_from_markup(p, m2.s, m2.n);
    if (a->kind != EX_LIT || b->kind != EX_LIT) {
        lq_expr *e = new_expr(p, EX_RANGE);
        e->rstart = a;
        e->rend = b;
        return e;
    }
    int64_t x, y;
    const char *bad = NULL;
    size_t badn = 0;
    if (!const_to_i(a->lit, &x)) { bad = m1.s; badn = m1.n; }
    else if (!const_to_i(b->lit, &y)) { bad = m2.s; badn = m2.n; }
    if (bad) {
        lq_buf eb;
        lq_buf_init(&eb);
        lq_buf_puts(&eb, "Invalid expression type '");
        lq_buf_put(&eb, bad, badn);
        lq_buf_puts(&eb, "' in range expression");
        perr_buf(p, LQ_ERR_SYNTAX, &eb);
    }
    return lit_expr(p, lq_range_new(x, y));
}

static lq_expr *expr_from_markup(P *p, const char *s, size_t n) {
    rb_strip(&s, &n);
    if (n >= 1 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        if (n == 1) return lit_expr(p, lq_str_empty());
        return lit_expr(p, lq_str_new(s + 1, n - 2));
    }
    if (n == 0 || sv_eq(s, n, "nil") || sv_eq(s, n, "null")) return lit_expr(p, lq_nil());
    if (sv_eq(s, n, "true")) return lit_expr(p, lq_bool(true));
    if (sv_eq(s, n, "false")) return lit_expr(p, lq_bool(false));
    if (sv_eq(s, n, "blank") || sv_eq(s, n, "empty")) return lit_expr(p, lq_str_empty());
    if (++p->edepth > MAX_EXPR_DEPTH) perr(p, "Expression too deeply nested");
    lq_expr *r;
    sv m1, m2;
    lq_value num;
    if (match_range(s, n, &m1, &m2)) r = range_from_markups(p, m1, m2);
    else if (parse_number_markup(s, n, &num)) r = lit_expr(p, num);
    else r = variable_lookup_from_markup(p, s, n);
    p->edepth--;
    return r;
}

/* parse_context.safe_parse_expression(parser) */
static lq_expr *parse_expr(P *p) {
    sv t = parse_expression_text(p);
    return expr_from_markup(p, t.s, t.n);
}

/* Condition.parse_expression: blank/empty method literals */
static lq_expr *parse_cond_expr(P *p) {
    sv t = parse_expression_text(p);
    if (sv_eq(t.s, t.n, "blank")) return lit_expr(p, lq_special(LQ_BLANK));
    if (sv_eq(t.s, t.n, "empty")) return lit_expr(p, lq_special(LQ_EMPTY));
    return expr_from_markup(p, t.s, t.n);
}

/* ------------------------------------------------------------------ */
/* Variables and filters                                               */
/* ------------------------------------------------------------------ */

static bool end_of_arguments(P *p) { return look(p, T_PIPE, 0) || look(p, T_EOS, 0); }

typedef struct exprvec {
    lq_expr **v;
    size_t n, cap;
} exprvec;

static void ev_push(P *p, exprvec *ev, lq_expr *e) {
    if (ev->n == ev->cap) {
        size_t nc = ev->cap ? ev->cap * 2 : 4;
        lq_expr **nv = lq_arena_alloc(p->A, nc * sizeof(lq_expr *));
        if (ev->n) memcpy(nv, ev->v, ev->n * sizeof(lq_expr *));
        ev->v = nv;
        ev->cap = nc;
    }
    ev->v[ev->n++] = e;
}

static void parse_filter(P *p, lq_filter *f) {
    etok *name = consume(p, T_ID);
    f->def = lq_filter_lookup(name->s, name->n);
    exprvec args = {0}, kwv = {0};
    lq_str **kwn = NULL;
    size_t kwcap = 0;
    if (accept(p, T_COLON)) {
        bool first = true;
        for (;;) {
            if (first) {
                first = false;
                if (end_of_arguments(p)) break;
            } else {
                if (!accept(p, T_COMMA)) break;
                if (end_of_arguments(p)) break;
            }
            if (look(p, T_ID, 0) && look(p, T_COLON, 1)) {
                etok *key = consume(p, T_ID);
                consume(p, T_COLON);
                lq_expr *val = parse_expr(p);
                /* duplicate keyword: overwrite keeping position */
                bool dup = false;
                for (size_t i = 0; i < kwv.n; i++) {
                    if (kwn[i]->len == key->n && memcmp(kwn[i]->data, key->s, key->n) == 0) {
                        kwv.v[i] = val;
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    if (kwv.n == kwcap) {
                        size_t nc = kwcap ? kwcap * 2 : 4;
                        lq_str **nn = lq_arena_alloc(p->A, nc * sizeof(lq_str *));
                        if (kwv.n) memcpy(nn, kwn, kwv.n * sizeof(lq_str *));
                        kwn = nn;
                        kwcap = nc;
                    }
                    kwn[kwv.n] = mkstr(p, key->s, key->n);
                    ev_push(p, &kwv, val);
                }
            } else {
                ev_push(p, &args, parse_expr(p));
            }
        }
    }
    f->nargs = args.n;
    f->args = args.v;
    f->nkw = kwv.n;
    f->kwargs = kwv.v;
    f->kwnames = kwn;
}

/* Variable#strict2_parse */
static lq_variable *parse_variable_markup(P *p, const char *m, size_t n, int line) {
    lq_variable *v = lq_arena_alloc(p->A, sizeof(lq_variable));
    v->line = line;
    lex(p, m, n);
    if (look(p, T_EOS, 0)) return v;
    v->expr = parse_expr(p);
    size_t cap = 0;
    while (accept(p, T_PIPE)) {
        if (v->nfilters == cap) {
            size_t nc = cap ? cap * 2 : 2;
            lq_filter *nf = lq_arena_alloc(p->A, nc * sizeof(lq_filter));
            if (v->nfilters) memcpy(nf, v->filters, v->nfilters * sizeof(lq_filter));
            v->filters = nf;
            cap = nc;
        }
        parse_filter(p, &v->filters[v->nfilters++]);
    }
    consume(p, T_EOS);
    return v;
}

/* ------------------------------------------------------------------ */
/* Nodes and bodies                                                    */
/* ------------------------------------------------------------------ */

static lq_node *new_node(P *p, lq_node_kind k, int line) {
    lq_node *n = lq_arena_alloc(p->A, sizeof(lq_node));
    n->kind = k;
    n->line = line;
    return n;
}

static lq_body *new_body(P *p) {
    lq_body *b = lq_arena_alloc(p->A, sizeof(lq_body));
    b->blank = true;
    return b;
}

static void body_push(P *p, lq_body *b, lq_node *n) {
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8;
        lq_node **nn = lq_arena_alloc(p->A, nc * sizeof(lq_node *));
        if (b->n) memcpy(nn, b->nodes, b->n * sizeof(lq_node *));
        b->nodes = nn;
        b->cap = nc;
    }
    b->nodes[b->n++] = n;
}

static void remove_blank_strings(lq_body *b) {
    size_t o = 0;
    for (size_t i = 0; i < b->n; i++)
        if (b->nodes[i]->kind != N_TEXT) b->nodes[o++] = b->nodes[i];
    b->n = o;
}

typedef struct endtag {
    bool eof;
    const char *name;
    size_t nlen;
    const char *markup;
    size_t mlen;
} endtag;

static void body_parse(P *p, tokenizer *t, lq_body *b, endtag *out);

/* FullToken: /\A{%-?(\s*)(#|\w+)(\s*)(.*?)-?%}\z/m */
static bool match_full_token(const char *tok, size_t n, sv *ws1, sv *name, sv *ws3, sv *markup) {
    if (n < 4 || tok[0] != '{' || tok[1] != '%' || tok[n - 2] != '%' || tok[n - 1] != '}') return false;
    size_t i = 2, end = n - 2;
    if (i < end && tok[i] == '-') i++;
    size_t w1 = i;
    while (i < end && lq_regex_space((unsigned char)tok[i])) i++;
    ws1->s = tok + w1;
    ws1->n = i - w1;
    size_t ns = i;
    if (i < end && tok[i] == '#') i++;
    else
        while (i < end && is_word((unsigned char)tok[i])) i++;
    if (i == ns) {
        /* the optional '-' may have been the only candidate: retry without it */
        return false;
    }
    name->s = tok + ns;
    name->n = i - ns;
    size_t w3 = i;
    while (i < end && lq_regex_space((unsigned char)tok[i])) i++;
    ws3->s = tok + w3;
    ws3->n = i - w3;
    size_t me = end;
    if (me > i && tok[me - 1] == '-') me--;
    markup->s = tok + i;
    markup->n = me - i;
    return true;
}

/* FullTokenPossiblyInvalid: /\A(.*){%-?\s*(\w+)\s*(.*)?-?%}\z/m
   Returns the tag name and the text before the (last) "{%". */
static bool match_possibly_invalid(const char *tok, size_t n, sv *before, sv *name) {
    if (n < 4 || tok[n - 2] != '%' || tok[n - 1] != '}') return false;
    /* (.*) is greedy: try the last "{%" first */
    for (size_t k = n - 2; k-- > 0;) {
        if (tok[k] != '{' || tok[k + 1] != '%') continue;
        size_t i = k + 2;
        size_t end = n - 2;
        if (i < end && tok[i] == '-') i++;
        while (i < end && lq_regex_space((unsigned char)tok[i])) i++;
        size_t ns = i;
        while (i < end && is_word((unsigned char)tok[i])) i++;
        if (i == ns) {
            /* "{%-" where '-' is not followed by a word: try without '-' */
            continue;
        }
        before->s = tok;
        before->n = k;
        name->s = tok + ns;
        name->n = i - ns;
        return true;
    }
    return false;
}

static void whitespace_handler(P *p, lq_body *b, const char *tok, size_t n) {
    if (n >= 3 && tok[2] == '-') {
        if (b->n && b->nodes[b->n - 1]->kind == N_TEXT) {
            lq_node *prev = b->nodes[b->n - 1];
            while (prev->u.text.len && lq_ruby_space((unsigned char)prev->u.text.s[prev->u.text.len - 1]))
                prev->u.text.len--;
        }
    }
    p->trim = n >= 3 && tok[n - 3] == '-';
}

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void raise_unknown_tag(P *p, sv tag, const char *block_name) {
    lq_buf b;
    lq_buf_init(&b);
    if (sv_eq(tag.s, tag.n, "else")) {
        lq_buf_puts(&b, block_name);
        lq_buf_puts(&b, " tag does not expect 'else' tag");
    } else if (tag.n >= 3 && memcmp(tag.s, "end", 3) == 0) {
        lq_buf_putc(&b, '\'');
        lq_buf_put(&b, tag.s, tag.n);
        lq_buf_puts(&b, "' is not a valid delimiter for ");
        lq_buf_puts(&b, block_name);
        lq_buf_puts(&b, " tags. use ");
        if (strcmp(block_name, "liquid") == 0) lq_buf_puts(&b, "%}");
        else {
            lq_buf_puts(&b, "end");
            lq_buf_puts(&b, block_name);
        }
    } else {
        lq_buf_puts(&b, "Unknown tag '");
        lq_buf_put(&b, tag.s, tag.n);
        lq_buf_putc(&b, '\'');
    }
    p->in_markup = false;
    perr_buf(p, LQ_ERR_SYNTAX, &b);
}

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void raise_never_closed(P *p, const char *block_name) {
    p->in_markup = false;
    perr(p, "'%s' tag was never closed", block_name);
}

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void raise_nesting(P *p) {
    lq_buf b;
    lq_buf_init(&b);
    lq_buf_puts(&b, "Nesting too deep");
    perr_buf(p, LQ_ERR_STACK, &b);
}

/* Block#parse_body: parse one body; returns true when the body ended at an
   unknown/intermediate tag (stored in *et), false when the block ended. */
static bool block_parse_body(P *p, tokenizer *t, lq_node *node, const char *block_name, lq_body *b,
                             endtag *et) {
    if (p->depth >= MAX_BLOCK_DEPTH) raise_nesting(p);
    p->depth++;
    body_parse(p, t, b, et);
    p->depth--;
    node->blank = node->blank && b->blank;
    if (et->eof) raise_never_closed(p, block_name);
    size_t bl = strlen(block_name);
    if (et->nlen == bl + 3 && memcmp(et->name, "end", 3) == 0 && memcmp(et->name + 3, block_name, bl) == 0)
        return false;
    return true;
}

/* begin/end markup parsing context for a tag */
static void begin_markup(P *p, const char *m, size_t n, int line, bool variable) {
    p->mctx = markup_context(p, m, n, variable);
    p->mline = line;
    p->in_markup = true;
    p->edepth = 0;
}

static void end_markup(P *p) {
    p->in_markup = false;
    p->mctx = NULL;
}

/* ------------------------------------------------------------------ */
/* Tags                                                                */
/* ------------------------------------------------------------------ */

static lq_cond *parse_condition(P *p, const char *m, size_t n) {
    lex(p, m, n);
    lq_cond *first = NULL, *last = NULL;
    for (;;) {
        lq_cond *c = lq_arena_alloc(p->A, sizeof(lq_cond));
        c->left = parse_cond_expr(p);
        if (look(p, T_COMPARISON, 0)) {
            etok *op = consume_any(p);
            if (sv_eq(op->s, op->n, "==")) c->op = OP_EQ;
            else if (sv_eq(op->s, op->n, "!=") || sv_eq(op->s, op->n, "<>")) c->op = OP_NE;
            else if (sv_eq(op->s, op->n, "<")) c->op = OP_LT;
            else if (sv_eq(op->s, op->n, ">")) c->op = OP_GT;
            else if (sv_eq(op->s, op->n, "<=")) c->op = OP_LE;
            else if (sv_eq(op->s, op->n, ">=")) c->op = OP_GE;
            else c->op = OP_CONTAINS;
            c->right = parse_cond_expr(p);
        }
        if (!first) first = c;
        else last->child = c;
        last = c;
        if (accept_id(p, "and")) last->rel = REL_AND;
        else if (accept_id(p, "or")) last->rel = REL_OR;
        else break;
    }
    consume(p, T_EOS);
    return first;
}

typedef struct cbvec {
    lq_cblock *v;
    size_t n, cap;
} cbvec;

static void cb_push(P *p, cbvec *cv, lq_cond *c, lq_body *b) {
    if (cv->n == cv->cap) {
        size_t nc = cv->cap ? cv->cap * 2 : 4;
        lq_cblock *nv = lq_arena_alloc(p->A, nc * sizeof(lq_cblock));
        if (cv->n) memcpy(nv, cv->v, cv->n * sizeof(lq_cblock));
        cv->v = nv;
        cv->cap = nc;
    }
    cv->v[cv->n].cond = c;
    cv->v[cv->n].body = b;
    cv->n++;
}

static lq_node *parse_if(P *p, tokenizer *t, sv tag, sv markup, int line, bool is_unless) {
    lq_node *node = new_node(p, is_unless ? N_UNLESS : N_IF, line);
    node->blank = true;
    const char *bname = is_unless ? "unless" : "if";
    cbvec cv = {0};
    begin_markup(p, markup.s, markup.n, line, false);
    lq_cond *c = parse_condition(p, markup.s, markup.n);
    end_markup(p);
    cb_push(p, &cv, c, new_body(p));
    (void)tag;
    endtag et;
    while (block_parse_body(p, t, node, bname, cv.v[cv.n - 1].body, &et)) {
        sv nm = {et.name, et.nlen};
        if (sv_eq(et.name, et.nlen, "elsif")) {
            begin_markup(p, et.markup, et.mlen, line, false);
            lq_cond *ec = parse_condition(p, et.markup, et.mlen);
            end_markup(p);
            cb_push(p, &cv, ec, new_body(p));
        } else if (sv_eq(et.name, et.nlen, "else")) {
            cb_push(p, &cv, NULL, new_body(p));
        } else {
            raise_unknown_tag(p, nm, bname);
        }
    }
    if (node->blank)
        for (size_t i = 0; i < cv.n; i++) remove_blank_strings(cv.v[i].body);
    node->u.cond.n = cv.n;
    node->u.cond.blocks = cv.v;
    return node;
}

static lq_node *parse_case(P *p, tokenizer *t, sv markup, int line) {
    lq_node *node = new_node(p, N_CASE, line);
    node->blank = true;
    begin_markup(p, markup.s, markup.n, line, false);
    lex(p, markup.s, markup.n);
    lq_expr *left = parse_expr(p);
    consume(p, T_EOS);
    end_markup(p);
    cbvec cv = {0};
    lq_body *case_body = new_body(p);
    lq_body *body = case_body;
    endtag et;
    while (block_parse_body(p, t, node, "case", body, &et)) {
        sv nm = {et.name, et.nlen};
        if (sv_eq(et.name, et.nlen, "when")) {
            lq_body *wb = new_body(p);
            /* when conditions are parsed without markup context */
            p->edepth = 0;
            lex(p, et.markup, et.mlen);
            for (;;) {
                lq_cond *c = lq_arena_alloc(p->A, sizeof(lq_cond));
                c->left = left;
                c->op = OP_EQ;
                c->right = parse_cond_expr(p);
                cb_push(p, &cv, c, wb);
                if (!(accept_id(p, "or") || accept(p, T_COMMA))) break;
            }
            consume(p, T_EOS);
        } else if (sv_eq(et.name, et.nlen, "else")) {
            const char *m = et.markup;
            size_t mn = et.mlen;
            rb_strip(&m, &mn);
            if (mn) perr(p, "Syntax Error in tag 'case' - Valid else condition: {%% else %%} (no parameters) ");
            cb_push(p, &cv, NULL, new_body(p));
        } else {
            raise_unknown_tag(p, nm, "case");
        }
        body = cv.v[cv.n - 1].body;
    }
    if (node->blank) {
        for (size_t i = 0; i < cv.n; i++) remove_blank_strings(cv.v[i].body);
    }
    node->u.cond.n = cv.n;
    node->u.cond.blocks = cv.v;
    return node;
}

static lq_node *parse_for(P *p, tokenizer *t, sv markup, int line) {
    lq_node *node = new_node(p, N_FOR, line);
    node->blank = true;
    begin_markup(p, markup.s, markup.n, line, false);
    lex(p, markup.s, markup.n);
    etok *var = consume(p, T_ID);
    if (!accept_id(p, "in")) perr(p, "For loops require an 'in' clause");
    sv coll = parse_expression_text(p);
    node->u.loop.var = mkstr(p, var->s, var->n);
    node->u.loop.coll = expr_from_markup(p, coll.s, coll.n);
    {
        lq_buf nb;
        lq_buf_init(&nb);
        lq_buf_put(&nb, var->s, var->n);
        lq_buf_putc(&nb, '-');
        lq_buf_put(&nb, coll.s, coll.n);
        node->u.loop.name = mkstr(p, nb.p, nb.len);
        lq_buf_free(&nb);
    }
    node->u.loop.reversed = accept_id(p, "reversed");
    while (look(p, T_COMMA, 0) || look(p, T_ID, 0)) {
        accept(p, T_COMMA);
        bool is_limit;
        if (accept_id(p, "limit")) is_limit = true;
        else if (accept_id(p, "offset")) is_limit = false;
        else perr(p, "Invalid attribute in for loop. Valid attributes are limit and offset");
        consume(p, T_COLON);
        sv ex = parse_expression_text(p);
        if (is_limit) node->u.loop.limit = expr_from_markup(p, ex.s, ex.n);
        else if (sv_eq(ex.s, ex.n, "continue")) {
            node->u.loop.offset_continue = true;
            node->u.loop.offset = NULL;
        } else {
            node->u.loop.offset_continue = false;
            node->u.loop.offset = expr_from_markup(p, ex.s, ex.n);
        }
    }
    consume(p, T_EOS);
    end_markup(p);
    node->u.loop.body = new_body(p);
    endtag et;
    if (block_parse_body(p, t, node, "for", node->u.loop.body, &et)) {
        sv nm = {et.name, et.nlen};
        if (!sv_eq(et.name, et.nlen, "else")) raise_unknown_tag(p, nm, "for");
        node->u.loop.else_body = new_body(p);
        if (block_parse_body(p, t, node, "for", node->u.loop.else_body, &et)) {
            sv nm2 = {et.name, et.nlen};
            if (!sv_eq(et.name, et.nlen, "else")) raise_unknown_tag(p, nm2, "for");
            /* a second else replaces the else block and ends the for tag */
            node->u.loop.else_body = new_body(p);
        }
    }
    if (node->blank) {
        if (node->u.loop.else_body) remove_blank_strings(node->u.loop.else_body);
        remove_blank_strings(node->u.loop.body);
    }
    return node;
}

static lq_node *parse_tablerow(P *p, tokenizer *t, sv markup, int line) {
    lq_node *node = new_node(p, N_TABLEROW, line);
    node->blank = true;
    begin_markup(p, markup.s, markup.n, line, false);
    lex(p, markup.s, markup.n);
    etok *var = consume(p, T_ID);
    if (!accept_id(p, "in")) perr(p, "For loops require an 'in' clause");
    node->u.trow.var = mkstr(p, var->s, var->n);
    node->u.trow.coll = parse_expr(p);
    accept(p, T_COMMA);
    size_t cap = 0;
    while (look(p, T_ID, 0)) {
        etok *key = consume_any(p);
        if (!(sv_eq(key->s, key->n, "cols") || sv_eq(key->s, key->n, "limit") ||
              sv_eq(key->s, key->n, "offset") || sv_eq(key->s, key->n, "range"))) {
            lq_buf b;
            lq_buf_init(&b);
            lq_buf_puts(&b, "Invalid attribute '");
            lq_buf_put(&b, key->s, key->n);
            lq_buf_puts(&b, "' in tablerow loop. Valid attributes are cols, limit, offset, and range");
            perr_buf(p, LQ_ERR_SYNTAX, &b);
        }
        consume(p, T_COLON);
        lq_expr *val = parse_expr(p);
        bool dup = false;
        for (size_t i = 0; i < node->u.trow.nattrs; i++) {
            lq_str *k = node->u.trow.attrs[i].name;
            if (k->len == key->n && memcmp(k->data, key->s, key->n) == 0) {
                node->u.trow.attrs[i].expr = val;
                dup = true;
            }
        }
        if (!dup) {
            if (node->u.trow.nattrs == cap) {
                size_t nc = cap ? cap * 2 : 4;
                lq_attr *na = lq_arena_alloc(p->A, nc * sizeof(lq_attr));
                if (node->u.trow.nattrs) memcpy(na, node->u.trow.attrs, node->u.trow.nattrs * sizeof(lq_attr));
                node->u.trow.attrs = na;
                cap = nc;
            }
            node->u.trow.attrs[node->u.trow.nattrs].name = mkstr(p, key->s, key->n);
            node->u.trow.attrs[node->u.trow.nattrs].expr = val;
            node->u.trow.nattrs++;
        }
        accept(p, T_COMMA);
    }
    consume(p, T_EOS);
    end_markup(p);
    node->u.trow.body = new_body(p);
    endtag et;
    while (block_parse_body(p, t, node, "tablerow", node->u.trow.body, &et)) {
        sv nm = {et.name, et.nlen};
        raise_unknown_tag(p, nm, "tablerow");
    }
    return node;
}

/* Assign Syntax: (VariableSignature+) \s* = \s* (.*) with VariableSignature
   being an optionally parenthesized [\w\-.\[\]] char. Unanchored: the leftmost
   match wins. */
static bool sig_char(unsigned char c) { return is_word(c) || c == '-' || c == '.' || c == '[' || c == ']'; }

static bool match_assign(const char *s, size_t n, sv *lhs, sv *rhs) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        size_t reps = 0;
        for (;;) {
            size_t k = j;
            if (k < n && s[k] == '(') k++;
            if (k < n && sig_char((unsigned char)s[k])) {
                k++;
                if (k < n && s[k] == ')') k++;
                j = k;
                reps++;
            } else break;
        }
        if (reps) {
            size_t q = j;
            while (q < n && lq_regex_space((unsigned char)s[q])) q++;
            if (q < n && s[q] == '=') {
                lhs->s = s + i;
                lhs->n = j - i;
                q++;
                while (q < n && lq_regex_space((unsigned char)s[q])) q++;
                rhs->s = s + q;
                rhs->n = n - q;
                return true;
            }
            i = j > i ? j : i + 1;
        } else i++;
    }
    return false;
}

static lq_node *parse_assign(P *p, sv markup, int line) {
    lq_node *node = new_node(p, N_ASSIGN, line);
    node->blank = true;
    begin_markup(p, markup.s, markup.n, line, false);
    sv lhs, rhs;
    if (!match_assign(markup.s, markup.n, &lhs, &rhs))
        perr(p, "Syntax Error in 'assign' - Valid syntax: assign [var] = [source]");
    rb_strip(&lhs.s, &lhs.n);
    lex(p, lhs.s, lhs.n);
    etok *to = consume(p, T_ID);
    consume(p, T_EOS);
    node->u.assign.to = mkstr(p, to->s, to->n);
    node->u.assign.from = parse_variable_markup(p, rhs.s, rhs.n, line);
    end_markup(p);
    return node;
}

static lq_node *parse_capture(P *p, tokenizer *t, sv markup, int line) {
    lq_node *node = new_node(p, N_CAPTURE, line);
    begin_markup(p, markup.s, markup.n, line, false);
    const char *m = markup.s;
    size_t mn = markup.n;
    rb_strip(&m, &mn);
    lex(p, m, mn);
    etok *to = consume(p, T_ID);
    consume(p, T_EOS);
    end_markup(p);
    node->u.capture.to = mkstr(p, to->s, to->n);
    node->u.capture.body = new_body(p);
    endtag et;
    node->blank = true;
    while (block_parse_body(p, t, node, "capture", node->u.capture.body, &et)) {
        sv nm = {et.name, et.nlen};
        raise_unknown_tag(p, nm, "capture");
    }
    node->blank = true; /* Capture#blank? is always true */
    return node;
}

static void inspect_const_expr(lq_buf *b, lq_expr *e, lq_node *node, size_t idx) {
    if (e->kind == EX_LIT) {
        lq_inspect(b, e->lit, NULL);
    } else {
        /* VariableLookup/RangeLookup#inspect includes an object address: each
           unnamed cycle using lookups gets its own counter */
        lq_buf_printf(b, "#<Liquid::VariableLookup:%p:%zu>", (void *)node, idx);
    }
}

static lq_node *parse_cycle(P *p, sv markup, int line) {
    lq_node *node = new_node(p, N_CYCLE, line);
    begin_markup(p, markup.s, markup.n, line, false);
    lex(p, markup.s, markup.n);
    if (look(p, T_EOS, 0)) perr(p, "Syntax Error in 'cycle' - Valid syntax: cycle [name :] var [, var2, var3 ...]");
    exprvec vals = {0};
    lq_expr *first = parse_expr(p);
    bool named = false;
    if (look(p, T_COLON, 0)) {
        node->u.cycle.name = first;
        named = true;
        consume(p, T_COLON);
        ev_push(p, &vals, parse_expr(p));
    } else {
        ev_push(p, &vals, first);
    }
    while (accept(p, T_COMMA)) {
        if (look(p, T_EOS, 0)) break;
        ev_push(p, &vals, parse_expr(p));
    }
    consume(p, T_EOS);
    end_markup(p);
    node->u.cycle.n = vals.n;
    node->u.cycle.vals = vals.v;
    if (!named) {
        lq_buf b;
        lq_buf_init(&b);
        lq_buf_putc(&b, '[');
        for (size_t i = 0; i < vals.n; i++) {
            if (i) lq_buf_put(&b, ", ", 2);
            inspect_const_expr(&b, vals.v[i], node, i);
        }
        lq_buf_putc(&b, ']');
        node->u.cycle.key = mkstr(p, b.p, b.len);
        lq_buf_free(&b);
    }
    return node;
}

static lq_node *parse_counter(P *p, sv markup, int line, bool inc) {
    lq_node *node = new_node(p, inc ? N_INCREMENT : N_DECREMENT, line);
    begin_markup(p, markup.s, markup.n, line, false);
    const char *m = markup.s;
    size_t mn = markup.n;
    rb_strip(&m, &mn);
    lex(p, m, mn);
    etok *id = consume(p, T_ID);
    consume(p, T_EOS);
    end_markup(p);
    node->u.counter.name = mkstr(p, id->s, id->n);
    return node;
}

static lq_node *parse_partial(P *p, sv markup, int line, bool is_render) {
    lq_node *node = new_node(p, is_render ? N_RENDER : N_INCLUDE, line);
    begin_markup(p, markup.s, markup.n, line, false);
    lex(p, markup.s, markup.n);
    if (is_render) {
        etok *s = consume(p, T_STRING);
        node->u.partial.tname = expr_from_markup(p, s->s, s->n);
    } else {
        node->u.partial.tname = parse_expr(p);
    }
    bool with_or_for = false, is_for = false;
    if (accept_id(p, "for")) with_or_for = is_for = true;
    else if (accept_id(p, "with")) with_or_for = true;
    if (with_or_for) node->u.partial.var = parse_expr(p);
    if (accept_id(p, "as")) {
        etok *a = consume(p, T_ID);
        node->u.partial.alias = mkstr(p, a->s, a->n);
    }
    node->u.partial.is_for = is_for;
    accept(p, T_COMMA);
    size_t cap = 0;
    while (look(p, T_ID, 0)) {
        etok *key = consume_any(p);
        consume(p, T_COLON);
        lq_expr *val = parse_expr(p);
        bool dup = false;
        for (size_t i = 0; i < node->u.partial.nattrs; i++) {
            lq_str *k = node->u.partial.attrs[i].name;
            if (k->len == key->n && memcmp(k->data, key->s, key->n) == 0) {
                node->u.partial.attrs[i].expr = val;
                dup = true;
            }
        }
        if (!dup) {
            if (node->u.partial.nattrs == cap) {
                size_t nc = cap ? cap * 2 : 4;
                lq_attr *na = lq_arena_alloc(p->A, nc * sizeof(lq_attr));
                if (node->u.partial.nattrs)
                    memcpy(na, node->u.partial.attrs, node->u.partial.nattrs * sizeof(lq_attr));
                node->u.partial.attrs = na;
                cap = nc;
            }
            node->u.partial.attrs[node->u.partial.nattrs].name = mkstr(p, key->s, key->n);
            node->u.partial.attrs[node->u.partial.nattrs].expr = val;
            node->u.partial.nattrs++;
        }
        accept(p, T_COMMA);
    }
    consume(p, T_EOS);
    end_markup(p);
    return node;
}

/* raw / doc bodies: consume tokens verbatim until the end tag */
static lq_node *parse_verbatim(P *p, tokenizer *t, sv markup, int line, bool is_doc) {
    if (!all_regex_space(markup.s, markup.n)) {
        if (is_doc) perr(p, "Syntax Error in 'doc' - Valid syntax: {%% doc %%}{%% enddoc %%}");
        perr(p, "Syntax Error in 'raw' - Valid syntax: raw");
    }
    lq_node *node = new_node(p, is_doc ? N_NOOP : N_RAW, line);
    lq_buf body;
    lq_buf_init(&body);
    const char *tok;
    size_t n;
    const char *endname = is_doc ? "enddoc" : "endraw";
    bool closed = false;
    while (tok_shift(t, &tok, &n)) {
        sv before, name;
        bool m = match_possibly_invalid(tok, n, &before, &name);
        if (is_doc && m && sv_eq(name.s, name.n, "doc")) {
            lq_buf_free(&body);
            perr(p, "Syntax Error in 'doc' - Nested doc tags are not allowed");
        }
        if (m && sv_eq(name.s, name.n, endname)) {
            p->trim = n >= 3 && tok[n - 3] == '-';
            lq_buf_put(&body, before.s, before.n);
            closed = true;
            break;
        }
        lq_buf_put(&body, tok, n);
    }
    if (!closed) {
        lq_buf_free(&body);
        raise_never_closed(p, is_doc ? "doc" : "raw");
    }
    node->u.text.s = lq_arena_strndup(p->A, body.p ? body.p : "", body.len);
    node->u.text.len = body.len;
    node->blank = body.len == 0;
    lq_buf_free(&body);
    return node;
}

/* LiquidTagToken: /\A\s*(#|\w+)\s*(.*?)\z/ */
static bool match_liquid_line(const char *s, size_t n, sv *name, sv *markup) {
    size_t i = 0;
    while (i < n && lq_regex_space((unsigned char)s[i])) i++;
    size_t ns = i;
    if (i < n && s[i] == '#') i++;
    else
        while (i < n && is_word((unsigned char)s[i])) i++;
    if (i == ns) return false;
    name->s = s + ns;
    name->n = i - ns;
    while (i < n && lq_regex_space((unsigned char)s[i])) i++;
    markup->s = s + i;
    markup->n = n - i;
    return true;
}

static lq_node *parse_comment(P *p, tokenizer *t, int line) {
    if (p->depth >= MAX_BLOCK_DEPTH) raise_nesting(p);
    lq_node *node = new_node(p, N_NOOP, line);
    node->blank = true;
    int depth = 1;
    const char *tok;
    size_t n;
    while (tok_shift(t, &tok, &n)) {
        sv name = {NULL, 0}, markup;
        if (t->liquid) {
            if (n == 0 || all_regex_space(tok, n)) continue;
            if (!match_liquid_line(tok, n, &name, &markup)) continue;
        } else {
            sv ws1, ws3;
            if (!match_full_token(tok, n, &ws1, &name, &ws3, &markup)) name.s = NULL;
        }
        if (!name.s) continue;
        if (sv_eq(name.s, name.n, "raw")) {
            bool closed = false;
            const char *rt;
            size_t rn;
            while (tok_shift(t, &rt, &rn)) {
                sv before, nm;
                if (match_possibly_invalid(rt, rn, &before, &nm) && sv_eq(nm.s, nm.n, "endraw")) {
                    closed = true;
                    break;
                }
            }
            if (!closed) raise_never_closed(p, "raw");
        } else if (sv_eq(name.s, name.n, "comment")) {
            depth++;
        } else if (sv_eq(name.s, name.n, "endcomment")) {
            depth--;
        }
        if (depth == 0) {
            if (!t->liquid) p->trim = n >= 3 && tok[n - 3] == '-';
            return node;
        }
    }
    raise_never_closed(p, "comment");
}

static lq_node *parse_inline_comment(P *p, sv markup, int line) {
    /* /\n\s*[^#\s]/ */
    for (size_t i = 0; i < markup.n; i++) {
        if (markup.s[i] != '\n') continue;
        size_t j = i + 1;
        while (j < markup.n && lq_regex_space((unsigned char)markup.s[j])) j++;
        /* \s* is greedy but may backtrack: any following non-# non-space char */
        if (j < markup.n && markup.s[j] != '#') {
            perr(p, "Syntax error in tag '#' - Each line of comments must be prefixed by the '#' character");
        }
    }
    lq_node *node = new_node(p, N_NOOP, line);
    node->blank = true;
    return node;
}

static lq_node *parse_ifchanged(P *p, tokenizer *t, int line) {
    lq_node *node = new_node(p, N_IFCHANGED, line);
    node->blank = true;
    node->u.body = new_body(p);
    endtag et;
    while (block_parse_body(p, t, node, "ifchanged", node->u.body, &et)) {
        sv nm = {et.name, et.nlen};
        raise_unknown_tag(p, nm, "ifchanged");
    }
    return node;
}

static lq_node *parse_simple_block(P *p, tokenizer *t, sv tag, int line) {
    (void)tag;
    return parse_ifchanged(p, t, line);
}

/* Parse a known tag; returns NULL if the tag name is not registered. */
static lq_node *parse_tag(P *p, tokenizer *t, sv name, sv markup) {
    int line = p->line;
    if (sv_eq(name.s, name.n, "if")) return parse_if(p, t, name, markup, line, false);
    if (sv_eq(name.s, name.n, "unless")) return parse_if(p, t, name, markup, line, true);
    if (sv_eq(name.s, name.n, "for")) return parse_for(p, t, markup, line);
    if (sv_eq(name.s, name.n, "assign")) return parse_assign(p, markup, line);
    if (sv_eq(name.s, name.n, "echo")) {
        lq_node *node = new_node(p, N_ECHO, line);
        begin_markup(p, markup.s, markup.n, line, true);
        node->u.var = parse_variable_markup(p, markup.s, markup.n, line);
        end_markup(p);
        return node;
    }
    if (sv_eq(name.s, name.n, "case")) return parse_case(p, t, markup, line);
    if (sv_eq(name.s, name.n, "capture")) return parse_capture(p, t, markup, line);
    if (sv_eq(name.s, name.n, "cycle")) return parse_cycle(p, markup, line);
    if (sv_eq(name.s, name.n, "increment")) return parse_counter(p, markup, line, true);
    if (sv_eq(name.s, name.n, "decrement")) return parse_counter(p, markup, line, false);
    if (sv_eq(name.s, name.n, "include")) return parse_partial(p, markup, line, false);
    if (sv_eq(name.s, name.n, "render")) return parse_partial(p, markup, line, true);
    if (sv_eq(name.s, name.n, "tablerow")) return parse_tablerow(p, t, markup, line);
    if (sv_eq(name.s, name.n, "raw")) return parse_verbatim(p, t, markup, line, false);
    if (sv_eq(name.s, name.n, "doc")) return parse_verbatim(p, t, markup, line, true);
    if (sv_eq(name.s, name.n, "comment")) return parse_comment(p, t, line);
    if (sv_eq(name.s, name.n, "#")) return parse_inline_comment(p, markup, line);
    if (sv_eq(name.s, name.n, "ifchanged")) return parse_simple_block(p, t, name, line);
    if (sv_eq(name.s, name.n, "break")) return new_node(p, N_BREAK, line);
    if (sv_eq(name.s, name.n, "continue")) return new_node(p, N_CONTINUE, line);
    return NULL;
}

static void parse_liquid_tag(P *p, lq_body *b, sv markup);

static void body_parse_liquid(P *p, tokenizer *t, lq_body *b, endtag *out) {
    const char *tok;
    size_t n;
    while (tok_shift(t, &tok, &n)) {
        if (n && !all_regex_space(tok, n)) {
            sv name, markup;
            if (!match_liquid_line(tok, n, &name, &markup)) {
                out->eof = false;
                out->name = tok;
                out->nlen = n;
                out->markup = tok;
                out->mlen = n;
                return;
            }
            if (sv_eq(name.s, name.n, "liquid")) {
                p->line = line_add(p->line, -1);
                parse_liquid_tag(p, b, markup);
                continue;
            }
            lq_node *node = parse_tag(p, t, name, markup);
            if (!node) {
                out->eof = false;
                out->name = name.s;
                out->nlen = name.n;
                out->markup = markup.s;
                out->mlen = markup.n;
                return;
            }
            b->blank = b->blank && node->blank;
            body_push(p, b, node);
        }
        p->line = t->line;
    }
    out->eof = true;
}

static void parse_liquid_tag(P *p, lq_body *b, sv markup) {
    if (p->depth >= MAX_BLOCK_DEPTH) raise_nesting(p);
    tokenizer lt;
    tok_init_liquid(&lt, markup.s, markup.n, p->line);
    endtag et;
    p->depth++;
    body_parse_liquid(p, &lt, b, &et);
    p->depth--;
    if (!et.eof) {
        sv nm = {et.name, et.nlen};
        raise_unknown_tag(p, nm, "liquid");
    }
}

static void body_parse(P *p, tokenizer *t, lq_body *b, endtag *out) {
    p->line = t->line;
    if (t->liquid) {
        body_parse_liquid(p, t, b, out);
        return;
    }
    const char *tok;
    size_t n;
    while (tok_shift(t, &tok, &n)) {
        if (n == 0) continue;
        if (n >= 2 && tok[0] == '{' && tok[1] == '%') {
            whitespace_handler(p, b, tok, n);
            sv ws1, name, ws3, markup;
            if (!match_full_token(tok, n, &ws1, &name, &ws3, &markup)) {
                if (n >= 2 && tok[n - 2] == '%' && tok[n - 1] == '}') {
                    out->eof = false;
                    out->name = tok;
                    out->nlen = n;
                    out->markup = tok;
                    out->mlen = n;
                    return;
                }
                lq_buf eb;
                lq_buf_init(&eb);
                lq_buf_puts(&eb, "Tag '");
                lq_buf_put(&eb, tok, n);
                lq_buf_puts(&eb, "' was not properly terminated with regexp: /\\%\\}/");
                perr_buf(p, LQ_ERR_SYNTAX, &eb);
            }
            p->line = line_add(line_add(p->line, count_nl(ws1.s, ws1.n)), count_nl(ws3.s, ws3.n));
            if (sv_eq(name.s, name.n, "liquid")) {
                parse_liquid_tag(p, b, markup);
                continue;
            }
            lq_node *node = parse_tag(p, t, name, markup);
            if (!node) {
                out->eof = false;
                out->name = name.s;
                out->nlen = name.n;
                out->markup = markup.s;
                out->mlen = markup.n;
                return;
            }
            b->blank = b->blank && node->blank;
            body_push(p, b, node);
        } else if (n >= 2 && tok[0] == '{' && tok[1] == '{') {
            whitespace_handler(p, b, tok, n);
            if (!(n >= 4 && tok[n - 2] == '}' && tok[n - 1] == '}')) {
                lq_buf eb;
                lq_buf_init(&eb);
                lq_buf_puts(&eb, "Variable '");
                lq_buf_put(&eb, tok, n);
                lq_buf_puts(&eb, "' was not properly terminated with regexp: /\\}\\}/");
                perr_buf(p, LQ_ERR_SYNTAX, &eb);
            }
            long i = 2;
            if (tok[i] == '-') i = 3;
            long pe = (long)n - 3;
            if (tok[pe] == '-') pe--;
            long mlen = pe - i + 1;
            const char *m = tok + i;
            size_t ml = mlen <= 0 ? 0 : (size_t)mlen;
            lq_node *node = new_node(p, N_VAR, p->line);
            begin_markup(p, m, ml, p->line, true);
            node->u.var = parse_variable_markup(p, m, ml, p->line);
            end_markup(p);
            body_push(p, b, node);
            b->blank = false;
        } else {
            const char *s = tok;
            size_t len = n;
            if (p->trim) {
                while (len && lq_ruby_space((unsigned char)*s)) {
                    s++;
                    len--;
                }
            }
            p->trim = false;
            lq_node *node = new_node(p, N_TEXT, p->line);
            node->u.text.s = s; /* points into the template source copy */
            node->u.text.len = len;
            body_push(p, b, node);
            b->blank = b->blank && all_regex_space(s, len);
        }
        p->line = t->line;
    }
    out->eof = true;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

lq_template *lq_template_parse(const char *src, size_t len, bool line_numbers, lq_error_info *err) {
    lq_error_info_clear(err);
    if (!lq_utf8_valid(src, len)) {
        err->kind = LQ_ERR_ENCODING;
        err->msg = lq_xstrndup("Invalid template encoding", 25);
        return NULL;
    }
    lq_template *t = lq_xcalloc(1, sizeof(lq_template));
    lq_arena_init(&t->arena);
    /* keep a private copy of the source: text nodes point into it */
    char *copy = lq_arena_strndup(&t->arena, src, len);
    P *p = lq_xcalloc(1, sizeof(P));
    p->A = &t->arena;
    lq_arena_init(&p->tmp);
    p->err = err;
    p->line_numbers = line_numbers;
    p->line = 1;
    bool ok = false;
    if (setjmp(p->jb) == 0) {
        tokenizer tk;
        tok_init_doc(&tk, copy, len, 1);
        t->root = new_body(p);
        endtag et;
        body_parse(p, &tk, t->root, &et);
        if (!et.eof) {
            sv nm = {et.name, et.nlen};
            if (sv_eq(nm.s, nm.n, "else") || sv_eq(nm.s, nm.n, "end")) {
                lq_buf b;
                lq_buf_init(&b);
                lq_buf_puts(&b, "Unexpected outer '");
                lq_buf_put(&b, nm.s, nm.n);
                lq_buf_puts(&b, "' tag");
                p->in_markup = false;
                perr_buf(p, LQ_ERR_SYNTAX, &b);
            }
            /* Document#unknown_tag */
            lq_buf b;
            lq_buf_init(&b);
            lq_buf_puts(&b, "Unknown tag '");
            lq_buf_put(&b, nm.s, nm.n);
            lq_buf_putc(&b, '\'');
            p->in_markup = false;
            perr_buf(p, LQ_ERR_SYNTAX, &b);
        }
        ok = true;
    }
    free(p->toks);
    lq_arena_free(&p->tmp);
    free(p);
    if (!ok) {
        lq_template_free(t);
        return NULL;
    }
    /* literals become read-only: rendering never writes template memory */
    for (size_t i = 0; i < t->arena.nvals; i++) lq_pin(t->arena.vals[i]);
    return t;
}

void lq_template_free(lq_template *t) {
    if (!t) return;
    lq_arena_free(&t->arena);
    free(t);
}
