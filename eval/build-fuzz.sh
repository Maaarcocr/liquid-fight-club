#!/usr/bin/env bash
# Build an instrumented copy of one arm and link eval/fuzz.c against it.
#   eval/build-fuzz.sh ARM SRC_CHECKOUT OUT_DIR
# Both arms get SanitizerCoverage in the library so libFuzzer has feedback.
# C additionally gets ASan+UBSan (via a compiler wrapper, so the arm's own
# bin/build is used unchanged). Rust's own checks play that role on its side;
# ASan for Rust needs nightly.
set -euo pipefail
arm=$1; src=$2; out=$3
here="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$out"; mkdir -p "$out"
git clone -q --no-hardlinks "$src" "$out/src"
cd "$out/src"
COV="-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp"
case "$arm" in
  c)
    mkdir -p "$out/wrap"
    for cc in cc gcc clang; do
      cat > "$out/wrap/$cc" <<EOF
#!/bin/sh
exec /usr/bin/clang "\$@" -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined $COV
EOF
      chmod +x "$out/wrap/$cc"
    done
    PATH="$out/wrap:$PATH" CC=clang bin/build > "$out/build.log" 2>&1
    EXTRA=(-fsanitize=address,undefined)
    ;;
  rust)
    RUSTFLAGS="-C passes=sancov-module -C llvm-args=-sanitizer-coverage-level=3 \
-C llvm-args=-sanitizer-coverage-inline-8bit-counters -C llvm-args=-sanitizer-coverage-pc-table \
-C llvm-args=-sanitizer-coverage-trace-compares -C debug-assertions=off -g \
-C link-arg=-Wl,--unresolved-symbols=ignore-all" \
      bin/build > "$out/build.log" 2>&1
    EXTRA=(-fsanitize=address,undefined)
    ;;
esac
lib="$out/src/build/libliquid_fc.a"
echo "sancov symbols in lib: $(nm "$lib" 2>/dev/null | grep -c '__sanitizer_cov_8bit_counters_init\|__sancov_gen_' || true)"
echo "asan symbols in lib: $(nm "$lib" 2>/dev/null | grep -c '__asan_report' || true)"
clang -g -O1 -fsanitize=fuzzer "${EXTRA[@]}" -I"$out/src/harness" "$here/fuzz.c" "$lib" \
  -lpthread -ldl -lm -o "$out/fuzz"
echo "built $out/fuzz"
