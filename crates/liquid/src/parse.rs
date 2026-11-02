//! Template tokenizer and parser (strict2 mode).
//!
//! The structure follows Ruby Liquid closely: a tokenizer splits the source
//! into text, `{{ }}` and `{% %}` tokens; block bodies consume tokens and hand
//! unknown tags back to the enclosing block, which decides whether they are
//! delimiters (`endif`), intermediate tags (`else`) or errors.

use crate::error::{Error, Result};
use crate::expression::{self, ruby_lstrip, ruby_rstrip, ruby_strip, Expr};
use crate::lexer::{is_space, is_word, Parser, Tok};
use crate::value::{inspect, RcStr};
use std::rc::Rc;

/// Maximum block nesting (`Liquid::Block::MAX_DEPTH`).
pub const MAX_DEPTH: usize = 100;

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------

#[derive(Debug, Default)]
pub struct Body {
    pub nodes: Vec<Node>,
    pub blank: bool,
}

impl Body {
    fn new() -> Body {
        Body { nodes: Vec::new(), blank: true }
    }

    fn remove_blank_strings(&mut self) {
        self.nodes.retain(|n| !matches!(n, Node::Text(_)));
    }
}

#[derive(Debug)]
pub enum Node {
    Text(RcStr),
    Output(Box<Variable>),
    Tag(Box<Tag>),
}

#[derive(Debug)]
pub struct FilterCall {
    pub name: RcStr,
    pub args: Vec<Expr>,
    pub kwargs: Option<Vec<(RcStr, Expr)>>,
}

#[derive(Debug)]
pub struct Variable {
    pub expr: Option<Expr>,
    pub filters: Vec<FilterCall>,
    pub line: Option<u32>,
}

#[derive(Debug)]
pub struct Tag {
    pub line: Option<u32>,
    pub kind: TagKind,
}

#[derive(Debug)]
pub enum CondOp {
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Contains,
}

#[derive(Debug)]
pub struct Condition {
    pub left: Expr,
    pub op: Option<(CondOp, Expr)>,
    /// `and` (true) / `or` (false) chained condition.
    pub child: Option<(bool, Box<Condition>)>,
}

#[derive(Debug)]
pub enum CaseArm {
    When(Expr, usize),
    Else(usize),
}

#[derive(Debug)]
pub enum CycleKey {
    Named(Expr),
    Unnamed(RcStr),
}

#[derive(Debug)]
pub struct ForTag {
    pub var: RcStr,
    pub collection: Expr,
    pub name: RcStr,
    pub reversed: bool,
    pub limit: Option<Expr>,
    /// None: no offset; Some(None): `offset: continue`.
    pub offset: Option<Option<Expr>>,
    pub body: Body,
    pub else_body: Option<Body>,
}

#[derive(Debug)]
pub struct TableRowTag {
    pub var: RcStr,
    pub collection: Expr,
    pub attributes: Vec<(RcStr, Expr)>,
    pub body: Body,
}

#[derive(Debug)]
pub struct PartialTag {
    pub template: Expr,
    pub variable: Option<Expr>,
    pub alias: Option<RcStr>,
    pub is_for: bool,
    pub attributes: Vec<(RcStr, Expr)>,
}

#[derive(Debug)]
pub enum TagKind {
    Assign { to: RcStr, from: Variable },
    Capture { to: RcStr, body: Body },
    Cycle { key: CycleKey, values: Vec<Expr> },
    Echo(Variable),
    Increment(RcStr),
    Decrement(RcStr),
    Raw(RcStr),
    Comment,
    Doc { blank: bool },
    InlineComment,
    If { unless: bool, blocks: Vec<(Option<Condition>, Body)>, blank: bool },
    Case { left: Expr, arms: Vec<CaseArm>, bodies: Vec<Body>, blank: bool },
    For(Box<ForTag>, bool),
    TableRow(Box<TableRowTag>, bool),
    Break,
    Continue,
    IfChanged(Body, bool),
    Include(Box<PartialTag>),
    Render(Box<PartialTag>),
}

impl Tag {
    pub fn blank(&self) -> bool {
        match &self.kind {
            TagKind::Assign { .. } | TagKind::Capture { .. } | TagKind::Comment | TagKind::InlineComment => true,
            TagKind::Raw(s) => s.is_empty(),
            TagKind::Doc { blank } => *blank,
            TagKind::If { blank, .. } | TagKind::Case { blank, .. } => *blank,
            TagKind::For(_, b) | TagKind::TableRow(_, b) | TagKind::IfChanged(_, b) => *b,
            _ => false,
        }
    }
}

/// A parsed template.
#[derive(Debug)]
pub struct Template {
    pub(crate) root: Body,
    /// The parser's pending whitespace-trim flag after the last token. Ruby
    /// keeps this in the parse context, so it leaks into partials parsed later.
    pub(crate) trailing_trim: bool,
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

struct Tokenizer<'s> {
    tokens: Vec<&'s str>,
    offset: usize,
    line: Option<u32>,
    for_liquid_tag: bool,
}

