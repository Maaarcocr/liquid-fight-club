#!/usr/bin/env python3
"""Held-out divergences net of the baseline.

The baseline is Ruby Liquid itself run as the subject through the same
JSON-RPC adapter, so cases where it already disagrees with the in-process
reference are artifacts of the bridge/adapter config, not implementation bugs.

  eval/heldout_diff.py BASELINE_DIR ARM_DIR [ARM_DIR...]
"""
import collections, glob, json, os, sys


def load(d):
    cases, executed = {}, collections.Counter()
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        mode = os.path.basename(f).split("-")[0]
        data = json.load(open(f))
        executed[mode] += data.get("executed", 0)
        for x in data.get("failures", []):
            cases[(mode, x["id"])] = x
    return cases, executed


base, _ = load(sys.argv[1])
print("arm\tmode\texecuted\tdivergences\tnet_of_baseline\tnet_per_1000\tfixed_vs_baseline")
for d in sys.argv[2:]:
    cases, executed = load(d)
    arm = os.path.basename(d.rstrip("/")).replace("heldout-", "")
    for mode in sorted(executed):
        mine = {k for k in cases if k[0] == mode}
        theirs = {k for k in base if k[0] == mode}
        net = mine - theirs
        print(f"{arm}\t{mode}\t{executed[mode]}\t{len(mine)}\t{len(net)}\t"
              f"{1000 * len(net) / executed[mode] if executed[mode] else 0:.2f}\t{len(theirs - mine)}")
        for k in sorted(net)[:5]:
            x = cases[k]
            print(f"#   {k[1]} {x.get('classification')}: {x.get('template', '')[:90]!r}")
