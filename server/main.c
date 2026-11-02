/*
 * liquid-server - JSON-RPC 2.0 server (newline-delimited, stdin/stdout)
 * used by liquid-spec. A thin wrapper around the liquid library.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/lq.h"
#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct tpl_entry {
    char *id;
    liquid_template *tpl;
    liquid_data *fs; /* may be NULL */
} tpl_entry;

static tpl_entry *g_tpls;
static size_t g_ntpls, g_captpls;
static long g_next_tpl = 1;
static long g_next_cb = 1000000;

static void write_line(lq_buf *b) {
    fwrite(b->p ? b->p : "", 1, b->len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void begin_response(lq_buf *b, lq_value id) {
    lq_buf_puts(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    lq_json_write_value(b, id, 0);
}

static void send_error(lq_value id, int code, const char *msg) {
    lq_buf b;
    lq_buf_init(&b);
    begin_response(&b, id);
    lq_buf_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    lq_json_write_str(&b, msg, strlen(msg));
    lq_buf_puts(&b, "}}");
    write_line(&b);
    lq_buf_free(&b);
}

static lq_value get(lq_value obj, const char *key) {
    lq_value v = lq_nil();
    if (obj.type == LQ_HASH) lq_hash_get_cstr(obj.u.h, key, &v);
    return v;
}

/* ---- drop callbacks ---- */

static char *read_line(size_t *len) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return NULL;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    *len = (size_t)n;
    return line;
}

static char *callback(const char *method, const char *drop_id, const char *prop, size_t plen, const char *field,
                      size_t *out_len) {
    long id = g_next_cb++;
    lq_buf b;
    lq_buf_init(&b);
    lq_buf_printf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"method\":\"%s\",\"params\":{\"drop_id\":", id, method);
    lq_json_write_str(&b, drop_id, strlen(drop_id));
    if (prop) {
        lq_buf_puts(&b, ",\"property\":");
        lq_json_write_str(&b, prop, plen);
    }
    lq_buf_puts(&b, "}}");
    write_line(&b);
    lq_buf_free(&b);
    for (;;) {
        size_t n;
        char *line = read_line(&n);
        if (!line) return NULL;
        lq_value msg;
        char *err = NULL;
        if (!lq_json_parse(line, n, NULL, &msg, &err)) {
            free(err);
            free(line);
            continue;
        }
        free(line);
        lq_value rid = get(msg, "id");
        if (rid.type == LQ_INT && rid.u.i == id) {
            lq_value result = get(msg, "result");
            lq_value v = get(result, field);
            lq_buf o;
            lq_buf_init(&o);
            lq_json_write_value(&o, v, 0);
            lq_release(msg);
            return lq_buf_detach(&o, out_len);
        }
        lq_release(msg);
    }
}

static char *cb_get(void *ud, const char *drop_id, const char *prop, size_t plen, size_t *out_len) {
    (void)ud;
    return callback("drop_get", drop_id, prop, plen, "value", out_len);
}

static char *cb_iterate(void *ud, const char *drop_id, size_t *out_len) {
    (void)ud;
    return callback("drop_iterate", drop_id, NULL, 0, "items", out_len);
}

static const liquid_drop_host g_host = {"_rpc_drop", NULL, cb_get, cb_iterate};

/* ---- methods ---- */

static void handle_compile(lq_value id, lq_value params) {
    lq_value tsrc = get(params, "template");
    if (tsrc.type != LQ_STR) {
        send_error(id, -32602, "Missing required parameter 'template' in compile request");
        return;
    }
    liquid_parse_options po = {1};
    liquid_error err = {0};
    liquid_template *t = liquid_parse(tsrc.u.s->data, tsrc.u.s->len, &po, &err);
    lq_buf b;
    lq_buf_init(&b);
    begin_response(&b, id);
    if (!t) {
        lq_buf_puts(&b, ",\"result\":{\"template_id\":null,\"error\":{\"type\":\"parse_error\",\"message\":");
        lq_json_write_str(&b, err.message, strlen(err.message));
        if (err.line) lq_buf_printf(&b, ",\"line\":%d", err.line);
        lq_buf_puts(&b, "}}}");
        liquid_error_clear(&err);
    } else {
        if (g_ntpls == g_captpls) {
            g_captpls = g_captpls ? g_captpls * 2 : 64;
            g_tpls = lq_xrealloc(g_tpls, g_captpls * sizeof(tpl_entry));
        }
        tpl_entry *e = &g_tpls[g_ntpls++];
        char idbuf[32];
        snprintf(idbuf, sizeof idbuf, "t%ld", g_next_tpl++);
        e->id = lq_xstrndup(idbuf, strlen(idbuf));
        e->tpl = t;
        lq_value fs = get(params, "filesystem");
        e->fs = fs.type == LQ_HASH ? lq_data_wrap(fs) : NULL;
        lq_buf_puts(&b, ",\"result\":{\"template_id\":");
        lq_json_write_str(&b, e->id, strlen(e->id));
        lq_buf_puts(&b, "}}");
    }
    write_line(&b);
    lq_buf_free(&b);
}

static int64_t get_int(lq_value obj, const char *key) {
    lq_value v = get(obj, key);
    if (v.type == LQ_INT) return v.u.i;
    if (v.type == LQ_FLOAT) return (int64_t)v.u.f;
    return 0;
}

static void handle_render(lq_value id, lq_value params) {
    lq_value tid = get(params, "template_id");
    tpl_entry *e = NULL;
    if (tid.type == LQ_STR)
        for (size_t i = 0; i < g_ntpls; i++)
            if (strcmp(g_tpls[i].id, tid.u.s->data) == 0) e = &g_tpls[i];
    if (!e) {
        send_error(id, -32602, "Unknown template_id. Call compile first.");
        return;
    }
    lq_value env = get(params, "environment");
    lq_value opts = get(params, "options");
    liquid_render_options o;
    liquid_render_options_init(&o);
    lq_value se = get(opts, "strict_errors");
    o.strict_errors = se.type == LQ_BOOL ? se.u.b : 1;
    o.file_system = e->fs;
    lq_value regs = get(opts, "registers");
    lq_value now = get(regs, "current_time");
    if (now.type == LQ_STR) {
        int64_t ep;
        int32_t off;
        if (liquid_parse_time(now.u.s->data, now.u.s->len, &ep, &off)) {
            o.has_current_time = 1;
            o.current_time = ep;
            o.current_time_offset = off;
        }
    }
    lq_value rl = get(opts, "resource_limits");
    if (rl.type == LQ_HASH) {
        o.render_score_limit = get_int(rl, "render_score_limit");
        o.cumulative_render_score_limit = get_int(rl, "cumulative_render_score_limit");
        o.assign_score_limit = get_int(rl, "assign_score_limit");
        o.cumulative_assign_score_limit = get_int(rl, "cumulative_assign_score_limit");
        o.render_length_limit = get_int(rl, "render_length_limit");
    }
    liquid_data *envd = lq_data_wrap(env.type == LQ_HASH ? env : lq_nil());
    char *out = NULL;
    size_t len = 0;
    liquid_error err = {0};
    int rc = liquid_render(e->tpl, envd, &o, &out, &len, &err);
    liquid_data_free(envd);
    lq_buf b;
    lq_buf_init(&b);
    begin_response(&b, id);
    if (rc) {
        lq_buf_puts(&b, ",\"error\":{\"code\":-32001,\"message\":\"Render error\",\"data\":{\"type\":\"render_error\",\"message\":");
        lq_json_write_str(&b, err.message, strlen(err.message));
        if (err.line) lq_buf_printf(&b, ",\"line\":%d", err.line);
        lq_buf_puts(&b, "}}}");
        liquid_error_clear(&err);
    } else {
        lq_buf_puts(&b, ",\"result\":{\"output\":");
        lq_json_write_str(&b, out, len);
        lq_buf_puts(&b, "}}");
        free(out);
    }
    write_line(&b);
    lq_buf_free(&b);
}

int main(void) {
    for (;;) {
        size_t n;
        char *line = read_line(&n);
        if (!line) break;
        if (n == 0) {
            free(line);
            continue;
        }
        lq_json_opts jo = {&g_host};
        lq_value msg;
        char *err = NULL;
        if (!lq_json_parse(line, n, &jo, &msg, &err)) {
            free(line);
            send_error(lq_nil(), -32700, err ? err : "Parse error");
            free(err);
            continue;
        }
        free(line);
        lq_value id = get(msg, "id");
        lq_value method = get(msg, "method");
        lq_value params = get(msg, "params");
        if (method.type != LQ_STR) {
            send_error(id, -32600, "Invalid request");
        } else if (strcmp(method.u.s->data, "initialize") == 0) {
            lq_buf b;
            lq_buf_init(&b);
            begin_response(&b, id);
            lq_buf_puts(&b, ",\"result\":{\"version\":\"1.0\",\"implementation\":\"liquid-fc\","
                            "\"liquid_version\":\"5.13.0\",\"features\":[\"drops\"]}}");
            write_line(&b);
            lq_buf_free(&b);
        } else if (strcmp(method.u.s->data, "compile") == 0) {
            handle_compile(id, params);
        } else if (strcmp(method.u.s->data, "render") == 0) {
            handle_render(id, params);
        } else if (strcmp(method.u.s->data, "quit") == 0) {
            lq_release(msg);
            break;
        } else {
            send_error(id, -32601, "Method not found");
        }
        lq_release(msg);
    }
    for (size_t i = 0; i < g_ntpls; i++) {
        free(g_tpls[i].id);
        liquid_template_free(g_tpls[i].tpl);
        liquid_data_free(g_tpls[i].fs);
    }
    free(g_tpls);
    return 0;
}
