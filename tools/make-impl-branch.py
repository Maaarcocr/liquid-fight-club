#!/usr/bin/env python3
"""Render the scaffold for one language into a directory (used to seed impl/<lang>).
Usage: tools/make-impl-branch.py <rust|c> <dest-dir>"""
import shutil, sys, pathlib

RULES = {
    "rust": ("Rust", """- Stable Rust, edition 2024, built with `cargo`.
- Standard library only: no crates, and no calling C libraries (no
  `extern "C"` imports). Exporting the C ABI is the only FFI."""),
    "c": ("C", """- C17, built with `make`; must compile cleanly with gcc and clang.
- C standard library, libm, and POSIX only."""),
}

lang, dest = sys.argv[1], pathlib.Path(sys.argv[2])
name, rules = RULES[lang]
src = pathlib.Path(__file__).resolve().parent.parent / "scaffold"
for p in src.rglob("*"):
    rel = p.relative_to(src)
    if p.is_dir() or rel.name == "TASK.md.in":
        continue
    out = dest / rel
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, out)
task = (src / "TASK.md.in").read_text().replace("{{LANG}}", name).replace("{{LANG_RULES}}", rules)
(dest / "TASK.md").write_text(task)
(dest / "README.md").write_text(f"# Liquid in {name}\n\nSee [TASK.md](TASK.md).\n")
