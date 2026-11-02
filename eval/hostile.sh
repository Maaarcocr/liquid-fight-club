#!/usr/bin/env bash
# Hostile templates through each arm's C ABI and through Ruby Liquid, each
# with a 10 s timeout, recording wall time, peak RSS and the outcome.
#   eval/hostile.sh EVAL_WORK_DIR OUT_TSV
# Assumes EVAL_WORK_DIR/{rust,c}/build/libliquid_fc.a exist.
set -uo pipefail
w=$1; out=$2
here="$(cd "$(dirname "$0")" && pwd)"
cases="$w/hostile"; rm -rf "$cases"; mkdir -p "$cases"

python3 - "$cases" <<'EOF'
import json, os, sys
d = sys.argv[1]
def case(name, tpl, partials=None):
    os.makedirs(f"{d}/{name}", exist_ok=True)
    open(f"{d}/{name}/template.liquid", "w").write(tpl)
    json.dump(partials or {}, open(f"{d}/{name}/partials.json", "w"))
case("nested_if_10000", "{% if true %}" * 10000 + "x" + "{% endif %}" * 10000)
case("for_100M_empty", "{% for i in (1..100000000) %}{% endfor %}done")
case("for_100M_output", "{% for i in (1..100000000) %}x{% endfor %}")
case("self_include", "{% include 'loop' %}", {"loop": "a{% include 'loop' %}"})
case("self_render", "{% render 'loop' %}", {"loop": "a{% render 'loop' %}"})
case("append_doubling", "{% assign s = 'x' %}{% for i in (1..64) %}{% assign s = s | append: s %}{% endfor %}{{ s | size }}")
EOF

for arm in rust c; do
  clang -O2 -I"$w/$arm/harness" "$here/run1.c" "$w/$arm/build/libliquid_fc.a" -lpthread -ldl -lm -o "$w/run1-$arm"
done
cat > "$w/run1.rb" <<'EOF'
require "bundler/setup"
require "json"
require "liquid"
class MemFS
  def initialize(h) = @h = h
  def read_template_file(name) = @h.fetch(name) { raise Liquid::FileSystemError, "No such template '#{name}'" }
end
src = File.read(ARGV[0]); partials = JSON.parse(File.read(ARGV[1]))
env = Liquid::Environment.build(file_system: MemFS.new(partials))
begin
  t = Liquid::Template.parse(src, environment: env, error_mode: :strict2, line_numbers: true)
rescue Liquid::Error => e
  puts "parse_error\t0\t#{e.message[0, 120]}"; exit
end
begin
  o = t.render!({}); puts "ok\t#{o.bytesize}\t"
rescue Liquid::Error, SystemStackError, NoMemoryError => e
  puts "render_error\t0\t#{e.class}: #{e.message[0, 100]}"
end
EOF

export BUNDLE_GEMFILE="$w/rust/harness/Gemfile"  # pinned Ruby Liquid for the reference runs
printf 'case\timpl\tstatus\toutput_bytes\tmessage\twall_s\tpeak_rss_kb\texit\n' > "$out"
for dir in "$cases"/*/; do
  name=$(basename "$dir")
  for impl in rust c ruby; do
    if [ "$impl" = ruby ]; then
      cmd=(ruby "$w/run1.rb" "$dir/template.liquid" "$dir/partials.json")
    else
      cmd=("$w/run1-$impl" "$dir/template.liquid" "$dir/partials.json")
    fi
    res=$( { /usr/bin/time -f '%e %M' -o "$w/time.txt" prlimit --as=$((10 << 30)) timeout -s KILL 10 "${cmd[@]}"; echo "exit=$?" >> "$w/time.txt"; } 2>"$w/stderr.txt" | head -c 400)
    read -r wall rss < <(grep -E '^[0-9.]+ [0-9]+$' "$w/time.txt" | tail -1)
    code=$(grep -o 'exit=[0-9]*' "$w/time.txt" | cut -d= -f2)
    [ -z "$res" ] && res=$(case "$code" in 137) echo "TIMEOUT(10s)	0	" ;; *) echo "CRASH(exit $code)	0	$(tail -c 120 "$w/stderr.txt" | tr '\n\t' '  ')" ;; esac)
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$impl" "$res" "${wall:-?}" "${rss:-?}" "$code" | tee -a "$out"
  done
done
