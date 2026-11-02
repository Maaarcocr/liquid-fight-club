# Render a few minimized randspec cases with Ruby Liquid in-process, without
# the JSON-RPC bridge, to confirm what Ruby itself does.
#   ruby -I<liquid/lib> eval/randspec_verify.rb
require "liquid"

FS = {
  "card" => "[{{ p.name | default: a }}|{{ thing }}|{{ b }}]",
}

class MemFS
  def read_template_file(name) = FS.fetch(name) { raise Liquid::FileSystemError, "Could not find asset #{name}" }
end

CASES = [
  # Rust: property-taking array filters on a scalar
  ["{{ 1 | sort: 'n' }}", {}],
  ["{{ m | sort_natural: 'id' }}", { "m" => 255 }],
  ["{{ n | uniq: 'n' }}", { "n" => -1 }],
  ["{{ f | uniq: 'n' }}", { "f" => 0.5 }],
  # Rust: negative zero
  ["{{ -0.0 | times: 0 }}", {}],
  ["{{ t | divided_by: -2.5 }}", {}],
  # C: dates
  ["{{ '2025-03-15T14:30:00Z' | date: '%-d/%-m/%Y' }}", {}],
  ["{{ '2025-03-15T14:30:00Z' | date: '%U %W %u %w' }}", {}],
  ["{{ d | date: '%-d/%-m/%Y' }}", { "d" => "March 5, 2021 10:00" }],
  # C: float modulo after divide by blank
  ["{% echo 0.1 | divided_by: blank | modulo: 'March 5, 2021 10:00' %}", {}],
  # Both arms: false passed to a partial
  ["{% render 'card' with false as thing %}", {}],
  ["{% include 'card' with false as thing %}", {}],
  # Both arms: big integers
  ["{{ (n..1) }}", { "n" => 18446744073709551616 }],
]

CASES.each do |tpl, env|
  t = Liquid::Template.parse(tpl)
  t.registers[:file_system] = MemFS.new
  out = begin
    t.render!(env, registers: { file_system: MemFS.new })
  rescue => e
    "ERROR #{e.class}: #{e.message}"
  end
  puts "#{tpl.inspect}\n    => #{out.inspect}"
end
