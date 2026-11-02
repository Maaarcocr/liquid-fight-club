#!/usr/bin/env python3
"""Random Liquid specs, run differentially on Rust, C and Ruby Liquid.

The generator is written from the Liquid language (tags, operators and Ruby
Liquid's standard filter list), never from either implementation. Every case
is sent as the identical JSON-RPC request to three servers: each arm's
build/liquid-server and Ruby Liquid's own reference server
(liquid-spec scripts/remote-liquid). Ruby is the oracle.

  randspec.py run --seed S --cases N --out DIR --rust CMD --c CMD --ruby CMD
  randspec.py show CASE_JSON          # print one generated case

Each run writes DIR/cases-S.jsonl (divergent cases only) and DIR/stats-S.json.
"""
import argparse, json, os, random, select, subprocess, sys, time

# ----------------------------------------------------------------- generator

STRINGS = ["", " ", "hello", "Hello World", "  padded  ", "a,b,c", "a b  c", "x",
           "<p>Hi & 'bye'</p>", "&amp;&lt;", "line1\nline2\r\nline3", "tab\there",
           "école", "ÉTÉ", "straße", "İstanbul", "日本語テキスト", "emoji 🎉👍", "ź",
           "12", "-3", "3.50", "1e3", "0x1F", " 42 ", "abc123", "true", "nil",
           "2024-02-29", "2025-03-15T14:30:00Z", "March 5, 2021 10:00", "1700000000",
           "Fri Jul 16 01:00:00 2004", "foo-bar_baz", "a%20b+c", "Zm9vYmFy", "Zm9v_-Y",
           "{{ not }}", "%}", "{%", "the quick brown fox jumps over the lazy dog"]
INTS = [0, 1, -1, 2, 3, 7, 10, -10, 42, 100, 255, 1000, -2147483648, 2147483647,
        9007199254740993, 18446744073709551616, -99999999999999999999]
FLOATS = [0.5, -0.5, 1.0, 2.5, 3.14159, -0.0, 1e20, 1.5e-7, 100.0, 0.1, 99.999, 1234567.891]
DATE_FMTS = ["%Y-%m-%d", "%b %d, %Y", "%B %e, %Y at %l:%M %p", "%a %A %j", "%H:%M:%S %Z",
             "%s", "%y%m%d", "%-d/%-m/%Y", "%^B %_d", "%U %W %u %w", "%z", "%c", "%D %F %T",
             "%I%P", "%10A|%-10b|", "%%Y", "%G-W%V", "%e%k%L", "%N"]

FILTERS = {  # name: list of argument kinds
    "abs": [], "append": ["s"], "at_least": ["n"], "at_most": ["n"], "base64_decode": [],
    "base64_encode": [], "base64_url_safe_decode": [], "base64_url_safe_encode": [],
    "capitalize": [], "ceil": [], "compact": ["?p"], "concat": ["a"], "date": ["fmt"],
    "default": ["v", "?allow_false"], "divided_by": ["n"], "downcase": [], "escape": [],
    "escape_once": [], "first": [], "floor": [], "join": ["?s"], "last": [], "lstrip": [],
    "map": ["p"], "minus": ["n"], "modulo": ["n"], "newline_to_br": [], "plus": ["n"],
    "prepend": ["s"], "remove": ["s"], "remove_first": ["s"], "remove_last": ["s"],
    "replace": ["s", "s"], "replace_first": ["s", "s"], "replace_last": ["s", "s"],
    "reverse": [], "round": ["?n"], "rstrip": [], "size": [], "slice": ["n", "?n"],
    "sort": ["?p"], "sort_natural": ["?p"], "split": ["s"], "strip": [], "strip_html": [],
    "strip_newlines": [], "sum": ["?p"], "times": ["n"], "truncate": ["n", "?s"],
    "truncatewords": ["n", "?s"], "uniq": ["?p"], "upcase": [], "url_decode": [],
    "url_encode": [], "where": ["p", "?v"], "reject": ["p", "?v"], "find": ["p", "?v"],
    "find_index": ["p", "?v"], "has": ["p", "?v"], "escape_html": [], "h": [],
}
PROPS = ["name", "price", "tags", "id", "title", "missing", "available", "n"]


