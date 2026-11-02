/*
 * libFuzzer target over the shared C ABI (harness/liquid_fc.h).
 * Input = template source; assigns and partials are fixed small JSON objects.
 * Any crash, abort (a Rust panic across the ABI aborts), sanitizer report,
 * leak, timeout or OOM counts as a finding.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "liquid_fc.h"

static const char ASSIGNS[] =
    "{\"x\":\"hello\",\"n\":42,\"neg\":-7,\"f\":3.5,\"b\":true,\"z\":null,"
    "\"s\":\"<b>&amp; \\u00e9t\\u00e9</b>\",\"arr\":[1,2,3,\"a\",null,[4,5]],"
    "\"h\":{\"a\":1,\"b\":[{\"c\":\"d\"}],\"e\":{\"f\":{\"g\":\"deep\"}}},"
    "\"products\":[{\"title\":\"A\",\"price\":10,\"tags\":[\"x\",\"y\"]},"
    "{\"title\":\"B\",\"price\":20.5,\"tags\":[]}],"
    "\"date\":\"2024-02-29 13:45:00\",\"big\":12345678901234567890}";

static const char PARTIALS[] =
    "{\"p\":\"{{ x }}\",\"card\":\"{{ product.title }}-{{ product.price }}\","
    "\"self_render\":\"{% render 'self_render' %}\","
    "\"self_include\":\"{% include 'self_include' %}\","
    "\"args\":\"{{ a }}{{ b | default: 'none' }}\"}";

static lfc_context *ctx;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    ctx = lfc_context_new(ASSIGNS, sizeof ASSIGNS - 1, PARTIALS, sizeof PARTIALS - 1, NULL);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    lfc_string err = {NULL, 0};
    lfc_template *tpl = lfc_parse((const char *)data, size, &err);
    lfc_string_free(err);
    if (tpl && ctx) {
        lfc_string out = {NULL, 0}, rerr = {NULL, 0};
        lfc_render(tpl, ctx, &out, &rerr);
        lfc_string_free(out);
        lfc_string_free(rerr);
    }
    lfc_template_free(tpl);
    return 0;
}
