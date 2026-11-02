//! Rendering: the execution context and the tree-walking renderer.

use crate::date::Timestamp;
use crate::drop::Drop;
use crate::error::{Error, ErrorKind, Result};
use crate::expression::{Expr, Lookup, VarLookup, VarName};
use crate::filters;
use crate::num;
use crate::parse::{
    parse_template, Body, CaseArm, CondOp, Condition, CycleKey, ForTag, Node, PartialTag, TableRowTag, Tag, TagKind,
    Template, Variable, MAX_DEPTH,
};
use crate::value::{self, inspect, is_blank, is_empty, num_cmp, ruby_eq, Hash, RcStr, Value};
use std::cell::Cell;
use std::cmp::Ordering;
use std::collections::HashMap;
use std::rc::Rc;

/// Source of partial templates for `include` and `render`.
pub trait FileSystem {
    /// Returns the template source, or an error (usually `ErrorKind::FileSystem`).
    fn read_template_file(&self, name: &str) -> Result<String>;
}

/// The file system used when none is configured.
pub struct BlankFileSystem;

impl FileSystem for BlankFileSystem {
    fn read_template_file(&self, _name: &str) -> Result<String> {
        Err(Error::new(ErrorKind::FileSystem, "This liquid context does not allow includes."))
    }
}

/// Resource limits (all optional, like Ruby's `ResourceLimits`), plus the
/// engine's own safety caps.
#[derive(Clone, Debug)]
pub struct Limits {
    pub render_length_limit: Option<u64>,
    pub render_score_limit: Option<u64>,
    pub assign_score_limit: Option<u64>,
    pub cumulative_render_score_limit: Option<u64>,
    pub cumulative_assign_score_limit: Option<u64>,
    /// Hard cap on output buffer size in bytes.
    pub max_output_bytes: u64,
    /// Hard cap on render work (nodes rendered + loop iterations).
    pub max_steps: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            render_length_limit: None,
            render_score_limit: None,
            assign_score_limit: None,
            cumulative_render_score_limit: None,
            cumulative_assign_score_limit: None,
            max_output_bytes: 256 * 1024 * 1024,
            max_steps: 50_000_000,
        }
    }
}

/// Options for one render.
pub struct RenderOptions<'a> {
    /// Raise render errors (true) instead of rendering them inline.
    pub strict_errors: bool,
    pub limits: Limits,
    /// The current time for `now`/`today` in the date filter.
    pub now: Option<Timestamp>,
    pub file_system: Option<&'a dyn FileSystem>,
    /// Name of the root template (shown in error messages).
    pub template_name: Option<RcStr>,
}

impl Default for RenderOptions<'_> {
    fn default() -> Self {
        RenderOptions { strict_errors: true, limits: Limits::default(), now: None, file_system: None, template_name: None }
    }
}

/// Maximum length of strings and arrays produced while rendering.
pub const MAX_STRING_BYTES: usize = 128 * 1024 * 1024;
pub const MAX_ARRAY_LEN: usize = 16 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Loop drops
// ---------------------------------------------------------------------------

pub struct ForloopDrop {
    name: RcStr,
    length: i64,
    index: Cell<i64>,
    parent: Option<Rc<ForloopDrop>>,
}

impl ForloopDrop {
    fn increment(&self) {
        self.index.set(self.index.get() + 1);
    }
}

impl Drop for ForloopDrop {
    fn class_name(&self) -> &str {
        "Liquid::ForloopDrop"
    }

    fn get(&self, key: &Value) -> Result<Value> {
        let i = self.index.get();
        let len = self.length;
        Ok(match key.as_str().unwrap_or("") {
            "length" => Value::Int(len),
            "parentloop" => match &self.parent {
                Some(p) => Value::Drop(p.clone()),
                None => Value::Nil,
            },
            "name" => Value::Str(self.name.clone()),
            "index" => Value::Int(i + 1),
            "index0" => Value::Int(i),
            "rindex" => Value::Int(len - i),
            "rindex0" => Value::Int(len - i - 1),
            "first" => Value::Bool(i == 0),
            "last" => Value::Bool(i == len - 1),
            _ => Value::Nil,
        })
    }
}

pub struct TablerowloopDrop {
    length: i64,
    cols: i64,
    index: Cell<i64>,
    row: Cell<i64>,
    col: Cell<i64>,
}

impl TablerowloopDrop {
    fn increment(&self) {
        self.index.set(self.index.get() + 1);
        if self.col.get() == self.cols {
            self.col.set(1);
            self.row.set(self.row.get() + 1);
        } else {
            self.col.set(self.col.get() + 1);
        }
    }
}

impl Drop for TablerowloopDrop {
    fn class_name(&self) -> &str {
        "Liquid::TablerowloopDrop"
    }

    fn get(&self, key: &Value) -> Result<Value> {
        let i = self.index.get();
        let len = self.length;
        Ok(match key.as_str().unwrap_or("") {
            "length" => Value::Int(len),
            "col" => Value::Int(self.col.get()),
            "row" => Value::Int(self.row.get()),
            "index" => Value::Int(i + 1),
            "index0" => Value::Int(i),
            "col0" => Value::Int(self.col.get() - 1),
            "rindex" => Value::Int(len - i),
            "rindex0" => Value::Int(len - i - 1),
            "first" => Value::Bool(i == 0),
            "last" => Value::Bool(i == len - 1),
            "col_first" => Value::Bool(self.col.get() == 1),
            "col_last" => Value::Bool(self.col.get() == self.cols),
            _ => Value::Nil,
        })
    }
}

/// `self`: resolves variables through the scope chain of the context it was
/// created in. Lookups are handled by the renderer, which recognizes this drop.
pub struct SelfDrop {
    ctx_id: u64,
}

impl Drop for SelfDrop {
    fn class_name(&self) -> &str {
        "Liquid::SelfDrop"
    }

    fn get(&self, _key: &Value) -> Result<Value> {
        Ok(Value::Nil)
    }

    fn equals(&self, other: &Value) -> bool {
        match other {
            Value::Drop(d) => d.as_any().and_then(|a| a.downcast_ref::<SelfDrop>()).is_some_and(|o| o.ctx_id == self.ctx_id),
            _ => false,
        }
    }

