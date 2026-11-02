/* Render one template + context from several threads at once (run under
   -fsanitize=thread) to check that rendering never writes shared state. */
#include "../harness/liquid_fc.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static lfc_template *T;
static lfc_context *C;
static char expected[65536];
static size_t expected_len;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 200; i++) {
        lfc_string o = {0}, e = {0};
        lfc_render(T, C, &o, &e);
        if (o.len != expected_len || memcmp(o.ptr, expected, o.len)) { fprintf(stderr, "MISMATCH\n"); }
        lfc_string_free(o);
        lfc_string_free(e);
    }
    return NULL;
}

int main(void) {
    const char *src =
        "{% for p in products %}{{ forloop.index }}:{{ p.title | upcase | append: '!' }}"
        "{% cycle 'a','b' %}{% if p.price > 10 %}{{ p.price | times: 1.5 | round: 2 }}{% endif %}"
        "{% render 'card', item: p %}{% include 'card' %}{% increment c %}{% endfor %}"
        "{{ products | map: 'title' | sort | join: ',' }}{{ 'now' | date: '%Y' }}{{ self.products.size }}"
        "{% capture x %}{{ products | size }}{% endcapture %}{{ x | plus: 1 }}{{ big | plus: 1 }}";
    const char *assigns = "{\"products\":[{\"title\":\"b\",\"price\":12.5},{\"title\":\"a\",\"price\":3}],"
                          "\"big\":123456789012345678901234567890,\"item\":{\"x\":1}}";
    const char *partials = "{\"card\":\"[{{ item.title }}{{ p.title }}]\"}";
    T = lfc_parse(src, strlen(src), NULL);
    C = lfc_context_new(assigns, strlen(assigns), partials, strlen(partials), NULL);
    lfc_string o = {0}, e = {0};
    if (lfc_render(T, C, &o, &e)) { printf("error: %.*s\n", (int)e.len, e.ptr); return 1; }
    memcpy(expected, o.ptr, o.len);
    expected_len = o.len;
    printf("%.*s\n", (int)o.len, o.ptr);
    lfc_string_free(o);
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    lfc_template_free(T);
    lfc_context_free(C);
    puts("done");
    return 0;
}
