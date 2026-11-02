# Ruby Liquid reference timings on the same inputs as eval/bench.c
# (strict2, render errors raise). Same method: median of 10 rounds, each
# round at least ~50 ms, mean per call.
#   ruby eval/bench_ruby.rb DIR...
require "bundler/setup"
require "json"
require "liquid"

def measure
  reps = 1
  loop do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    reps.times { yield }
    dt = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
    break if dt >= 0.05
    reps = dt < 1e-4 ? reps * 100 : (reps * 0.05 / dt).ceil + 1
  end
  rounds = Array.new(10) do
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    reps.times { yield }
    (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) / reps * 1e6
  end.sort
  (rounds[4] + rounds[5]) / 2
end

class MemFS
  def initialize(h) = @h = h
  def read_template_file(name)
    @h.fetch(name) { @h.fetch("#{name}.liquid") { raise Liquid::FileSystemError, "No such template '#{name}'" } }
  end
end

puts %w[name parse_us context_us render_us output_bytes].join("\t")
ARGV.each do |dir|
  src = File.read(File.join(dir, "template.liquid"))
  assigns_json = File.read(File.join(dir, "assigns.json"))
  partials = JSON.parse(File.read(File.join(dir, "partials.json")))
  env = Liquid::Environment.build(file_system: MemFS.new(partials))
  parse = ->(s) { Liquid::Template.parse(s, environment: env, error_mode: :strict2, line_numbers: true) }
  tpl = parse.(src)
  assigns = JSON.parse(assigns_json)
  out = tpl.render!(assigns)
  p_us = measure { parse.(src) }
  c_us = measure { JSON.parse(assigns_json) }
  r_us = measure { tpl.render!(assigns) }
  puts [File.basename(dir), format("%.2f", p_us), format("%.2f", c_us), format("%.2f", r_us), out.bytesize].join("\t")
end