def gen_env(r):
    def scalar():
        k = r.random()
        if k < 0.35: return r.choice(STRINGS)
        if k < 0.6: return r.choice(INTS)
        if k < 0.75: return r.choice(FLOATS)
        return r.choice([True, False, None])
    def item(i):
        return {"name": r.choice(STRINGS[:20]), "price": r.choice(INTS[:12] + FLOATS[:6]),
                "tags": r.sample(["a", "b", "sale", "new", "B", "é"], r.randint(0, 3)),
                "id": i, "title": r.choice(["Shirt", "shoe", "Hat", "äpfel", "", "10", "2"]),
                "available": r.choice([True, False, None])}
    env = {
        "s": r.choice(STRINGS), "t": r.choice(STRINGS), "n": r.choice(INTS), "m": r.choice(INTS[:12]),
        "f": r.choice(FLOATS), "b": r.choice([True, False]), "z": None, "e": "", "big": r.choice(INTS[-4:]),
        "arr": [scalar() for _ in range(r.randint(0, 6))],
        "nums": [r.choice(INTS[:12] + FLOATS) for _ in range(r.randint(0, 6))],
        "words": r.sample(["banana", "Apple", "cherry", "apple", "Banana", "10", "9", "é", ""], r.randint(0, 6)),
        "products": [item(i) for i in range(r.randint(0, 5))],
        "h": {"a": scalar(), "b": [scalar(), scalar()], "c": {"d": scalar(), "e": [1, {"f": "deep"}]},
              "first": "hfirst", "size": 99},
        "empty_arr": [], "empty_h": {}, "date": r.choice(STRINGS[28:34] + [1700000000, 0, -1]),
    }
    return env


