# Implementing a Liquid Template Engine

> **Project note:** this is liquid-spec's standard implementer guide, with command paths
> adjusted for this repository (`harness/liquid-spec`, `harness/adapter.rb`). In this
> project the adapter and its `missing_features` scope are fixed - see TASK.md.


You are building a production-grade [Liquid](https://shopify.github.io/liquid/)
template engine. This directory is wired to **liquid-spec**, which defines what
"correct" means: thousands of executable specs, ordered by complexity from trivial
passthrough (score 0) to full production compatibility (score 1000). The suite
is a curriculum for observable behavior, not a mandated internal architecture:
tree-walking, bytecode, compiled templates, strict-first, and compatibility-first
implementations can all use the same ramp.

**Your Liquid library is standalone.** The adapter file(s) in this directory are
test harnesses that run liquid-spec against your library — they are NOT part of your
library and have no intrinsic value to it. Your library's public API, architecture,
and distribution are entirely your own. The Ruby adapter calls your code directly;
the JSON-RPC adapter talks to your engine over stdin/stdout. Both are disposable
shims that exist only so liquid-spec can exercise your implementation. Build your
library as a library; wire up the adapter afterward.

## Not Using Ruby?

**You are building a standalone Liquid library in your language.**
The JSON-RPC adapter generated here is purely a harness for running the
liquid-spec test suite against your library — it is NOT part of your library
and has no intrinsic value to it. Your library's public API, architecture,
and distribution are entirely your own; the adapter is a thin test shim that
translates liquid-spec's Ruby protocol calls into JSON-RPC requests your
server can answer. Once your library passes the specs, the adapter is
disposable.

**This adapter uses JSON-RPC** to communicate with your Liquid implementation over stdin/stdout.
You can implement your Liquid engine in **any language** (Rust, Go, Python, Node.js, etc.).

See `docs/json-rpc-protocol.md` for the full protocol specification, or check the comments
in your adapter file for a quick reference.

**Key points:**
- Your server reads JSON-RPC requests from stdin, writes responses to stdout
- Debug output goes to stderr (liquid-spec forwards it to the terminal)
- Implement 4 methods: `initialize`, `compile`, `render`, `quit`


## Which adapter file?

- **Implementing in Ruby?** Not applicable here. (The Ruby adapter calls code directly)
  (see "The Adapter Pattern" below).
- **Implementing in ANY other language** (Rust, Go, Python, Node, Zig, ...)? Use
  `harness/adapter.rb`. It talks to your engine over a simple JSON-RPC
  protocol on stdin/stdout — run `harness/liquid-spec docs json-rpc-protocol` for the
  full protocol.

## The loop (follow this exactly)

```bash
harness/liquid-spec run harness/adapter.rb
```

1. Run the suite. Specs execute in complexity order, so the FIRST failure is
   usually the best next lesson. You can batch related work or follow product
   needs, but keep returning to the ramp because later specs often assume earlier
   semantics are already solid.
2. Read the failing spec completely — template, environment, expected, got, and
   especially the `hint:`. Hints are written by implementers for implementers;
   they usually state the exact rule you are missing.
3. Keep the implementer docs close. Run `harness/liquid-spec docs` to print the full
   docs directory and the available guide names; start with
   `harness/liquid-spec docs curriculum`, then open the guide named by the failing spec
   or hint. These docs are the intended companion to the spec ramp.
4. If the behavior is unclear, read the relevant guide first
   (see "Documentation" below) — minutes of reading routinely save hours of
   guessing. Reproduce interactively before coding:
   ```bash
   cat <<'EOF' | harness/liquid-spec tools eval harness/adapter.rb --compare
   name: scratch_assign
   template: "{% assign x = 1 %}{{ x }}"
   expected: "1"
   complexity: 40
   hint: "Assign stores a value in the current scope."
   EOF

   cat <<'EOF' | harness/liquid-spec tools eval harness/adapter.rb --compare
   name: scratch_array_size
   template: "{{ x | size }}"
   environment:
     x: [1, 2, 3]
   expected: "3"
   complexity: 40
   hint: "The size filter returns array length."
   EOF
   ```
   `--compare` shows the reference implementation's output next to yours.
5. Implement the smallest change that makes the spec pass for the RIGHT reason.
6. Re-run. Every previously-passing spec must still pass — a fix that breaks
   earlier specs is wrong, not a tradeoff.
7. Judge progress by **`Complexity level cleared`**, not the raw pass count. A
   partial implementation accidentally passes many later specs whose expected
   output happens to be empty; the cleared complexity level is the honest meter.

## Hard rules

1. **Never special-case a spec.** No matching on spec names, template strings,
   or expected outputs. Every fix must implement the general rule the spec is
   an instance of. (Passing a spec without implementing its rule will break on
   the next spec of the same family anyway.)
2. **The expected output is recorded reference behavior — conform, don't
   argue.** When an expectation looks insane, it is usually a real, documented
   quirk: read the spec's hint, then `QUIRKS.md` in the liquid-spec repository.
   Liquid has many deliberate oddities (they are what "compatible" means).
3. **Use `missing_features` honestly.** It exists for surface you are
   deliberately not building — `shopify_*` features (needed only to render
   Shopify themes) and legacy parse modes are the legitimate entries. It is
   not for dodging hard specs; every entry is debt that keeps specs from
   running.
4. **Never delete or edit specs to make them pass.**
5. **Keep your engine's behavior in ONE place per rule.** If you find yourself
   patching the same symptom at several call sites, you modeled the rule at the
   wrong layer.

## Error modes: build for strict2 (read this before complexity 100)

**Target `:strict2` from the start** — it is the modern parser contract
(Liquid 5.12+) and the adapters generated here default to it. The legacy
parse modes exist only for compatibility:

- `:strict` / `:lax` — implement these ONLY if compatibility with older
  Liquid or Shopify production behavior matters to you. Until then, let the
  specs that target those modes skip; do not contort your parser around
  them. (One example of what lives there: lax mode suppresses the error
  TEXT of blank-bodied tags — a backwards-compatibility quirk with its own
  spec suite whose hints explain it if and when you take it on.)
- `shopify_*` feature-gated specs (Shopify platform filters and tags)
  matter only if you want to render Shopify themes. Declare them in
  `missing_features` until that is a goal.

Render-time error rules apply in every mode — get these right early:

- Undefined variables are NOT errors: they render as empty string and
  lookups on them return nil. (`{{ missing }}` → ``, `{{ missing.a.b }}` → ``.)
- Real runtime errors (bad comparison, invalid filter argument, ...) render
  inline as `Liquid error (line N): <message>` when the spec sets
  `render_errors: true`; otherwise rendering raises. Match the message
  TEXT — specs pin it.
- Parse errors render as `Liquid syntax error (line N): <message>` or
  raise; specs that expect them say so.
- When a failing spec's expected output surprises you, the rule you are
  missing is almost always stated in its `hint:` — the suite is designed
  to teach the semantics just-in-time, in complexity order. Read the hint
  before reading anything else.

## Documentation

All implementer guides are served by the CLI. `harness/liquid-spec docs` prints the
full docs directory plus a tree of guide names; `harness/liquid-spec docs <name>`
prints one guide. These guides explain Liquid semantics at the level the specs
expect, without requiring you to mirror any particular internal architecture:

| `harness/liquid-spec docs ...` | What it explains |
|---|---|
| `curriculum` | the learning path: what to build, when to read each guide, and what to defer |
| `core-abstractions` | truthiness, nil, output conversion, iteration, emptiness, scope shape |
| `grammar` | the syntax: tags, output, expressions, literals, irregularities |
| `complexity` | the full ramp: why each lesson appears when it does |
| `scopes` | variable scoping: assign, capture, loops, includes |
| `filters` | filter dispatch, arguments, coercion rules |
| `for-loops` | for/forloop/offset/limit/else, iteration protocol |
| `interrupts` | break/continue, incl. across include boundaries |
| `partials` | include vs render semantics |
| `filesystem` | partial lookup, extensions, path rules |
| `cycle`, `tablerow` | the stateful tags |
| `ruby-quirks` | behaviors inherited from Ruby's semantics |
| `adversarial` | generated differential mutation, fuzz-style testing, and bounded stress |
| `json-rpc-protocol` | the non-Ruby adapter protocol |

Also read `QUIRKS.md` in the liquid-spec repository when an expectation looks
wrong — if the quirk is documented there, it is intentional.

## Architecture advice for a fresh implementation

A common low-risk route is tokenizer → parser → node tree → tree-walking
renderer with a scope stack. A compiler/VM is also fine if that fits your project,
but correctness across thousands of specs is the hard part, so keep the observable
semantics easy to test. Two things ARE worth designing in from day one, because
they weave through everything and are painful to retrofit:

1. **Error plumbing** — every node needs a line number and a uniform way to
   either emit `Liquid error (line N): ...` text or raise, per the error model
   above.
2. **The value model** — one module that answers "is this truthy?",
   "how does this print?", "how does this coerce to a number?",
   "how does this iterate?" for every type. Most spec failures at
   complexity 150+ are inconsistencies between call sites that answered those
   questions independently.

### Implementation phases

| Phase | Complexity | Features |
|-------|------------|----------|
| 0 | 0-20 | Pipeline, static text, object tags, literal output, nil-as-empty |
| 1 | 30-50 | Variables, missing variables, simple filters, assign |
| 2 | 55-100 | Basic conditionals, loops, comparisons, capture/case/forloop basics |
| 3 | 105-180 | Standard filters/tags, comments/raw, whitespace control, interrupts, collection helpers |
| 4 | 190-400 | Partials/filesystem, scope interactions, generated compatibility breadth |
| 5 | 500-900 | Parser error matrices, error-model matrix, recursion/deep nesting, security/date/time/Ruby quirks |
| 6 | 1000 | Production recordings and unscored mature-compatibility checks |

## JSON-RPC Protocol (Non-Ruby Implementations)

Your implementation communicates with liquid-spec via JSON-RPC 2.0 over stdin/stdout.
Implement a server that reads JSON requests from stdin and writes responses to stdout.

### Running Your Implementation

```bash
# Run with your server command
harness/liquid-spec run harness/adapter.rb --command="./your-liquid-server"

# Or set DEFAULT_COMMAND in the adapter file
```

### Protocol Overview

All messages are newline-delimited JSON. The lifecycle is:

1. **initialize** - liquid-spec sends version info, your server responds with informational metadata/features
2. **compile** - Parse a template, return a template_id
3. **render** - Render a compiled template with variables
4. **quit** - Notification to exit cleanly (no response expected)

### Debug Output

Your server can write debug information to **stderr** at any time. liquid-spec forwards
all stderr output to the terminal, prefixed with your server name:

```
[my-liquid-server] Compiling template: {{ x | upcase }}
[my-liquid-server] Render complete in 0.5ms
```

This is useful for debugging your implementation without interfering with the JSON-RPC
protocol (which uses stdout). Write to stderr liberally during development.

**Important:** The default timeout is 2 seconds per request. If your server doesn't
respond within 2 seconds, liquid-spec fails that spec with a timeout, kills the wedged
subprocess, and starts a fresh server for the next spec.

### Example: Minimal Server (Node.js)

```javascript
#!/usr/bin/env node
const readline = require('readline');
const templates = new Map();
let nextId = 1;

const rl = readline.createInterface({ input: process.stdin });

rl.on('line', (line) => {
  const { id, method, params = {} } = JSON.parse(line);

  if (method === 'initialize') {
    // features is informational. Spec selection is configured in
    // the Ruby adapter's config.missing_features list.
    respond(id, { version: '1.0', features: [] });
  } else if (method === 'compile') {
    const templateId = `t${nextId++}`;
    templates.set(templateId, { source: params.template, filesystem: params.filesystem || {} });
    respond(id, { template_id: templateId });
  } else if (method === 'render') {
    const t = templates.get(params.template_id);
    const output = renderLiquid(t.source, params.environment || {}, t.filesystem);
    respond(id, { output });
  } else if (method === 'quit') {
    process.exit(0);  // No response needed for quit notification
  }
});

function respond(id, result) {
  console.log(JSON.stringify({ jsonrpc: '2.0', id, result }));
}

function renderLiquid(source, env, filesystem) {
  // Your Liquid implementation here!
  return source;
}
```

### Compile Request/Response

```json
// Request
{"jsonrpc":"2.0","id":1,"method":"compile","params":{
  "template": "{{ x | upcase }}",
  "options": {"error_mode": "strict2"},
  "filesystem": {"snippet.liquid": "Hello"}
}}

// Success response
{"jsonrpc":"2.0","id":1,"result":{"template_id":"abc123"}}

// Parse error response (preferred)
{"jsonrpc":"2.0","id":1,"result":{"template_id":null,"error":{"type":"parse_error","message":"Unknown tag"}}}

// Legacy parse error response also accepted
{"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"Parse error","data":{"type":"parse_error","message":"Unknown tag"}}}
```

### Render Request/Response

```json
// Request
{"jsonrpc":"2.0","id":2,"method":"render","params":{
  "template_id": "abc123",
  "environment": {"x": "hello", "items": [1,2,3]},
  "options": {"strict_errors": true}
}}

// Success response
{"jsonrpc":"2.0","id":2,"result":{"output":"HELLO"}}

// Render error response with strict_errors=true (preferred)
{"jsonrpc":"2.0","id":2,"result":{"output":null,"error":{"type":"render_error","message":"undefined variable"}}}

// Inline render error response with strict_errors=false
{"jsonrpc":"2.0","id":2,"result":{"output":"Liquid error: undefined variable","errors":[{"type":"render_error","message":"undefined variable"}]}}

// Legacy render error response also accepted
{"jsonrpc":"2.0","id":2,"error":{"code":-32001,"message":"Render error","data":{"type":"render_error","message":"undefined variable"}}}
```

### Error Codes

| Code | Meaning |
|------|---------|
| -32000 | Legacy parse error (accepted; prefer result.error) |
| -32001 | Legacy render error (accepted; prefer result.error) |
| -32700 | JSON parse error |
| -32600 | Invalid request |
| -32601 | Method not found |

See the adapter file (`adapter.rb`) for the complete protocol specification.


## After the recorded ramp: generated adversarial coverage

Once recorded specs are green, use the same corpus to search nearby behavior.
`mutate` deterministically varies templates, `fuzz` reproducibly chains random
mutations, and `stress` adds bounded valid nesting/repetition. All compare your
adapter with Shopify/liquid and save differences as runnable YAML regressions:

```bash
harness/liquid-spec tools mutate harness/adapter.rb --around=for_loops --limit=100
harness/liquid-spec tools fuzz harness/adapter.rb --seed=1234 --rounds=500 --minimize
harness/liquid-spec tools stress harness/adapter.rb --depth=64 --repetitions=100
```

Read `harness/liquid-spec docs adversarial` before promoting a generated case. Minimize
it, explain the general rule in its hint, place it after its prerequisites, and
add it permanently only when it creates a useful regression boundary. This is
differential corpus mutation, not native coverage-guided fuzzing.

## Local specs for development

Any `.yml` under a local `specs/` directory is picked up automatically — use it
to isolate a behavior before fixing it, and as regression tests afterwards:

```yaml
---
specs:
- name: my_assign_test
  template: "{% assign x = 'hello' %}{{ x }}"
  expected: "hello"
  complexity: 50
  hint: "What rule this checks, for your future self"
```

Specs support `environment:` (variables), `filesystem:` (partials),
`error_mode:`, `render_errors: true` (required whenever `expected` contains
error text), and `errors:` for parse-error expectations — copy shapes from the
built-in suites.

## Command reference

```bash
harness/liquid-spec run harness/adapter.rb                 # the loop
harness/liquid-spec run harness/adapter.rb -n case         # only specs matching a pattern
harness/liquid-spec run harness/adapter.rb -n /^case_b/    # regex form
harness/liquid-spec run harness/adapter.rb --list-suites   # what suites exist
harness/liquid-spec run harness/adapter.rb -l              # list specs without running
harness/liquid-spec run harness/adapter.rb --list-passed   # audit accidental passes
harness/liquid-spec run harness/adapter.rb --json          # machine-readable results
cat spec.yml | harness/liquid-spec tools eval harness/adapter.rb --compare     # one-off YAML spec
harness/liquid-spec tools mutate harness/adapter.rb --around=for_loops         # deterministic generated cases
harness/liquid-spec tools fuzz harness/adapter.rb --seed=1234 --json           # reproducible fuzz-style run
harness/liquid-spec tools stress harness/adapter.rb --depth=64                 # bounded structural stress
harness/liquid-spec docs curriculum                 # the implementation learning path
harness/liquid-spec docs complexity                 # any guide from the table above
```

## Reference

- [Liquid documentation](https://shopify.github.io/liquid/) (user-facing; the
  specs and `harness/liquid-spec docs` guides are the implementer-facing truth)
- [liquid-spec repository](https://github.com/Shopify/liquid-spec)
