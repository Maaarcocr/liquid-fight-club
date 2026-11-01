#!/usr/bin/env ruby
# frozen_string_literal: true

# When executed directly, re-launch through liquid-spec on this file.
# When loaded by liquid-spec, this guard is skipped and the adapter DSL runs.
if __FILE__ == $PROGRAM_NAME
  cmd = ["liquid-spec", "run", __FILE__, *ARGV]
  ENV["BUNDLE_GEMFILE"] ||= File.expand_path("Gemfile", __dir__)
  cmd = ["bundle", "exec"] + cmd
  exec(*cmd)
end

require "liquid/spec/cli/adapter_dsl"

# ==============================================================================
# JSON-RPC Liquid Adapter
# ==============================================================================
#
# This adapter communicates with a Liquid implementation subprocess via
# JSON-RPC 2.0 over stdin/stdout. Implement the protocol below in any language.
#
# Usage:
#   ./adapter.rb                         # run directly (re-launches via liquid-spec)
#   liquid-spec run adapter.rb
#   liquid-spec run adapter.rb --command="./my-liquid-server"
#
# ==============================================================================
# PROTOCOL SPECIFICATION
# ==============================================================================
#
# All messages are JSON-RPC 2.0, one JSON object per line (newline-delimited).
# The subprocess reads requests from stdin and writes responses to stdout.
#
# --- LIFECYCLE ---
#
# 1. initialize (parent -> subprocess)
#    Request:  {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"version":"1.0"}}
#    Response: {"jsonrpc":"2.0","id":1,"result":{"version":"1.0","features":[]}}
#    Note: features are informational. The Ruby adapter controls spec
#    selection with config.missing_features below.
#
# 2. quit (parent -> subprocess) - notification, no response expected
#    Notification: {"jsonrpc":"2.0","method":"quit","params":{}}
#    Subprocess should exit cleanly within 1 second.
#
# --- COMPILE ---
#
# Compile a template and return an ID for later rendering.
#
# Request:
#   {
#     "jsonrpc": "2.0",
#     "id": 2,
#     "method": "compile",
#     "params": {
#       "template": "{{ x | upcase }}",
#       "options": {
#         "error_mode": "strict2", // "strict2", "strict", "lax", or null
#         "line_numbers": true
#       },
#       "filesystem": {             // templates for {% include %} / {% render %}
#         "snippet.liquid": "hello {{ name }}"
#       }
#     }
#   }
#
# Response (success):
#   {"jsonrpc":"2.0","id":2,"result":{"template_id":"abc123"}}
#
# Response (parse error, preferred):
#   {"jsonrpc":"2.0","id":2,"result":{"template_id":null,"error":{"type":"parse_error","line":1,"message":"Unknown tag 'foo'"}}}
#
# Legacy response also accepted:
#   {"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Parse error","data":{"type":"parse_error","line":1,"message":"Unknown tag 'foo'"}}}
#
# --- RENDER ---
#
# Render a previously compiled template with variables.
#
# Request:
#   {
#     "jsonrpc": "2.0",
#     "id": 3,
#     "method": "render",
#     "params": {
#       "template_id": "abc123",
#       "environment": {
#         "x": "hello",
#         "items": [1, 2, 3],
#         "user": {"_rpc_drop": "drop_1", "type": "UserDrop"}
#       },
#       "options": {
#         "strict_errors": true  // true = report render errors, false = render inline text
#       }
#     }
#   }
#
# Response (success):
#   {"jsonrpc":"2.0","id":3,"result":{"output":"HELLO"}}
#
# Response (render error with strict_errors=true, preferred):
#   {"jsonrpc":"2.0","id":3,"result":{"output":null,"error":{"type":"render_error","line":1,"message":"undefined method"}}}
#
# Response (inline render error with strict_errors=false):
#   {"jsonrpc":"2.0","id":3,"result":{"output":"Liquid error: undefined method","errors":[{"type":"render_error","message":"undefined method"}]}}
#
# Legacy render-error JSON-RPC error code -32001 is also accepted.
#
# --- RPC DROPS (subprocess -> parent) ---
#
# When environment contains {"_rpc_drop": "ID", "type": "ClassName"},
# the subprocess must call back to parent to access properties/methods.
# Send the request to stdout, read the response from stdin.
#
# drop_get - Get a property value:
#   Request:  {"jsonrpc":"2.0","id":100,"method":"drop_get","params":{"drop_id":"drop_1","property":"name"}}
#   Response: {"jsonrpc":"2.0","id":100,"result":{"value":"John"}}
#   Note: value may itself be {"_rpc_drop": "drop_2", "type": "..."} for nested drops
#
# drop_call - Call a method with arguments:
#   Request:  {"jsonrpc":"2.0","id":101,"method":"drop_call","params":{"drop_id":"drop_1","method":"calculate","args":[1,2]}}
#   Response: {"jsonrpc":"2.0","id":101,"result":{"value":3}}
#
# drop_iterate - Get all items from enumerable (for {% for %} loops):
#   Request:  {"jsonrpc":"2.0","id":102,"method":"drop_iterate","params":{"drop_id":"drop_1"}}
#   Response: {"jsonrpc":"2.0","id":102,"result":{"items":[1,2,3,4,5]}}
#
# --- ERROR CODES ---
#
# -32000  Legacy parse error (accepted; prefer result.error)
# -32001  Legacy render error (accepted; prefer result.error)
# -32002  Drop access error (property/method not found)
# -32700  JSON parse error (invalid JSON received)
# -32600  Invalid request (malformed JSON-RPC)
# -32601  Method not found (unknown method name)
#
# ==============================================================================
#
# DEBUG OUTPUT
# ------------
# Your server can write to stderr for debug output. liquid-spec forwards all
# stderr to the terminal, prefixed with [server-name]. Example:
#   console.error("Compiling:", template);  // Node.js
#   eprintln!("Compiling: {}", template);   // Rust
#   print("Compiling:", template, file=sys.stderr)  # Python
#
# ==============================================================================

