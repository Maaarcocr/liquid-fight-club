# Quick audit for special-cased specs: look for spec names and long expected
# outputs from the in-scope spec files inside each arm's sources.
#   ruby eval/audit.rb ARM_CHECKOUT...
# Prints one line per hit and a per-arm summary (library vs tests/tools).
require "bundler/setup"
require "liquid"
require "liquid/spec"

root = File.join(Gem.loaded_specs["liquid-spec"].full_gem_path, "specs")
names, expected = [], []
%w[basics liquid_ruby parser_errors partials benchmarks].each do |dir|
  Dir[File.join(root, dir, "**", "*.yml")].each do |path|
    next if path.include?("/_data/")
    begin
      Liquid::Spec::SpecLoader.load_file(path).each do |s|
        names << s.name if s.name.is_a?(String) && s.name.length >= 12
        expected << s.expected if s.expected.is_a?(String) && s.expected.strip.length >= 40
      end
    rescue StandardError
      next
    end
  end
end
names.uniq!; expected.uniq!
warn "#{names.size} spec names, #{expected.size} long expected outputs"

ARGV.each do |co|
  files = Dir.chdir(co) { `git ls-files`.split("\n") }
    .grep(/\.(rs|c|h)\z/).reject { |f| f.start_with?("harness/") }
  hits = Hash.new(0)
  files.each do |f|
    text = File.read(File.join(co, f), encoding: "UTF-8", invalid: :replace)
    kind = f =~ %r{(\A|/)(tests?|examples)/} ? "tests" : "library"
    names.each { |n| (hits[[kind, "name"]] += 1; puts "#{co}: #{f}: spec name #{n}") if text.include?(n) }
    expected.each { |e| (hits[[kind, "expected"]] += 1; puts "#{co}: #{f}: expected #{e[0, 60].inspect}") if text.include?(e) }
  end
  puts "SUMMARY #{File.basename(co)}: library name hits=#{hits[%w[library name]]}, " \
       "library expected hits=#{hits[%w[library expected]]}, tests name hits=#{hits[%w[tests name]]}, " \
       "tests expected hits=#{hits[%w[tests expected]]}"
end
