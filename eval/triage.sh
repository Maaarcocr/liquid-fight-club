#!/usr/bin/env bash
# Triage libFuzzer artifacts: replay each one against the arm's plain release
# library and against Ruby Liquid, each with a 10 s timeout.
#   eval/triage.sh EVAL_WORK_DIR OUT_TSV
set -uo pipefail
w=$1; out=$2
here="$(cd "$(dirname "$0")" && pwd)"
for arm in rust c; do
  clang -O2 -I"$w/$arm/harness" "$here/fuzz.c" "$here/replay.c" "$w/$arm/build/libliquid_fc.a" \
    -lpthread -ldl -lm -o "$w/replay-$arm"
done
run() {  # prints "wall rss exit"
  # 10 s and a 10 GB address-space cap, so one runaway case can't take down the VM.
  /usr/bin/time -f '%e %M' -o "$w/t.txt" prlimit --as=$((10 << 30)) timeout -s KILL 10 "$@" > "$w/o.txt" 2>&1
  local code=$?
  read -r wall rss < <(grep -E '^[0-9.]+ [0-9]+$' "$w/t.txt" | tail -1)
  echo "${wall:-?} ${rss:-?} $code"
}
printf 'arm\tartifact\tbytes\trelease_wall_s\trelease_rss_kb\trelease_exit\truby_wall_s\truby_rss_kb\truby_result\tpreview\n' > "$out"
for arm in rust c; do
  for f in "$w/fuzzrun-$arm/artifacts"/*; do
    read -r rw rr rc < <(run "$w/replay-$arm" "$f")
    read -r bw br bc < <(cd "$here/.." && BUNDLE_GEMFILE="$w/$arm/harness/Gemfile" run ruby "$here/replay.rb" "$f")
    bres=$( [ "$bc" = 137 ] && echo TIMEOUT || head -c 60 "$w/o.txt" | tr '\n\t' '  ')
    preview=$(head -c 100 "$f" | tr -c '[:print:]' '.')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$(basename "$f")" "$(stat -c %s "$f")" \
      "$rw" "$rr" "$rc" "$bw" "$br" "$bres" "$preview" | tee -a "$out"
  done
done
