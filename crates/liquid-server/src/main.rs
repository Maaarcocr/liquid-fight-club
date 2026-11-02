//! JSON-RPC 2.0 server for liquid-spec (newline-delimited JSON on stdio).
//!
//! This is a thin adapter: it converts protocol messages into calls on the
//! `liquid` library and implements host-side pieces (file systems from the
//! compile request, drops backed by `drop_get`/`drop_call`/`drop_iterate`
//! callbacks to the client).

use liquid::convert::{json_to_assigns, json_to_value, value_to_json};
use liquid::json::{self, Json};
use liquid::{Error, ErrorKind, FileSystem, Limits, RenderOptions, Template, Timestamp, Value};
use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, BufReader, Read, Write};
use std::rc::Rc;

const MAX_TEMPLATES: usize = 2048;
const MAX_LINE_BYTES: usize = 256 * 1024 * 1024;

struct Io {
    reader: BufReader<std::io::Stdin>,
    writer: std::io::BufWriter<std::io::Stdout>,
    next_id: i64,
}

impl Io {
    /// Reads one line (without the newline). None on EOF or error.
    fn read_line(&mut self) -> Option<String> {
        let mut buf = Vec::new();
        let n = (&mut self.reader).take(MAX_LINE_BYTES as u64).read_until(b'\n', &mut buf).ok()?;
        if n == 0 {
            return None;
        }
        if buf.last() == Some(&b'\n') {
            buf.pop();
        }
        if buf.last() == Some(&b'\r') {
            buf.pop();
        }
        Some(String::from_utf8_lossy(&buf).into_owned())
    }

    fn send(&mut self, msg: &Json) -> bool {
        let mut s = msg.to_string();
        s.push('\n');
        self.writer.write_all(s.as_bytes()).is_ok() && self.writer.flush().is_ok()
    }
}

fn obj(members: Vec<(&str, Json)>) -> Json {
    Json::Object(members.into_iter().map(|(k, v)| (k.to_string(), v)).collect())
}

fn s(v: &str) -> Json {
    Json::Str(v.to_string())
}

// ---------------------------------------------------------------------------
// File system from the compile request
// ---------------------------------------------------------------------------

struct MapFileSystem {
    files: Vec<(String, String)>,
}

impl MapFileSystem {
    fn new(j: Option<&Json>) -> Option<MapFileSystem> {
        let Some(Json::Object(members)) = j else { return None };
        let mut files = Vec::new();
        for (k, v) in members {
            if let Json::Str(content) = v {
                let mut key = k.to_lowercase();
                if !key.ends_with(".liquid") {
                    key.push_str(".liquid");
                }
                files.push((key, content.clone()));
            }
        }
        if files.is_empty() { None } else { Some(MapFileSystem { files }) }
    }
}

impl FileSystem for MapFileSystem {
    fn read_template_file(&self, name: &str) -> liquid::Result<String> {
        let mut path = name.to_lowercase();
        if !path.ends_with(".liquid") {
            path.push_str(".liquid");
        }
        for (k, v) in &self.files {
            if k.eq_ignore_ascii_case(&path) {
                return Ok(v.clone());
            }
        }
        Err(Error::new(ErrorKind::FileSystem, format!("Could not find asset {}", name)))
    }
}

// ---------------------------------------------------------------------------
// RPC drops
// ---------------------------------------------------------------------------

struct RpcDrop {
    id: String,
    class: String,
    io: Rc<RefCell<Io>>,
}

impl RpcDrop {
    fn call(&self, method: &str, params: Json) -> liquid::Result<Json> {
        let mut io = self.io.borrow_mut();
        io.next_id += 1;
        let id = io.next_id + 1_000_000;
        let req = obj(vec![("jsonrpc", s("2.0")), ("id", Json::Int(id)), ("method", s(method)), ("params", params)]);
        if !io.send(&req) {
            return Err(Error::standard("drop callback failed"));
        }
        loop {
            let Some(line) = io.read_line() else {
                return Err(Error::standard("drop callback failed"));
            };
            let Ok(msg) = json::parse(&line) else { continue };
            if msg.get("id") != Some(&Json::Int(id)) {
                continue;
            }
            if let Some(err) = msg.get("error") {
                let m = err.get("message").and_then(|m| m.as_str()).unwrap_or("drop error");
                return Err(Error::standard(m.to_string()));
            }
            return Ok(msg.get("result").cloned().unwrap_or(Json::Null));
        }
    }

    fn value_of(&self, result: &Json, key: &str) -> Value {
        match result.get(key) {
            Some(v) => json_to_value(v, &mut |j| marker(j, &self.io)),
            None => Value::Nil,
        }
    }

    fn get_prop(&self, prop: Json) -> liquid::Result<Value> {
        let r = self.call("drop_get", obj(vec![("drop_id", s(&self.id)), ("property", prop)]))?;
        Ok(self.value_of(&r, "value"))
    }
}

