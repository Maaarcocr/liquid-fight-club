# Liquid Fight Club: Rust vs C

Same agent, same task, two languages. Which one is better for an agent to
build Liquid in, on **token efficiency**, **correctness**, **performance** and
**security**?

## Setup

- Two branches with the same starter files: `impl/rust` and `impl/c`. They
  differ only in the language name and language rules in `TASK.md`.
- Both run Claude Code locally on the same PC, same subscription, in
  parallel. The model (Opus 5.5) and effort (high) are pinned in each
  branch's `.claude/settings.json`.
- No third-party code on either side, JSON included.
- Target: every in-scope liquid-spec suite cleared. That is 6,240 specs: 4,313
  default, 1,905 parser_errors, 12 partials, 10 benchmarks. Ruby Liquid itself
  passes 100% of them through the same JSON-RPC bridge.
- Out of scope: Ruby-only types/objects, binary data, Shopify extensions, and
  two features the reference can't do over JSON-RPC.

## Running it

A manager agent runs the whole thing by following [MANAGER.md](MANAGER.md):

- start the two implementers as normal Claude Code sessions in the desktop
  app, each in its own folder and kicked off with the same `/goal` (Claude
  keeps working until a check confirms the goal is met);
- nudge them with the same message if they stop early;
- archive the transcripts;
- score both implementations and write `REPORT.md`.

Prerequisites: Ruby 3.x with bundler, Rust (stable), gcc, clang, make.

## Measuring

- **Tokens:** run `tools/usage.py ~/.claude/projects/<dir> --repo <checkout>`.
  It gives totals per model and cumulative tokens at every commit.
  Re-scoring each commit with `harness/spec` gives the curve of specs
  passing vs tokens spent.
- **Correctness:**
  - final `harness/spec` results;
  - held-out checks: liquid-spec `tools fuzz` / `tools mutate` against the
    reference, with new seeds.
- **Performance:**
  - one small C benchmark driver linked against each `build/libliquid_fc.a`
    (shared ABI: `harness/liquid_fc.h`), run on liquid-spec's benchmark
    templates;
  - Ruby Liquid as a reference point.
- **Security:**
  - the same libFuzzer harness over the same ABI, with ASan/UBSan and equal
    fuzzing time;
  - a handful of hostile templates (deep nesting, huge loops, recursive
    partials).

## Caveats

- One run per language, so differences may be noise.
- Each side gets only its own standard library, and they cover different
  ground. Rust has vectors, hash maps, strings and sorting built in, which C
  must write itself. C has date/time formatting in libc, which Rust must
  write. That gap is part of what's being compared.
- The shared C ABI is free for C and extra glue for Rust.
