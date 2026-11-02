/*
 * fc_abi.c - the C ABI declared in harness/liquid_fc.h, as a thin wrapper
 * over the public liquid.h API.
 */
#include "../harness/liquid_fc.h"
#include "../include/liquid.h"

#include <stdlib.h>
#include <string.h>

struct lfc_template {
    liquid_template *t;
};

struct lfc_context {
    liquid_data *assigns;
    liquid_data *partials; /* NULL = none */
};

static void set_str(lfc_string *dst, const char *s) {
    if (!dst) return;
    size_t n = s ? strlen(s) : 0;
    char *p = malloc(n + 1);
    if (!p) {
        dst->ptr = NULL;
        dst->len = 0;
        return;
    }
    if (n) memcpy(p, s, n);
    p[n] = 0;
    dst->ptr = p;
    dst->len = n;
}

static void clear_str(lfc_string *s) {
    if (!s) return;
    s->ptr = NULL;
    s->len = 0;
}

lfc_template *lfc_parse(const char *source, size_t source_len, lfc_string *error) {
    clear_str(error);
    liquid_error err = {0};
    liquid_parse_options po = {1};
    liquid_template *t = liquid_parse(source ? source : "", source ? source_len : 0, &po, &err);
    if (!t) {
        set_str(error, err.message);
        liquid_error_clear(&err);
        return NULL;
    }
    lfc_template *r = malloc(sizeof *r);
    if (!r) {
        liquid_template_free(t);
        set_str(error, "Liquid error: out of memory");
        return NULL;
    }
    r->t = t;
    return r;
}

lfc_context *lfc_context_new(const char *assigns_json, size_t assigns_len, const char *partials_json,
                             size_t partials_len, lfc_string *error) {
    clear_str(error);
    liquid_error err = {0};
    liquid_data *assigns;
    if (!assigns_json || assigns_len == 0) assigns = liquid_data_from_json("{}", 2, NULL, &err);
    else assigns = liquid_data_from_json(assigns_json, assigns_len, NULL, &err);
    if (!assigns) {
        set_str(error, err.message);
        liquid_error_clear(&err);
        return NULL;
    }
    if (!liquid_data_is_object(assigns)) {
        liquid_data_free(assigns);
        set_str(error, "assigns must be a JSON object");
        return NULL;
    }
    liquid_data *partials = NULL;
    if (partials_json && partials_len) {
        partials = liquid_data_from_json(partials_json, partials_len, NULL, &err);
        if (!partials) {
            liquid_data_free(assigns);
            set_str(error, err.message);
            liquid_error_clear(&err);
            return NULL;
        }
        if (!liquid_data_is_object(partials)) {
            liquid_data_free(assigns);
            liquid_data_free(partials);
            set_str(error, "partials must be a JSON object");
            return NULL;
        }
    }
    lfc_context *c = malloc(sizeof *c);
    if (!c) {
        liquid_data_free(assigns);
        liquid_data_free(partials);
        set_str(error, "out of memory");
        return NULL;
    }
    c->assigns = assigns;
    c->partials = partials;
    return c;
}

int lfc_render(const lfc_template *tpl, const lfc_context *ctx, lfc_string *output, lfc_string *error) {
    clear_str(output);
    clear_str(error);
    if (!tpl || !ctx) {
        set_str(error, "Liquid error: missing template or context");
        return 1;
    }
    liquid_render_options o;
    liquid_render_options_init(&o);
    o.strict_errors = 1;
    o.file_system = ctx->partials;
    liquid_error err = {0};
    char *out = NULL;
    size_t len = 0;
    if (liquid_render(tpl->t, ctx->assigns, &o, &out, &len, &err) != 0) {
        set_str(error, err.message);
        liquid_error_clear(&err);
        return 1;
    }
    if (output) {
        output->ptr = out;
        output->len = len;
    } else {
        free(out);
    }
    return 0;
}

void lfc_template_free(lfc_template *tpl) {
    if (!tpl) return;
    liquid_template_free(tpl->t);
    free(tpl);
}

void lfc_context_free(lfc_context *ctx) {
    if (!ctx) return;
    liquid_data_free(ctx->assigns);
    liquid_data_free(ctx->partials);
    free(ctx);
}

void lfc_string_free(lfc_string s) { free(s.ptr); }
