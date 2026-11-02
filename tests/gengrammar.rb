# Random template generator for differential testing (filters, conditions,
# loops) with values of many types. Prints JSONL cases.
#   ruby tests/gengrammar.rb COUNT SEED > cases.jsonl
require "json"
n = (ARGV[0] || 5000).to_i
srand((ARGV[1] || 1).to_i)

VALUES = {
  "n" => nil, "t" => true, "f" => false, "z" => 0, "i" => 7, "neg" => -3, "big" => 12345678901234567890,
  "fl" => 2.5, "fz" => 0.0, "fneg" => -1.75, "fbig" => 1.0e20, "s" => "hello world", "e" => "",
  "sp" => "  pad  ", "num" => "3.5", "inum" => "42", "u" => "héllo ünï ß", "html" => "<b>a & 'b'</b>",
  "arr" => [3, 1, 2], "sarr" => ["b", "A", "c", nil], "mix" => [1, "a", nil, 2.5, true],
  "nest" => [[1, 2], [3, [4]]], "h" => { "a" => 1, "b" => "x" }, "eh" => {}, "ea" => [],
  "objs" => [{ "k" => 2, "v" => "x" }, { "k" => 1, "v" => nil }, { "k" => 2, "v" => "y" }, { "v" => 5 }],
  "words" => "one two  three four five six", "nl" => "a\nb\r\nc", "date" => "2020-06-15 14:30:00",
  "ts" => 1591331445,
}
VARS = VALUES.keys
LITS = ["nil", "true", "false", "0", "1", "-1", "2", "3", "10", "-5", "1.5", "0.0", "-2.25", "'a'", "''", "'abc'",
        "'3'", "' 2 '", "'x,y'", "'%Y'", "(1..3)", "empty", "blank"]
FILTERS = {
  "abs" => 0, "append" => 1, "at_least" => 1, "at_most" => 1, "base64_decode" => 0, "base64_encode" => 0,
  "base64_url_safe_decode" => 0, "base64_url_safe_encode" => 0, "capitalize" => 0, "ceil" => 0, "compact" => 0..1,
  "concat" => 1, "date" => 1, "default" => 0..2, "divided_by" => 1, "downcase" => 0, "escape" => 0,
  "escape_once" => 0, "first" => 0, "floor" => 0, "join" => 0..1, "last" => 0, "lstrip" => 0, "map" => 1,
  "minus" => 1, "modulo" => 1, "newline_to_br" => 0, "plus" => 1, "prepend" => 1, "remove" => 1,
  "remove_first" => 1, "remove_last" => 1, "replace" => 1..2, "replace_first" => 1..2, "replace_last" => 2,
  "reverse" => 0, "round" => 0..1, "rstrip" => 0, "size" => 0, "slice" => 1..2, "sort" => 0..1,
  "sort_natural" => 0..1, "split" => 1, "squish" => 0, "strip" => 0, "strip_html" => 0, "strip_newlines" => 0,
  "sum" => 0..1, "times" => 1, "truncate" => 0..2, "truncatewords" => 0..2, "uniq" => 0..1, "upcase" => 0,
  "url_decode" => 0, "url_encode" => 0, "where" => 1..2, "reject" => 1..2, "has" => 1..2, "find" => 1..2,
  "find_index" => 1..2, "h" => 0,
}

def operand
  r = rand
  if r < 0.55 then VARS.sample
  elsif r < 0.9 then LITS.sample
  else "#{VARS.sample}.#{%w[size first last a k 0].sample}"
  end
end

def arg
  rand < 0.15 ? "'k'" : operand
end

def filter_chain(k)
  out = []
  k.times do
    f, ar = FILTERS.to_a.sample
    cnt = ar.is_a?(Range) ? rand(ar) : ar
    cnt += 1 if rand < 0.05
    args = Array.new(cnt) { arg }
    args << "allow_false: #{%w[true false].sample}" if f == "default" && rand < 0.3
    out << (args.empty? ? f : "#{f}: #{args.join(', ')}")
  end
  out.map { |f| " | #{f}" }.join
end

def output
  "{{ #{operand}#{filter_chain(rand(1..3))} }}"
end

def cond
  ops = ["==", "!=", "<", ">", "<=", ">=", "contains"]
  c = rand < 0.2 ? operand : "#{operand} #{ops.sample} #{operand}"
  c += " #{%w[and or].sample} #{operand} #{ops.sample} #{operand}" if rand < 0.3
  c
end

def piece(depth = 0)
  r = rand
  if depth > 2 || r < 0.45 then output
  elsif r < 0.6 then "{% if #{cond} %}A#{piece(depth + 1)}{% else %}B{% endif %}"
  elsif r < 0.7 then "{% unless #{cond} %}U{% endunless %}"
  elsif r < 0.8
    attrs = []
    attrs << "limit: #{%w[0 1 2 -1 n s num].sample}" if rand < 0.3
    attrs << "offset: #{%w[0 1 2 -1 n s inum].sample}" if rand < 0.3
    attrs << "reversed" if rand < 0.2
    rev = attrs.delete("reversed")
    "{% for x in #{operand}#{rev ? ' reversed' : ''} #{attrs.join(' ')} %}[{{ x }}{{ forloop.index }}]#{rand < 0.3 ? piece(depth + 1) : ''}{% else %}E{% endfor %}"
  elsif r < 0.87 then "{% assign v = #{operand}#{filter_chain(rand(0..2))} %}{{ v }}"
  elsif r < 0.93 then "{% case #{operand} %}{% when #{operand}, #{operand} %}W{% when #{operand} %}X{% else %}Y{% endcase %}"
  else "{% tablerow x in #{operand} cols: #{%w[1 2 0 n].sample} %}{{ x }}{% endtablerow %}"
  end
end

n.times do
  t = Array.new(rand(1..3)) { piece }.join("|")
  puts JSON.generate({ "template" => t, "environment" => VALUES, "filesystem" => {} })
end
