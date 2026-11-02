# Export a liquid-spec suite as JSONL (name, template, environment, filesystem)
#   BUNDLE_GEMFILE=harness/Gemfile bundle exec ruby tests/export_suite.rb benchmarks
require "json"
require "liquid"
require "liquid/spec"
require "liquid/spec/deps/liquid_ruby"
suite = Liquid::Spec::Suite.all.find { |s| s.name.to_s.downcase.include?(ARGV[0].to_s.downcase) || s.path.to_s.end_with?(ARGV[0].to_s) }
abort "no suite" unless suite
Liquid::Spec::SpecLoader.load_suite(suite).each do |spec|
  env = spec.instantiate_environment rescue next
  fs = spec.instantiate_filesystem
  fsh = fs.respond_to?(:to_h) ? fs.to_h : {}
  puts JSON.generate({ "name" => spec.name, "template" => spec.template, "environment" => JSON.parse(JSON.generate(env)), "filesystem" => fsh })
end
