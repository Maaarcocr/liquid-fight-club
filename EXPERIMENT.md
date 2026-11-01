# Liquid Fight Club: Rust vs C, built by agents

Pre-registered protocol. Written before either implementation run starts;
any later change is logged under "Amendments" with a reason.

## Question

When the same coding agent implements the same non-trivial system (the
Liquid template language) once in Rust and once in C, under identical
conditions, which language comes out ahead on:

1. **Token efficiency**: tokens and dollars spent to reach each level of
   correctness.
2. **Correctness**: on the visible spec suite and on held-out generated cases.
3. **Performance**: parse/render speed, memory use.
4. **Security**: memory-safety bugs, crashes, hangs and resource exhaustion
   under hostile input.

## Arms

| | Rust | C |
|---|---|---|
| Branch | `impl/rust` | `impl/c` |
| Language rules | stable Rust 1.97, edition 2024, `std` only, glibc via own `extern "C"` allowed | C17, `make`, gcc + clang clean, libc + libm + POSIX only |
| Dependencies | none (JSON hand-written too) | none (JSON hand-written too) |

Both branches start from the same scaffold (`scaffold/`, rendered by
`tools/make-impl-branch.py`). `git diff impl/rust impl/c` at the scaffold
commit touches only `README.md` and `TASK.md`: the language name and the
language rules above.

Harness files (git blob ids, identical on both branches; verified again at
evaluation time to detect edits):

```
harness/adapter.rb            16695cbaf9eca4dd5a99871b53c95d133ebac3a2
harness/spec                  f9a36282f96ec2527703a976e162756a1781c273
harness/liquid-spec           158d51cf5aad20271732b368ad629804d4c68ea2
harness/setup.sh              26ee360b5caf265e178ab90263498f807966508d
harness/Gemfile               56eee62ac26e5bc5b5b5e98cce1994fc3aacd286
harness/Gemfile.lock          d4a3e0135d80079ad4787a77939e414bc51b5019
harness/liquid_fc.h           5730d278a8cf16f199478922af0096c491c345e4
harness/LIQUID_SPEC_GUIDE.md  af4055924a363775a86b4f2efee76708a4a58ee4
.claude/settings.json         9089d185ebf3c20b60542af51c340a11866c7921
CLAUDE.md                     26ca93920f1915a525d1a2ee2de28208b24c6bb9
```

## Agent configuration (identical for both)

- Claude Code cloud session, model **Opus 5.5** (`claude-opus-5-5`), effort
  **high**, same environment, same permission mode.
- Effort is pinned by `CLAUDE_CODE_EFFORT_LEVEL=high` in the branch's
  `.claude/settings.json`. Verified locally: this overrides both an inherited
  session effort and an explicit `--effort xhigh` flag (the transcript records
  `"effort":"high"`).
- Denied tools: WebFetch, WebSearch, sub-agents (Agent/Task/Workflow), `curl`,
  `wget`, edits under `harness/`.
- One session per arm, run in parallel. The agents are not told that they
  are part of a comparison.

## Scope (what "done" means)

Pinned liquid-spec `84bf25e0edbc` and Shopify/liquid `v5.13.0`. Parse mode
strict2; render errors both raised and inline. Out of scope: Ruby-only types
and drop classes, binary data, Shopify platform extensions, and two features
(`range_resource_limits`, `strict2_blank_body_errors`) that the reference
implementation itself cannot pass over JSON-RPC. Drops are in scope via the
JSON-RPC callbacks.

Ceiling check: the reference Ruby implementation, run through the same
JSON-RPC bridge with this exact scope, passes **100%**:

| Suite | Specs |
|---|---|
| default (basics, liquid_ruby, portable production recordings) | 4,313 |
| parser_errors | 1,905 |
| partials | 12 |
| benchmarks | 10 |
| **Total** | **6,240** |

## References: allowed and forbidden

- Allowed: liquid-spec (specs, docs, QUIRKS.md), Shopify's Ruby Liquid
  source, the language's own documentation.
