#!/usr/bin/env python3
"""Assign each arm-only randspec divergence to a root cause (hand-written rules,
derived from reading every minimized cluster), and tally them.

  randspec_causes.py MINIMIZED_JSONL
"""
import collections, json, re, sys


def kind(r):
    res = r["results"]
    other = "c" if r["arm"] == "rust" else "rust"
    if res[other] == res["ruby"]:
        return "arm-only"
    return "shared" if res["rust"] == res["c"] else "both-differ"


def cause(r):
    t, res = r["template"], r["results"]
    mine, ruby = res[r["arm"]], res["ruby"]
    if r["arm"] == "rust":
        if "cannot select the property" in mine[1] or (
                re.search(r"\|\s*(uniq|sort|sort_natural)\s*:", t) and len(mine[1]) < len(ruby[1])):
            return "R1 sort / sort_natural / uniq with a property on a non-array input"
        if re.search(r"-0\.0\d*", ruby[1]) and not re.search(r"-0\.0\d*", mine[1]):
            return "R2 negative zero printed as 0.0"
        if ruby[1].endswith("9") and mine[1].endswith("16"):
            return "R3 .size of a big integer"
        if "date" in t:
            return "R4 date leaves big-integer timestamps and junk strings unparsed"
        if "base64" in t:
            return "R5 invalid UTF-8 from base64_decode collapsed differently"
        if "slice" in t:
            return "R6 slice with a false length raises"
        if "truncate" in t:
            return "R7 truncate with a huge negative length"
    else:
        if "date" in t:
            return "C1 date strings misparsed (regression in 76b2ba4)"
        if "url_decode" in t:
            return "C2 url_decode on invalid UTF-8 raises"
        if "modulo" in t:
            return "C3 float modulo after divide by blank prints NaN"
    return "unassigned: " + t[:60]


recs = [json.loads(l) for l in open(sys.argv[1])]
tally = collections.defaultdict(collections.Counter)
for r in recs:
    k = kind(r)
    tally[(r["arm"], k)][cause(r) if k == "arm-only" else "-"] += 1
for key in sorted(tally):
    print(f"{key[0]} / {key[1]}: {sum(tally[key].values())} cases")
    if key[1] == "arm-only":
        for c, n in sorted(tally[key].items()):
            print(f"   {n:4d}  {c}")
