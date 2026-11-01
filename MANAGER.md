# Manager plan

You are the manager of the Rust vs C experiment (see `EXPERIMENT.md`). You:

- start two implementer agents (Rust and C);
- keep them running until they finish;
- collect the data;
- score both implementations;
- write `REPORT.md`.

You never help either implementer, and you treat both identically.

The implementers are separate headless Claude Code processes (`claude -p`),
not in-session sub-agents. That way:

- each can use `/goal`;
- each keeps its own transcript (token data);
- neither sees the other.

Everything runs on this PC under the user's Claude subscription.

## Fairness rules

- Same command, same goal text, same restart policy for both. Launch them
  within a minute of each other.
- The only thing you ever send an implementer is the goal command below.
- Don't touch an implementer's checkout while its process is running. That
  includes running `harness/spec` there, which writes to its `build/`.
- Keep state in files (`logs/`), not in your context, so you can recover
  after compaction or a restart.

## 1. Preflight

```bash
mkdir -p ~/lfc && cd ~/lfc
git clone --single-branch -b experiment https://github.com/Maaarcocr/liquid-fight-club experiment
git clone --single-branch -b impl/rust  https://github.com/Maaarcocr/liquid-fight-club liquid-rust
git clone --single-branch -b impl/c     https://github.com/Maaarcocr/liquid-fight-club liquid-c
mkdir -p logs results
```

- Check that these exist: `ruby`, `bundle`, `cargo`, `gcc`, `clang`, `make`,
  `git`, `claude`.
- Install the spec harness once, before launch, so the two agents don't race
  on `bundle install`: `(cd liquid-rust && harness/liquid-spec docs >/dev/null)`.
- Generate one session UUID per arm and save them in `logs/sessions.env`:

  ```bash
  echo "RUST_ID=$(uuidgen | tr A-Z a-z)" >> logs/sessions.env
  echo "C_ID=$(uuidgen | tr A-Z a-z)"    >> logs/sessions.env
  ```

## 2. Launch

The goal text is identical for both arms:

```bash
GOAL="Everything TASK.md asks for is done: harness/spec shows all four suites at 'Complexity level cleared: 1000 of 1000' with 0 failures, NOTES.md is written, and all work is committed."
```

Model (Opus 5.5) and effort (high) are pinned by each checkout's
`.claude/settings.json`. Don't pass `--model` or `--effort`.

Start both detached, so they outlive your shell command:

```bash
source logs/sessions.env
cd ~/lfc/liquid-rust && nohup claude -p "/goal $GOAL" --session-id "$RUST_ID" \
  --permission-mode auto --output-format stream-json --verbose \
  > ~/lfc/logs/rust-1.jsonl 2> ~/lfc/logs/rust-1.err & echo $! > ~/lfc/logs/rust.pid
cd ~/lfc/liquid-c && nohup claude -p "/goal $GOAL" --session-id "$C_ID" \
  --permission-mode auto --output-format stream-json --verbose \
  > ~/lfc/logs/c-1.jsonl 2> ~/lfc/logs/c-1.err & echo $! > ~/lfc/logs/c.pid
```

If auto mode isn't available on this account, use
`--permission-mode bypassPermissions` for both.

Log the launch times in `logs/events.tsv`.

## 3. Monitor (about every 30 minutes)

Use whatever scheduling your harness offers (for example a wakeup or loop,
or waiting on a background `sleep 1800`). For each arm:

- Alive? `kill -0 $(cat logs/<arm>.pid)`.
- Progress: number of commits and the last commit message
  (`git -C liquid-<arm> log --oneline | wc -l`, `git -C liquid-<arm> log -1 --format=%s`).
  The agents put their `harness/spec` summary in commit messages.
- Tokens:
  `python3 experiment/tools/usage.py "$(dirname ~/.claude/projects/*/$<ARM>_ID.jsonl)"`
  (total line only).
- Append one row per arm to `logs/progress.tsv`: time, arm, alive, commits,
  last commit subject, total tokens.

Don't read their code or transcripts in detail while they run.

## 4. When an implementer process exits

1. Read the final `result` line of its latest `logs/<arm>-N.jsonl` (subtype,
   `is_error`, result text). Record the exit and the reason in
   `logs/events.tsv`.
2. Verify for yourself:

   ```bash
   cd liquid-<arm> && harness/spec && test -s NOTES.md && test -z "$(git status --porcelain)"
   ```

   If all three pass, the arm is **DONE**. Record its finish time and final
   commit.
