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

Prerequisites: Ruby 3.x with bundler, Rust (stable), gcc, clang, make.

```bash
git clone --single-branch -b impl/rust https://github.com/Maaarcocr/liquid-fight-club liquid-rust
git clone --single-branch -b impl/c    https://github.com/Maaarcocr/liquid-fight-club liquid-c
# terminal 1                 # terminal 2
cd liquid-rust && claude     cd liquid-c && claude
```

- Use the same permission mode in both, then send the same first message:
  `Read TASK.md and complete it.`
- If a session stops before `harness/spec` is fully green, send exactly:
  `Continue working on TASK.md until its definition of done is met.`
- Stop a session after 3 nudges in a row with no new commit.
- Keep the transcripts: `~/.claude/projects/<checkout-path>/*.jsonl`. Copy
  them somewhere safe once the runs end, because Claude Code deletes old
  transcripts after `cleanupPeriodDays` (default 30).

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
- C gets date/time formatting from libc. Rust gets only its standard
  library (no libc calls), so it has to write its own.
- The shared C ABI is free for C and extra glue for Rust.
