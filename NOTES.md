# Liquid in Rust — implementation notes

A Liquid template engine in Rust (std only, no dependencies), compatible with
Shopify's Ruby reference implementation (Liquid 5.13) in **strict2** parse
mode. `bin/build` produces:

- `build/liquid-server` — the JSON-RPC server liquid-spec talks to.
- `build/libliquid_fc.a` — a static library exporting the C ABI in
  `harness/liquid_fc.h`.

## Status

`harness/spec`:

```
default        Complexity level cleared: 1000 of 1000, 4313 passes, 0 failures, 1008 skipped.
parser_errors  Complexity level cleared: 500 of 1000, 1905 passes, 0 failures.
partials       Complexity level cleared: 1000 of 1000, 12 passes, 0 failures.
benchmarks     Complexity level cleared: 1000 of 1000, 10 passes, 0 failures.
```

Every spec that runs passes. **`parser_errors` cannot report "1000 of 1000"**:
all 1905 of its specs are complexity 500, and the runner reports "the highest
complexity level present" when nothing fails. Local specs cannot raise it either:
the runner rewrites local specs marked complexity 1000 to 100
(`discover_local_specs` in liquid-spec's `cli/runner.rb`). I did not try to work
around the metric (e.g. by adding specs with made-up complexities). The 1008
skipped specs belong to features the fixed adapter declares out of scope
(`ruby_types`, `ruby_drops`, `binary_data`, `shopify_*`, ...).

Beyond the recorded specs, the engine was checked against the reference with
liquid-spec's differential tools (`tools mutate`, `tools fuzz`, `tools stress`,
about 10,000 generated cases, reference forced to strict2). The only remaining
differences are ones JSON-RPC cannot express: the Ruby exception class name
(e.g. `FileSystemError` vs a generic render error, same message) and Ruby
`Time` objects in the environment, which liquid-spec sends as ISO 8601 strings.

## Layout

```
crates/liquid          the library (all Liquid semantics live here)
  src/json.rs          JSON parser/serializer (ordered objects, big integers, depth limit)
  src/bigint.rs        arbitrary-precision integers (Ruby Integer)
  src/num.rs           Ruby numeric rules: to_i/to_f/Integer(), BigDecimal emulation, Num
  src/value.rs         the value model: to_s, inspect, output, ==, <=>, empty/blank
  src/lexer.rs         expression lexer + token parser (Liquid::Lexer/Parser)
  src/expression.rs    markup -> expression tree (Liquid::Expression/VariableLookup)
  src/parse.rs         template tokenizer, block structure, every tag's strict2 parser
  src/render.rs        context, scopes, variable lookup, tags, partials, limits
  src/filters.rs       standard filters (+ json)
  src/date.rs          Time.parse subset and full strftime
  src/drop.rs          the Drop trait (host objects)
  src/test_drops.rs    liquid-spec's portable test drops (_instantiate markers)
  src/convert.rs       JSON <-> Value
  src/budget.rs        work accounting for the step budget
  tests/hostile.rs     hostile-input tests (run on a 512 KB stack)
  examples/replay.rs   replays captured JSON-RPC requests and times each stage
crates/liquid-server   JSON-RPC wrapper (stdio), RPC-backed drops
crates/liquid-capi     C ABI wrapper (staticlib `liquid_fc`)
```

Both wrappers are thin: they convert inputs, call `Template::parse` /
`Template::render`, and format results.

## Architecture

**Parsing.** The tokenizer is a port of `Liquid::Tokenizer`, including its
odd cases (`{{ x {% y %}`, unterminated tags). Block bodies hand unknown tags
back to the enclosing block, which decides whether they are delimiters,
intermediate tags (`else`, `when`) or errors, so error messages and line
numbers match the reference exactly. Like Ruby, the strict parser's
`expression` returns a *normalized markup string*, which `expression::parse`
turns into a tree with hand-written equivalents of the reference's regexes
(`VariableParser`, `RANGES_REGEX`, the `assign` syntax). Keeping that round
trip means odd inputs behave like the reference instead of approximately.