3. Otherwise, resume the same session with the same goal:

   ```bash
   cd ~/lfc/liquid-<arm> && nohup claude -p "/goal $GOAL" --resume "$<ARM>_ID" \
     --permission-mode auto --output-format stream-json --verbose \
     > ~/lfc/logs/<arm>-<N+1>.jsonl 2> ~/lfc/logs/<arm>-<N+1>.err & echo $! > ~/lfc/logs/<arm>.pid
   ```

   - **Usage limit hit:** wait until the limit resets, then resume. Both arms
     share the subscription, so the other one is probably waiting too.
   - **Stalled:** 3 restarts in a row with no new commit. Stop the arm and
     mark it **STALLED**.

## 5. Freeze (both arms DONE or STALLED)

- Record final commit SHAs, total wall-clock, and the number of restarts.
- Push the implementations: `git -C liquid-rust push origin impl/rust`, and
  the same for C.
- Archive the transcripts (they get auto-deleted after 30 days otherwise):
  copy `dirname ~/.claude/projects/*/$RUST_ID.jsonl` to
  `results/transcripts/rust/`, and the same for C. Also copy `logs/`
  to `results/logs/`.
- Token data:
  `python3 experiment/tools/usage.py results/transcripts/<arm> --repo liquid-<arm> --csv results/usage-<arm>.csv`

## 6. Evaluate

Write the scoring tools in `experiment/eval/`, ideally while the agents are
still running. Use only `harness/liquid_fc.h` and liquid-spec; never look at
either implementation's code to tailor them. Build each arm with its own
`bin/build`. Run every measurement the same way on both arms.

**Correctness**
- Final `harness/spec` line per suite.
- **Curve:** re-score each commit in a temporary worktree (`bin/build` +
  `harness/spec`; a failed build counts as 0 passes). If there are more than
  200 commits, sample about 100 evenly. Join with `usage-<arm>.csv` into
  `results/curve-<arm>.csv`: commit, tokens so far, passes per suite.
- **Held-out:** pick 5 random seeds now, the same for both arms, and run:

  ```bash
  harness/liquid-spec tools fuzz harness/adapter.rb --seed=S --rounds=1000 --json
  harness/liquid-spec tools mutate harness/adapter.rb --json
  harness/liquid-spec tools stress harness/adapter.rb --json
  ```

  Check `tools help` for the exact flags. Report divergences from the
  reference per 1,000 cases.
- Quick audit for special-cased specs: grep the source for spec names and
  long expected strings.

**Performance**
- `eval/bench.c`, linked against `liquid-<arm>/build/libliquid_fc.a`.
- Inputs: each liquid-spec benchmark template (from `specs/benchmarks/*.yml`),
  with its environment as assigns JSON and its filesystem as partials JSON.
- Time parse and render separately, as the median of 10 rounds.
- Also record peak RSS (`/usr/bin/time`), library size, and clean build time.
- Same compiler flags for both drivers. Optionally run Ruby Liquid on the
  same templates as a reference point.

**Security**
- `eval/fuzz.c`, a libFuzzer target over the ABI. Input = template; assigns =
  a fixed small JSON object. Build:
  `clang -fsanitize=fuzzer,address,undefined eval/fuzz.c liquid-<arm>/build/libliquid_fc.a ...`
- Run 1 hour per arm with the same seed corpus (every template in the spec
  files): `-max_total_time=3600 -timeout=10 -rss_limit_mb=2048`. A Rust
  panic aborts, so it counts as a crash.
- Count unique crashes, timeouts and OOMs; minimize and triage each.
- Hostile templates, each with a 10 s timeout. Compare with Ruby Liquid's
  behaviour (a hang Ruby shares isn't a bug):
  - 10,000 nested `{% if %}` blocks;
  - a 100-million-item `for` loop;
  - a partial that includes itself;
  - a string doubled by `append` in a loop until it's huge.
- Optional: one fresh reviewer session per arm, same prompt and goal ("find
  security bugs; report only ones with a working reproducer").

**Code stats**
- Lines of code, excluding `harness/`.
- `unsafe` occurrences in Rust outside the C-ABI glue.
- Warnings: C with `-Wall -Wextra` under gcc and clang; Rust with
  `cargo clippy`.

## 7. Report

Write `experiment/REPORT.md` and commit it to the `experiment` branch along
with `results/` (transcripts can stay local if they are large).

- A summary table: tokens to finish, final passes, held-out divergences,
  speed, fuzz crashes, LOC.
- The specs-passing-vs-tokens curve for both arms (CSV, plus a chart if
  convenient).
- Restarts and stalls.
- The caveats from `EXPERIMENT.md`.

Lead with the answer: which language did better on each dimension, and by
how much.
