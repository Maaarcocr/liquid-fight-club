/*
 * Benchmark driver over the shared C ABI (harness/liquid_fc.h).
 *
 *   bench DIR...   each DIR holds template.liquid, assigns.json, partials.json
 *
 * For each input, times lfc_parse, lfc_context_new and lfc_render separately.
 * Each measurement is the median of 10 rounds; a round repeats the operation
 * enough times to last at least ~50 ms and reports the mean per call.
 * Prints TSV: name, parse_us, context_us, render_us, output_bytes, output_fnv1a.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "liquid_fc.h"

#define ROUNDS 10
#define MIN_ROUND_NS 50000000.0

typedef struct { char *p; size_t n; } buf;

static buf slurp(const char *dir, const char *file) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    buf b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.p = malloc((size_t)len + 1);
    b.n = fread(b.p, 1, (size_t)len, f);
    fclose(f);
    return b;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

enum op { OP_PARSE, OP_CONTEXT, OP_RENDER };

typedef struct {
    buf tpl, assigns, partials;
    lfc_template *t;
    lfc_context *c;
} input;

static void run_once(enum op op, input *in) {
    switch (op) {
    case OP_PARSE: lfc_template_free(lfc_parse(in->tpl.p, in->tpl.n, NULL)); break;
    case OP_CONTEXT:
        lfc_context_free(lfc_context_new(in->assigns.p, in->assigns.n, in->partials.p,
                                         in->partials.n, NULL));
        break;
    case OP_RENDER: {
        lfc_string out = {NULL, 0}, err = {NULL, 0};
        lfc_render(in->t, in->c, &out, &err);
        lfc_string_free(out);
        lfc_string_free(err);
        break;
    }
    }
}

/* Median over ROUNDS of the mean time per call, in microseconds. */
static double measure(enum op op, input *in) {
    long reps = 1;
    for (;;) { /* calibrate */
        double t0 = now_ns();
        for (long i = 0; i < reps; i++) run_once(op, in);
        double dt = now_ns() - t0;
        if (dt >= MIN_ROUND_NS || reps >= (1L << 30)) break;
        reps = dt < 1e5 ? reps * 100 : (long)(reps * MIN_ROUND_NS / dt) + 1;
    }
    double r[ROUNDS];
    for (int k = 0; k < ROUNDS; k++) {
        double t0 = now_ns();
        for (long i = 0; i < reps; i++) run_once(op, in);
        r[k] = (now_ns() - t0) / reps / 1e3;
    }
    qsort(r, ROUNDS, sizeof r[0], cmp_double);
    return (r[ROUNDS / 2 - 1] + r[ROUNDS / 2]) / 2;
}

int main(int argc, char **argv) {
    printf("name\tparse_us\tcontext_us\trender_us\toutput_bytes\toutput_fnv1a\n");
    for (int a = 1; a < argc; a++) {
        const char *dir = argv[a];
        const char *name = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
        input in = {slurp(dir, "template.liquid"), slurp(dir, "assigns.json"),
                    slurp(dir, "partials.json"), NULL, NULL};
        lfc_string err = {NULL, 0};
        in.t = lfc_parse(in.tpl.p, in.tpl.n, &err);
        if (!in.t) {
            printf("%s\tPARSE_ERROR: %.*s\n", name, (int)err.len, err.ptr);
            lfc_string_free(err);
            continue;
        }
        in.c = lfc_context_new(in.assigns.p, in.assigns.n, in.partials.p, in.partials.n, &err);
        if (!in.c) {
            printf("%s\tCONTEXT_ERROR: %.*s\n", name, (int)err.len, err.ptr);
            lfc_string_free(err);
            continue;
        }
        lfc_string out = {NULL, 0}, rerr = {NULL, 0};
        int rc = lfc_render(in.t, in.c, &out, &rerr);
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < out.len; i++) h = (h ^ (unsigned char)out.ptr[i]) * 1099511628211ULL;
        size_t outlen = out.len;
        if (rc != 0) printf("# %s render error: %.*s\n", name, (int)rerr.len, rerr.ptr);
        lfc_string_free(out);
        lfc_string_free(rerr);
        double p = measure(OP_PARSE, &in), c = measure(OP_CONTEXT, &in), r = measure(OP_RENDER, &in);
        printf("%s\t%.2f\t%.2f\t%.2f\t%zu\t%016llx\n", name, p, c, r, outlen, (unsigned long long)h);
        fflush(stdout);
        lfc_template_free(in.t);
        lfc_context_free(in.c);
        free(in.tpl.p);
        free(in.assigns.p);
        free(in.partials.p);
    }
    return 0;
}