class Gen:
    VARS = ["s", "t", "n", "m", "f", "b", "z", "e", "big", "arr", "nums", "words", "products",
            "h", "empty_arr", "empty_h", "date", "missing", "x", "y", "i", "item", "forloop", "p"]

    def __init__(self, r, depth=3):
        self.r, self.depth = r, depth

    def lit(self):
        r = self.r; k = r.random()
        if k < 0.35:
            s = r.choice(STRINGS).replace("\n", "\\n")
            q = r.choice(["'", '"'])
            return q + s.replace(q, "") + q
        if k < 0.55: return str(r.choice(INTS))
        if k < 0.68: return repr(r.choice(FLOATS)).replace("e+", "e")
        if k < 0.78: return f"({self.simple()}..{self.simple()})"
        return r.choice(["nil", "true", "false", "empty", "blank", "null"])

    def simple(self):
        r = self.r
        return r.choice([str(r.choice(INTS[:12])), "n", "m", "arr.size", "-2", "1.5", "'3'", "x"])

    def lookup(self):
        r = self.r; v = r.choice(self.VARS)
        for _ in range(r.choice([0, 0, 1, 1, 2, 3])):
            k = r.random()
            if k < 0.35: v += "." + r.choice(PROPS + ["first", "last", "size", "a", "c", "d", "index", "length"])
            elif k < 0.7: v += "[" + r.choice(["0", "1", "-1", "99", "m", '"a"', "'name'", "1.5", "n"]) + "]"
            else: v += "." + r.choice(["first", "last", "size"])
        return v

    def value(self):
        return self.lit() if self.r.random() < 0.4 else self.lookup()

    def farg(self, kind):
        r = self.r
        kind = kind.lstrip("?")
        if kind == "s": return self.value() if r.random() < 0.3 else "'" + r.choice(STRINGS[:24]).replace("'", "").replace("\n", "\\n") + "'"
        if kind == "n": return self.value() if r.random() < 0.3 else r.choice(["0", "1", "2", "-1", "3", "10", "0.5", "-2.5", "100", "1e3", "'2'", "nil"])
        if kind == "a": return r.choice(["arr", "nums", "words", "empty_arr", "s", "z", "products"])
        if kind == "p": return "'" + r.choice(PROPS) + "'"
        if kind == "fmt": return "'" + r.choice(DATE_FMTS) + "'"
        if kind == "allow_false": return "allow_false: " + r.choice(["true", "false"])
        return self.value()

    def filters(self):
        r = self.r; out = ""
        for _ in range(r.choice([0, 1, 1, 1, 2, 2, 3, 4])):
            name = r.choice(list(FILTERS)) if r.random() > 0.01 else "no_such_filter"
            args = [a for a in FILTERS.get(name, []) if not a.startswith("?") or r.random() < 0.5]
            if r.random() < 0.03: args = args + ["s"]  # wrong arity
            out += f" | {name}" + (": " + ", ".join(self.farg(a) for a in args) if args else "")
        return out

    def expr(self):
        return self.value() + self.filters()

    def cond(self, n=0):
        r = self.r
        c = self.value()
        if r.random() < 0.7:
            c += f" {r.choice(['==', '!=', '<>', '<', '>', '<=', '>=', 'contains'])} {self.value()}"
        if n < 2 and r.random() < 0.25:
            c += f" {r.choice(['and', 'or'])} {self.cond(n + 1)}"
        return c

    def ws(self):
        return self.r.choice(["", "", "", "-"])

    def tag(self, body):
        return "{%" + self.ws() + " " + body + " " + self.ws() + "%}"

    def block(self, d):
        r = self.r
        return "".join(self.node(d) for _ in range(r.randint(0, 3 if d < self.depth else 1)))

    def node(self, d):
        r = self.r
        k = r.random()
        if k < 0.25 or d >= self.depth:
            return r.choice(["", " ", "x", "\n", " | ", "é", "<b>", "{", "}"]) if r.random() < 0.4 else \
                "{{" + self.ws() + " " + self.expr() + " " + self.ws() + "}}"
        d += 1
        t = r.choice(["if", "unless", "case", "for", "assign", "capture", "tablerow", "cycle",
                      "incdec", "echo", "liquid", "raw", "comment", "include", "render", "break",
                      "ifchanged", "inline_comment"])
        b = lambda: self.block(d)
        if t == "if":
            s = self.tag("if " + self.cond()) + b()
            for _ in range(r.choice([0, 0, 1, 2])): s += self.tag("elsif " + self.cond()) + b()
            if r.random() < 0.4: s += self.tag("else") + b()
            return s + self.tag("endif")
        if t == "unless":
            return self.tag("unless " + self.cond()) + b() + (self.tag("else") + b() if r.random() < 0.3 else "") + self.tag("endunless")
        if t == "case":
            s = self.tag("case " + self.value())
            for _ in range(r.randint(0, 3)):
                vals = [self.value() for _ in range(r.randint(1, 3))]
                s += self.tag("when " + r.choice([", ", " or "]).join(vals)) + b()
            if r.random() < 0.4: s += self.tag("else") + b()
            return s + self.tag("endcase")
        if t in ("for", "tablerow"):
            col = r.choice(["arr", "nums", "words", "products", "h", "s", "z", "empty_arr", "(1..3)",
                            "(m..n)", "(-2..2)", "products.first.tags", "(1..5)", "missing"])
            opts = ""
            for o in ("limit", "offset", "cols") if t == "tablerow" else ("limit", "offset"):
                if r.random() < 0.25: opts += f" {o}: {r.choice(['0', '1', '2', '-1', 'm', 'continue' if o == 'offset' else '3', 'nil'])}"
            if t == "for" and r.random() < 0.2: opts += " reversed"
            loopvar = "forloop" if t == "for" else "tablerowloop"
            s = self.tag(f"{t} {r.choice(['i', 'item', 'x', 'p'])} in {col}{opts}") + b()
            if r.random() < 0.3:
                s += "{{ " + loopvar + "." + r.choice(["index", "rindex0", "first", "last", "length", "col", "row", "parentloop.index"]) + " }}"
            if t == "for" and r.random() < 0.2: s += self.tag("else") + b()
            return s + self.tag("end" + t)
        if t == "assign":
            return self.tag(f"assign {r.choice(['x', 'y', 's', 'arr'])} = {self.expr()}")
        if t == "capture":
            return self.tag(f"capture {r.choice(['x', 'y'])}") + b() + self.tag("endcapture")
        if t == "cycle":
            vals = ", ".join(self.lit() for _ in range(r.randint(1, 3)))
            return self.tag("cycle " + (r.choice(["'g': ", "x: "]) if r.random() < 0.3 else "") + vals)
        if t == "incdec":
            return self.tag(f"{r.choice(['increment', 'decrement'])} {r.choice(['x', 'c', 'n'])}")
        if t == "echo":
            return self.tag("echo " + self.expr())
        if t == "liquid":
            lines = [f"assign x = {self.expr()}", f"echo {self.expr()}", f"if {self.cond()}", f"echo {self.value()}", "endif"]
            return "{%" + self.ws() + " liquid\n  " + "\n  ".join(lines[: r.randint(1, 5)] if r.random() < 0.2 else lines) + "\n" + self.ws() + "%}"
        if t == "raw":
            return self.tag("raw") + r.choice(["{{ x }}", "{% if %}", "plain", ""]) + self.tag("endraw")
        if t == "comment":
            return self.tag("comment") + r.choice(["{{ x }}", "{% if %}", "c", ""]) + self.tag("endcomment")
        if t == "inline_comment":
            return "{%" + self.ws() + " # " + r.choice(["note", "{{ x }}", ""]) + " %}"
        if t in ("include", "render"):
            name = r.choice(["'card'", "'list'", "'self'", "'missing'", "'nested'", "s"])
            extra = r.choice(["", f" with {self.value()}", f" for products as p", f", a: {self.value()}, b: 2",
                              f" with {self.value()} as thing", " for arr"])
            return self.tag(f"{t} {name}{extra}")
        if t == "break":
            return self.tag(r.choice(["break", "continue"]))
        if t == "ifchanged":
            return self.tag("ifchanged") + b() + self.tag("endifchanged")
        return ""

    def template(self):
        return "".join(self.node(0) for _ in range(self.r.randint(1, 4)))


