#!/usr/bin/env bash
# Held-out differential checks against Ruby Liquid with fixed seeds, same for
# both arms: liquid-spec tools fuzz / mutate / stress.
#   eval/heldout.sh ARM_CHECKOUT OUT_DIR SEED...
# Writes one JSON summary per (mode, seed) plus summary.tsv.
# SUBJECT_CMD, if set, replaces the arm's build/liquid-server as the subject
# (used for the baseline: Ruby Liquid itself behind the same JSON-RPC adapter).
set -uo pipefail
co=$1; out=$2; shift 2
mkdir -p "$out"
cd "$co"
subj=(); [ -n "${SUBJECT_CMD:-}" ] && subj=(--command="$SUBJECT_CMD")
for s in "$@"; do
  harness/liquid-spec tools fuzz harness/adapter.rb "${subj[@]}" --seed="$s" --rounds=1000 --limit=1000 \
    --save="$out/cases-fuzz-$s" --json > "$out/fuzz-$s.json" 2> "$out/fuzz-$s.err"
  harness/liquid-spec tools mutate harness/adapter.rb "${subj[@]}" --seed="$s" --limit=1000 \
    --save="$out/cases-mutate-$s" --json > "$out/mutate-$s.json" 2> "$out/mutate-$s.err"
  harness/liquid-spec tools stress harness/adapter.rb "${subj[@]}" --seed="$s" --limit=100 \
    --save="$out/cases-stress-$s" --json > "$out/stress-$s.json" 2> "$out/stress-$s.err"
done
python3 - "$out" <<'EOF'
import json, glob, os, sys
out = sys.argv[1]
rows = {}
for f in sorted(glob.glob(os.path.join(out, "*.json"))):
    mode = os.path.basename(f).split("-")[0]
    try:
        d = json.load(open(f))
    except ValueError:
        print(f"unreadable: {f}", file=sys.stderr); continue
    r = rows.setdefault(mode, [0, 0])
    r[0] += d.get("executed", 0); r[1] += d.get("failed", 0)
with open(os.path.join(out, "summary.tsv"), "w") as fh:
    fh.write("mode\texecuted\tdivergences\tper_1000\n")
    for mode, (ex, fail) in sorted(rows.items()):
        fh.write(f"{mode}\t{ex}\t{fail}\t{1000 * fail / ex if ex else 0:.2f}\n")
print(open(os.path.join(out, "summary.tsv")).read())
EOF
