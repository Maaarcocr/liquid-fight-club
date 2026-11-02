/*
 * liquid.h - public API of the liquid-fc Liquid template engine.
 *
 * Typical use:
 *
 *   liquid_error err = {0};
 *   liquid_template *t = liquid_parse(src, src_len, NULL, &err);
 *   liquid_data *env = liquid_data_from_json(json, json_len, NULL, &err);
 *   char *out; size_t out_len;
 *   if (liquid_render(t, env, NULL, &out, &out_len, &err) == 0) { ... free(out); }
 *   liquid_data_free(env);
 *   liquid_template_free(t);
 *   liquid_error_clear(&err);
 *
 * All strings are explicit-length UTF-8 byte buffers. Every function is safe
 * on arbitrary input: invalid templates and data produce errors, never
 * crashes. Parsed templates and data are immutable during rendering, so one
 * template/data pair can be rendered any number of times.
 */
#ifndef LIQUID_H
#define LIQUID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lq_template liquid_template;
typedef struct liquid_data liquid_data;

typedef enum liquid_error_type {
    LIQUID_OK = 0,
    LIQUID_SYNTAX_ERROR = 1, /* template failed to parse */
    LIQUID_RENDER_ERROR = 2, /* error raised while rendering */
    LIQUID_DATA_ERROR = 3    /* invalid JSON / data */
} liquid_error_type;

typedef struct liquid_error {
    liquid_error_type type;
    char *message; /* full message, e.g. "Liquid syntax error (line 1): ..." */
    char *core;    /* message without the "Liquid ... (line N): " prefix */
    int line;      /* 0 when unknown */
} liquid_error;

void liquid_error_clear(liquid_error *err);

/* ---------------------------------------------------------------- */
/* Parsing                                                           */
/* ---------------------------------------------------------------- */

typedef struct liquid_parse_options {
    int line_numbers; /* include "(line N)" in error messages (default on) */
} liquid_parse_options;

/* Parse in strict2 mode. Returns NULL on error (details in *err). */
liquid_template *liquid_parse(const char *src, size_t len,
                              const liquid_parse_options *opts, liquid_error *err);
void liquid_template_free(liquid_template *t);

/* ---------------------------------------------------------------- */
/* Data (render inputs)                                              */
/* ---------------------------------------------------------------- */

/*
 * Host drops: objects whose properties are resolved by the host on demand.
 * A JSON object containing the key `marker_key` (e.g. "_rpc_drop") becomes
 * a host drop whose identifier is the string value of that key. The
 * callbacks return a malloc'd JSON document (or NULL for nil); returned
 * JSON may itself contain host drop markers.
 */
typedef struct liquid_drop_host {
    const char *marker_key;
    void *userdata;
    char *(*get)(void *ud, const char *drop_id, const char *prop, size_t prop_len,
                 size_t *out_len);
    char *(*iterate)(void *ud, const char *drop_id, size_t *out_len);
} liquid_drop_host;

typedef struct liquid_data_options {
    const liquid_drop_host *drop_host; /* may be NULL */
} liquid_data_options;

/*
 * Parse a JSON document into render data. Objects of the form
 * {"_instantiate": "Name", "params": {...}} create the built-in portable test
 * drops (BooleanDrop, NumberDrop, StringDrop, MethodDrop, IndexDrop,
 * SequenceDrop, NilDrop, OpaqueDrop, ErrorDrop, NestedDrop).
 */
liquid_data *liquid_data_from_json(const char *json, size_t len,
                                   const liquid_data_options *opts, liquid_error *err);
/* True when the data is a JSON object (required for environments). */
int liquid_data_is_object(const liquid_data *d);
void liquid_data_free(liquid_data *d);

/* ---------------------------------------------------------------- */
/* Rendering                                                         */
/* ---------------------------------------------------------------- */

typedef struct liquid_render_options {
    /* nonzero: render errors abort rendering and are returned (default).
       zero: they are rendered inline as "Liquid error (line N): ..." */
    int strict_errors;
    /* Partials for include/render: a data object mapping file names to
       template source. Names are matched case-insensitively with an optional
       ".liquid" extension. NULL = no file system. */
    const liquid_data *file_system;
    /* Message used when file_system is NULL. */
    const char *no_file_system_message;
    /* Clock for "now"/"today" in the date filter. */
    int has_current_time;
    int64_t current_time;        /* seconds since the epoch */
    int32_t current_time_offset; /* UTC offset in seconds */
    /* Resource limits (0 = unlimited). */
    int64_t render_score_limit;
    int64_t cumulative_render_score_limit;
    int64_t assign_score_limit;
    int64_t cumulative_assign_score_limit;
    int64_t render_length_limit;
    /* Hard safety limits that also apply when the above are 0.
       0 selects the built-in defaults. */
    int64_t max_output_bytes;    /* default 64 MiB (output and any single string) */
    int64_t max_steps;           /* default 100 million steps (nodes, loop
                                    iterations, 32-byte units of filter output) */
} liquid_render_options;

void liquid_render_options_init(liquid_render_options *o);

/*
 * Render. Returns 0 on success with a malloc'd, NUL-terminated output in
 * *out. With strict_errors, returns nonzero on a render error (message in
 * *err, *out = NULL). With inline errors, rendering always succeeds unless
 * a resource limit aborts it.
 */
int liquid_render(const liquid_template *t, const liquid_data *env,
                  const liquid_render_options *opts, char **out, size_t *out_len,
                  liquid_error *err);

/* Parse an ISO 8601 / RFC 2822-ish timestamp. Returns nonzero on success. */
int liquid_parse_time(const char *s, size_t n, int64_t *epoch, int32_t *utc_offset);

#ifdef __cplusplus
}
#endif

#endif
