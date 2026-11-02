# Differential tester: renders templates with the reference Ruby Liquid
# (strict2, inline errors) and with build/liquid-server, reporting mismatches.
#
#   BUNDLE_GEMFILE=harness/Gemfile bundle exec ruby tests/difftest.rb cases.jsonl
#
# Each input line: {"template": "...", "environment": {...}, "filesystem": {...}}
require "json"
require "liquid"
require "liquid/spec"
require "liquid/spec/json_rpc/adapter"
require "timeout"

class FS
  def initialize(h)
    @h = (h || {}).transform_keys { |k| k = k.to_s.downcase; k.end_with?(".liquid") ? k : "#{k}.liquid" }
  end

  def read_template_file(path)
    p = path.to_s.downcase
    p = "#{p}.liquid" unless p.end_with?(".liquid")
    @h.find { |k, _| k.casecmp?(p) }&.last or raise Liquid::FileSystemError, "Could not find asset #{path}"
  end
end

NOW = Time.utc(2024, 1, 1, 0, 1, 58)

def reference(c)
  ENV["TZ"] = "UTC"
  Timeout.timeout(5) do
    t = Liquid::Template.parse(c["template"], error_mode: :strict2, line_numbers: true)
    ctx = Liquid::Context.build(static_environments: c["environment"] || {}, registers: { file_system: FS.new(c["filesystem"]) })
    def Time.now; NOW; end
    t.render(ctx)
  end
rescue Timeout::Error
  :timeout
rescue Liquid::Error => e
  e.message
rescue StandardError, SystemStackError => e
  "RUBY #{e.class}"
end

cmd = ENV["LIQUID_SERVER"] || File.expand_path("../build/liquid-server", __dir__)
adapter = Liquid::Spec::JsonRpc::Adapter.new(cmd, timeout: 10)
adapter.start

def subject(adapter, c)
  id = adapter.compile(c["template"], { error_mode: :strict2, line_numbers: true, file_system: c["filesystem"] || {} })
  adapter.render(id, c["environment"] || {}, { strict_errors: false, registers: { current_time: NOW } })
rescue => e
  e.message.sub(/\ALiquid error: /, "")
end

mismatch = 0
total = 0
ARGF.each_line do |line|
  c = JSON.parse(line) rescue next
  next unless c["template"].is_a?(String)
  total += 1
  r = reference(c)
  next if r == :timeout || r.to_s.start_with?("RUBY ")
  s = subject(adapter, c)
  next if r == s
  mismatch += 1
  puts JSON.generate({ "template" => c["template"], "environment" => c["environment"], "filesystem" => c["filesystem"], "reference" => r, "subject" => s })
end
warn "#{total} cases, #{mismatch} mismatches"
adapter.shutdown
