# Liquid in C

A standalone, dependency-free implementation of the
[Liquid](https://shopify.github.io/liquid/) template language in C17
(strict2 mode, compatible with Shopify's Ruby Liquid 5.13).

```sh
bin/build        # release build: build/libliquid_fc.a and build/liquid-server
harness/spec     # run the liquid-spec suites
make test        # C API unit tests
```

* `include/liquid.h` — the library API (parse, render data, render).
* `harness/liquid_fc.h` — the C ABI exported by `build/libliquid_fc.a`.
* `build/liquid-server` — the JSON-RPC server used by liquid-spec.

```c
liquid_error err = {0};
liquid_template *t = liquid_parse(src, strlen(src), NULL, &err);
liquid_data *env = liquid_data_from_json(json, strlen(json), NULL, &err);
char *out; size_t len;
if (liquid_render(t, env, NULL, &out, &len, &err) == 0) { fwrite(out, 1, len, stdout); free(out); }
liquid_data_free(env);
liquid_template_free(t);
liquid_error_clear(&err);
```

See [NOTES.md](NOTES.md) for the architecture, design decisions, testing,
and known limitations, and [TASK.md](TASK.md) for the original assignment.