DEFAULT_COMMAND = File.expand_path("../build/liquid-server", __dir__)
DEFAULT_TIMEOUT = 5  # seconds

LiquidSpec.setup do |ctx|
  require "liquid"
  require "liquid/spec/json_rpc/adapter"

  # CLI --command flag overrides DEFAULT_COMMAND
  command = LiquidSpec.cli_options[:command] || DEFAULT_COMMAND
  timeout = LiquidSpec.cli_options[:timeout]&.to_i || DEFAULT_TIMEOUT

  ctx[:adapter] = Liquid::Spec::JsonRpc::Adapter.new(command, timeout: timeout)
  ctx[:adapter].start

  # Features reported by the subprocess are informational. Spec
  # selection is controlled by config.missing_features below.
  ctx[:features] = ctx[:adapter].features

  at_exit { ctx[:adapter]&.shutdown }
end

LiquidSpec.configure do |config|
  # FIXED SCOPE FOR THIS PROJECT - do not edit (this file is checksummed).
  config.error_modes = [:strict2]
  config.render_error_modes = [:raise, :inline]

  # Out of scope: Ruby-only value types and drop classes, raw binary data
  # (cannot cross JSON), two resource-limit/blank-body behaviours that the
  # reference implementation itself cannot express over JSON-RPC, and
  # Shopify platform extensions. Everything else is in scope, including
  # drops via the JSON-RPC drop_get / drop_call / drop_iterate callbacks.
  config.missing_features = [
    :ruby_types,
    :ruby_drops,
    :binary_data,
    :range_resource_limits,
    :strict2_blank_body_errors,
    :shopify_tags,
    :shopify_objects,
    :shopify_filters,
    :shopify_includes,
    :shopify_blank,
    :shopify_error_handling,
    :shopify_error_format,
    :shopify_string_access,
  ]
end

# ERROR MODES
# -----------
# liquid-spec sends one of the modes declared in config.error_modes in the
# compile request's options.error_mode field. Unannotated specs use the
# highest supported strictness. Explicit multi-mode specs use their
# highest supported strict mode; explicit :lax still runs separately.
#
# Legacy modes (:strict, :lax) exist for backwards compatibility
# with older Liquid versions or Shopify production but are NOT recommended
# for new implementations unless you need that compatibility. Let specs
# targeting those modes be skipped.
LiquidSpec.compile do |ctx, source, options|
  ctx[:adapter].spec_context = ctx
  ctx[:template_id] = ctx[:adapter].compile(source, options)
end

# Render errors are raised by default (strict_errors: true). Only specs
# that opt into render_errors set strict_errors to false.
LiquidSpec.render do |ctx, assigns, options|
  ctx[:adapter].spec_context = ctx
  ctx[:adapter].render(ctx[:template_id], assigns, options)
end
