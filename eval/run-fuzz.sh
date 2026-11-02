#!/usr/bin/env bash
# One libFuzzer campaign for one arm, identical flags for both.
#   eval/run-fuzz.sh ARM FUZZ_BIN SEED_CORPUS OUT_DIR [SECONDS]
# Keeps going after crashes/timeouts/OOMs so findings can be counted;
# artifacts land in OUT_DIR/artifacts, the log in OUT_DIR/fuzz.log.
set -uo pipefail
arm=$1; bin=$2; seeds=$3; out=$4; secs=${5:-3600}
mkdir -p "$out/corpus" "$out/artifacts"
cd "$out"
date -Is > started
"$bin" "$out/corpus" "$seeds" \
  -fork=1 -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1 \
  -max_total_time="$secs" -timeout=10 -rss_limit_mb=2048 -max_len=65536 \
  -seed=20261102 -print_final_stats=1 \
  -artifact_prefix="$out/artifacts/" > "$out/fuzz.log" 2>&1
echo "exit=$?" > finished; date -Is >> finished
