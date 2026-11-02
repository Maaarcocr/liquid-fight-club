/* Unit tests for the public liquid.h API and the liquid_fc.h ABI. */
#include "../include/liquid.h"
#include "../harness/liquid_fc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            failures++;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

/* render src with JSON env; returns malloc'd output or error message */
static char *render(const char *src, const char *json, liquid_render_options *o, int *rc) {
    liquid_error err = {0};
    liquid_template *t = liquid_parse(src, strlen(src), NULL, &err);
    if (!t) {
        *rc = -1;
        char *m = strdup(err.message);
        liquid_error_clear(&err);
        return m;
    }
    liquid_data *d = liquid_data_from_json(json, strlen(json), NULL, &err);
    char *out = NULL;
    size_t len = 0;
    *rc = liquid_render(t, d, o, &out, &len, &err);
    if (*rc) out = strdup(err.message);
    liquid_error_clear(&err);
    liquid_data_free(d);
    liquid_template_free(t);
    return out;
}

static void expect(const char *src, const char *json, liquid_render_options *o, int want_rc, const char *want) {
    int rc;
    char *got = render(src, json, o, &rc);
    CHECK(rc == want_rc && strcmp(got, want) == 0, "%s => [%d] %s (want [%d] %s)", src, rc, got, want_rc, want);
    free(got);
}

int main(void) {
    liquid_render_options o;
    liquid_render_options_init(&o);

    /* basics */
    expect("Hello {{ name | upcase }}!", "{\"name\":\"world\"}", &o, 0, "Hello WORLD!");
    expect("{% for i in (1..3) %}{{ i }}{% endfor %}", "{}", &o, 0, "123");
    expect("{{ 0.1 | plus: 0.2 }} {{ 10 | divided_by: 3 }} {{ 9223372036854775807 | plus: 1 }}", "{}", &o, 0,
           "0.3 3 9223372036854775808");

    /* errors: raised vs inline */
    expect("a{{ 1 | divided_by: 0 }}b", "{}", &o, 1, "Liquid error (line 1): divided by 0");
    o.strict_errors = 0;
    expect("a{{ 1 | divided_by: 0 }}b", "{}", &o, 0, "aLiquid error (line 1): divided by 0b");
    o.strict_errors = 1;
    expect("{% if %}", "{}", &o, -1, "Liquid syntax error (line 1): [:end_of_string] is not a valid expression in \"\"");

    /* partials */
    liquid_error err = {0};
    const char *fs_json = "{\"card.liquid\":\"[{{ item }}]\",\"Header\":\"H{{ x }}\"}";
    liquid_data *fs = liquid_data_from_json(fs_json, strlen(fs_json), NULL, &err);
    o.file_system = fs;
    expect("{% render 'card' for list as item %}{% include 'header' %}", "{\"list\":[1,2],\"x\":7}", &o, 0, "[1][2]H7");
    expect("{% include 'missing' %}", "{}", &o, 1, "Liquid error (line 1): Could not find asset missing");
    o.file_system = NULL;
    liquid_data_free(fs);

    /* resource limits and safety budget */
    o.render_length_limit = 10;
    o.strict_errors = 0;
    expect("{% for i in (1..100) %}xx{% endfor %}", "{}", &o, 0, "Liquid error: Memory limits exceeded");
    o.render_length_limit = 0;
    o.max_steps = 1000;
    expect("{% for i in (1..100000) %}{% endfor %}done", "{}", &o, 0, "Liquid error: Memory limits exceeded");
    o.max_steps = 0;
    o.strict_errors = 1;

    /* frozen clock */
    o.has_current_time = 1;
    CHECK(liquid_parse_time("2024-01-01T00:01:58Z", 20, &o.current_time, &o.current_time_offset), "parse time");
    expect("{{ 'now' | date: '%Y-%m-%d %H:%M:%S' }}", "{}", &o, 0, "2024-01-01 00:01:58");
    o.has_current_time = 0;

    /* invalid data */
    liquid_data *bad = liquid_data_from_json("{\"a\":", 5, NULL, &err);
    CHECK(bad == NULL && err.type == LIQUID_DATA_ERROR, "invalid JSON rejected");
    liquid_error_clear(&err);

    /* C ABI: arbitrary bytes, repeated renders */
    lfc_string e = {0};
    lfc_template *t = lfc_parse("{{ x }}\xff", 8, &e);
    CHECK(t == NULL && e.len > 0, "invalid UTF-8 rejected");
    lfc_string_free(e);
    const char *src2 = "{% increment c %}{% cycle 'a','b' %}{{ x }}";
    t = lfc_parse(src2, strlen(src2), &e);
    lfc_context *c = lfc_context_new("{\"x\":1}", 7, NULL, 0, &e);
    for (int i = 0; i < 3; i++) {
        lfc_string out = {0};
        int r = lfc_render(t, c, &out, &e);
        CHECK(r == 0 && out.len == 3 && memcmp(out.ptr, "0a1", 3) == 0, "repeat render %d: %.*s", i, (int)out.len, out.ptr);
        lfc_string_free(out);
    }
    CHECK(lfc_context_new("[1]", 3, NULL, 0, &e) == NULL, "non-object assigns rejected");
    lfc_string_free(e);
    lfc_template_free(t);
    lfc_context_free(c);
    lfc_template_free(NULL);
    lfc_context_free(NULL);
    lfc_string_free((lfc_string){NULL, 0});

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
