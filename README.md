# Liquid Fight Club: report

Run of 2026-11-02. One run per language, same agent (Claude Opus 5.5, effort
high, auto permission mode), same task, same kickoff, launched in the same
second. Both arms finished: every in-scope liquid-spec spec passes (6,240:
4,313 default, 1,905 parser_errors, 12 partials, 10 benchmarks, 0 failures).

## The answer

| Dimension | Winner | By how much |
|---|---|---|
| Tokens to pass every spec | **Tie** | Rust 30.2M, C 29.4M (C 3% fewer total, Rust 9% fewer output tokens) |
| Tokens to finish (final commit) | **Rust** | 66.5M vs 115.2M total (42% fewer); 350K vs 477K output (27% fewer) |
| Correctness | **Split** | Random differential testing: C diverges from Ruby in 3 ways (7 cases), Rust in 7 ways (288 cases). But C's main bug (string dates render garbage, added in its last commit) is worse than any of Rust's |
| Performance | **C** | Parse 1.7x and render 1.55x faster (geomean over the 10 benchmarks); 9 MB less peak RSS |
| Security | **Tie, slight edge Rust** | 0 crashes in 1 h of fuzzing each; both survive every hostile template; C uses 2–6.5 GB on inputs where Rust stays under 0.4 GB |
| Code size | **Rust** | 9,130 vs 10,723 non-blank lines (15% fewer) |

The headline is that the token gap is not about getting the specs to pass. Both
arms reached 6,240/6,240 at almost the same cost, about 30M tokens (C after 47
minutes; Rust after 75, of which 30 were spent blocked on a permission prompt).
Everything after that was the "production quality" half of the task: hardening,
fuzzing, polish. C spent 86M tokens over 14 more commits on it, Rust 36M over 4. Whether that extra C work bought anything measurable is
mixed: C is faster, but it is not more robust than Rust on any test here, and it
still shipped a date bug Rust does not have.

## Tokens

From `tools/usage.py` on the archived transcripts, joined to each commit
(`results/usage-*.csv`, `results/curve-*.csv`). Most tokens are cache reads,
which cost far less than output tokens, so the output column is the better
proxy for work done.

| | Rust | C |
|---|---|---|
| First commit (engine, all suites built) | 23.0M total / 259K output, 34 failures | 27.8M / 306K, 2 failures |
| First commit with all 6,240 passing | `7f88240`: 30.2M / 281K | `2bae1b7`: 29.4M / 308K |
| Final commit | `b65cb8e`: 66.5M / 350K | `76b2ba4`: 115.2M / 477K |
| Whole transcript | 67.9M, 152 requests | 117.5M, 220 requests |
| Commits after the scaffold | 6 | 16 |
| Restarts ("continue" sent) | 0 | 0 |

Specs passing vs tokens (sum of the four suites, out of 6,240):

| Rust: tokens | passes | | C: tokens | passes |
|---|---|---|---|---|
| 0 | 0 | | 0 | 0 |
| 23.0M | 6,206 | | 27.8M | 6,238 |
| 30.2M | 6,240 | | 29.4M | 6,240 |
| 36.6M → 66.5M (4 commits) | 6,240 | | 52.8M → 115.2M (14 commits) | 6,240 |

