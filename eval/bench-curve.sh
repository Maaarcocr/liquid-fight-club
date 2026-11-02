#!/usr/bin/env bash
# Benchmark every commit of one arm that builds build/libliquid_fc.a, with the
# same eval/bench.c driver and inputs, to see when performance changed.
#   eval/bench-curve.sh ARM REPO BENCH_DATA_DIR OUT_TSV WORK_DIR
set -uo pipefail
arm=$1; repo=$2; data=$3; out=$4; work=$5
here="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$work"; mkdir -p "$work"
git clone -q --no-hardlinks "$repo" "$work/src"
dirs=$(ls -d "$data"/*/ | sed 's#/$##')
printf 'commit\tname\tparse_us\tcontext_us\trender_us\toutput_bytes\toutput_fnv1a\n' > "$out"
for sha in $(git -C "$work/src" rev-list --reverse HEAD); do
  rm -rf "$work/wt"; git -C "$work/src" worktree prune
  git -C "$work/src" worktree add -q --detach "$work/wt" "$sha"
  short=${sha:0:7}
  if ! (cd "$work/wt" && [ -x bin/build ] && bin/build > "$work/build-$short.log" 2>&1) \
     || [ ! -f "$work/wt/build/libliquid_fc.a" ]; then
    echo "$short: no C ABI library, skipped"; continue
  fi
  clang -O2 -I"$work/wt/harness" "$here/bench.c" "$work/wt/build/libliquid_fc.a" \
    -lpthread -ldl -lm -o "$work/bench" || { echo "$short: link failed"; continue; }
  "$work/bench" $dirs | tail -n +2 | sed "s/^/$short\t/" >> "$out"
  echo "$short: done"
done