impl liquid::Drop for RpcDrop {
    fn class_name(&self) -> &str {
        &self.class
    }

    fn get(&self, key: &Value) -> liquid::Result<Value> {
        self.get_prop(value_to_json(key, 0))
    }

    fn to_liquid_value(&self) -> Option<Value> {
        match self.get_prop(s("to_liquid_value")) {
            Ok(Value::Nil) | Err(_) => None,
            Ok(v) => Some(v),
        }
    }

    fn to_number(&self) -> Option<Value> {
        match self.get_prop(s("to_number")) {
            Ok(Value::Nil) | Err(_) => None,
            Ok(v) => Some(v),
        }
    }

    fn to_s(&self) -> String {
        match self.get_prop(s("to_s")) {
            Ok(v) => liquid::value::to_s_string(&v),
            Err(_) => self.class.clone(),
        }
    }

    fn iterate(&self) -> Option<liquid::Result<Vec<Value>>> {
        Some(self.call("drop_iterate", obj(vec![("drop_id", s(&self.id))])).map(|r| match self.value_of(&r, "items") {
            Value::Array(a) => a.to_vec(),
            _ => Vec::new(),
        }))
    }

    fn responds_to(&self, method: &str) -> bool {
        matches!(method, "size" | "first" | "last")
    }

    fn equals(&self, other: &Value) -> bool {
        match other {
            Value::Drop(d) => d.class_name() == self.class && liquid::value::to_s_string(other) == self.to_s(),
            _ => false,
        }
    }
}

/// Turns protocol marker objects into values.
fn marker(j: &Json, io: &Rc<RefCell<Io>>) -> Option<Value> {
    if let Some(id) = j.get("_rpc_drop").and_then(|v| v.as_str()) {
        let class = j.get("type").and_then(|v| v.as_str()).unwrap_or("Drop").to_string();
        return Some(Value::Drop(Rc::new(RpcDrop { id: id.to_string(), class, io: io.clone() })));
    }
    if let Some(name) = j.get("_instantiate").and_then(|v| v.as_str()) {
        let params = j.get("params").map(|p| json_to_value(p, &mut |x| marker(x, io)));
        return liquid::test_drops::instantiate(name, params.as_ref()).or(Some(Value::Nil));
    }
    if let Some(t) = j.get("_ruby_type").and_then(|v| v.as_str()) {
        return Some(match t {
            "Range" => {
                let b = j.get("begin").and_then(|v| v.as_i64()).unwrap_or(0);
                let mut e = j.get("end").and_then(|v| v.as_i64()).unwrap_or(0);
                if j.get("exclude_end").and_then(|v| v.as_bool()) == Some(true) {
                    e -= 1;
                }
                Value::Range(b, e)
            }
            "Symbol" => Value::str(j.get("value").and_then(|v| v.as_str()).unwrap_or("")),
            "Hash" => match j.get("data") {
                Some(d) => json_to_value(d, &mut |x| marker(x, io)),
                None => Value::Nil,
            },
            _ => Value::str(j.get("inspect").and_then(|v| v.as_str()).unwrap_or("")),
        });
    }
    None
}

// ---------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------

struct Compiled {
    template: Template,
    fs: Option<MapFileSystem>,
}

struct Server {
    io: Rc<RefCell<Io>>,
    templates: HashMap<String, Compiled>,
    order: VecDeque<String>,
    next_template: u64,
}

fn error_json(e: &Error, kind: &str) -> Json {
    let mut m = vec![("type", s(kind)), ("message", s(&e.to_string()))];
    if let Some(l) = e.line {
        m.push(("line", Json::Int(l as i64)));
    }
    obj(m)
}

fn limit_opt(j: Option<&Json>, key: &str) -> Option<u64> {
    j.and_then(|r| r.get(key)).and_then(|v| v.as_i64()).map(|v| v.max(0) as u64)
}