def mutate(r, t):
    """Small syntax damage, to exercise parse errors."""
    if not t: return t
    i = r.randrange(len(t))
    op = r.choice(["del", "dup", "ins"])
    if op == "del": return t[:i] + t[i + 1:]
    if op == "dup": return t[:i] + t[i] + t[i:]
    return t[:i] + r.choice(["{", "}", "%", "|", ":", ",", "'", '"', ".", "-", "=", " ", "("]) + t[i:]


def gen_case(seed, i):
    r = random.Random(f"{seed}:{i}")
    g = Gen(r)
    t = g.template()
    if r.random() < 0.12:
        for _ in range(r.randint(1, 2)): t = mutate(r, t)
    fs = {
        "card": "[{{ p.name | default: a }}|{{ thing }}|{{ b }}]",
        "list": "{% for i in (1..2) %}{{ i }}{{ x }}{% endfor %}",
        "self": "s{% if forloop.index < 3 %}{% include 'self' %}{% endif %}",
        "nested": "<{% render 'card', p: p %}>",
        **({"hello": Gen(r, 1).template()} if r.random() < 0.3 else {}),
    }
    return {"id": f"{seed}-{i}", "template": t, "environment": gen_env(r), "filesystem": fs,
            "strict_errors": r.random() < 0.7}


# -------------------------------------------------------------------- runner

