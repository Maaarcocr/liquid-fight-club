# Liquid in Rust

A [Liquid](https://shopify.github.io/liquid/) template engine in Rust, compatible
with Shopify's reference implementation (strict2 mode). No dependencies.

- `bin/build` — builds `build/liquid-server` (JSON-RPC, for liquid-spec) and
  `build/libliquid_fc.a` (C ABI, see `harness/liquid_fc.h`).
- `harness/spec` — runs the liquid-spec suites.
- `cargo test --release` — unit tests and hostile-input tests.

See [NOTES.md](NOTES.md) for architecture, decisions and limitations, and
[TASK.md](TASK.md) for the assignment.
