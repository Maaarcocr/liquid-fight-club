# Random tag-heavy template generator (loops, counters, cycle, capture,
# partials, whitespace control, liquid tag, interrupts) for differential tests.
#   ruby tests/gentags.rb COUNT SEED > cases.jsonl
require "json"
n = (ARGV[0] || 5000).to_i
srand((ARGV[1] || 1).to_i)

ENV_DATA = {
  "arr" => [1, 2, 3, 4], "words" => %w[a b c], "h" => { "x" => 1, "y" => 2 }, "n" => nil, "s" => "str",
  "i" => 2, "objs" => [{ "a" => 1 }, { "a" => 2 }], "e" => [], "big" => (1..12).to_a, "t" => true, "f" => false,
}
PARTIALS = {
  "p" => "[{{ p }}{{ x }}{{ forloop.index }}{% increment c %}{% cycle 'q','r' %}]",
  "q" => "{% assign leak = 'L' %}<{{ q }}{{ item }}>",
  "loop" => "{% for z in arr limit: 2 %}{{ z }}{% break %}{% endfor %}{% continue %}",
  "deep" => "{% if t %}{% include 'q' %}{% endif %}",
}
COLLS = %w[arr words h n s i objs e big (1..3) (3..1) (0..5)]

def ws(open)
  rand < 0.2 ? (open ? "{%-" : "-%}") : (open ? "{%" : "%}")
end

def out(expr)
  o = rand < 0.15 ? "{{-" : "{{"
  c = rand < 0.15 ? "-}}" : "}}"
  "#{o} #{expr} #{c}"
end

def simple
  case rand(16)
  when 0 then out(%w[x i s forloop.index forloop.rindex0 forloop.first forloop.last forloop.length forloop.parentloop.index tablerowloop.col tablerowloop.row leak c].sample)
  when 1 then "#{ws(true)} increment #{%w[c d i].sample} #{ws(false)}"
  when 2 then "#{ws(true)} decrement #{%w[c d i].sample} #{ws(false)}"
  when 3 then "{% cycle #{["'a', 'b'", "'a', 'b', 'c'", "1, 2", "x, i", "'g': 'a', 'b'", "s: 1, 2"].sample} %}"
  when 4 then "{% capture cap %}#{out('x')}-{% endcapture %}#{out('cap')}"
  when 5 then "#{ws(true)} break #{ws(false)}"
  when 6 then "#{ws(true)} continue #{ws(false)}"
  when 7 then "{% raw %}{{ x }}{% endraw %}"
  when 8 then "{% comment %}{{ bad {% endcomment %}"
  when 9 then "{% include '#{%w[p q loop deep].sample}'#{[' with x', ' for arr', '', ' x: 5'].sample} %}"
  when 10 then "{% render '#{%w[p q].sample}'#{[' with x as p', ' for arr as p', '', ', x: i'].sample} %}"
  when 11 then "{% liquid\n  assign y = x\n  echo y\n  increment c\n%}"
  when 12 then "{% ifchanged %}#{out('x')}{% endifchanged %}"
  when 13 then "{% echo x | default: 'none' %}"
  when 14 then "{% assign x = #{%w[1 'a' arr.first nil i].sample} %}"
  else " \n "
  end
end

def body(depth)
  Array.new(rand(1..3)) { rand < 0.3 && depth < 3 ? block(depth + 1) : simple }.join
end

def block(depth)
  case rand(5)
  when 0
    attrs = []
    attrs << "limit: #{%w[1 2 i].sample}" if rand < 0.3
    attrs << "offset: #{%w[1 continue i].sample}" if rand < 0.3
    attrs.unshift("reversed") if rand < 0.2
    "#{ws(true)} for x in #{COLLS.sample} #{attrs.join(' ')} #{ws(false)}#{body(depth)}#{rand < 0.3 ? '{% else %}E' : ''}{% endfor %}"
  when 1 then "{% if #{%w[x i t f n arr s].sample} #{['', '== 2', '> 1', 'contains 2', '!= nil'].sample} %}#{body(depth)}{% elsif t %}EL{% else %}#{body(depth)}{% endif %}"
  when 2 then "{% case x %}{% when 1, 2 %}#{body(depth)}{% when 'a' or 'b' %}W{% else %}#{body(depth)}{% endcase %}"
  when 3 then "{% tablerow x in #{COLLS.sample} #{['cols: 2', 'limit: 2', 'offset: 1', 'cols: 2 limit: 3', ''].sample} %}#{body(depth)}{% endtablerow %}"
  else "{% unless #{%w[x t f].sample} %}#{body(depth)}{% endunless %}"
  end
end

n.times do
  t = Array.new(rand(1..3)) { block(0) }.join("|")
  puts JSON.generate({ "template" => t, "environment" => ENV_DATA, "filesystem" => PARTIALS })
end