impl Server {
    fn handle(&mut self, method: &str, params: &Json) -> Result<Json, (i64, String, Option<Json>)> {
        match method {
            "initialize" => Ok(obj(vec![
                ("version", s("1.0")),
                ("implementation", s("liquid-rust")),
                ("liquid_version", s("5.13.0")),
                ("features", Json::Array(vec![s("drops")])),
            ])),
            "compile" => {
                let src = params.get("template").and_then(|t| t.as_str()).unwrap_or("");
                match Template::parse(src) {
                    Ok(t) => {
                        self.next_template += 1;
                        let id = format!("t{}", self.next_template);
                        let fs = MapFileSystem::new(params.get("filesystem"));
                        self.templates.insert(id.clone(), Compiled { template: t, fs });
                        self.order.push_back(id.clone());
                        while self.order.len() > MAX_TEMPLATES {
                            if let Some(old) = self.order.pop_front() {
                                self.templates.remove(&old);
                            }
                        }
                        Ok(obj(vec![("template_id", Json::Str(id))]))
                    }
                    Err(e) => Ok(obj(vec![("template_id", Json::Null), ("error", error_json(&e, "parse_error"))])),
                }
            }
            "render" => {
                let id = params.get("template_id").and_then(|t| t.as_str()).unwrap_or("");
                let Some(compiled) = self.templates.get(id) else {
                    return Err((-32602, "Invalid params".into(), Some(obj(vec![("message", s("unknown template_id"))]))));
                };
                let io = self.io.clone();
                let env = params.get("environment").cloned().unwrap_or(Json::Null);
                let Some(assigns) = json_to_assigns(&env, &mut |j| marker(j, &io)) else {
                    return Err((-32602, "Invalid params".into(), Some(obj(vec![("message", s("environment must be an object"))]))));
                };
                let options = params.get("options");
                let strict = options.and_then(|o| o.get("strict_errors")).and_then(|v| v.as_bool()).unwrap_or(true);
                let now = options
                    .and_then(|o| o.get("registers"))
                    .and_then(|r| r.get("current_time"))
                    .and_then(|t| t.as_str())
                    .and_then(|t| liquid::date::parse_time(t, Timestamp::now()));
                let rl = options.and_then(|o| o.get("resource_limits"));
                let limits = Limits {
                    render_length_limit: limit_opt(rl, "render_length_limit"),
                    render_score_limit: limit_opt(rl, "render_score_limit"),
                    assign_score_limit: limit_opt(rl, "assign_score_limit"),
                    cumulative_render_score_limit: limit_opt(rl, "cumulative_render_score_limit"),
                    cumulative_assign_score_limit: limit_opt(rl, "cumulative_assign_score_limit"),
                    ..Limits::default()
                };
                let opts = RenderOptions {
                    strict_errors: strict,
                    limits,
                    now,
                    file_system: compiled.fs.as_ref().map(|f| f as &dyn FileSystem),
                    template_name: None,
                };
                match compiled.template.render(assigns, &opts) {
                    Ok(r) => {
                        let errors = r.errors.iter().map(|e| error_json(e, "render_error")).collect();
                        Ok(obj(vec![("output", Json::Str(r.output)), ("errors", Json::Array(errors))]))
                    }
                    Err(e) => {
                        let msg = e.to_string();
                        Err((-32001, msg, Some(error_json(&e, "render_error"))))
                    }
                }
            }
            _ => Err((-32601, "Method not found".into(), None)),
        }
    }
}

fn serve() {
    let io = Rc::new(RefCell::new(Io {
        reader: BufReader::with_capacity(1 << 16, std::io::stdin()),
        writer: std::io::BufWriter::new(std::io::stdout()),
        next_id: 0,
    }));
    let mut server = Server { io: io.clone(), templates: HashMap::new(), order: VecDeque::new(), next_template: 0 };
    loop {
        let line = io.borrow_mut().read_line();
        let Some(line) = line else { break };
        if line.trim().is_empty() {
            continue;
        }
        let req = match json::parse(&line) {
            Ok(r) => r,
            Err(e) => {
                let resp = obj(vec![
                    ("jsonrpc", s("2.0")),
                    ("id", Json::Null),
                    ("error", obj(vec![("code", Json::Int(-32700)), ("message", s(&format!("Parse error: {}", e)))])),
                ]);
                if !io.borrow_mut().send(&resp) {
                    break;
                }
                continue;
            }
        };
        let method = req.get("method").and_then(|m| m.as_str()).unwrap_or("").to_string();
        if method == "quit" {
            break;
        }
        let id = req.get("id").cloned();
        let params = req.get("params").cloned().unwrap_or(Json::Object(Vec::new()));
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| server.handle(&method, &params)));
        let Some(id) = id else { continue };
        let resp = match result {
            Ok(Ok(r)) => obj(vec![("jsonrpc", s("2.0")), ("id", id), ("result", r)]),
            Ok(Err((code, message, data))) => {
                let mut e = vec![("code", Json::Int(code)), ("message", Json::Str(message))];
                if let Some(d) = data {
                    e.push(("data", d));
                }
                obj(vec![("jsonrpc", s("2.0")), ("id", id), ("error", obj(e))])
            }
            Err(_) => obj(vec![
                ("jsonrpc", s("2.0")),
                ("id", id),
                ("error", obj(vec![("code", Json::Int(-32603)), ("message", s("Internal error"))])),
            ]),
        };
        if !io.borrow_mut().send(&resp) {
            break;
        }
    }
}

fn main() {
    std::panic::set_hook(Box::new(|info| {
        eprintln!("liquid-server internal error: {}", info);
    }));
    // Run on a thread with a generous stack; the engine bounds its own recursion.
    let handle = std::thread::Builder::new().stack_size(256 * 1024 * 1024).spawn(serve).expect("spawn");
    let _ = handle.join();
}