- Forbidden: any other Liquid implementation (liquid-c, liquid-rust,
  liquidjs, ...), the C source of Ruby or of any Ruby gem, the internet, the
  other arm's branch, this `experiment` branch.
- Enforcement:
  - session setup deletes all C/C++ sources and headers from the installed
    gem tree, plus Ruby's headers (Liquid depends on the C extensions
    `strscan` and `bigdecimal`, whose sources otherwise ship on disk);
  - denied tools, as listed above;
  - post-run transcript audit for forbidden paths, URLs, other branches, and
    harness edits.
- Note: liquid-spec's `docs/implementers/parsing.md` and `il.md` are prose
  descriptions of liquid-c's parser design. They are language-neutral,
  visible to both arms, and part of the official curriculum, so they stay.

## Run protocol

- **Kickoff prompt** (identical): `Read TASK.md and complete it.`
- **Nudges:** when a session ends a turn before the definition of done is
  met, it receives exactly `Continue working on TASK.md until its definition
  of done is met.` Done is checked by the orchestrator running `harness/spec`
  on the pushed branch, not by the agent's claim.
- **Stop:** the definition of done is met, or 3 consecutive nudges produce no
  new commit (stall).
- **Budget:** no cap.
- **Replicates:** one run per arm. More may be added later, same protocol.
- **Restart:** if the harness turns out broken during the first hour, both
  arms are killed and restarted from scratch together.

## Measurements

### Token efficiency

- Per turn, from the session's `result` events: input, cache-write,
  cache-read, output and thinking tokens per model; `total_cost_usd`; number
  of model calls; wall-clock and API time.
- After the run ends: per-request usage with timestamps, extracted as
  numbers only (no conversation content), to build the curve.
- **Headline:** specs passing (and complexity cleared) vs cumulative cost.
  Every commit is re-scored with the frozen harness and joined to cumulative
  usage by timestamp.
- **Secondary:**
  - tokens to reach 50/90/100% of specs;
  - share of effort spent on build/compile errors vs crashes vs wrong output;
  - final code size (lines, bytes).

### Correctness

- Pass rate and complexity cleared per suite: the visible target.
- **Held-out:** liquid-spec `tools fuzz`, `tools mutate` and `tools stress`
  against the reference implementation, with seeds chosen after the runs end.
  Metric: divergences per 1,000 generated cases.
- Audit for special-casing (spec names, expected strings in source).

### Performance

- One C benchmark driver, linked against each arm's `build/libliquid_fc.a`
  through `harness/liquid_fc.h`.
- Cases: liquid-spec's benchmark templates.
- Metrics: parse and render time per operation (median and spread over
  interleaved runs), allocations, peak RSS, binary size, and clean build time.
- The orchestrator builds both arms with fixed release settings.
- Reference points: Ruby Liquid (with YJIT) and liquid-c. The orchestrator
  runs these; the agents never do.

### Security

- One libFuzzer harness over the C ABI, same seed corpus (all spec
  templates), same CPU time per arm, with ASan and UBSan. A Rust panic across
  the ABI counts as a crash. Crashes are deduplicated by stack.
- A hostile template corpus (deep nesting, huge ranges, recursive partials,
  very large strings and loops), checked for crashes, stack overflow, OOM
  and hangs. Behaviour is compared with the reference, so a hang the
  reference shares is not counted.
- The full spec suite under ASan/UBSan (and Valgrind for C).
- Static analysis:
  - C: compiler warnings, clang-tidy, cppcheck;
  - Rust: clippy, plus `unsafe` counted outside the FFI module.
- One security-review agent per arm, same prompt and budget. Findings count
  only with a working proof of concept.

## Known asymmetries / threats to validity

- **Single run per arm:** results can be noise. Treat differences as
  indicative until replicated.
- **Standard-library breadth:** C has `strftime`/time zones via libc. Rust
  may call the same glibc functions, but only through `unsafe` FFI.
- **The C ABI** is native for C, extra FFI glue for Rust.
- **The auto-mode safety classifier** may allow or block different actions
  in the two sessions.

## Amendments

- (none yet)
