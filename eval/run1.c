/*
 * Parse and render one template through the C ABI and report what happened.
 *   run1 TEMPLATE_FILE [PARTIALS_JSON_FILE]
 * Assigns are an empty object. Prints one line: status, output bytes and the
 * first 120 bytes of the error message, if any.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "liquid_fc.h"

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)len + 1);
    *n = fread(p, 1, (size_t)len, f);
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: run1 TEMPLATE [PARTIALS_JSON]\n"); return 2; }
    size_t tn, pn = 2;
    char *tpl_src = slurp(argv[1], &tn);
    char *partials = argc > 2 ? slurp(argv[2], &pn) : strdup("{}");
    lfc_string err = {NULL, 0};
    lfc_template *t = lfc_parse(tpl_src, tn, &err);
    if (!t) {
        printf("parse_error\t0\t%.*s\n", (int)(err.len > 120 ? 120 : err.len), err.ptr);
        lfc_string_free(err);
        return 0;
    }
    lfc_context *c = lfc_context_new("{}", 2, partials, pn, &err);
    if (!c) {
        printf("context_error\t0\t%.*s\n", (int)(err.len > 120 ? 120 : err.len), err.ptr);
        return 0;
    }
    lfc_string out = {NULL, 0};
    int rc = lfc_render(t, c, &out, &err);
    if (rc != 0)
        printf("render_error\t0\t%.*s\n", (int)(err.len > 120 ? 120 : err.len), err.ptr);
    else
        printf("ok\t%zu\t\n", out.len);
    lfc_string_free(out);
    lfc_string_free(err);
    lfc_template_free(t);
    lfc_context_free(c);
    free(tpl_src);
    free(partials);
    return 0;
}
