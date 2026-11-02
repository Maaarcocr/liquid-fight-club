#!/usr/bin/env bash
# Re-score every commit of one arm: bin/build + harness/spec in a scratch
# worktree (a failed or missing build counts as 0 passes), joined with the
# cumulative token counts from usage-ARM.csv.
#   eval/curve.sh ARM REPO USAGE_CSV OUT_CSV WORK_DIR
set -uo pipefail
arm=$1; repo=$2; usage=$3; out=$4; work=$5
rm -rf "$work"; mkdir -p "$work"
git clone -q --no-hardlinks "$repo" "$work/src"
commits=$(git -C "$work/src" rev-list --reverse HEAD)
n=$(wc -w <<<"$commits")
if [ "$n" -gt 200 ]; then  # sample about 100 evenly, always keep the last
  commits=$(awk -v n="$n" '{a[NR]=$0} END {for (i=1;i<=100;i++) print a[int((i*n)/100)]}' <<<"$commits" | uniq)
fi
echo "commit,time,total_tokens,output_tokens,build,default,parser_errors,partials,benchmarks,failures" > "$out"
for sha in $commits; do
  rm -rf "$work/wt"; git -C "$work/src" worktree prune
  git -C "$work/src" worktree add -q --detach "$work/wt" "$sha"
  short=${sha:0:10}
  read -r tot outtok when < <(python3 -c '
import csv, sys
for r in csv.DictReader(open(sys.argv[1])):
    if r["commit"] == sys.argv[2]:
        print(r["total"], r["output"], r["time"])' "$usage" "$short")
  if [ -x "$work/wt/bin/build" ] && (cd "$work/wt" && bin/build > "$work/build-$short.log" 2>&1); then
    build=ok
    res=$(cd "$work/wt" && harness/spec 2>/dev/null)
  else
    build=fail; res=""
  fi
  passes() { grep -E "^$1 " <<<"$res" | grep -oE '[0-9]+ passes' | grep -oE '[0-9]+' || echo 0; }
  fails=$(grep -oE '[0-9]+ failures' <<<"$res" | grep -oE '[0-9]+' | paste -sd+ | bc 2>/dev/null || echo 0)
  echo "$short,$when,${tot:-0},${outtok:-0},$build,$(passes default),$(passes parser_errors),$(passes partials),$(passes benchmarks),${fails:-0}" | tee -a "$out"
done
git -C "$work/src" worktree remove --force "$work/wt" 2>/dev/null