Neither agent committed before it had a working engine for every suite, so the
curve is a step, not a ramp: the commit rule ("commit whenever the pass count
goes up") was effectively "commit once it all works".

## Correctness

- **Final spec runs:** both 6,240/6,240, 0 failures (`harness/spec`, re-run by
  the manager on idle checkouts and again per commit in scratch worktrees).
- **Held-out differential checks** (`liquid-spec tools fuzz/mutate/stress`
  against Ruby Liquid, seeds 404744, 340066, 484061, 508624, 639754;
  `results/eval/heldout-*`). Ruby Liquid itself, run as the subject through the
  same JSON-RPC adapter, already "diverges" on 235 fuzz and 18 mutate cases; those
  are artifacts of the bridge and adapter config, so they are subtracted.

  | Mode | Cases | Rust net divergences | C net divergences |
  |---|---|---|---|
  | fuzz (5 x 1,000 rounds) | 3,896 | 93 (23.9 per 1,000) | 93 (23.9 per 1,000) |
  | mutate (5 x 1,000) | 5,000 | 0 | 0 |
  | stress (5 x 100) | 500 | 0 | 0 |

  The two arms fail exactly the same 93 cases. 83 are missing partials in
  `include`/`render` given odd names, where the error category differs from Ruby;
  most of the rest are `-` whitespace control followed by a newline before `%}`
  (e.g. `{% case 1 -\n%}`), which both reject and Ruby accepts. Both agents
  learned the same parser rules from the same 1,905 parser_errors specs, gaps
  included.
- **A C bug the suite does not catch:** `{{ "2025-03-15T14:30:00Z" | date: "%B %e, %Y" }}`
  renders `December 13, 349486574350` in C; Rust and Ruby render `March 15, 2025`.
  It shows up in the `bench_storefront_order_email` benchmark, which only checks
  that rendering succeeds. Every other benchmark output is byte-identical across
  Rust, C and Ruby. The bug came in with C's last commit (`76b2ba4` "date:
  support bignum Unix timestamps"); the commit before it renders these dates
  correctly.
- **Random differential testing** (`eval/randspec.py`, `results/eval/randspec/`).
  A generator of random templates and data: Ruby's standard filters with
  well-typed and badly typed arguments, every tag, whitespace control, nested
  partials, unicode, big numbers, and syntax damage in 12% of cases. Each case
  runs on Rust, C and Ruby Liquid 5.13, all through the same JSON-RPC server
  protocol the specs use.

  How it was kept fair:
  - The generator was written from Ruby Liquid's filter and tag list, not from
    either arm's code, and committed (`89eff7a`) before the first full run.
  - 8 seeds x 6,000 cases; both arms get exactly the same cases.
  - 81 cases where Ruby itself crashed or timed out are dropped.
  - Each divergent case is shrunk to a minimal template, separately per arm.
  - Divergences are counted by root cause, not by case. The generator hits
    some code paths far more often than others, so raw counts mostly measure
    the generator.
  - Where both arms give the same answer and Ruby differs, neither arm is
    charged. Those cases were re-checked against Ruby in-process (no bridge),
    and Ruby gives the same answer there (`ruby-inprocess.txt`), so they are
    real Ruby behavior both arms miss, not harness artifacts.

  Of 47,919 usable cases, Rust diverges on 351 and C on 74. After shrinking
  and setting aside shared and "both differ" cases, the divergences that
  belong to one arm alone are:

  | Arm | Root cause | Cases | Impact |
  |---|---|---|---|
  | C | String dates misparsed: `"2025-03-15T14:30:00Z"`, `"March 5, 2021 10:00"`, `"2024-02-29"` give garbage years and weekdays | 5 | **High**: common in real templates; regression in `76b2ba4` |
  | C | `url_decode` raises on invalid UTF-8 (after `base64_decode`) | 1 | Low |
  | C | `0.1 \| divided_by: blank \| modulo: x` prints `NaN`; Ruby raises "divided by 0" | 1 | Low |
  | Rust | `sort` / `sort_natural` / `uniq` with a property, on a value that is not an array: Rust raises "cannot select the property" (or prints nothing for `uniq` on floats and booleans); Ruby returns the value unchanged | 212 | Medium: a page whose data has an unexpected shape errors in Rust and renders in Ruby |
  | Rust | Negative zero printed as `0.0` after `times`, `divided_by`, `at_least`, `at_most`, `round` | 56 | Cosmetic |
  | Rust | `.size` of an integer over 64 bits: 16 vs Ruby's 9 | 9 | Ruby quirk |
  | Rust | `date` leaves over-64-bit timestamps and junk strings untouched; Ruby parses them | 5 | Low |
  | Rust | `base64_decode` output that is not valid UTF-8 gets one fewer replacement character | 4 | Cosmetic |
  | Rust | `slice: 100, false` raises "invalid integer" | 1 | Low |
  | Rust | `truncate` with a huge negative length | 1 | Ruby quirk |

  Shared by both arms (not charged): integers beyond 64 bits are clamped where
  Ruby has bignums (most shared cases); `{% render 'p' with false as x %}` and
  the `include` form leave `x` unset in Ruby but pass `false` in both arms; `-0.0
  | modulo` loses its sign; Ruby raises on joining `base64_decode` output with
  UTF-8 text, both arms don't. Both arms also disagree with Ruby, and with each
  other, on how leniently junk strings parse as dates. Ruby's answers there
  depend on today's date.

  So C has fewer bugs (3 root causes vs 7) and far fewer divergent cases. But
  its one serious bug is the most damaging thing found in either arm, and it
  came from extra work after all specs passed. Without that last commit, C
  would have 2 root causes, both low impact.
- **Special-casing audit:** no spec names and none of the 188 long expected
  outputs appear in either arm's sources. C's 6 name hits are function names
  (`lq_tablerowloop_col`) that coincide with spec names.

## Performance

`eval/bench.c` linked against each `build/libliquid_fc.a` (same driver, same
`clang -O2`), on the 10 liquid-spec benchmark inputs. Median of 10 rounds, each
round at least 50 ms. Ruby Liquid 5.13 (YJIT off) on the same inputs as a
reference point. Full numbers in `results/eval/bench-*.tsv`.

| Geomean over 10 inputs | Rust | C | Ruby |
|---|---|---|---|
| Parse | 15.0 µs | **8.9 µs** | 353 µs |
| Build context (JSON decode) | 22.3 µs | **21.7 µs** | 15.6 µs (C-extension JSON) |
| Render | 25.7 µs | **16.6 µs** | 423 µs |

- C parses 1.69x and renders 1.55x faster than Rust. Both are 16–40x faster than
  Ruby Liquid.
- **C's speed was there from the start, not from tuning.** Benchmarking every
  commit of each arm with the same driver (`eval/bench-curve.sh`,
  `results/eval/bench-curve-*.tsv`) shows C's first working commit (`d55185d`)
  already at 8.2 µs parse / 16.4 µs render, and its final commit at 8.6 / 16.6.
  Rust went from 15.6 / 25.0 (`1896d64`) to 15.3 / 26.2. Neither arm has a
  performance commit. C ran one profiling pass with no gain, and its context
  building got slower (16.7 → 21.7 µs at `8b223d5`). C's 14 post-pass commits
  went to hardening, Ruby-quirk fidelity, test tooling and cleanup. The gap
  was in each arm's first working design, not in later work.
- Peak RSS of the whole benchmark run: Rust 40.8 MB, C 31.9 MB.
- Clean release build: Rust 7.2 s, C 0.8 s.
- Static library: Rust 26.1 MB (it bundles Rust's standard library), C 0.35 MB.
  JSON-RPC server binary: Rust 1.10 MB, C 0.26 MB.

## Security

**Coverage-guided fuzzing** (`eval/fuzz.c`, libFuzzer, 1 hour per arm,
concurrently, same seed corpus of 3,879 spec templates, `-timeout=10
-rss_limit_mb=2048`, fork mode so findings don't stop the run). Both libraries
were built with SanitizerCoverage so libFuzzer has feedback. C was also built
with ASan and UBSan (UB is fatal); Rust relies on its own checks, and a panic
across the C ABI aborts, so it counts as a crash. ASan for Rust would need
nightly. ASan makes C 2–3x slower and hungrier, so every finding was replayed
against the plain release build and against Ruby Liquid (`results/eval/fuzz-triage.tsv`,
`fuzz-cross.tsv`).

| | Rust | C (ASan+UBSan) |
|---|---|---|
| Executions | 23.4M | 9.7M |
| Crashes, sanitizer reports, leaks | **0** | **0** |
| Timeouts / OOMs under the fuzzer | 2 / 0 | 9 / 10 |
| ...that Ruby Liquid does not share | 0 | 0 |

- **No finding is implementation-specific.** All of them are templates that
  legitimately expand into enormous output (mostly `date` and `split` on garbled
  strings inside big loops). On each one Ruby Liquid also produces hundreds of
  MB of output or hits the same 10 s limit, and both arms are faster than Ruby.
  MANAGER.md's rule is that a hang Ruby shares is not a bug.
- **Memory is where they differ.** On C's 10 OOM inputs the C release build
  peaks at 2.0–6.5 GB; Rust runs the same inputs in 34–400 MB, so Rust bounds
  memory much more tightly here. On 2 inputs both are bad (10 s, or 9.5 s and
  6.7 GB).

**Hostile templates** (`eval/hostile.sh`, 10 s timeout, `results/eval/hostile.tsv`):

| Template | Rust | C | Ruby Liquid |
|---|---|---|---|
| 10,000 nested `{% if %}` | parse error, "Nesting too deep", 0.00 s | same, 0.00 s | same, 0.12 s |
| `for` over 100M items, empty body | "Memory limits exceeded", 0.15 s | same, 0.72 s | **timeout** |
| `for` over 100M items, 1 byte each | "Memory limits exceeded", 0.10 s | same, 0.52 s | **timeout** |
| Partial that `include`s itself | "Nesting too deep", 0.00 s | same | same |
| Partial that `render`s itself | "Nesting too deep", 0.00 s | same | same |
| String doubled by `append` 64 times | "Memory limits exceeded", 0.20 s, 347 MB | same, 0.09 s, 166 MB | **NoMemoryError at 7.4 GB** |

Both arms beat the reference here: every case ends in a proper Liquid error
within a second.

## Code

`eval/stats.sh` on the final commits (`results/eval/stats.tsv`). Non-blank lines,
excluding `harness/`.

| | Rust | C |
|---|---|---|
| Library | 8,317 | 9,766 |
| JSON-RPC server and C-ABI glue | 598 | 268 |
| Tests and tools | 215 | 689 |
| **Total** | **9,130** | **10,723** |
| `unsafe` outside the C-ABI crate | 0 | – |
| `unsafe` in the C-ABI crate | 36 | – |
| Warnings | 43 clippy (29 are "collapsible `if`"; all style) | 0 under gcc and clang with `-Wall -Wextra` |

The glue line counts show the caveat from EXPERIMENT.md: the C ABI costs Rust
598 lines of wrapper and all 36 of its `unsafe` blocks, and is free for C. The
standard-library gap shows the other way: C wrote its own containers, strings
and sorting and still came out only 17% larger in the library.

## Restarts, stalls and process notes

- **Restarts:** 0 for both. **Stalls:** none.
- **The `/goal` text could never be met.** It asked for "Complexity level
  cleared: 1000 of 1000" on every suite, but every parser_errors spec is rated
  complexity 500, so that suite tops out at "500 of 1000" even when perfect. Both
  agents found this on their own and refused to game it (no spec or harness
  edits). The manager judged "done" on the pass counts instead (user decision).
  `TASK.md` and `MANAGER.md` should say "0 failures and every in-scope spec
  passing" for any future run.
- **Rust lost about 30 minutes of wall clock** (14:46–15:16) on a one-time
  Claude Code prompt asking to read the Ruby Liquid reference outside its
  folder. It was answered with "keep allowing", which applies to both arms. No
  tokens were lost, but wall-clock times are skewed against Rust and are not
  compared here.
- **Environment:** WSL Ubuntu 24.04 on a 24-core Windows PC, Claude Code CLI
  2.1.316 in tmux, Ruby 3.2.3, Rust 1.99.1, gcc 13.3, clang 18.1. Both agents
  started from `~/lfc/launch.sh` with identical flags.
- Tokens to finish are measured at each arm's final commit. Both transcripts
  continue a little past it (Rust arguing with the unmeetable goal, C running
  a 30-minute fuzz campaign of its own).

## Caveats (from EXPERIMENT.md, plus this run)

- One run per language, so differences may be noise. The near-identical cost to
  reach 6,240 passes suggests the spec suite, not the language, dominated the
  first half.
- Each side has only its own standard library, and they cover different ground
  (Rust: containers, strings, sorting; C: date/time formatting in libc). C's
  worst bug is in its date handling anyway.
- The random generator is one person's guess at what templates look like. It
  leans on filter chains and type mismatches, which is where Rust's main bug
  lives, so Rust's raw case count is inflated; compare root causes.
- The shared C ABI is free for C and extra glue for Rust.
- The fuzzing setups are not identical by necessity: C runs under ASan+UBSan,
  Rust under its own runtime checks. Rust's 36 `unsafe` blocks were not
  sanitizer-checked.
- The held-out tools compare against an in-process Ruby reference whose config
  differs from the project adapter; results are reported net of a Ruby-through-
  the-bridge baseline.

## Reproducing

Everything is in `eval/` and runs from WSL with both arms checked out under
`~/lfc` and their final commits cloned to `~/lfc/eval-work/{rust,c}`:

- `eval/curve.sh`: per-commit re-scoring joined with token counts.
- `eval/heldout.sh`, `eval/heldout_diff.py`: held-out checks and the baseline subtraction.
- `eval/bench.c`, `eval/bench_ruby.rb`, `eval/extract.rb`: benchmarks.
- `eval/fuzz.c`, `eval/build-fuzz.sh`, `eval/run-fuzz.sh`, `eval/replay.c`,
  `eval/replay.rb`, `eval/triage.sh`: fuzzing and triage.
- `eval/hostile.sh`, `eval/run1.c`: hostile templates.
- `eval/stats.sh`, `eval/audit.rb`: code stats and the special-casing audit.
- `eval/bench-curve.sh`: benchmarks at every commit.
- `eval/randspec.py` (generate and run), `eval/randspec_min.py` (shrink and
  cluster), `eval/randspec_split.py` (arm-only vs shared),
  `eval/randspec_causes.py` (root-cause tally), `eval/randspec_replay.py`
  (replay on another build), `eval/randspec_verify.rb` (Ruby in-process).

Raw data is in `results/`. The agent transcripts (12 MB) stay local in
`~/lfc/results/transcripts/` and are not committed.