class Server:
    def __init__(self, name, cmd, timeout=5.0):
        self.name, self.cmd, self.timeout = name, cmd, timeout
        self.p, self.next_id = None, 1

    def start(self):
        self.stop()
        self.p = subprocess.Popen(self.cmd, shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, env={**os.environ, "TZ": "UTC"})
        self.buf = b""
        self.call("initialize", {"version": "1.0"})

    def stop(self):
        if self.p:
            try: self.p.kill(); self.p.wait(timeout=2)
            except Exception: pass
        self.p = None

    def _readline(self, deadline):
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0: raise TimeoutError
            ready, _, _ = select.select([self.p.stdout], [], [], left)
            if not ready: raise TimeoutError
            chunk = os.read(self.p.stdout.fileno(), 1 << 20)
            if not chunk: raise EOFError
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def call(self, method, params):
        rid = self.next_id; self.next_id += 1
        self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n").encode())
        self.p.stdin.flush()
        deadline = time.time() + self.timeout
        while True:
            msg = json.loads(self._readline(deadline))
            if msg.get("id") == rid and "method" not in msg:
                return msg

    @staticmethod
    def err_message(m):
        """The error text from either the preferred result.error or a legacy JSON-RPC error."""
        res = m.get("result") or {}
        if isinstance(res.get("error"), dict):
            return res["error"].get("message", "")
        e = m.get("error") or {}
        data = e.get("data")
        if isinstance(data, dict) and "message" in data:
            return data["message"]
        return e.get("message", json.dumps(m)[:200])

    def run_case(self, c):
        """-> (status, payload): ok/output, parse_error/msg, render_error/msg, timeout, crash."""
        try:
            if not self.p: self.start()
            m = self.call("compile", {"template": c["template"], "filesystem": c["filesystem"],
                                      "options": {"error_mode": "strict2", "line_numbers": True}})
            res = m.get("result") or {}
            if not res.get("template_id"):
                return ("parse_error", self.err_message(m))
            m = self.call("render", {"template_id": res["template_id"], "environment": c["environment"],
                                     "options": {"strict_errors": c["strict_errors"]}})
            res = m.get("result") or {}
            if res.get("output") is not None:
                return ("ok", res["output"])
            return ("render_error", self.err_message(m))
        except TimeoutError:
            self.stop(); return ("timeout", "")
        except (EOFError, BrokenPipeError, ValueError, OSError) as e:
            code = self.p.poll() if self.p else None
            self.stop(); return ("crash", f"{type(e).__name__} exit={code}")


def classify(arm, ref):
    if arm == ref: return None
    (sa, pa), (sr, pr) = arm, ref
    if sa in ("crash", "timeout"): return sa
    if sa == sr: return "output_mismatch" if sa == "ok" else "message_mismatch"
    return f"{sr}->{sa}"


def cmd_run(a):
    os.makedirs(a.out, exist_ok=True)
    servers = {n: Server(n, cmd) for n, cmd in (("rust", a.rust), ("c", a.c), ("ruby", a.ruby))}
    stats = {"seed": a.seed, "cases": 0, "ruby_unusable": 0, "rust": {}, "c": {}}
    with open(os.path.join(a.out, f"cases-{a.seed}.jsonl"), "w") as out:
        for i in range(a.cases):
            c = gen_case(a.seed, i)
            ref = servers["ruby"].run_case(c)
            if ref[0] in ("timeout", "crash"):
                stats["ruby_unusable"] += 1; continue
            stats["cases"] += 1
            res = {"ruby": ref}
            for arm in ("rust", "c"):
                res[arm] = servers[arm].run_case(c)
                k = classify(res[arm], ref)
                if k: stats[arm][k] = stats[arm].get(k, 0) + 1
            if any(classify(res[arm], ref) for arm in ("rust", "c")):
                out.write(json.dumps({**c, "results": res}) + "\n")
    for s in servers.values(): s.stop()
    json.dump(stats, open(os.path.join(a.out, f"stats-{a.seed}.json"), "w"), indent=1)
    print(json.dumps(stats))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--seed", type=int, required=True); r.add_argument("--cases", type=int, default=1000)
    r.add_argument("--out", required=True)
    for n in ("rust", "c", "ruby"): r.add_argument("--" + n, required=True)
    s = sub.add_parser("show"); s.add_argument("seed", type=int); s.add_argument("i", type=int)
    a = ap.parse_args()
    if a.cmd == "run": cmd_run(a)
    else: print(json.dumps(gen_case(a.seed, a.i), indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