**Value model.** `Value` is a small enum (`Nil`, `Bool`, `Int(i64)`,
`BigInt`, `Float`, `Str(Rc<str>)`, `Array(Rc<Vec>)`, `Hash(Rc<Hash>)`,
`Range`, `Drop`). Containers are immutable and shared by reference counting,
so assigning or passing values is cheap. `Hash` keeps insertion order and
builds a hash index lazily for larger maps. Printing (`Utils.to_s`, Ruby
`inspect`, Float#to_s), equality, ordering, truthiness and empty/blank
are answered in one module (`value.rs`) and reused everywhere.

**Rendering.** A tree-walking renderer over the parsed AST. The template is
never mutated: per-render state (scopes, counters, cycle/for registers,
partial cache) lives in a `Context`, so a template can be rendered any number
of times with identical results. Errors carry kind, line, template name and
markup context, formatted like `Liquid::Error#to_s`; per node they are either
rendered inline or propagated (`strict_errors`), following
`BlockBody.rescue_render_node` (including blank-tag suppression).

**Numbers.** Integers overflow into big integers like Ruby. The math filters
convert floats to BigDecimal in the reference (`0.1 | plus: 0.2` is `0.3`), so
`num::Dec` emulates BigDecimal: exact +, -, *, floored %, half-up rounding,
and division to 40 significant digits before converting back to `f64`.

## Notable decisions

- **strict2 only.** The compile request's `error_mode` is ignored; lax and
  strict parsing are not implemented (the harness only runs strict2).
- **Assigns are static environments.** The reference harness renders with
  `Context.build(static_environments: assigns)`, so render variables are
  visible inside `{% render %}` partials and `increment`/`decrement` counters
  start from an empty environment. The engine follows that.
- **`self`** resolves in the context it was created in, even when passed into
  a `render` partial (suspended contexts are kept on a stack).
- **Ruby quirks kept on purpose**: `Integer()`'s NUL-byte behaviour (strings
  that fit in a u64 reject trailing garbage, big ones stop at NUL), `gsub`
  replacement escapes (`\0`, `\&`, `` \` ``, `\'`, `\k<..>` errors), awk-style
  `split(" ", limit)`, `Integer#size == 8`, hash/array `inspect` formatting,
  case/when rendering every matching branch, the parse-context trim flag
  leaking into partials parsed later.
- **File systems.** Names are lowercased and get `.liquid`, like liquid-spec's
  `SimpleFileSystem`. A missing or empty `filesystem` in a compile request
  behaves as an empty file system ("Could not find asset ...").
- **The `json` filter** is implemented. It is not a Liquid standard filter, but
  liquid-spec registers it for the reference and recorded specs use it.
- **Drops.** liquid-spec's portable test drops are native (`test_drops.rs`).
  Other host objects arrive as `_rpc_drop` markers and are proxied through
  `drop_get` / `drop_iterate` callbacks on the same stdio channel.
- **Raised render errors** are returned as JSON-RPC error `-32001` with the
  full message: the current Ruby adapter ignores `result.error` on render and
  would report an empty output.

## Robustness and limits

Hostile templates and data must not crash, hang, or grow memory without bound.
Rust removes memory-safety bugs; the rest is explicit limits:

- **Stack.** Parse nesting is limited to 100 blocks (the reference's limit),
  liquid-tag nesting to 100, expression nesting to 64, JSON nesting to 512,
  and rendered body nesting across partials to 300. Value recursion (inspect,
  ==, <=>, output) is depth-guarded. Long `and`/`or` chains are flat vectors.
  The deepest legal render fits on a 512 KB stack (tested).
- **CPU.** Each render has a step budget (default 20M) charged for nodes,
  loop iterations, partial renders and, through `budget.rs`, big-integer work,
  collection flattening and bytes produced. Huge ranges are never materialized
  in `for`/`tablerow`. Exceeding the budget raises "Memory limits exceeded".
  All liquid-spec suites pass with a budget 20x smaller than the default.
- **Memory.** Output buffers are capped at 256 MB, strings produced by filters
  at 128 MB, arrays at 4M elements, big integers at 1024 limbs (~9,800 digits),
  templates at 64 MB. Size checks happen before large allocations.
- **Parsing is linear** in the worst cases found (unterminated `{%` runs,
  long `assign` markup without `=`).
- The spec's `resource_limits` (`render_score_limit`, `assign_score_limit`,
  `render_length_limit` and the cumulative ones) are honoured with the
  reference's accounting.
- Every C ABI entry point catches panics; the server catches panics per
  request and runs on a thread with a large stack.

## Performance

Rendering is a tree walk over `Rc`-shared values with no per-render copies of
the assigns. With assigns already built, the largest benchmark template renders
in about 3 µs versus about 90 µs for the Ruby reference. Across the benchmark
suite (`liquid-spec bench`), parse + first render is faster than the
reference; for the two Shopify theme pages the JSON-RPC round trip dominates
(each render ships a 1.1 MB environment, most of the time being Ruby
serializing it).

## Known limitations

- Only strict2 parsing; no lax/strict error modes.
- `Time.parse` is a pragmatic subset of Ruby's `Date._parse` (ISO dates,
  `/` and `.` separated dates, month names, times with am/pm, zones and
  offsets, day-of-year, filling from "now"); exotic formats (era names like
  `H13.02.03`, relative words) are not parsed and the input is returned as is.
- Local time is UTC (the reference runs specs with `TZ=UTC`); named zones
  outside a small table are ignored.
- BigDecimal division rounds to 40 significant digits rather than Ruby's
  variable precision; results agree after conversion to `f64` except in
  extremely rare double-rounding cases.
- Range endpoints are `i64` (big-integer endpoints are clamped).
- RPC drops: a `to_liquid_value` that returns nil cannot be told apart from a
  drop without that method, so such drops are treated as truthy.
- Strings are UTF-8; `base64_decode` of non-UTF-8 bytes is lossy (binary data
  is out of scope for the harness).
- Templates and values use `Rc`, so they are not `Send`; use one thread per
  template (the C ABI does not require thread safety).
- `capitalize` uppercases the first character (Ruby uses titlecase, which only
  differs for a few digraphs such as `ǆ`).
