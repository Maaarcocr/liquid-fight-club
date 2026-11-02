# Replay one fuzz input through Ruby Liquid with the same assigns and partials
# as eval/fuzz.c, to tell real hangs from ones the reference shares.
#   ruby eval/replay.rb FILE
require "bundler/setup"
require "json"
require "liquid"

src = File.read(File.join(__dir__, "fuzz.c"))
cstr = ->(name) { JSON.parse(src[/#{name}\[\] =\n(.*?);\n/m, 1].scan(/"((?:[^"\\]|\\.)*)"/).map { |(s)| s.gsub(/\\(.)/, '\1') }.join) }
assigns = cstr.("ASSIGNS")
partials = cstr.("PARTIALS")

class MemFS
  def initialize(h) = @h = h
  def read_template_file(name) = @h.fetch(name) { raise Liquid::FileSystemError, "No such template '#{name}'" }
end
env = Liquid::Environment.build(file_system: MemFS.new(partials))
begin
  t = Liquid::Template.parse(File.binread(ARGV[0]).force_encoding("UTF-8"), environment: env,
                             error_mode: :strict2, line_numbers: true)
  out = t.render!(assigns)
  puts "ok #{out.bytesize}"
rescue Liquid::Error, ArgumentError, SystemStackError, NoMemoryError, EncodingError => e
  puts "error #{e.class}"
end
