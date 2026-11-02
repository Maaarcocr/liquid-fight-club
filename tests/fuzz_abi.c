/*
 * libFuzzer driver for the liquid_fc.h C ABI.
 *
 * Input layout: template \x00 assigns-json \x00 partials-json
 * Each render is performed twice to check that rendering does not mutate
 * the template or the context.
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined tests/fuzz_abi.c \
 *         src/<all>.c -lm -o build-fuzz/fuzz_abi
 */
#include "../harness/liquid_fc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *p = (const char *)data;
    const char *z1 = memchr(p, 0, size);
    size_t tlen = z1 ? (size_t)(z1 - p) : size;
    const char *a = z1 ? z1 + 1 : NULL;
    size_t alen = 0;
    const char *pt = NULL;
    size_t plen = 0;
    if (a) {
        size_t rem = size - tlen - 1;
        const char *z2 = memchr(a, 0, rem);
        alen = z2 ? (size_t)(z2 - a) : rem;
        if (z2) {
            pt = z2 + 1;
            plen = rem - alen - 1;
        }
    }
    lfc_string err = {0};
    lfc_template *t = lfc_parse(p, tlen, &err);
    lfc_string_free(err);
    if (!t) return 0;
    lfc_context *c = lfc_context_new(a, alen, pt, plen, &err);
    lfc_string_free(err);
    if (!c) c = lfc_context_new("{}", 2, NULL, 0, NULL);
    lfc_string o1 = {0}, e1 = {0}, o2 = {0}, e2 = {0};
    int r1 = lfc_render(t, c, &o1, &e1);
    int r2 = lfc_render(t, c, &o2, &e2);
    if (r1 != r2) abort();
    if (o1.len != o2.len || (o1.len && memcmp(o1.ptr, o2.ptr, o1.len))) abort();
    lfc_string_free(o1);
    lfc_string_free(o2);
    lfc_string_free(e1);
    lfc_string_free(e2);
    lfc_context_free(c);
    lfc_template_free(t);
    return 0;
}
