#!/usr/bin/env python3
"""Replay one arm's minimized divergences against another build of that arm
(e.g. an earlier commit) and report which ones match Ruby there.

  randspec_replay.py MINIMIZED_JSONL ARM SERVER_CMD
"""
import json, os, sys

sys.path.insert(0, os.path.dirname(__file__))
from randspec import Server, classify  # noqa: E402

path, arm, cmd = sys.argv[1:4]
srv = Server(arm + "-alt", cmd)
fixed = still = 0
for line in open(path):
    r = json.loads(line)
    if r["arm"] != arm: continue
    got = srv.run_case(r)
    if classify(got, tuple(r["results"]["ruby"])):
        still += 1
    else:
        fixed += 1
        print(f"matches Ruby: {r['template'][:80]!r}")
srv.stop()
print(f"{arm}: {fixed} match Ruby on this build, {still} still diverge")
