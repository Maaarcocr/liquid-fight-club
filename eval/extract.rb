# Export liquid-spec data for the eval drivers, using liquid-spec's own loader.
#
#   ruby eval/extract.rb bench  OUT_DIR   # one JSON file per benchmark spec
#   ruby eval/extract.rb corpus OUT_DIR   # one file per in-scope spec template
#
# Run with BUNDLE_GEMFILE pointing at an arm's harness/Gemfile.
require "bundler/setup"
require "json"
require "fileutils"
require "digest"
require "liquid"
require "liquid/spec"

mode, out = ARGV
abort "usage: extract.rb bench|corpus OUT_DIR" unless %w[bench corpus].include?(mode) && out
FileUtils.mkdir_p(out)

specs_root = File.join(Gem.loaded_specs["liquid-spec"].full_gem_path, "specs")
loader = Liquid::Spec::SpecLoader

# Plain data only: the JSON-RPC bridge carries JSON, so these are the inputs
# the implementations were tested with.
def jsonable(v)
  case v
  when Hash then v.to_h { |k, x| [k.to_s, jsonable(x)] }
  when Array then v.map { |x| jsonable(x) }
  when String, Integer, Float, true, false, nil then v
  when Range then v.to_a
  else v.to_s
  end
end

case mode
when "bench"
  Dir[File.join(specs_root, "benchmarks", "*.yml")].sort.each do |path|
    loader.load_file(path).each do |spec|
      File.write(File.join(out, "#{spec.name}.json"), JSON.generate(
        "name" => spec.name,
        "template" => spec.template,
        "assigns" => jsonable(spec.raw_environment || {}),
        "partials" => jsonable(spec.raw_filesystem || {}),
        "expected" => spec.expected,
      ))
    end
  end
when "corpus"
  dirs = %w[basics liquid_ruby parser_errors partials benchmarks]
  dirs.each do |dir|
    Dir[File.join(specs_root, dir, "**", "*.yml")].sort.each do |path|
      next if path.include?("/_data/")
      begin
        loader.load_file(path).each do |spec|
          t = spec.template
          next unless t.is_a?(String) && !t.empty?
          File.binwrite(File.join(out, Digest::SHA1.hexdigest(t)), t)
        end
      rescue StandardError => e
        warn "skip #{path}: #{e.class}: #{e.message[0, 120]}"
      end
    end
  end
end
puts "#{Dir[File.join(out, '*')].size} files in #{out}"
