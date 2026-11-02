/*
 * api.c - the public liquid.h API.
 */
#include "lq.h"
#include "internal.h"

#include <stdlib.h>

void liquid_error_clear(liquid_error *err) {
    if (!err) return;
    free(err->message);
    free(err->core);
    memset(err, 0, sizeof *err);
}

static void fill_error(liquid_error *err, liquid_error_type type, const lq_error_info *info) {
    if (!err) return;
    liquid_error_clear(err);
    err->type = type;
    err->message = lq_error_format(info);
    lq_buf b;
    lq_buf_init(&b);
    if (info->msg) lq_buf_puts(&b, info->msg);
    if (info->markup_ctx) {
        lq_buf_putc(&b, ' ');
        lq_buf_puts(&b, info->markup_ctx);
    }
    err->core = lq_buf_detach(&b, NULL);
    err->line = info->has_line ? info->line : 0;
}

static void fill_simple(liquid_error *err, liquid_error_type type, const char *msg) {
    if (!err) return;
    liquid_error_clear(err);
    err->type = type;
    err->message = lq_xstrndup(msg, strlen(msg));
    err->core = lq_xstrndup(msg, strlen(msg));
}

liquid_template *liquid_parse(const char *src, size_t len, const liquid_parse_options *opts, liquid_error *err) {
    if (!src) {
        if (len) {
            fill_simple(err, LIQUID_SYNTAX_ERROR, "Liquid error: no template source");
            return NULL;
        }
        src = "";
    }
    bool line_numbers = opts ? opts->line_numbers != 0 : true;
    lq_error_info info;
    memset(&info, 0, sizeof info);
    lq_template *t = lq_template_parse(src, len, line_numbers, &info);
    if (!t) {
        fill_error(err, LIQUID_SYNTAX_ERROR, &info);
        lq_error_info_clear(&info);
        return NULL;
    }
    lq_error_info_clear(&info);
    return t;
}

void liquid_template_free(liquid_template *t) { lq_template_free(t); }

liquid_data *lq_data_wrap(lq_value v) {
    liquid_data *d = lq_xmalloc(sizeof(liquid_data));
    d->v = lq_retain(v);
    return d;
}

liquid_data *liquid_data_from_json(const char *json, size_t len, const liquid_data_options *opts, liquid_error *err) {
    lq_json_opts o = {opts ? opts->drop_host : NULL};
    lq_value v;
    char *msg = NULL;
    if (!json && len) {
        fill_simple(err, LIQUID_DATA_ERROR, "invalid JSON: NULL input");
        return NULL;
    }
    if (!lq_json_parse(json ? json : "", json ? len : 0, &o, &v, &msg)) {
        lq_buf b;
        lq_buf_init(&b);
        lq_buf_puts(&b, "invalid JSON: ");
        lq_buf_puts(&b, msg ? msg : "parse error");
        free(msg);
        char *m = lq_buf_detach(&b, NULL);
        fill_simple(err, LIQUID_DATA_ERROR, m);
        free(m);
        return NULL;
    }
    liquid_data *d = lq_xmalloc(sizeof(liquid_data));
    d->v = v;
    return d;
}

int liquid_data_is_object(const liquid_data *d) { return d && d->v.type == LQ_HASH; }

void liquid_data_free(liquid_data *d) {
    if (!d) return;
    lq_release(d->v);
    free(d);
}

void liquid_render_options_init(liquid_render_options *o) {
    memset(o, 0, sizeof *o);
    o->strict_errors = 1;
}

int liquid_render(const liquid_template *t, const liquid_data *env, const liquid_render_options *opts, char **out,
                  size_t *out_len, liquid_error *err) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!t) {
        fill_simple(err, LIQUID_RENDER_ERROR, "Liquid error: no template");
        return 1;
    }
    liquid_render_options defaults;
    if (!opts) {
        liquid_render_options_init(&defaults);
        opts = &defaults;
    }
    lq_value empty = lq_hash_new(0);
    const lq_hash *h = (env && env->v.type == LQ_HASH) ? env->v.u.h : empty.u.h;
    lq_buf buf;
    lq_buf_init(&buf);
    lq_error_info info;
    memset(&info, 0, sizeof info);
    int rc = lq_render_template(t, h, opts, &buf, &info);
    lq_release(empty);
    if (rc) {
        fill_error(err, LIQUID_RENDER_ERROR, &info);
        lq_error_info_clear(&info);
        lq_buf_free(&buf);
        return 1;
    }
    lq_error_info_clear(&info);
    size_t len;
    char *s = lq_buf_detach(&buf, &len);
    if (out) *out = s;
    else free(s);
    if (out_len) *out_len = len;
    return 0;
}

int liquid_parse_time(const char *s, size_t n, int64_t *epoch, int32_t *utc_offset) {
    if (!s) return 0;
    lq_time t;
    if (!lq_parse_time_text(s, n, &t)) return 0;
    if (epoch) *epoch = t.sec;
    if (utc_offset) *utc_offset = t.off;
    return 1;
}
