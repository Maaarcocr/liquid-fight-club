/*
 * liquid_fc.h - the C ABI your library must export (do not edit).
 *
 * External tools (benchmark and fuzzing drivers) link against
 * build/libliquid_fc.a and call only these functions. Semantics must match
 * your JSON-RPC server: same parser (strict2 mode, line numbers on), same
 * renderer, same error messages.
 *
 * Conventions
 * - Input strings are UTF-8 byte buffers with an explicit length; they are
 *   NOT NUL-terminated and may contain arbitrary bytes.
 * - Every lfc_string returned to the caller is owned by the caller and must
 *   be released with lfc_string_free. An "empty" lfc_string is {NULL, 0}.
 * - The `error` out-parameters may be NULL (caller not interested).
 * - No function may crash, abort, leak, or loop forever on any input,
 *   including malformed JSON and hostile templates.
 * - Thread safety is not required, but repeated calls must be independent:
 *   rendering must not modify the template or the context, so the same pair
 *   can be rendered many times with identical results.
 */
#ifndef LIQUID_FC_H
#define LIQUID_FC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lfc_template lfc_template; /* opaque parsed template */
typedef struct lfc_context lfc_context;   /* opaque render inputs */

typedef struct lfc_string {
    char *ptr;  /* UTF-8 bytes, not necessarily NUL-terminated */
    size_t len;
} lfc_string;

/*
 * Parse a template. Returns NULL on a syntax error and, if `error` is not
 * NULL, stores the error message (the same text the JSON-RPC server reports
 * for this compile error) in *error.
 */
lfc_template *lfc_parse(const char *source, size_t source_len, lfc_string *error);

/*
 * Build render inputs.
 *   assigns_json : a JSON object of template variables.
 *   partials_json: a JSON object mapping partial names to template source
 *                  (used by include/render), or NULL / length 0 for none.
 * Returns NULL if either argument is not valid JSON of the right shape,
 * with a message in *error.
 */
lfc_context *lfc_context_new(const char *assigns_json, size_t assigns_len,
                             const char *partials_json, size_t partials_len,
                             lfc_string *error);

/*
 * Render with default options: render errors raise (strict_errors: true).
 * Returns 0 on success with the output in *output. Returns non-zero on a
 * render error with the message in *error (and *output set to {NULL, 0}).
 */
int lfc_render(const lfc_template *tpl, const lfc_context *ctx,
               lfc_string *output, lfc_string *error);

void lfc_template_free(lfc_template *tpl);   /* NULL is allowed */
void lfc_context_free(lfc_context *ctx);     /* NULL is allowed */
void lfc_string_free(lfc_string s);          /* {NULL, 0} is allowed */

#ifdef __cplusplus
}
#endif

#endif /* LIQUID_FC_H */
