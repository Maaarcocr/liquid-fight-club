#!/usr/bin/env bash
# Session setup (runs automatically at session start): installs the pinned
# test harness. Safe to re-run.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BUNDLE_GEMFILE="$ROOT/harness/Gemfile" LANG=C.UTF-8 LC_ALL=C.UTF-8
if ! bundle check >/dev/null 2>&1; then
  bundle install --quiet >/dev/null 2>&1 || bundle install >&2
fi
# Out-of-bounds material: C sources of Ruby and of Ruby gems are not part of
# this project's references. Remove them from the installed gem tree.
GEMDIR="$(gem env gemdir 2>/dev/null)"
if [ -n "$GEMDIR" ]; then
  find "$GEMDIR" -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -delete 2>/dev/null
fi
RUBYPREFIX="$(ruby -e 'print RbConfig::CONFIG["prefix"]' 2>/dev/null)"
[ -n "$RUBYPREFIX" ] && rm -rf "$RUBYPREFIX"/include/ruby-* 2>/dev/null
exit 0
