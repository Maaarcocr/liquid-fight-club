# Random (mostly invalid) tag/variable markup for parse-error differential tests.
#   ruby tests/gensyntax.rb COUNT SEED > cases.jsonl
require "json"
n = (ARGV[0] || 5000).to_i
srand((ARGV[1] || 1).to_i)
TOKS = ["x", "y.z", "a[0]", "'s'", "\"d\"", "1", "-2", "3.5", "(1..2)", "(", ")", "[", "]", ".", "..", ",", ":",
        "|", "==", "!=", "<>", "<", ">=", "contains", "and", "or", "-", "?", "=", "!", "%", "{", "}", "nil", "empty",
        "blank", "true", "in", "with", "for", "as", "limit:", "offset:", "reversed", "cols:", "upcase", "plus:",
        "é", "#", "\n", " ", "self", "self['x']", "x?", "@", "$", "&", "*"]
TAGS = %w[if elsif unless case when for tablerow assign capture cycle increment decrement echo include render
          liquid raw comment doc break continue ifchanged # else endif endfor bogus]
def markup
  Array.new(rand(0..6)) { TOKS.sample }.join(rand < 0.7 ? " " : "")
end
def piece
  r = rand
  if r < 0.35 then "{{#{rand < 0.2 ? '-' : ''} #{markup} #{rand < 0.2 ? '-' : ''}}}"
  elsif r < 0.85
    tag = TAGS.sample
    body = case tag
           when "if", "unless" then "{% #{tag} #{markup} %}a{% end#{tag} %}"
           when "for", "tablerow" then "{% #{tag} #{markup} %}a{% end#{tag} %}"
           when "case" then "{% case #{markup} %}{% when #{markup} %}a{% endcase %}"
           when "capture", "ifchanged", "comment", "raw", "doc" then "{% #{tag} #{markup} %}a{% end#{tag} %}"
           when "liquid" then "{% liquid #{markup}\n#{%w[echo assign if cycle].sample} #{markup} %}"
           else "{% #{tag} #{markup} %}"
           end
    body
  else
    ["{%", "{{", "%}", "}}", "{% %}", "{%-%}", "{{-}}", "{{ x }", "{% if x %}", "{% endif %}", "{% raw %}"].sample
  end
end
n.times do
  puts JSON.generate({ "template" => Array.new(rand(1..2)) { piece }.join("\n"), "environment" => { "x" => 1, "y" => { "z" => 2 }, "a" => [5] }, "filesystem" => { "x" => "X" } })
end
