/* bench FILE.jsonl - time parse and render of each case through the C ABI.
   Each line: {"name","template","environment","filesystem"}. */
#define _POSIX_C_SOURCE 200809L
#include "../harness/liquid_fc.h"
#include "../src/lq.h"
#include "../src/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static lq_value field(lq_value o, const char *k) {
    lq_value v = lq_nil();
    if (o.type == LQ_HASH) lq_hash_get_cstr(o.u.h, k, &v);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    double budget = argc > 2 ? atof(argv[2]) : 1.0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    double total_parse = 0, total_render = 0;
    while ((n = getline(&line, &cap, f)) > 0) {
        lq_value c;
        char *err = NULL;
        if (!lq_json_parse(line, (size_t)n, NULL, &c, &err)) { free(err); continue; }
        lq_value name = field(c, "name"), tpl = field(c, "template");
        lq_buf env, fs;
        lq_buf_init(&env);
        lq_buf_init(&fs);
        lq_json_write_value(&env, field(c, "environment"), 0);
        lq_json_write_value(&fs, field(c, "filesystem"), 0);
        lfc_context *ctx = lfc_context_new(env.p, env.len, fs.p, fs.len, NULL);
        /* parse timing */
        int iters = 0;
        double t0 = now_s(), t1;
        do {
            lfc_template *t = lfc_parse(tpl.u.s->data, tpl.u.s->len, NULL);
            lfc_template_free(t);
            iters++;
            t1 = now_s();
        } while (t1 - t0 < budget / 2);
        double parse_us = (t1 - t0) / iters * 1e6;
        lfc_template *t = lfc_parse(tpl.u.s->data, tpl.u.s->len, NULL);
        iters = 0;
        size_t outlen = 0;
        t0 = now_s();
        do {
            lfc_string o = {0}, e = {0};
            lfc_render(t, ctx, &o, &e);
            outlen = o.len;
            lfc_string_free(o);
            lfc_string_free(e);
            iters++;
            t1 = now_s();
        } while (t1 - t0 < budget / 2);
        double render_us = (t1 - t0) / iters * 1e6;
        total_parse += parse_us;
        total_render += render_us;
        printf("%-45.45s parse %9.1f us  render %9.1f us  (%zu bytes)\n", name.type == LQ_STR ? name.u.s->data : "?",
               parse_us, render_us, outlen);
        lfc_template_free(t);
        lfc_context_free(ctx);
        lq_buf_free(&env);
        lq_buf_free(&fs);
        lq_release(c);
    }
    printf("TOTAL parse %.1f us  render %.1f us\n", total_parse, total_render);
    free(line);
    fclose(f);
    return 0;
}
