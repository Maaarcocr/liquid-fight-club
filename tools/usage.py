#!/usr/bin/env python3
"""Token usage from local Claude Code transcripts.

Usage:
  tools/usage.py TRANSCRIPT_DIR [--repo REPO] [--csv OUT.csv]

TRANSCRIPT_DIR is the session folder under ~/.claude/projects/ for the
implementation checkout (e.g. ~/.claude/projects/-home-me-liquid-rust).
All *.jsonl files below it are read, including sub-agent transcripts.

Prints totals per model. With --repo, also prints cumulative usage at each
commit of that checkout (joined by timestamp), optionally written as CSV.
"""
import argparse, csv, json, pathlib, subprocess, sys
from datetime import datetime

FIELDS = ["input", "cache_write", "cache_read", "output"]


def ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def load(dirpath):
    msgs = {}  # message id -> (time, model, usage); last entry wins
    for f in sorted(pathlib.Path(dirpath).expanduser().rglob("*.jsonl")):
        for line in f.open(encoding="utf-8", errors="replace"):
            try:
                e = json.loads(line)
            except ValueError:
                continue
            m = e.get("message") if e.get("type") == "assistant" else None
            if not isinstance(m, dict) or not m.get("usage") or not e.get("timestamp"):
                continue
            u = m["usage"]
            msgs[m.get("id") or e.get("uuid")] = (ts(e["timestamp"]), m.get("model", "?"), {
                "input": u.get("input_tokens", 0) or 0,
                "cache_write": u.get("cache_creation_input_tokens", 0) or 0,
                "cache_read": u.get("cache_read_input_tokens", 0) or 0,
                "output": u.get("output_tokens", 0) or 0,
            })
    return sorted(msgs.values(), key=lambda r: r[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("transcripts")
    ap.add_argument("--repo")
    ap.add_argument("--csv")
    a = ap.parse_args()
    rows = load(a.transcripts)
    if not rows:
        sys.exit("no assistant messages with usage found")

    per_model = {}
    for _, model, u in rows:
        t = per_model.setdefault(model, dict.fromkeys(FIELDS, 0) | {"requests": 0})
        for k in FIELDS:
            t[k] += u[k]
        t["requests"] += 1
    print(f"{'model':<24}{'requests':>10}" + "".join(f"{k:>14}" for k in FIELDS) + f"{'total':>15}")
    for model, t in sorted(per_model.items()):
        print(f"{model:<24}{t['requests']:>10}" + "".join(f"{t[k]:>14,}" for k in FIELDS)
              + f"{sum(t[k] for k in FIELDS):>15,}")
    print(f"first request {rows[0][0]:%Y-%m-%d %H:%M}, last {rows[-1][0]:%Y-%m-%d %H:%M}, "
          f"wall clock {rows[-1][0] - rows[0][0]}")

    if not a.repo:
        return
    log = subprocess.run(["git", "-C", a.repo, "log", "--reverse", "--format=%H%x09%cI%x09%s"],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    out, i, cum = [], 0, dict.fromkeys(FIELDS, 0)
    for line in log:
        sha, when, subject = line.split("\t", 2)
        when = ts(when)
        while i < len(rows) and rows[i][0] <= when:
            for k in FIELDS:
                cum[k] += rows[i][2][k]
            i += 1
        out.append({"commit": sha[:10], "time": when.isoformat(), **cum,
                    "total": sum(cum.values()), "subject": subject})
    print(f"\n{'commit':<12}{'time':<27}{'output':>12}{'total':>15}  subject")
    for r in out:
        print(f"{r['commit']:<12}{r['time']:<27}{r['output']:>12,}{r['total']:>15,}  {r['subject'][:60]}")
    if a.csv:
        with open(a.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(out[0]))
            w.writeheader()
            w.writerows(out)


if __name__ == "__main__":
    main()