impl<'s> Tokenizer<'s> {
    fn document(source: &'s str, line_numbers: bool) -> Tokenizer<'s> {
        Tokenizer { tokens: tokenize(source), offset: 0, line: if line_numbers { Some(1) } else { None }, for_liquid_tag: false }
    }

    fn liquid(markup: &'s str, line: Option<u32>) -> Tokenizer<'s> {
        let mut tokens: Vec<&str> = markup.split('\n').collect();
        while tokens.last() == Some(&"") {
            tokens.pop();
        }
        Tokenizer { tokens, offset: 0, line, for_liquid_tag: true }
    }

    fn shift(&mut self) -> Option<&'s str> {
        let t = *self.tokens.get(self.offset)?;
        self.offset += 1;
        if let Some(l) = self.line {
            let add = if self.for_liquid_tag { 1 } else { t.bytes().filter(|&b| b == b'\n').count() as u32 };
            self.line = Some(l.saturating_add(add));
        }
        Some(t)
    }
}

/// Splits a template into tokens exactly like `Liquid::Tokenizer`.
fn tokenize(src: &str) -> Vec<&str> {
    let b = src.as_bytes();
    let n = b.len();
    let mut out = Vec::new();
    let mut pos = 0;
    while pos < n {
        if b[pos] == b'{' && pos + 1 < n && (b[pos + 1] == b'%' || b[pos + 1] == b'{') {
            let start = pos;
            if b[pos + 1] == b'%' {
                match find(b, pos + 2, b"%}") {
                    Some(e) => {
                        out.push(&src[start..e + 2]);
                        pos = e + 2;
                    }
                    None => {
                        out.push("{%");
                        pos += 2;
                    }
                }
            } else {
                let (tok, newpos) = next_variable_token(src, start);
                out.push(tok);
                pos = newpos;
            }
        } else {
            let start = pos;
            let mut i = pos;
            let mut found = None;
            while i + 1 < n {
                if b[i] == b'{' && (b[i + 1] == b'{' || b[i + 1] == b'%') {
                    found = Some(i);
                    break;
                }
                i += 1;
            }
            match found {
                Some(e) => {
                    out.push(&src[start..e]);
                    pos = e;
                }
                None => {
                    out.push(&src[start..]);
                    pos = n;
                }
            }
        }
    }
    out
}

fn find(b: &[u8], from: usize, pat: &[u8]) -> Option<usize> {
    if from > b.len() {
        return None;
    }
    b[from..].windows(pat.len()).position(|w| w == pat).map(|p| p + from)
}

/// `Tokenizer#next_variable_token`. `start` points at `{{`.
fn next_variable_token(src: &str, start: usize) -> (&str, usize) {
    let b = src.as_bytes();
    let n = b.len();
    let mut pos = start + 2;
    let scan = |pos: &mut usize| -> Option<u8> {
        if *pos < n {
            let c = b[*pos];
            *pos += 1;
            Some(c)
        } else {
            None
        }
    };
    let mut byte_a = scan(&mut pos);
    let mut byte_b = byte_a;
    while byte_b.is_some() {
        while let Some(a) = byte_a {
            if a == b'}' || a == b'{' {
                break;
            }
            byte_a = scan(&mut pos);
        }
        let Some(a) = byte_a else { break };
        if pos >= n {
            return if a == b'}' { (&src[start..pos], pos) } else { ("{{", n) };
        }
        byte_b = scan(&mut pos);
        if a == b'}' {
            if byte_b == Some(b'}') {
                return (&src[start..pos], pos);
            }
            pos -= 1;
            return (&src[start..pos], pos);
        } else if a == b'{' && byte_b == Some(b'%') {
            // next_tag_token_with_start
            return match find(b, pos, b"%}") {
                Some(e) => (&src[start..e + 2], e + 2),
                None => (&src[start..pos], pos),
            };
        }
        byte_a = byte_b;
    }
    ("{{", n)
}

// ---------------------------------------------------------------------------
// Token regexes
// ---------------------------------------------------------------------------

/// `FullToken = /\A\{\%-?(\s*)(#|\w+)(\s*)(.*?)-?\%\}\z/m`
/// Returns (leading ws, tag name, trailing ws, markup).
fn full_token(t: &str) -> Option<(&str, &str, &str, &str)> {
    let b = t.as_bytes();
    let n = b.len();
    if n < 4 || !t.starts_with("{%") || !t.ends_with("%}") {
        return None;
    }
    let end = n - 2;
    let try_from = |mut i: usize| -> Option<(&str, &str, &str, &str)> {
        let ws1s = i;
        while i < end && is_space(b[i]) {
            i += 1;
        }
        let ws1 = &t[ws1s..i];
        let ns = i;
        if i < end && b[i] == b'#' {
            i += 1;
        } else {
            while i < end && is_word(b[i]) {
                i += 1;
            }
        }
        if i == ns {
            return None;
        }
        let name = &t[ns..i];
        let ws2s = i;
        while i < end && is_space(b[i]) {
            i += 1;
        }
        let ws2 = &t[ws2s..i];
        let mut mend = end;
        if mend > i && b[mend - 1] == b'-' {
            mend -= 1;
        }
        Some((ws1, name, ws2, &t[i..mend]))
    };
    if b[2] == b'-' {
        if let Some(r) = try_from(3) {
            return Some(r);
        }
    }
    try_from(2)
}

/// `FullTokenPossiblyInvalid = /\A(.*)\{\%-?\s*(\w+)\s*(.*)?-?\%\}\z/m`
/// Returns (prefix, tag name).
fn full_token_possibly_invalid(t: &str) -> Option<(&str, &str)> {
    let b = t.as_bytes();
    let n = b.len();
    if n < 4 || !t.ends_with("%}") {
        return None;
    }
    let end = n - 2;
    let mut p = end;
    while p > 0 {
        p -= 1;
        if b[p] == b'{' && p + 1 < n && b[p + 1] == b'%' && p + 2 <= end {
            let mut i = p + 2;
            if i < end && b[i] == b'-' {
                i += 1;
            }
            while i < end && is_space(b[i]) {
                i += 1;
            }
            let ns = i;
            while i < end && is_word(b[i]) {
                i += 1;
            }
            if i > ns {
                return Some((&t[..p], &t[ns..i]));
            }
        }
    }
    None
}

/// `LiquidTagToken = /\A\s*(#|\w+)\s*(.*?)\z/`
fn liquid_tag_token(t: &str) -> Option<(&str, &str)> {
    let b = t.as_bytes();
    let mut i = 0;
    while i < b.len() && is_space(b[i]) {
        i += 1;
    }
    let ns = i;
    if i < b.len() && b[i] == b'#' {
        i += 1;
    } else {
        while i < b.len() && is_word(b[i]) {
            i += 1;
        }
    }
    if i == ns {
        return None;
    }
    let name = &t[ns..i];
    while i < b.len() && is_space(b[i]) {
        i += 1;
    }
    Some((name, &t[i..]))
}

/// `/\A\s*\z/`
fn whitespace_or_nothing(s: &str) -> bool {
    s.bytes().all(is_space)
}

fn count_nl(s: &str) -> u32 {
    s.bytes().filter(|&b| b == b'\n').count() as u32
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

fn unknown_tag_error(tag: &str, block_name: &str, block_delimiter: &str) -> Error {
    if tag == "else" {
        Error::syntax(format!("{} tag does not expect 'else' tag", block_name))
    } else if tag.starts_with("end") {
        Error::syntax(format!(
            "'{}' is not a valid delimiter for {} tags. use {}",
            tag, block_name, block_delimiter
        ))
    } else {
        Error::syntax(format!("Unknown tag '{}'", tag))
    }
}

fn never_closed(block_name: &str) -> Error {
    Error::syntax(format!("'{}' tag was never closed", block_name))
}

const KNOWN_TAGS: &[&str] = &[
    "cycle", "render", "raw", "comment", "increment", "unless", "decrement", "capture", "continue", "include", "case",
    "ifchanged", "assign", "for", "#", "break", "if", "echo", "tablerow", "doc",
];

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// End of a block body: None at end of input, or an unhandled tag.
type BodyEnd = Option<(String, String)>;

pub(crate) struct TemplateParser {
    line: Option<u32>,
    trim_whitespace: bool,
    depth: usize,
}

/// Parses a template source in strict2 mode. `initial_trim` is the pending
/// trim flag inherited from earlier parses sharing the same parse context.
pub(crate) fn parse_template(source: &str, line_numbers: bool, initial_trim: bool) -> Result<Template> {
    let mut p = TemplateParser { line: None, trim_whitespace: initial_trim, depth: 0 };
    let mut tok = Tokenizer::document(source, line_numbers);
    let mut body = Body::new();
    let r = (|| -> Result<()> {
        loop {
            match p.parse_body(&mut tok, &mut body)? {
                None => return Ok(()),
                Some((name, _markup)) => {
                    return Err(match name.as_str() {
                        "else" | "end" => Error::syntax(format!("Unexpected outer '{}' tag", name)),
                        _ => Error::syntax(format!("Unknown tag '{}'", name)),
                    });
                }
            }
        }
    })();
    match r {
        Ok(()) => Ok(Template { root: body, trailing_trim: p.trim_whitespace }),
        Err(mut e) => {
            if e.is_syntax() && e.line.is_none() {
                e.line = p.line;
            }
            Err(e)
        }
    }
}

impl TemplateParser {
    fn parse_body(&mut self, tok: &mut Tokenizer, body: &mut Body) -> Result<BodyEnd> {
        self.line = tok.line;
        if tok.for_liquid_tag { self.parse_for_liquid_tag(tok, body) } else { self.parse_for_document(tok, body) }
    }

    /// `Block#parse_body`: parses one body and returns the unhandled tag that
    /// ended it, or None when the block's own delimiter was reached.
    fn parse_block_body(
        &mut self,
        tok: &mut Tokenizer,
        body: &mut Body,
        block_name: &str,
        blank: &mut bool,
    ) -> Result<BodyEnd> {
        if self.depth >= MAX_DEPTH {
            return Err(Error::nesting());
        }
        self.depth += 1;
        let r = self.parse_body(tok, body);
        self.depth -= 1;
        let end = r?;
        *blank &= body.blank;
        match end {
            None => Err(never_closed(block_name)),
            Some((name, _)) if name.len() == block_name.len() + 3 && name.starts_with("end") && &name[3..] == block_name => {
                Ok(None)
            }
            Some(e) => Ok(Some(e)),
        }
    }

    fn whitespace_handler(&mut self, token: &str, body: &mut Body) {
        let b = token.as_bytes();
        if b.len() > 2 && b[2] == b'-' {
            if let Some(Node::Text(prev)) = body.nodes.last_mut() {
                let trimmed = ruby_rstrip(prev);
                if trimmed.len() != prev.len() {
                    *prev = Rc::from(trimmed);
                }
            }
        }
        self.trim_whitespace = b.len() >= 3 && b[b.len() - 3] == b'-';
    }

    fn parse_for_document(&mut self, tok: &mut Tokenizer, body: &mut Body) -> Result<BodyEnd> {
        while let Some(token) = tok.shift() {
            if token.is_empty() {
                continue;
            }
            if token.starts_with("{%") {
                self.whitespace_handler(token, body);
                let Some((ws1, name, ws2, markup)) = full_token(token) else {
                    if token.ends_with("%}") {
                        return Ok(Some((token.to_string(), token.to_string())));
                    }
                    return Err(Error::syntax(format!(
                        "Tag '{}' was not properly terminated with regexp: /\\%\\}}/",
                        token
                    )));
                };
                if let Some(l) = self.line {
                    self.line = Some(l + count_nl(ws1) + count_nl(ws2));
                }
                if name == "liquid" {
                    self.parse_liquid_tag(markup, body)?;
                    continue;
                }
                if !KNOWN_TAGS.contains(&name) {
                    return Ok(Some((name.to_string(), markup.to_string())));
                }
                let tag = self.parse_tag(name, markup, tok)?;
                body.blank &= tag.blank();
                body.nodes.push(Node::Tag(Box::new(tag)));
            } else if token.starts_with("{{") {
                self.whitespace_handler(token, body);
                let var = self.create_variable(token)?;
                body.nodes.push(Node::Output(Box::new(var)));
                body.blank = false;
            } else {
                let text = if self.trim_whitespace { ruby_lstrip(token) } else { token };
                self.trim_whitespace = false;
                body.blank &= whitespace_or_nothing(text);
                body.nodes.push(Node::Text(Rc::from(text)));
            }
            self.line = tok.line;
        }
        Ok(None)
    }

    fn parse_for_liquid_tag(&mut self, tok: &mut Tokenizer, body: &mut Body) -> Result<BodyEnd> {
        while let Some(token) = tok.shift() {
            if !(token.is_empty() || whitespace_or_nothing(token)) {
                let Some((name, markup)) = liquid_tag_token(token) else {
                    return Ok(Some((token.to_string(), token.to_string())));
                };
                if name == "liquid" {
                    self.line = self.line.map(|l| l.saturating_sub(1));
                    self.parse_liquid_tag(markup, body)?;
                    continue;
                }
                if !KNOWN_TAGS.contains(&name) {
                    return Ok(Some((name.to_string(), markup.to_string())));
                }
                let tag = self.parse_tag(name, markup, tok)?;
                body.blank &= tag.blank();
                body.nodes.push(Node::Tag(Box::new(tag)));
            }
            self.line = tok.line;
        }
        Ok(None)
    }

    fn parse_liquid_tag(&mut self, markup: &str, body: &mut Body) -> Result<()> {
        if self.depth >= MAX_DEPTH {
            return Err(Error::nesting());
        }
        let mut lt = Tokenizer::liquid(markup, self.line);
        self.depth += 1;
        let r = self.parse_for_liquid_tag(&mut lt, body);
        self.depth -= 1;
        if let Some((name, _)) = r? {
            return Err(unknown_tag_error(&name, "liquid", "%}"));
        }
        Ok(())
    }

    fn create_variable(&mut self, token: &str) -> Result<Variable> {
        let b = token.as_bytes();
        if token.ends_with("}}") && b.len() >= 4 {
            let i: isize = if b[2] == b'-' { 3 } else { 2 };
            let mut parse_end: isize = b.len() as isize - 3;
            if b[parse_end as usize] == b'-' {
                parse_end -= 1;
            }
            let markup_end = parse_end - i + 1;
            let markup = if markup_end <= 0 { "" } else { &token[i as usize..(i + markup_end) as usize] };
            return self.variable(markup, format!("in \"{{{{{}}}}}\"", markup));
        }
        Err(Error::syntax(format!("Variable '{}' was not properly terminated with regexp: /\\}}\\}}/", token)))
    }

    /// Parses `markup` as an output expression with filters (`Variable.new`).
    fn variable(&mut self, markup: &str, context: String) -> Result<Variable> {
        let line = self.line;
        parse_variable(markup, line).map_err(|mut e| {
            if e.is_syntax() {
                e.line = line;
                e.markup_context = Some(context);
            }
            e
        })
    }

    /// Wraps markup parse errors with the tag's line and markup context.
    fn with_context<T>(&self, line: Option<u32>, markup: &str, r: Result<T>) -> Result<T> {
        r.map_err(|mut e| {
            if e.is_syntax() {
                e.line = line;
                e.markup_context = Some(format!("in \"{}\"", ruby_strip(markup)));
            }
            e
        })
    }

    fn parse_tag(&mut self, name: &str, markup: &str, tok: &mut Tokenizer) -> Result<Tag> {
        let line = self.line;
        let kind = match name {
            "assign" => {
                let r = parse_assign(markup, line);
                let (to, from) = self.with_context(line, markup, r)?;
                TagKind::Assign { to, from }
            }
            "capture" => {
                let r = (|| {
                    let s = ruby_strip(markup);
                    let mut p = Parser::new(s)?;
                    let to = p.consume(Tok::Id)?;
                    p.consume(Tok::Eos)?;
                    Ok(Rc::from(to))
                })();
                let to: RcStr = self.with_context(line, markup, r)?;
                let mut body = Body::new();
                let mut blank = true;
                self.parse_simple_block(tok, &mut body, "capture", &mut blank)?;
                TagKind::Capture { to, body }
            }
            "cycle" => {
                let r = parse_cycle(markup);
                let (key, values) = self.with_context(line, markup, r)?;
                TagKind::Cycle { key, values }
            }
            "echo" => {
                let v = self.variable(markup, format!("in \"{{{{{}}}}}\"", markup))?;
                TagKind::Echo(v)
            }
            "increment" | "decrement" => {
                let r = (|| {
                    let s = ruby_strip(markup);
                    let mut p = Parser::new(s)?;
                    let to = p.consume(Tok::Id)?;
                    p.consume(Tok::Eos)?;
                    Ok(Rc::from(to))
                })();
                let v: RcStr = self.with_context(line, markup, r)?;
                if name == "increment" { TagKind::Increment(v) } else { TagKind::Decrement(v) }
            }
            "raw" => {
                if !whitespace_or_nothing(markup) {
                    return Err(Error::syntax("Syntax Error in 'raw' - Valid syntax: raw"));
                }
                let mut out = String::new();
                loop {
                    let Some(token) = tok.shift() else { return Err(never_closed("raw")) };
                    if let Some((prefix, tname)) = full_token_possibly_invalid(token) {
                        if tname == "endraw" {
                            let b = token.as_bytes();
                            self.trim_whitespace = b.len() >= 3 && b[b.len() - 3] == b'-';
                            out.push_str(prefix);
                            break;
                        }
                    }
                    out.push_str(token);
                }
                TagKind::Raw(Rc::from(out))
            }
            "doc" => {
                if !whitespace_or_nothing(markup) {
                    return Err(Error::syntax("Syntax Error in 'doc' - Valid syntax: {% doc %}{% enddoc %}"));
                }
                let mut empty = true;
                loop {
                    let Some(token) = tok.shift() else { return Err(never_closed("doc")) };
                    let tname = full_token_possibly_invalid(token);
                    if let Some((_, "doc")) = tname {
                        return Err(Error::syntax("Syntax Error in 'doc' - Nested doc tags are not allowed"));
                    }
                    if let Some((prefix, "enddoc")) = tname {
                        let b = token.as_bytes();
                        self.trim_whitespace = b.len() >= 3 && b[b.len() - 3] == b'-';
                        if !prefix.is_empty() {
                            empty = false;
                        }
                        break;
                    }
                    if !token.is_empty() {
                        empty = false;
                    }
                }
                TagKind::Doc { blank: empty }
            }
            "comment" => {
                self.parse_comment(tok)?;
                TagKind::Comment
            }
            "#" => {
                // /\n\s*[^#\s]/
                let b = markup.as_bytes();
                let mut bad = false;
                for (i, &c) in b.iter().enumerate() {
                    if c == b'\n' {
                        let mut j = i + 1;
                        while j < b.len() && is_space(b[j]) {
                            j += 1;
                        }
                        // \s* may backtrack, but any non-space non-# char after the
                        // newline run makes the pattern match.
                        if j < b.len() && b[j] != b'#' {
                            bad = true;
                            break;
                        }
                    }
                }
                if bad {
                    return Err(Error::syntax(
                        "Syntax error in tag '#' - Each line of comments must be prefixed by the '#' character",
                    ));
                }
                TagKind::InlineComment
            }
            "if" | "unless" => self.parse_if(name, markup, tok, line)?,
            "case" => self.parse_case(markup, tok, line)?,
            "for" => self.parse_for(markup, tok, line)?,
            "tablerow" => self.parse_tablerow(markup, tok, line)?,
            "break" => TagKind::Break,
            "continue" => TagKind::Continue,
            "ifchanged" => {
                let mut body = Body::new();
                let mut blank = true;
                self.parse_simple_block(tok, &mut body, "ifchanged", &mut blank)?;
                TagKind::IfChanged(body, blank)
            }
            "include" => {
                let r = parse_partial(markup, false);
                TagKind::Include(Box::new(self.with_context(line, markup, r)?))
            }
            "render" => {
                let r = parse_partial(markup, true);
                TagKind::Render(Box::new(self.with_context(line, markup, r)?))
            }
            _ => unreachable!("unknown tag {}", name),
        };
        Ok(Tag { line, kind })
    }

    /// `Block#parse` for blocks without intermediate tags.
    fn parse_simple_block(&mut self, tok: &mut Tokenizer, body: &mut Body, name: &str, blank: &mut bool) -> Result<()> {
        loop {
            match self.parse_block_body(tok, body, name, blank)? {
                None => return Ok(()),
                Some((t, _)) => return Err(unknown_tag_error(&t, name, &format!("end{}", name))),
            }
        }
    }

    fn parse_comment(&mut self, tok: &mut Tokenizer) -> Result<()> {
        if self.depth >= MAX_DEPTH {
            return Err(Error::nesting());
        }
        let mut depth = 1usize;
        while let Some(token) = tok.shift() {
            let tag_name = if tok.for_liquid_tag {
                if token.is_empty() || whitespace_or_nothing(token) {
                    continue;
                }
                match liquid_tag_token(token) {
                    Some((n, _)) => Some(n),
                    None => continue,
                }
            } else {
                full_token(token).map(|t| t.1)
            };
            match tag_name {
                Some("raw") => {
                    loop {
                        let Some(t) = tok.shift() else { return Err(never_closed("raw")) };
                        if let Some((_, "endraw")) = full_token_possibly_invalid(t) {
                            break;
                        }
                    }
                }
                Some("comment") => depth += 1,
                Some("endcomment") => depth -= 1,
                _ => {}
            }
            if depth == 0 {
                if !tok.for_liquid_tag {
                    let b = token.as_bytes();
                    self.trim_whitespace = b.len() >= 3 && b[b.len() - 3] == b'-';
                }
                return Ok(());
            }
        }
        Err(never_closed("comment"))
    }

    fn parse_if(&mut self, name: &str, markup: &str, tok: &mut Tokenizer, line: Option<u32>) -> Result<TagKind> {
        let first = parse_condition(markup);
        let first = self.with_context(line, markup, first)?;
        let mut blocks: Vec<(Option<Condition>, Body)> = vec![(Some(first), Body::new())];
        let mut blank = true;
        loop {
            let body = &mut blocks.last_mut().unwrap().1;
            match self.parse_block_body(tok, body, name, &mut blank)? {
                None => break,
                Some((t, m)) => match t.as_str() {
                    "elsif" => {
                        let c = parse_condition(&m);
                        let c = self.with_context(line, &m, c)?;
                        blocks.push((Some(c), Body::new()));
                    }
                    "else" => blocks.push((None, Body::new())),
                    _ => return Err(unknown_tag_error(&t, name, &format!("end{}", name))),
                },
            }
        }
        if blank {
            for (_, b) in blocks.iter_mut() {
                b.remove_blank_strings();
            }
        }
        Ok(TagKind::If { unless: name == "unless", blocks, blank })
    }

    fn parse_case(&mut self, markup: &str, tok: &mut Tokenizer, line: Option<u32>) -> Result<TagKind> {
        let r = (|| {
            let mut p = Parser::new(markup)?;
            let left = expression::parse(&p.expression()?)?;
            p.consume(Tok::Eos)?;
            Ok(left)
        })();
        let left = self.with_context(line, markup, r)?;
        let mut arms: Vec<CaseArm> = Vec::new();
        let mut bodies: Vec<Body> = Vec::new();
        let mut blank = true;
        let mut pre = Body::new();
        let mut current: Option<usize> = None;
        loop {
            let end = match current {
                None => self.parse_block_body(tok, &mut pre, "case", &mut blank)?,
                Some(i) => self.parse_block_body(tok, &mut bodies[i], "case", &mut blank)?,
            };
            match end {
                None => break,
                Some((t, m)) => match t.as_str() {
                    "when" => {
                        let idx = bodies.len();
                        bodies.push(Body::new());
                        let mut p = Parser::new(&m)?;
                        loop {
                            let e = condition_operand(&p.expression()?)?;
                            arms.push(CaseArm::When(e, idx));
                            if !(p.id_is("or") || p.consume_if(Tok::Comma).is_some()) {
                                break;
                            }
                        }
                        p.consume(Tok::Eos)?;
                        current = Some(idx);
                    }
                    "else" => {
                        if !ruby_strip(&m).is_empty() {
                            return Err(Error::syntax(
                                "Syntax Error in tag 'case' - Valid else condition: {% else %} (no parameters) ",
                            ));
                        }
                        let idx = bodies.len();
                        bodies.push(Body::new());
                        arms.push(CaseArm::Else(idx));
                        current = Some(idx);
                    }
                    _ => return Err(unknown_tag_error(&t, "case", "endcase")),
                },
            }
        }
        if blank {
            for b in bodies.iter_mut() {
                b.remove_blank_strings();
            }
        }
        Ok(TagKind::Case { left, arms, bodies, blank })
    }

    fn parse_for(&mut self, markup: &str, tok: &mut Tokenizer, line: Option<u32>) -> Result<TagKind> {
        let r = (|| {
            let mut p = Parser::new(markup)?;
            let var: RcStr = Rc::from(p.consume(Tok::Id)?);
            if !p.id_is("in") {
                return Err(Error::syntax("For loops require an 'in' clause"));
            }
            let coll_markup = p.expression()?;
            let collection = expression::parse(&coll_markup)?;
            let name: RcStr = Rc::from(format!("{}-{}", var, coll_markup));
            let reversed = p.id_is("reversed");
            let mut limit = None;
            let mut offset = None;
            while p.look(Tok::Comma) || p.look(Tok::Id) {
                p.consume_if(Tok::Comma);
                let attr = if p.id_is("limit") {
                    "limit"
                } else if p.id_is("offset") {
                    "offset"
                } else {
                    return Err(Error::syntax("Invalid attribute in for loop. Valid attributes are limit and offset"));
                };
                p.consume(Tok::Colon)?;
                let m = p.expression()?;
                if attr == "offset" {
                    offset = Some(if m == "continue" { None } else { Some(expression::parse(&m)?) });
                } else {
                    limit = Some(expression::parse(&m)?);
                }
            }
            p.consume(Tok::Eos)?;
            Ok((var, collection, name, reversed, limit, offset))
        })();
        let (var, collection, name, reversed, limit, offset) = self.with_context(line, markup, r)?;
        let mut body = Body::new();
        let mut else_body: Option<Body> = None;
        let mut blank = true;
        if let Some((t, _)) = self.parse_block_body(tok, &mut body, "for", &mut blank)? {
            if t != "else" {
                return Err(unknown_tag_error(&t, "for", "endfor"));
            }
            let mut eb = Body::new();
            let end = self.parse_block_body(tok, &mut eb, "for", &mut blank)?;
            else_body = Some(eb);
            if let Some((t2, _)) = end {
                if t2 != "else" {
                    return Err(unknown_tag_error(&t2, "for", "endfor"));
                }
                // A second else replaces the else block and ends the for tag.
                else_body = Some(Body::new());
            }
        }
        if blank {
            if let Some(eb) = else_body.as_mut() {
                eb.remove_blank_strings();
            }
            body.remove_blank_strings();
        }
        Ok(TagKind::For(Box::new(ForTag { var, collection, name, reversed, limit, offset, body, else_body }), blank))
    }

    fn parse_tablerow(&mut self, markup: &str, tok: &mut Tokenizer, line: Option<u32>) -> Result<TagKind> {
        let r = (|| {
            let mut p = Parser::new(markup)?;
            let var: RcStr = Rc::from(p.consume(Tok::Id)?);
            if !p.id_is("in") {
                return Err(Error::syntax("For loops require an 'in' clause"));
            }
            let collection = expression::parse(&p.expression()?)?;
            p.consume_if(Tok::Comma);
            let mut attributes: Vec<(RcStr, Expr)> = Vec::new();
            while p.look(Tok::Id) {
                let key = p.consume_any();
                if !matches!(key, "cols" | "limit" | "offset" | "range") {
                    return Err(Error::syntax(format!(
                        "Invalid attribute '{}' in tablerow loop. Valid attributes are cols, limit, offset, and range",
                        key
                    )));
                }
                p.consume(Tok::Colon)?;
                let e = expression::parse(&p.expression()?)?;
                if let Some(slot) = attributes.iter_mut().find(|(k, _)| &**k == key) {
                    slot.1 = e;
                } else {
                    attributes.push((Rc::from(key), e));
                }
                p.consume_if(Tok::Comma);
            }
            p.consume(Tok::Eos)?;
            Ok((var, collection, attributes))
        })();
        let (var, collection, attributes) = self.with_context(line, markup, r)?;
        let mut body = Body::new();
        let mut blank = true;
        self.parse_simple_block(tok, &mut body, "tablerow", &mut blank)?;
        Ok(TagKind::TableRow(Box::new(TableRowTag { var, collection, attributes, body }), blank))
    }
}

// ---------------------------------------------------------------------------
// Markup parsers
// ---------------------------------------------------------------------------

/// `Condition.parse_expression`: `empty`/`blank` become method literals.
fn condition_operand(markup: &str) -> Result<Expr> {
    match markup {
        "empty" => Ok(Expr::Empty),
        "blank" => Ok(Expr::Blank),
        _ => expression::parse(markup),
    }
}

fn parse_condition(markup: &str) -> Result<Condition> {
    let mut p = Parser::new(markup)?;
    // Parse a flat list then link it (right-associative evaluation).
    let mut items: Vec<(Condition, Option<bool>)> = Vec::new();
    loop {
        let left = condition_operand(&p.expression()?)?;
        let op = if let Some(op) = p.consume_if(Tok::Comparison) {
            let op = match op {
                "==" => CondOp::Eq,
                "!=" | "<>" => CondOp::Ne,
                "<" => CondOp::Lt,
                ">" => CondOp::Gt,
                "<=" => CondOp::Le,
                ">=" => CondOp::Ge,
                "contains" => CondOp::Contains,
                other => return Err(Error::argument(format!("Unknown operator {}", other))),
            };
            Some((op, condition_operand(&p.expression()?)?))
        } else {
            None
        };
        let rel = if p.id_is("and") {
            Some(true)
        } else if p.id_is("or") {
            Some(false)
        } else {
            None
        };
        items.push((Condition { left, op, child: None }, rel));
        if rel.is_none() {
            break;
        }
        if items.len() > 10_000 {
            return Err(Error::nesting());
        }
    }
    p.consume(Tok::Eos)?;
    let mut cond: Option<Condition> = None;
    while let Some((mut c, rel)) = items.pop() {
        if let (Some(child), Some(r)) = (cond.take(), rel) {
            c.child = Some((r, Box::new(child)));
        }
        cond = Some(c);
    }
    Ok(cond.expect("at least one condition"))
}

/// Strict2 output markup: `expr (| filter (: args)?)*`.
pub(crate) fn parse_variable(markup: &str, line: Option<u32>) -> Result<Variable> {
    let mut p = Parser::new(markup)?;
    if p.look(Tok::Eos) {
        return Ok(Variable { expr: None, filters: Vec::new(), line });
    }
    let expr = expression::parse(&p.expression()?)?;
    let mut filters = Vec::new();
    while p.consume_if(Tok::Pipe).is_some() {
        let name: RcStr = Rc::from(p.consume(Tok::Id)?);
        let mut args = Vec::new();
        let mut kwargs: Vec<(RcStr, Expr)> = Vec::new();
        let end_of_args = |p: &Parser| p.look(Tok::Pipe) || p.look(Tok::Eos);
        if p.consume_if(Tok::Colon).is_some() {
            if !end_of_args(&p) {
                filter_argument(&mut p, &mut args, &mut kwargs)?;
            }
            while p.consume_if(Tok::Comma).is_some() && !end_of_args(&p) {
                filter_argument(&mut p, &mut args, &mut kwargs)?;
            }
        }
        filters.push(FilterCall { name, args, kwargs: if kwargs.is_empty() { None } else { Some(kwargs) } });
    }
    p.consume(Tok::Eos)?;
    Ok(Variable { expr: Some(expr), filters, line })
}

fn filter_argument(p: &mut Parser, args: &mut Vec<Expr>, kwargs: &mut Vec<(RcStr, Expr)>) -> Result<()> {
    if p.look(Tok::Id) && p.look_ahead(Tok::Colon, 1) {
        let key = p.consume(Tok::Id)?;
        p.consume(Tok::Colon)?;
        let v = expression::parse(&p.expression()?)?;
        if let Some(slot) = kwargs.iter_mut().find(|(k, _)| &**k == key) {
            slot.1 = v;
        } else {
            kwargs.push((Rc::from(key), v));
        }
    } else {
        args.push(expression::parse(&p.expression()?)?);
    }
    Ok(())
}

/// `Assign::Syntax = /(#{VariableSignature}+)\s*=\s*(.*)\s*/om` (unanchored).
fn assign_syntax(markup: &str) -> Option<(&str, &str)> {
    let b = markup.as_bytes();
    let n = b.len();
    let sig = |c: u8| is_word(c) || c == b'-' || c == b'.' || c == b'[' || c == b']';
    let mut start = 0;
    while start < n {
        // Greedy run of VariableSignature units: \(?[sig]\)?
        let mut i = start;
        loop {
            let unit_start = i;
            let mut j = i;
            if j < n && b[j] == b'(' && j + 1 < n && sig(b[j + 1]) {
                j += 1;
            }
            if j < n && sig(b[j]) {
                j += 1;
                if j < n && b[j] == b')' {
                    j += 1;
                }
                i = j;
            } else {
                i = unit_start;
                break;
            }
        }
        if i > start {
            let mut k = i;
            while k < n && is_space(b[k]) {
                k += 1;
            }
            if k < n && b[k] == b'=' {
                k += 1;
                while k < n && is_space(b[k]) {
                    k += 1;
                }
                return Some((&markup[start..i], &markup[k..]));
            }
        }
        start += 1;
        while start < n && !markup.is_char_boundary(start) {
            start += 1;
        }
    }
    None
}

fn parse_assign(markup: &str, line: Option<u32>) -> Result<(RcStr, Variable)> {
    let Some((lhs, rhs)) = assign_syntax(markup) else {
        return Err(Error::syntax("Syntax Error in 'assign' - Valid syntax: assign [var] = [source]"));
    };
    let lhs = ruby_strip(lhs);
    let mut p = Parser::new(lhs)?;
    let to = p.consume(Tok::Id)?;
    p.consume(Tok::Eos)?;
    let from = parse_variable(rhs, line)?;
    Ok((Rc::from(to), from))
}

fn parse_cycle(markup: &str) -> Result<(CycleKey, Vec<Expr>)> {
    let mut p = Parser::new(markup)?;
    if p.look(Tok::Eos) {
        return Err(Error::syntax("Syntax Error in 'cycle' - Valid syntax: cycle [name :] var [, var2, var3 ...]"));
    }
    let first = expression::parse(&p.expression()?)?;
    let mut values = Vec::new();
    let mut name = None;
    if p.look(Tok::Colon) {
        name = Some(first);
        p.consume(Tok::Colon)?;
        values.push(expression::parse(&p.expression()?)?);
    } else {
        values.push(first);
    }
    while p.consume_if(Tok::Comma).is_some() {
        if p.look(Tok::Eos) {
            break;
        }
        values.push(expression::parse(&p.expression()?)?);
    }
    p.consume(Tok::Eos)?;
    let key = match name {
        Some(n) => CycleKey::Named(n),
        None => {
            // `@variables.to_s`: literals inspect; lookups are unique per tag.
            let mut s = String::from("[");
            for (i, v) in values.iter().enumerate() {
                if i > 0 {
                    s.push_str(", ");
                }
                match v {
                    Expr::Lit(val) => s.push_str(&inspect(val)),
                    Expr::Var(l) => s.push_str(&format!("#<Liquid::VariableLookup:{}>", l.id)),
                    Expr::Range(_, m) => s.push_str(&format!("#<Liquid::RangeLookup:{}>", m)),
                    Expr::Empty | Expr::Blank => s.push_str("#<MethodLiteral>"),
                }
            }
            s.push(']');
            CycleKey::Unnamed(Rc::from(s))
        }
    };
    Ok((key, values))
}

fn parse_partial(markup: &str, is_render: bool) -> Result<PartialTag> {
    let mut p = Parser::new(markup)?;
    let template = if is_render {
        let s = p.consume(Tok::Str)?;
        expression::parse(s)?
    } else {
        expression::parse(&p.expression()?)?
    };
    let with_or_for = if p.id_is("for") {
        Some(true)
    } else if p.id_is("with") {
        Some(false)
    } else {
        None
    };
    let variable = match with_or_for {
        Some(_) => Some(expression::parse(&p.expression()?)?),
        None => None,
    };
    let alias = if p.id_is("as") { Some(Rc::from(p.consume(Tok::Id)?)) } else { None };
    p.consume_if(Tok::Comma);
    let mut attributes: Vec<(RcStr, Expr)> = Vec::new();
    while p.look(Tok::Id) {
        let key = p.consume_any();
        p.consume(Tok::Colon)?;
        let v = expression::parse(&p.expression()?)?;
        if let Some(slot) = attributes.iter_mut().find(|(k, _)| &**k == key) {
            slot.1 = v;
        } else {
            attributes.push((Rc::from(key), v));
        }
        p.consume_if(Tok::Comma);
    }
    p.consume(Tok::Eos)?;
    Ok(PartialTag { template, variable, alias, is_for: with_or_for == Some(true), attributes })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens() {
        assert_eq!(tokenize("a{{b}}c{%d%}e"), vec!["a", "{{b}}", "c", "{%d%}", "e"]);
        assert_eq!(tokenize("{{ x } y"), vec!["{{ x }", " y"]);
        assert_eq!(tokenize("{% if x"), vec!["{%", " if x"]);
        assert_eq!(tokenize("{{ abc"), vec!["{{"]);
        assert_eq!(tokenize("{{ x {% y %}z"), vec!["{{ x {% y %}", "z"]);
    }

    #[test]
    fn full_tokens() {
        assert_eq!(full_token("{% if x -%}"), Some((" ", "if", " ", "x ")));
        assert_eq!(full_token("{%- if x--%}"), Some((" ", "if", " ", "x-")));
        assert_eq!(full_token("{%-%}"), None);
        assert_eq!(full_token("{%# hi %}"), Some(("", "#", " ", "hi ")));
        assert_eq!(full_token_possibly_invalid("{{ x {% endraw %}"), Some(("{{ x ", "endraw")));
    }

    #[test]
    fn assign_regex() {
        assert_eq!(assign_syntax("x = 5"), Some(("x", "5")));
        assert_eq!(assign_syntax("foo bar = 1"), Some(("bar", "1")));
        assert_eq!(assign_syntax("x == 5"), Some(("x", "= 5")));
        assert_eq!(assign_syntax(" = 5"), None);
    }
}
