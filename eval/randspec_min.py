#!/usr/bin/env python3
"""Shrink and cluster randspec divergences, per arm.

For each divergent case and each arm that diverges, repeatedly remove template
chunks ({{ }}, {% %} or text), single filters and unused environment keys while
the arm still diverges from Ruby in the same category. Minimal cases are then
grouped by (category, tags and filters left in the template).

  randspec_min.py IN_DIR OUT_JSONL --rust CMD --c CMD --ruby CMD [--limit N]
"""
import argparse, collections, glob, json, os, random, re, sys

sys.path.insert(0, os.path.dirname(__file__))
from randspec import Server, classify  # noqa: E402

CHUNK = re.compile(r"(\{\{.*?\}\}|\{%.*?%\}|[^{]+|\{)", re.S)
FILTER = re.compile(r"\|\s*[a-z_0-9]+(?:\s*:\s*(?:'[^']*'|\"[^\"]*\"|[^|}%])*)?")


BLOCKS = {"if", "unless", "case", "for", "tablerow", "capture", "ifchanged", "raw", "comment"}
TAGNAME = re.compile(r"\{%-?\s*([a-z_#]+)")


def tag_name(chunk):
    m = TAGNAME.match(chunk)
    return m.group(1) if m else None


def block_edits(chunks):
    """Candidate chunk lists: each block removed whole, or unwrapped (its body kept)."""
    for i, ch in enumerate(chunks):
        name = tag_name(ch)
        if name not in BLOCKS: continue
        depth, mids = 0, []
        for j in range(i + 1, len(chunks)):
            n = tag_name(chunks[j])
            if n == name: depth += 1
            elif n == "end" + name:
                if depth == 0:
                    yield chunks[:i] + chunks[j + 1:]
                    keep = [k for k in range(i + 1, j) if k not in mids]
                    yield chunks[:i] + [chunks[k] for k in keep] + chunks[j + 1:]
                    break
                depth -= 1
            elif depth == 0 and n in ("else", "elsif", "when"):
                mids.append(j)


def signature(cat, t):
    filters = sorted(set(re.findall(r"\|\s*([a-z_0-9]+)", t)))
    if filters:
        return f"{cat} | filters: {','.join(filters)}"
    tags = sorted(set(TAGNAME.findall(t)) - {"else", "when", "elsif"} - {"end" + b for b in BLOCKS})
    return f"{cat} | tags: {','.join(tags) or '-'}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir"); ap.add_argument("out")
    for n in ("rust", "c", "ruby"): ap.add_argument("--" + n, required=True)
    ap.add_argument("--limit", type=int, default=0, help="max cases per arm (0 = all)")
    a = ap.parse_args()
    srv = {n: Server(n, getattr(a, n)) for n in ("rust", "c", "ruby")}

    def cat_of(arm, case):
        ref = srv["ruby"].run_case(case)
        if ref[0] in ("timeout", "crash"): return None
        return classify(srv[arm].run_case(case), ref)

    def shrink(arm, case, cat):
        c = dict(case)
        def still(t=None, env=None):
            x = dict(c);
            if t is not None: x["template"] = t
            if env is not None: x["environment"] = env
            return cat_of(arm, x) == cat
        changed = True
        while changed:
            changed = False
            chunks = CHUNK.findall(c["template"])
            progress = True
            while progress:  # whole blocks first
                progress = False
                for cand in block_edits(chunks):
                    t = "".join(cand)
                    if t != c["template"] and still(t=t):
                        chunks, c["template"], changed, progress = cand, t, True, True
                        break
            n = len(chunks); size = max(1, n // 2)
            while size >= 1:  # ddmin-style chunk removal
                i = 0
                while i < len(chunks):
                    t = "".join(chunks[:i] + chunks[i + size:])
                    if t != c["template"] and still(t=t):
                        chunks = chunks[:i] + chunks[i + size:]; c["template"] = t; changed = True
                    else:
                        i += size
                size //= 2
            for m in list(FILTER.finditer(c["template"]))[::-1]:  # single filters
                t = c["template"][:m.start()] + c["template"][m.end():]
                if still(t=t): c["template"] = t; changed = True
            for k in list(c["environment"]):  # unused data
                env = {kk: v for kk, v in c["environment"].items() if kk != k}
                if still(env=env): c["environment"] = env; changed = True
        c["filesystem"] = {k: v for k, v in c["filesystem"].items() if f"'{k}'" in c["template"] or k in c["template"]}
        if not still(): c["filesystem"] = case["filesystem"]
        return c

    cases = []
    for f in sorted(glob.glob(os.path.join(a.indir, "cases-*.jsonl"))):
        cases += [json.loads(l) for l in open(f)]
    rng = random.Random(0)
    clusters = {"rust": collections.defaultdict(list), "c": collections.defaultdict(list)}
    with open(a.out, "w") as out:
        for arm in ("rust", "c"):
            mine = [c for c in cases if classify(tuple(c["results"][arm]), tuple(c["results"]["ruby"]))]
            if a.limit and len(mine) > a.limit: mine = rng.sample(mine, a.limit)
            for c in mine:
                c = {k: v for k, v in c.items() if k != "results"}
                cat = cat_of(arm, c)
                if not cat: continue  # not reproducible in isolation
                m = shrink(arm, c, cat)
                res = {n: srv[n].run_case(m) for n in ("ruby", "rust", "c")}
                sig = signature(cat, m["template"])
                rec = {"arm": arm, "category": cat, "signature": sig, "id": c["id"], "template": m["template"],
                       "environment": m["environment"], "filesystem": m["filesystem"],
                       "strict_errors": m["strict_errors"], "results": res,
                       "other_arm_agrees_with_ruby": res["c" if arm == "rust" else "rust"] == res["ruby"]}
                out.write(json.dumps(rec, ensure_ascii=False) + "\n"); out.flush()
                clusters[arm][sig].append(rec)
    for s in srv.values(): s.stop()
    for arm in ("rust", "c"):
        print(f"== {arm}: {sum(len(v) for v in clusters[arm].values())} minimized cases, {len(clusters[arm])} clusters")
        for sig, recs in sorted(clusters[arm].items(), key=lambda kv: -len(kv[1])):
            ex = min(recs, key=lambda r: len(r["template"]))
            print(f"{len(recs):5d}  {sig}\n       e.g. {ex['template']!r}")


if __name__ == "__main__":
    main()
