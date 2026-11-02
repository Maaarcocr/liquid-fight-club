# Generate differential test cases (JSONL) from the liquid-spec corpus using
# liquid-spec's own template mutators.
#   BUNDLE_GEMFILE=harness/Gemfile bundle exec ruby tests/gencases.rb MODE LIMIT SEED > cases.jsonl
require "json"
require "liquid"
require "liquid/spec"
require "liquid/spec/adversarial"
require "liquid/spec/deps/liquid_ruby"

mode = (ARGV[0] || "mutate").to_sym
limit = (ARGV[1] || 20000).to_i
seed = (ARGV[2] || 1).to_i
seeds = Liquid::Spec::Adversarial::SeedLoader.new(suite: :all, seed: seed).load
skip = %i[ruby_drops ruby_types binary_data shopify_tags shopify_objects shopify_filters shopify_includes
          shopify_blank shopify_error_handling shopify_error_format shopify_string_access lax_parsing]
seeds = seeds.reject { |s| (s.features & skip).any? }
gen = Liquid::Spec::Adversarial::Generator.new(seed: seed)
cases = gen.generate(seeds, mode: mode, limit: limit)
cases.each do |c|
  spec = c.spec
  env = (spec.instantiate_environment rescue nil)
  next unless env.is_a?(Hash)
  begin
    env_json = JSON.parse(JSON.generate(env))
  rescue StandardError
    next
  end
  fs = spec.raw_filesystem.is_a?(Hash) ? spec.raw_filesystem.reject { |k, _| k.to_s.start_with?("_") || k.to_s == "instantiate" } : {}
  puts JSON.generate({ "template" => spec.template, "environment" => env_json, "filesystem" => fs })
end
