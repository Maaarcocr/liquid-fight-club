/* lqrender TEMPLATE [ASSIGNS_JSON [PARTIALS_JSON]] - render via the C ABI */
#include "../harness/liquid_fc.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    lfc_string err = {0};
    lfc_template *t = lfc_parse(argv[1], strlen(argv[1]), &err);
    if (!t) { printf("PARSE ERROR: %.*s\n", (int)err.len, err.ptr); lfc_string_free(err); return 1; }
    const char *a = argc > 2 ? argv[2] : "{}";
    const char *p = argc > 3 ? argv[3] : NULL;
    lfc_context *c = lfc_context_new(a, strlen(a), p, p ? strlen(p) : 0, &err);
    if (!c) { printf("CONTEXT ERROR: %.*s\n", (int)err.len, err.ptr); lfc_string_free(err); lfc_template_free(t); return 1; }
    lfc_string o = {0}, e = {0};
    if (lfc_render(t, c, &o, &e)) printf("RENDER ERROR: %.*s\n", (int)e.len, e.ptr);
    else { fwrite(o.ptr, 1, o.len, stdout); putchar('\n'); }
    lfc_string_free(o); lfc_string_free(e); lfc_context_free(c); lfc_template_free(t);
    return 0;
}
