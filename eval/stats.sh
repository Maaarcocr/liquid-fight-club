#!/usr/bin/env bash
# Code stats for both arms (eval clones at the final commits).
#   eval/stats.sh EVAL_WORK_DIR OUT_TSV
# Non-blank source lines excluding harness/, split into library / glue / tests;
# `unsafe` in Rust outside the C-ABI crate; compiler and clippy warnings.
set -uo pipefail
w=$1; out=$2
nb() { [ $# -eq 0 ] && { echo 0; return; }; cat "$@" | grep -cv '^\s*$'; }
files() { (cd "$1" && git ls-files "${@:2}" | grep -v '^harness/'); }
{
printf 'metric\trust\tc\n'
r=$w/rust; c=$w/c
cd "$r"; rlib=$(nb $(files "$r" 'crates/liquid/src/*.rs'))
rglue=$(nb $(files "$r" 'crates/liquid-capi/*.rs' 'crates/liquid-server/*.rs'))
rtest=$(nb $(files "$r" 'crates/liquid/tests/*.rs' 'crates/liquid/examples/*.rs'))
cd "$c"; clib=$(nb $(files "$c" 'src/*.c' 'src/*.h' 'include/*.h'))
cglue=$(nb $(files "$c" 'server/*.c' 'server/*.h'))
ctest=$(nb $(files "$c" 'tests/*'))
printf 'loc_library\t%s\t%s\n' "$rlib" "$clib"
printf 'loc_glue_server_abi\t%s\t%s\n' "$rglue" "$cglue"
printf 'loc_tests_tools\t%s\t%s\n' "$rtest" "$ctest"
printf 'loc_total\t%s\t%s\n' $((rlib + rglue + rtest)) $((clib + cglue + ctest))
cd "$r"
printf 'unsafe_outside_capi\t%s\t-\n' "$(cat $(files "$r" 'crates/liquid/*.rs' 'crates/liquid-server/*.rs') | grep -cw unsafe)"
printf 'unsafe_in_capi\t%s\t-\n' "$(cat $(files "$r" 'crates/liquid-capi/*.rs') | grep -cw unsafe)"
cargo clippy --release --all-targets --message-format=short > "$w/clippy.log" 2>&1
printf 'clippy_warnings\t%s\t-\n' "$(grep -cE ': warning: ' "$w/clippy.log")"
# C: the arm's own bin/build, with -Wall -Wextra appended by a compiler wrapper.
for cc in gcc clang; do
  rm -rf "$w/warn-$cc"; git clone -q "$c" "$w/warn-$cc"; mkdir -p "$w/warn-$cc/wrap"
  for n in cc gcc clang; do
    printf '#!/bin/sh\nexec /usr/bin/%s "$@" -Wall -Wextra\n' "$cc" > "$w/warn-$cc/wrap/$n"
    chmod +x "$w/warn-$cc/wrap/$n"
  done
  (cd "$w/warn-$cc" && PATH="$w/warn-$cc/wrap:$PATH" CC=$cc bin/build > build.log 2>&1)
  printf '%s_wall_wextra_warnings\t-\t%s\n' "$cc" "$(grep -c 'warning:' "$w/warn-$cc/build.log")"
done
} | tee "$out"
