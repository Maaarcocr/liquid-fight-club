#!/usr/bin/env python3
"""Split minimized randspec divergences into arm-only and shared.

  randspec_split.py MINIMIZED_JSONL

arm-only: the other arm matches Ruby on the minimized case, so the divergence
          is this arm's alone.
shared:   both arms give the same result and Ruby differs. That is either a
          bug both arms copied, or a difference in how Ruby is driven; these
          are checked by hand and not counted against either arm.
"""
import collections, json, sys

recs = [json.loads(l) for l in open(sys.argv[1])]
groups = collections.defaultdict(lambda: collections.defaultdict(list))
for r in recs:
    res = r["results"]
    other = "c" if r["arm"] == "rust" else "rust"
    if res[other] == res["ruby"]:
        kind = "arm-only"
    elif res["rust"] == res["c"]:
        kind = "shared"
    else:
        kind = "both-differ"
    groups[(r["arm"], kind)][r["signature"]].append(r)

def short(x):
    s = json.dumps(x, ensure_ascii=False)
    return s if len(s) < 160 else s[:157] + "..."

for (arm, kind), sigs in sorted(groups.items()):
    n = sum(len(v) for v in sigs.values())
    print(f"\n######## {arm} / {kind}: {n} cases, {len(sigs)} clusters")
    for sig, rs in sorted(sigs.items(), key=lambda kv: -len(kv[1])):
        ex = min(rs, key=lambda r: len(r["template"]))
        print(f"{len(rs):4d}  {sig}")
        print(f"      tpl  {ex['template']!r}")
        env = {k: v for k, v in ex["environment"].items()}
        if env: print(f"      env  {short(env)}")
        for n_ in ("ruby", "rust", "c"):
            print(f"      {n_:4s} {short(ex['results'][n_])}")