    fn as_any(&self) -> Option<&dyn std::any::Any> {
        Some(self)
    }
}

fn self_drop_id(v: &Value) -> Option<u64> {
    match v {
        Value::Drop(d) => d.as_any().and_then(|a| a.downcast_ref::<SelfDrop>()).map(|s| s.ctx_id),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum Interrupt {
    Break,
    Continue,
}

/// State that an isolated subcontext (`render`) starts fresh.
#[derive(Default)]
struct Local {
    /// Identifies the context (for `self`).
    id: u64,
    /// scopes[0] is the outermost (persistent) scope.
    scopes: Vec<Hash>,
    /// `environments.first`: increment/decrement counters.
    env_overlay: Hash,
    interrupts: Vec<Interrupt>,
    for_offsets: HashMap<RcStr, i128>,
    for_stack: Vec<Rc<ForloopDrop>>,
    cycles: HashMap<String, i64>,
    ifchanged: Option<String>,
    base_scope_depth: usize,
    template_name: Option<RcStr>,
}

struct Scores {
    render: u64,
    assign: u64,
    cumulative_render: u64,
    cumulative_assign: u64,
    /// Some(len) while capturing (`@last_capture_length`).
    last_capture_length: Option<u64>,
    steps: u64,
}

pub(crate) struct Context<'a> {
    opts: &'a RenderOptions<'a>,
    file_system: &'a dyn FileSystem,
    /// The render assigns (`static_environments`), shared by all contexts.
    statics: Rc<Hash>,
    local: Local,
    /// Contexts suspended by `render` (innermost last).
    saved: Vec<Local>,
    next_ctx_id: u64,
    pub(crate) errors: Vec<Error>,
    scores: Scores,
    disabled_include: usize,
    partials: HashMap<RcStr, Rc<Template>>,
    parse_trim: bool,
}

pub struct RenderResult {
    pub output: String,
    pub errors: Vec<Error>,
}

/// Renders a template. With `strict_errors`, the first render error is
/// returned as `Err`; otherwise errors are rendered inline.
pub fn render(template: &Template, assigns: Rc<Hash>, opts: &RenderOptions) -> std::result::Result<RenderResult, Error> {
    let mut ctx = Context {
        opts,
        file_system: opts.file_system.unwrap_or(&BlankFileSystem),
        statics: assigns,
        local: Local { id: 0, scopes: vec![Hash::new()], template_name: opts.template_name.clone(), ..Local::default() },
        saved: Vec::new(),
        next_ctx_id: 1,
        errors: Vec::new(),
        scores: Scores {
            render: 0,
            assign: 0,
            cumulative_render: 0,
            cumulative_assign: 0,
            last_capture_length: None,
            steps: 0,
        },
        disabled_include: 0,
        partials: HashMap::new(),
        parse_trim: template.trailing_trim,
    };
    let mut out = String::new();
    match ctx.render_template_body(&template.root, &mut out) {
        Ok(()) => Ok(RenderResult { output: out, errors: ctx.errors }),
        Err(e) => {
            if e.kind == ErrorKind::Memory && !opts.strict_errors {
                // Template#render rescues MemoryError: the output is the message.
                let e = ctx.record_error(e, None);
                let msg = e.to_string();
                return Ok(RenderResult { output: msg, errors: ctx.errors });
            }
            Err(e)
        }
    }
}

#[derive(Clone, Copy)]
enum Operand<'v> {
    Val(&'v Value),
    Empty,
    Blank,
}

enum Items {
    List(Vec<Value>),
    Range(i64, usize),
}

impl Items {
    fn len(&self) -> usize {
        match self {
            Items::List(v) => v.len(),
            Items::Range(_, n) => *n,
        }
    }

    fn get(&self, i: usize) -> Value {
        match self {
            Items::List(v) => v[i].clone(),
            Items::Range(s, _) => Value::Int(s + i as i64),
        }
    }
}

impl<'a> Context<'a> {
    // ----- scores and limits -------------------------------------------------

    fn limit_error(&self) -> Error {
        Error::memory()
    }

    fn add_steps(&mut self, n: u64) -> Result<()> {
        self.scores.steps = self.scores.steps.saturating_add(n);
        if self.scores.steps > self.opts.limits.max_steps {
            return Err(self.limit_error());
        }
        Ok(())
    }

    fn increment_render_score(&mut self, n: u64) -> Result<()> {
        self.add_steps(n)?;
        self.scores.render = self.scores.render.saturating_add(n);
        self.scores.cumulative_render = self.scores.cumulative_render.saturating_add(n);
        let l = &self.opts.limits;
        if l.render_score_limit.is_some_and(|m| self.scores.render > m)
            || l.cumulative_render_score_limit.is_some_and(|m| self.scores.cumulative_render > m)
        {
            return Err(self.limit_error());
        }
        Ok(())
    }

    fn increment_assign_score(&mut self, n: u64) -> Result<()> {
        self.scores.assign = self.scores.assign.saturating_add(n);
        self.scores.cumulative_assign = self.scores.cumulative_assign.saturating_add(n);
        let l = &self.opts.limits;
        if l.assign_score_limit.is_some_and(|m| self.scores.assign > m)
            || l.cumulative_assign_score_limit.is_some_and(|m| self.scores.cumulative_assign > m)
        {
            return Err(self.limit_error());
        }
        Ok(())
    }

    fn increment_write_score(&mut self, out: &str) -> Result<()> {
        let len = out.len() as u64;
        if len > self.opts.limits.max_output_bytes {
            return Err(self.limit_error());
        }
        if let Some(last) = self.scores.last_capture_length {
            let inc = len.saturating_sub(last);
            self.scores.last_capture_length = Some(len);
            self.increment_assign_score(inc)?;
        } else if let Some(m) = self.opts.limits.render_length_limit {
            if len > m {
                return Err(self.limit_error());
            }
        }
        Ok(())
    }

    /// `ResourceLimits#reset` (called whenever a template, including a partial, renders).
    fn reset_scores(&mut self) -> Result<()> {
        self.scores.render = 0;
        self.scores.assign = 0;
        self.scores.last_capture_length = None;
        let l = &self.opts.limits;
        if l.cumulative_render_score_limit.is_some_and(|m| self.scores.cumulative_render > m)
            || l.cumulative_assign_score_limit.is_some_and(|m| self.scores.cumulative_assign > m)
        {
            return Err(self.limit_error());
        }
        Ok(())
    }

    fn assign_score_of(v: &Value, depth: usize) -> u64 {
        match v {
            Value::Str(s) => s.len() as u64,
            Value::Array(a) if depth < 64 => 1 + a.iter().map(|c| Self::assign_score_of(c, depth + 1)).sum::<u64>(),
            Value::Hash(h) if depth < 64 => {
                1 + h.iter().map(|(k, c)| k.len() as u64 + Self::assign_score_of(c, depth + 1)).sum::<u64>()
            }
            _ => 1,
        }
    }

    // ----- errors -------------------------------------------------------------

    fn record_error(&mut self, mut e: Error, line: Option<u32>) -> Error {
        if e.template_name.is_none() {
            e.template_name = self.local.template_name.clone();
        }
        if e.line.is_none() {
            e.line = line;
        }
        self.errors.push(e.clone());
        e
    }

    /// `BlockBody.rescue_render_node`.
    fn handle_node_error(&mut self, e: Error, line: Option<u32>, blank_tag: bool, out: &mut String) -> Result<()> {
        if e.kind == ErrorKind::Memory {
            return Err(e);
        }
        let e = self.record_error(e, line);
        if self.opts.strict_errors {
            return Err(e);
        }
        if !blank_tag {
            out.push_str(&e.to_string());
        }
        Ok(())
    }

    // ----- scopes -------------------------------------------------------------

    fn push_scope(&mut self) -> Result<()> {
        self.local.scopes.push(Hash::new());
        if self.local.base_scope_depth + self.local.scopes.len() > MAX_DEPTH {
            self.local.scopes.pop();
            return Err(Error::nesting());
        }
        Ok(())
    }

    fn pop_scope(&mut self) {
        if self.local.scopes.len() > 1 {
            self.local.scopes.pop();
        }
    }

    /// `context[key] = value` (innermost scope).
    fn set_local(&mut self, key: RcStr, value: Value) {
        self.local.scopes.last_mut().expect("scope").insert(key, value);
    }

    /// `context.scopes.last[key] = value` (outermost scope).
    fn set_outer(&mut self, key: RcStr, value: Value) {
        self.local.scopes[0].insert(key, value);
    }

    fn env_get(&self, key: &str) -> Option<Value> {
        self.local.env_overlay.get(key).cloned()
    }

    /// The context a `self` drop refers to (the current one if it is gone).
    fn local_by_id(&self, id: u64) -> &Local {
        if self.local.id == id {
            return &self.local;
        }
        self.saved.iter().rev().find(|l| l.id == id).unwrap_or(&self.local)
    }

    /// `Context#find_variable`.
    fn find_variable(&self, key: &str) -> Value {
        self.find_variable_in(&self.local, key)
    }

    fn find_variable_in(&self, local: &Local, key: &str) -> Value {
        for scope in local.scopes.iter().rev() {
            if let Some(v) = scope.get(key) {
                return to_liquid(v.clone());
            }
        }
        if let Some(v) = local.env_overlay.get(key) {
            if !v.is_nil() {
                return to_liquid(v.clone());
            }
        }
        if let Some(v) = self.statics.get(key) {
            if !v.is_nil() {
                return to_liquid(v.clone());
            }
        }
        if key == "self" && !local.scopes.iter().any(|s| s.contains_key(key)) {
            return Value::Drop(Rc::new(SelfDrop { ctx_id: local.id }));
        }
        Value::Nil
    }

    /// `Context#variable_defined?`.
    fn variable_defined_in(&self, local: &Local, key: &str) -> bool {
        local.scopes.iter().any(|s| s.contains_key(key))
            || local.env_overlay.contains_key(key)
            || self.statics.contains_key(key)
    }

    /// `SelfDrop#[]`: looks a key up in the drop's context (nil when undefined).
    pub(crate) fn self_lookup(&self, v: &Value, key: &Value) -> Option<Option<Value>> {
        let id = self_drop_id(v)?;
        let local = self.local_by_id(id);
        Some(match key {
            Value::Str(k) => {
                if self.variable_defined_in(local, k) {
                    Some(self.find_variable_in(local, k))
                } else {
                    None
                }
            }
            _ => None,
        })
    }

    // ----- evaluation -----------------------------------------------------------

    pub(crate) fn evaluate(&mut self, e: &Expr) -> Result<Value> {
        match e {
            Expr::Lit(v) => Ok(v.clone()),
            Expr::Var(v) => self.eval_var(v),
            Expr::Range(r, _) => {
                let a = self.evaluate(&r.0)?;
                let b = self.evaluate(&r.1)?;
                Ok(Value::Range(range_to_integer(&a)?, range_to_integer(&b)?))
            }
            Expr::Empty | Expr::Blank => Ok(Value::str("")),
        }
    }

    fn eval_var(&mut self, v: &VarLookup) -> Result<Value> {
        let mut obj = match &v.name {
            VarName::Ident(name) => self.find_variable(name),
            VarName::Expr(e) => match self.evaluate(e)? {
                Value::Str(s) => self.find_variable(&s),
                _ => Value::Nil,
            },
            VarName::None => Value::Nil,
        };
        for l in &v.lookups {
            let (key, command) = match l {
                Lookup::Key(k) => (Value::Str(k.clone()), false),
                Lookup::Command(k) => (Value::Str(k.clone()), true),
                Lookup::Index(e) => (self.evaluate(e)?, false),
            };
            let key = key.to_liquid_value();
            match self.lookup_property(&obj, &key, command)? {
                Some(v) => obj = v,
                None => return Ok(Value::Nil),
            }
        }
        Ok(obj)
    }

    /// One step of `VariableLookup#evaluate`.
    fn lookup_property(&mut self, obj: &Value, key: &Value, command: bool) -> Result<Option<Value>> {
        let cmd = if command { key.as_str() } else { None };
        Ok(match obj {
            Value::Hash(h) => {
                if let Value::Str(k) = key {
                    if let Some(v) = h.get(k) {
                        return Ok(Some(to_liquid(v.clone())));
                    }
                }
                if key.is_integer() {
                    return Ok(Some(Value::Nil));
                }
                match cmd {
                    Some("size") => Some(Value::Int(h.len() as i64)),
                    Some("first") => Some(match h.first() {
                        Some((k, v)) => Value::from(vec![Value::Str(k.clone()), v.clone()]),
                        None => Value::Nil,
                    }),
                    _ => None,
                }
            }
            Value::Array(a) => {
                if let Value::Int(i) = key {
                    let idx = if *i < 0 { a.len() as i64 + i } else { *i };
                    return Ok(Some(if idx >= 0 && (idx as usize) < a.len() {
                        to_liquid(a[idx as usize].clone())
                    } else {
                        Value::Nil
                    }));
                }
                if key.is_integer() {
                    return Ok(Some(Value::Nil));
                }
                match cmd {
                    Some("size") => Some(Value::Int(a.len() as i64)),
                    Some("first") => Some(a.first().cloned().map(to_liquid).unwrap_or(Value::Nil)),
                    Some("last") => Some(a.last().cloned().map(to_liquid).unwrap_or(Value::Nil)),
                    _ => None,
                }
            }
            Value::Str(s) => match cmd {
                Some("size") => Some(Value::Int(s.chars().count() as i64)),
                Some("first") => Some(Value::str(s.chars().next().map(|c| c.to_string()).as_deref().unwrap_or(""))),
                Some("last") => Some(Value::str(s.chars().last().map(|c| c.to_string()).as_deref().unwrap_or(""))),
                _ => None,
            },
            Value::Int(_) => match cmd {
                Some("size") => Some(Value::Int(8)),
                _ => None,
            },
            Value::BigInt(b) => match cmd {
                Some("size") => Some(Value::Int(bigint_size(b))),
                _ => None,
            },
            Value::Range(a, b) => match cmd {
                Some("size") => Some(Value::Int(Value::range_len(*a, *b) as i64)),
                Some("first") => Some(Value::Int(*a)),
                Some("last") => Some(Value::Int(*b)),
                _ => None,
            },
            Value::Drop(d) => {
                if let Some(r) = self.self_lookup(obj, key) {
                    return Ok(r);
                }
                Some(to_liquid(d.get(key)?))
            }
            _ => None,
        })
    }

    fn operand<'v>(&mut self, e: &Expr, slot: &'v mut Value) -> Result<Operand<'v>> {
        Ok(match e {
            Expr::Empty => Operand::Empty,
            Expr::Blank => Operand::Blank,
            _ => {
                *slot = self.evaluate(e)?;
                Operand::Val(slot)
            }
        })
    }

    fn eval_condition(&mut self, c: &Condition) -> Result<bool> {
        let mut c = c;
        loop {
            let r = self.interpret_condition(c)?;
            match &c.child {
                Some((is_and, child)) => {
                    if *is_and != r {
                        return Ok(r);
                    }
                    c = child;
                }
                None => return Ok(r),
            }
        }
    }

    /// Returns the truthiness of one comparison.
    fn interpret_condition(&mut self, c: &Condition) -> Result<bool> {
        let mut lslot = Value::Nil;
        let left = self.operand(&c.left, &mut lslot)?;
        let Some((op, right_e)) = &c.op else {
            return Ok(match left {
                Operand::Val(v) => v.to_liquid_value().is_truthy(),
                _ => true,
            });
        };
        let lv;
        let left = match left {
            Operand::Val(v) => {
                lv = v.to_liquid_value();
                Operand::Val(&lv)
            }
            o => o,
        };
        let mut rslot = Value::Nil;
        let right = self.operand(right_e, &mut rslot)?;
        let rv;
        let right = match right {
            Operand::Val(v) => {
                rv = v.to_liquid_value();
                Operand::Val(&rv)
            }
            o => o,
        };
        compare_operands(op, left, right)
    }

    fn render_variable_value(&mut self, v: &Variable) -> Result<Value> {
        let mut obj = match &v.expr {
            Some(e) => self.evaluate(e)?,
            None => Value::Nil,
        };
        for f in &v.filters {
            let mut args = Vec::with_capacity(f.args.len() + 1);
            for a in &f.args {
                args.push(self.evaluate(a)?);
            }
            if let Some(kw) = &f.kwargs {
                let mut h = Hash::with_capacity(kw.len());
                for (k, e) in kw {
                    let val = self.evaluate(e)?;
                    h.insert(k.clone(), val);
                }
                args.push(Value::from(h));
            }
            obj = filters::invoke(self, &f.name, obj, &args)?;
            obj = to_liquid(obj);
        }
        Ok(obj)
    }

    pub(crate) fn now(&self) -> Option<Timestamp> {
        self.opts.now
    }

    // ----- rendering -------------------------------------------------------------

    /// `Template#render` for the root or a partial: resets scores and handles MemoryError.
    fn render_template_body(&mut self, body: &Body, out: &mut String) -> Result<()> {
        self.reset_scores()?;
        self.render_body(body, out)
    }

    fn render_body(&mut self, body: &Body, out: &mut String) -> Result<()> {
        self.increment_render_score(body.nodes.len() as u64)?;
        for node in &body.nodes {
            match node {
                Node::Text(t) => out.push_str(t),
                Node::Output(v) => {
                    if let Err(e) = self.render_output(v, out) {
                        self.handle_node_error(e, v.line, false, out)?;
                    }
                    if !self.local.interrupts.is_empty() {
                        break;
                    }
                }
                Node::Tag(t) => {
                    if let Err(e) = self.render_tag(t, out) {
                        self.handle_node_error(e, t.line, t.blank(), out)?;
                    }
                    if !self.local.interrupts.is_empty() {
                        break;
                    }
                }
            }
            self.increment_write_score(out)?;
        }
        Ok(())
    }

    fn render_body_to_string(&mut self, body: &Body) -> Result<String> {
        let mut s = String::new();
        self.render_body(body, &mut s)?;
        Ok(s)
    }

    fn render_output(&mut self, v: &Variable, out: &mut String) -> Result<()> {
        let val = self.render_variable_value(v)?;
        value::write_output(&val, out);
        Ok(())
    }

    fn render_tag(&mut self, tag: &Tag, out: &mut String) -> Result<()> {
        match &tag.kind {
            TagKind::Assign { to, from } => {
                let val = self.render_variable_value(from)?;
                let score = Self::assign_score_of(&val, 0);
                self.set_outer(to.clone(), val);
                self.increment_assign_score(score)?;
            }
            TagKind::Capture { to, body } => {
                let old = self.scores.last_capture_length;
                self.scores.last_capture_length = Some(0);
                let r = self.render_body_to_string(body);
                self.scores.last_capture_length = old;
                let s = r?;
                self.set_outer(to.clone(), Value::from(s));
            }
            TagKind::Cycle { key, values } => self.render_cycle(key, values, out)?,
            TagKind::Echo(v) => {
                let mut s = String::new();
                self.render_output(v, &mut s)?;
                out.push_str(&s);
            }
            TagKind::Increment(name) | TagKind::Decrement(name) => {
                let cur = self.env_get(name).unwrap_or(Value::Nil);
                let cur = if cur.is_truthy() { cur } else { Value::Int(0) };
                let inc = matches!(tag.kind, TagKind::Increment(_));
                let next = match &cur {
                    Value::Int(_) | Value::BigInt(_) => {
                        let n = num::to_number(&cur);
                        let one = num::Num::Int(1);
                        (if inc { n.add(&one)? } else { n.sub(&one)? }).into_value()
                    }
                    Value::Float(f) => Value::Float(if inc { f + 1.0 } else { f - 1.0 }),
                    _ => return Err(Error::new(ErrorKind::Internal, "internal")),
                };
                let shown = if inc { cur } else { next.clone() };
                self.local.env_overlay.insert(name.clone(), next);
                value::write_to_s(&shown, out);
            }
            TagKind::Raw(s) => out.push_str(s),
            TagKind::Comment | TagKind::Doc { .. } | TagKind::InlineComment => {}
            TagKind::If { unless, blocks, .. } => {
                for (i, (cond, body)) in blocks.iter().enumerate() {
                    let truthy = match cond {
                        Some(c) => {
                            let t = self.eval_condition(c)?;
                            if *unless && i == 0 { !t } else { t }
                        }
                        None => true,
                    };
                    if truthy {
                        return self.render_body(body, out);
                    }
                }
            }
            TagKind::Case { left, arms, bodies, .. } => {
                let mut run_else = true;
                for arm in arms {
                    match arm {
                        CaseArm::Else(idx) => {
                            if run_else {
                                self.render_body(&bodies[*idx], out)?;
                            }
                        }
                        CaseArm::When(e, idx) => {
                            let mut lslot = Value::Nil;
                            let l = self.operand(left, &mut lslot)?;
                            let lv;
                            let l = match l {
                                Operand::Val(v) => {
                                    lv = v.to_liquid_value();
                                    Operand::Val(&lv)
                                }
                                o => o,
                            };
                            let mut rslot = Value::Nil;
                            let r = self.operand(e, &mut rslot)?;
                            let rv;
                            let r = match r {
                                Operand::Val(v) => {
                                    rv = v.to_liquid_value();
                                    Operand::Val(&rv)
                                }
                                o => o,
                            };
                            if equal_operands(l, r) {
                                run_else = false;
                                self.render_body(&bodies[*idx], out)?;
                            }
                        }
                    }
                }
            }
            TagKind::For(f, _) => self.render_for(f, out)?,
            TagKind::TableRow(t, _) => self.render_tablerow(t, out)?,
            TagKind::Break => self.local.interrupts.push(Interrupt::Break),
            TagKind::Continue => self.local.interrupts.push(Interrupt::Continue),
            TagKind::IfChanged(body, _) => {
                let s = self.render_body_to_string(body)?;
                if self.local.ifchanged.as_deref() != Some(s.as_str()) {
                    out.push_str(&s);
                    self.local.ifchanged = Some(s);
                }
            }
            TagKind::Include(p) => self.render_include(p, tag.line, out)?,
            TagKind::Render(p) => self.render_render(p, out)?,
        }
        Ok(())
    }

    fn render_cycle(&mut self, key: &CycleKey, values: &[Expr], out: &mut String) -> Result<()> {
        let k = match key {
            CycleKey::Unnamed(s) => format!("s:{}", s),
            CycleKey::Named(e) => {
                let v = self.evaluate(e)?;
                hash_key(&v)
            }
        };
        let iteration = *self.local.cycles.get(&k).unwrap_or(&0);
        let val = match usize::try_from(iteration).ok().and_then(|i| values.get(i)) {
            Some(e) => self.evaluate(e)?,
            None => Value::Nil,
        };
        match &val {
            Value::Array(_) => out.push_str(&array_join(&val, 0)),
            Value::Str(s) => out.push_str(s),
            other => value::write_to_s(other, out),
        }
        let mut next = iteration + 1;
        if next >= values.len() as i64 {
            next = 0;
        }
        self.local.cycles.insert(k, next);
        Ok(())
    }

    /// Converts the collection of a `for`/`tablerow` into items (`slice_collection`).
    fn slice_collection(&mut self, collection: &Value, from: i128, to: Option<i128>) -> Result<Items> {
        let start = from.max(0);
        let bounded = |len: usize| -> (usize, usize) {
            let end = match to {
                Some(t) => t.min(len as i128),
                None => len as i128,
            };
            let s = start.min(len as i128);
            if end <= s { (0, 0) } else { (s as usize, (end - s) as usize) }
        };
        Ok(match collection {
            Value::Array(a) => {
                let (s, n) = bounded(a.len());
                Items::List(a[s..s + n].to_vec())
            }
            Value::Range(a, b) => {
                let len = Value::range_len(*a, *b);
                let (s, n) = bounded(len);
                Items::Range(a.saturating_add(s as i64), n)
            }
            Value::Hash(h) => {
                let (s, n) = bounded(h.len());
                Items::List(
                    h.iter().skip(s).take(n).map(|(k, v)| Value::from(vec![Value::Str(k.clone()), v.clone()])).collect(),
                )
            }
            Value::Str(s) => {
                if s.is_empty() {
                    Items::List(Vec::new())
                } else {
                    Items::List(vec![collection.clone()])
                }
            }
            Value::Drop(d) => match d.iterate() {
                Some(items) => {
                    let items = items?;
                    let (s, n) = bounded(items.len());
                    Items::List(items[s..s + n].to_vec())
                }
                None => Items::List(Vec::new()),
            },
            _ => Items::List(Vec::new()),
        })
    }

    fn render_for(&mut self, f: &ForTag, out: &mut String) -> Result<()> {
        let from: i128 = match &f.offset {
            Some(None) => *self.local.for_offsets.get(&f.name).unwrap_or(&0),
            Some(Some(e)) => {
                let v = self.evaluate(e)?;
                if v.is_nil() { 0 } else { to_integer(&v)? as i128 }
            }
            None => 0,
        };
        let collection = self.evaluate(&f.collection)?;
        let to = match &f.limit {
            Some(e) => {
                let v = self.evaluate(e)?;
                if v.is_nil() { None } else { Some(to_integer(&v)? as i128 + from) }
            }
            None => None,
        };
        let items = self.slice_collection(&collection, from, to)?;
        let len = items.len();
        self.local.for_offsets.insert(f.name.clone(), from + len as i128);
        if len == 0 {
            if let Some(eb) = &f.else_body {
                self.render_body(eb, out)?;
            }
            return Ok(());
        }
        self.push_scope()?;
        let loop_drop = Rc::new(ForloopDrop {
            name: f.name.clone(),
            length: len as i64,
            index: Cell::new(0),
            parent: self.local.for_stack.last().cloned(),
        });
        self.local.for_stack.push(loop_drop.clone());
        let r = (|| -> Result<()> {
            self.set_local(Rc::from("forloop"), Value::Drop(loop_drop.clone()));
            for i in 0..len {
                let idx = if f.reversed { len - 1 - i } else { i };
                self.add_steps(1)?;
                self.set_local(f.var.clone(), items.get(idx));
                self.render_body(&f.body, out)?;
                loop_drop.increment();
                if let Some(int) = self.local.interrupts.pop() {
                    if int == Interrupt::Break {
                        break;
                    }
                }
            }
            Ok(())
        })();
        self.local.for_stack.pop();
        self.pop_scope();
        r
    }

    fn render_tablerow(&mut self, t: &TableRowTag, out: &mut String) -> Result<()> {
        let collection = self.evaluate(&t.collection)?;
        if !collection.is_truthy() {
            return Ok(());
        }
        let attr = |name: &str| t.attributes.iter().find(|(k, _)| &**k == name).map(|(_, e)| e);
        let from = match attr("offset") {
            Some(e) => {
                let v = self.evaluate(e)?;
                tablerow_to_integer(&v)? as i128
            }
            None => 0,
        };
        let to = match attr("limit") {
            Some(e) => {
                let v = self.evaluate(e)?;
                Some(from + tablerow_to_integer(&v)? as i128)
            }
            None => None,
        };
        let items = self.slice_collection(&collection, from, to)?;
        let len = items.len() as i64;
        let cols = match attr("cols") {
            Some(e) => {
                let v = self.evaluate(e)?;
                tablerow_to_integer(&v)?
            }
            None => len,
        };
        out.push_str("<tr class=\"row1\">\n");
        self.push_scope()?;
        let loop_drop =
            Rc::new(TablerowloopDrop { length: len, cols, index: Cell::new(0), row: Cell::new(1), col: Cell::new(1) });
        let r = (|| -> Result<()> {
            self.set_local(Rc::from("tablerowloop"), Value::Drop(loop_drop.clone()));
            for i in 0..items.len() {
                self.add_steps(1)?;
                self.set_local(t.var.clone(), items.get(i));
                out.push_str("<td class=\"col");
                out.push_str(&loop_drop.col.get().to_string());
                out.push_str("\">");
                let s = self.render_body_to_string(&t.body)?;
                out.push_str(&s);
                out.push_str("</td>");
                if let Some(int) = self.local.interrupts.pop() {
                    if int == Interrupt::Break {
                        break;
                    }
                }
                let col_last = loop_drop.col.get() == loop_drop.cols;
                let last = loop_drop.index.get() == loop_drop.length - 1;
                if col_last && !last {
                    out.push_str("</tr>\n<tr class=\"row");
                    out.push_str(&(loop_drop.row.get() + 1).to_string());
                    out.push_str("\">");
                }
                loop_drop.increment();
            }
            Ok(())
        })();
        self.pop_scope();
        r?;
        out.push_str("</tr>\n");
        Ok(())
    }

    /// `PartialCache.load`.
    fn load_partial(&mut self, name: &str) -> Result<Rc<Template>> {
        if let Some(t) = self.partials.get(name) {
            return Ok(t.clone());
        }
        let source = self.file_system.read_template_file(name)?;
        match parse_template(&source, true, self.parse_trim) {
            Ok(t) => {
                self.parse_trim = t.trailing_trim;
                let t = Rc::new(t);
                if self.partials.len() < 10_000 {
                    self.partials.insert(Rc::from(name), t.clone());
                }
                Ok(t)
            }
            Err(mut e) => {
                e.template_name = Some(Rc::from(name));
                Err(e)
            }
        }
    }

    /// Renders a partial's root body like `Template#render` with an existing context.
    fn render_partial_template(&mut self, t: &Template, out: &mut String) -> Result<()> {
        match self.render_template_body(&t.root, out) {
            Err(e) if e.kind == ErrorKind::Memory && !self.opts.strict_errors => {
                self.record_error(e, None);
                Ok(())
            }
            r => r,
        }
    }

    fn render_include(&mut self, p: &PartialTag, line: Option<u32>, out: &mut String) -> Result<()> {
        if self.disabled_include > 0 {
            let e = Error::new(ErrorKind::Disabled, "include usage is not allowed in this context");
            let e = self.record_error(e, line);
            if self.opts.strict_errors {
                return Err(e);
            }
            out.push_str(&e.to_string());
            return Ok(());
        }
        let name = match self.evaluate(&p.template)? {
            Value::Str(s) => s,
            _ => return Err(Error::argument("Argument error in tag 'include' - Illegal template name")),
        };
        let partial = self.load_partial(&name)?;
        let var_name: RcStr = match &p.alias {
            Some(a) => a.clone(),
            None => Rc::from(name.rsplit('/').next().unwrap_or(&name)),
        };
        let variable = match &p.variable {
            Some(e) => self.evaluate(e)?,
            None => self.find_variable(&name),
        };
        let old_name = self.local.template_name.replace(name.clone());
        let r = (|| -> Result<()> {
            self.push_scope()?;
            let r = (|| -> Result<()> {
                for (k, e) in &p.attributes {
                    let v = self.evaluate(e)?;
                    self.set_local(k.clone(), v);
                }
                if let Value::Array(items) = &variable {
                    for item in items.iter() {
                        self.set_local(var_name.clone(), item.clone());
                        self.render_partial_template(&partial, out)?;
                    }
                } else {
                    self.set_local(var_name.clone(), variable.clone());
                    self.render_partial_template(&partial, out)?;
                }
                Ok(())
            })();
            self.pop_scope();
            r
        })();
        self.local.template_name = old_name;
        r
    }

    fn render_render(&mut self, p: &PartialTag, out: &mut String) -> Result<()> {
        self.disabled_include += 1;
        let r = self.render_render_inner(p, out);
        self.disabled_include -= 1;
        r
    }

    fn render_render_inner(&mut self, p: &PartialTag, out: &mut String) -> Result<()> {
        let name = match &p.template {
            Expr::Lit(Value::Str(s)) => s.clone(),
            _ => return Err(Error::argument("Argument error in tag 'render' - Illegal template name")),
        };
        let partial = self.load_partial(&name)?;
        let var_name: RcStr = match &p.alias {
            Some(a) => a.clone(),
            None => Rc::from(name.rsplit('/').next().unwrap_or(&name)),
        };
        let variable = match &p.variable {
            Some(e) => self.evaluate(e)?,
            None => Value::Nil,
        };
        let items: Option<Vec<Value>> = if p.is_for {
            match &variable {
                Value::Array(a) => Some(a.to_vec()),
                Value::Hash(h) => {
                    Some(h.iter().map(|(k, v)| Value::from(vec![Value::Str(k.clone()), v.clone()])).collect())
                }
                Value::Range(a, b) => {
                    let n = Value::range_len(*a, *b);
                    if n > MAX_ARRAY_LEN {
                        return Err(Error::memory());
                    }
                    Some((0..n).map(|i| Value::Int(a + i as i64)).collect())
                }
                Value::Drop(d) => match d.iterate() {
                    Some(r) => Some(r?),
                    None => None,
                },
                _ => None,
            }
        } else {
            None
        };
        match items {
            Some(items) => {
                let forloop = Rc::new(ForloopDrop {
                    name: name.clone(),
                    length: items.len() as i64,
                    index: Cell::new(0),
                    parent: None,
                });
                for item in items {
                    self.add_steps(1)?;
                    self.render_isolated(p, &partial, &name, &var_name, item, Some(forloop.clone()), out)?;
                    forloop.increment();
                }
            }
            None => self.render_isolated(p, &partial, &name, &var_name, variable, None, out)?,
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn render_isolated(
        &mut self,
        p: &PartialTag,
        partial: &Template,
        name: &RcStr,
        var_name: &RcStr,
        var: Value,
        forloop: Option<Rc<ForloopDrop>>,
        out: &mut String,
    ) -> Result<()> {
        // new_isolated_subcontext checks the depth first.
        if self.local.base_scope_depth + self.local.scopes.len() > MAX_DEPTH {
            return Err(Error::nesting());
        }
        // Attributes are evaluated in the outer context.
        let mut attrs = Vec::with_capacity(p.attributes.len());
        for (k, e) in &p.attributes {
            attrs.push((k.clone(), self.evaluate(e)?));
        }
        let inner = Local {
            id: self.next_ctx_id,
            scopes: vec![Hash::new()],
            base_scope_depth: self.local.base_scope_depth + 1,
            template_name: Some(name.clone()),
            ..Local::default()
        };
        self.next_ctx_id += 1;
        let saved = std::mem::replace(&mut self.local, inner);
        self.saved.push(saved);
        if let Some(f) = forloop {
            self.set_local(Rc::from("forloop"), Value::Drop(f));
        }
        for (k, v) in attrs {
            self.set_local(k, v);
        }
        if !var.is_nil() {
            self.set_local(var_name.clone(), var);
        }
        let r = self.render_partial_template(partial, out);
        self.local = self.saved.pop().expect("saved context");
        r
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// `to_liquid`: drops may turn into other values.
pub(crate) fn to_liquid(v: Value) -> Value {
    match &v {
        Value::Drop(d) => match d.to_liquid() {
            Some(x) => x,
            None => v,
        },
        _ => v,
    }
}

fn bigint_size(b: &crate::bigint::BigInt) -> i64 {
    let bytes = b.bit_length().div_ceil(8) as i64;
    (bytes + 7) / 8 * 8
}

/// `Utils.to_integer`.
pub(crate) fn to_integer(v: &Value) -> Result<i64> {
    match v {
        Value::Int(i) => Ok(*i),
        Value::BigInt(b) => Ok(num::clamp_bigint(b)),
        _ => {
            let s = value::to_s_string(v);
            match num::ruby_integer(&s) {
                Some(b) => Ok(num::clamp_bigint(&b)),
                None => Err(Error::argument("invalid integer")),
            }
        }
    }
}

/// `RangeLookup#to_integer`.
fn range_to_integer(v: &Value) -> Result<i64> {
    match v {
        Value::Int(i) => Ok(*i),
        Value::Nil => Ok(0),
        Value::Str(s) => Ok(num::ruby_str_to_i64(s)),
        _ => to_integer(v),
    }
}

/// `TableRow#to_integer`: `value.to_i`, failing for values without `to_i`.
fn tablerow_to_integer(v: &Value) -> Result<i64> {
    match v {
        Value::Int(i) => Ok(*i),
        Value::BigInt(b) => Ok(num::clamp_bigint(b)),
        Value::Float(f) => {
            if f.is_finite() {
                Ok(num::float_to_i64_saturating(*f))
            } else {
                Err(Error::new(ErrorKind::FloatDomain, value::format_float(*f)))
            }
        }
        Value::Str(s) => Ok(num::ruby_str_to_i64(s)),
        Value::Nil => Ok(0),
        _ => Err(Error::argument("invalid integer")),
    }
}

/// A key for Ruby-hash-like maps keyed by arbitrary values (cycle groups).
fn hash_key(v: &Value) -> String {
    match v {
        Value::Str(s) => format!("s:{}", s),
        Value::Int(i) => format!("i:{}", i),
        Value::BigInt(b) => format!("i:{}", b),
        Value::Float(f) => format!("f:{}", value::format_float(*f)),
        Value::Nil => "n".to_string(),
        Value::Bool(b) => format!("b:{}", b),
        Value::Drop(d) => format!("d:{:p}", Rc::as_ptr(d) as *const u8),
        other => format!("o:{}", inspect(other)),
    }
}

/// `Array#join` with no separator (nested arrays are flattened).
fn array_join(v: &Value, depth: usize) -> String {
    let mut s = String::new();
    if let Value::Array(a) = v {
        for item in a.iter() {
            match item {
                Value::Array(_) if depth < 64 => s.push_str(&array_join(item, depth + 1)),
                other => value::write_to_s(other, &mut s),
            }
        }
    }
    s
}

fn responds_to_comparison(v: &Value) -> bool {
    match v {
        Value::Int(_) | Value::BigInt(_) | Value::Float(_) | Value::Str(_) => true,
        Value::Drop(d) => d.is_comparable(),
        _ => false,
    }
}

/// Ruby's `rb_cmperr` description of the right-hand operand.
fn cmp_operand_desc(v: &Value) -> String {
    match v {
        Value::Int(i) => i.to_string(),
        Value::Float(f) => value::format_float(*f),
        Value::Nil => "nil".into(),
        Value::Bool(b) => b.to_string(),
        other => other.class_name().to_string(),
    }
}

fn compare_values(op: &CondOp, l: &Value, r: &Value) -> Result<bool> {
    if !(responds_to_comparison(l) && responds_to_comparison(r)) {
        return Ok(false);
    }
    let ord = match (l, r) {
        (Value::Str(a), Value::Str(b)) => Some(a.as_bytes().cmp(b.as_bytes())),
        _ if l.is_number() && r.is_number() => {
            match num_cmp(l, r) {
                Some(o) => Some(o),
                None => return Ok(false), // NaN
            }
        }
        (Value::Drop(d), _) => d.compare(r),
        _ => None,
    };
    let Some(ord) = ord else {
        return Err(Error::argument(format!(
            "comparison of {} with {} failed",
            l.class_name(),
            cmp_operand_desc(r)
        )));
    };
    Ok(match op {
        CondOp::Lt => ord == Ordering::Less,
        CondOp::Gt => ord == Ordering::Greater,
        CondOp::Le => ord != Ordering::Greater,
        CondOp::Ge => ord != Ordering::Less,
        _ => false,
    })
}

fn method_literal_matches(is_empty_lit: bool, v: &Operand) -> bool {
    match v {
        Operand::Val(v) => {
            if is_empty_lit {
                is_empty(v)
            } else {
                is_blank(v)
            }
        }
        _ => false,
    }
}

fn equal_operands(l: Operand, r: Operand) -> bool {
    match (l, r) {
        (Operand::Empty, other) | (other, Operand::Empty) => method_literal_matches(true, &other),
        (Operand::Blank, other) | (other, Operand::Blank) => method_literal_matches(false, &other),
        (Operand::Val(a), Operand::Val(b)) => ruby_eq(a, b),
    }
}

fn compare_operands(op: &CondOp, l: Operand, r: Operand) -> Result<bool> {
    match op {
        CondOp::Eq => Ok(equal_operands(l, r)),
        CondOp::Ne => Ok(!equal_operands(l, r)),
        CondOp::Contains => Ok(match (l, r) {
            (Operand::Val(a), r) => {
                if !a.is_truthy() {
                    return Ok(false);
                }
                match r {
                    Operand::Val(b) => {
                        if !b.is_truthy() {
                            return Ok(false);
                        }
                        contains(a, b)
                    }
                    // A method literal's to_s is "".
                    _ => matches!(a, Value::Str(_)),
                }
            }
            _ => false,
        }),
        _ => match (l, r) {
            (Operand::Val(a), Operand::Val(b)) => compare_values(op, a, b),
            _ => Ok(false),
        },
    }
}

fn contains(a: &Value, b: &Value) -> bool {
    match a {
        Value::Str(s) => {
            let needle = match b {
                Value::Str(t) => t.clone(),
                other => Rc::from(ruby_to_s(other)),
            };
            s.contains(&*needle)
        }
        Value::Array(items) => items.iter().any(|x| ruby_eq(x, b)),
        Value::Hash(h) => match b {
            Value::Str(k) => h.contains_key(k),
            _ => false,
        },
        Value::Range(x, y) => match b {
            Value::Int(_) | Value::BigInt(_) | Value::Float(_) => {
                num_cmp(&Value::Int(*x), b).is_some_and(|o| o != Ordering::Greater)
                    && num_cmp(b, &Value::Int(*y)).is_some_and(|o| o != Ordering::Greater)
            }
            _ => false,
        },
        _ => false,
    }
}

/// Ruby's `to_s` (not `Utils.to_s`): arrays and hashes use inspect.
pub(crate) fn ruby_to_s(v: &Value) -> String {
    match v {
        Value::Array(_) | Value::Hash(_) => inspect(v),
        _ => value::to_s_string(v),
    }
}
