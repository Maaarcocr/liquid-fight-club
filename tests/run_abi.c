/* Run one fuzz input file through the C ABI (template \0 assigns \0 partials)
   and print the result. Used to reproduce fuzzer findings. */
#include "../harness/liquid_fc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        LLVMFuzzerTestOneInput((const unsigned char *)buf, n);
        const char *z = memchr(buf, 0, n);
        size_t tl = z ? (size_t)(z - buf) : n;
        lfc_string err = {0};
        lfc_template *t = lfc_parse(buf, tl, &err);
        if (!t) { printf("parse error: %.*s\n", (int)err.len, err.ptr); lfc_string_free(err); continue; }
        lfc_context *c = lfc_context_new("{}", 2, NULL, 0, NULL);
        lfc_string o = {0}, e = {0};
        int r = lfc_render(t, c, &o, &e);
        if (r) printf("render error: %.*s\n", (int)e.len, e.ptr);
        else printf("output (%zu bytes): %.200s\n", o.len, o.ptr ? o.ptr : "");
        lfc_string_free(o); lfc_string_free(e); lfc_context_free(c); lfc_template_free(t);
    }
    return 0;
}
