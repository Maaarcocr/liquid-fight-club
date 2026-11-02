//! Expression trees, built from normalized markup (`Liquid::Expression.parse`).

use crate::bigint::BigInt;
use crate::error::{Error, Result};
use crate::lexer::is_space;
use crate::num;
use crate::value::{RcStr, Value};
use std::rc::Rc;

#[derive(Clone, Debug)]
pub enum Expr {
    Lit(Value),
    Var(Rc<VarLookup>),
    /// A range with at least one non-constant end (`RangeLookup`).
    Range(Rc<(Expr, Expr)>, RcStr),
    /// The `empty` / `blank` method literals, only produced in conditions.
    Empty,
    Blank,
}

#[derive(Clone, Debug)]
pub enum VarName {
    Ident(RcStr),
    Expr(Expr),
    /// No name could be scanned from the markup (evaluates to nil).
    None,
}

#[derive(Clone, Debug)]
pub enum Lookup {
    /// `.name` (a plain key).
    Key(RcStr),
    /// `.size`, `.first` or `.last` written without brackets.
    Command(RcStr),
    /// `[expr]`
    Index(Expr),
}

#[derive(Clone, Debug)]
pub struct VarLookup {
    pub name: VarName,
    pub lookups: Vec<Lookup>,
    /// Unique id per parsed occurrence (used for cycle grouping).
    pub id: u64,
}

impl Expr {
    pub fn is_constant(&self) -> bool {
        matches!(self, Expr::Lit(_))
    }

    pub fn as_lit(&self) -> Option<&Value> {
        match self {
            Expr::Lit(v) => Some(v),
            _ => None,
        }
    }
}

/// Ruby `String#strip` (whitespace and NUL).
pub fn ruby_strip(s: &str) -> &str {
    s.trim_matches(|c: char| c == '\0' || (c.is_ascii() && is_space(c as u8)))
}

pub fn ruby_lstrip(s: &str) -> &str {
    s.trim_start_matches(|c: char| c == '\0' || (c.is_ascii() && is_space(c as u8)))
}

pub fn ruby_rstrip(s: &str) -> &str {
    s.trim_end_matches(|c: char| c == '\0' || (c.is_ascii() && is_space(c as u8)))
}

thread_local! {
    static NEXT_ID: std::cell::Cell<u64> = const { std::cell::Cell::new(1) };
}

fn next_id() -> u64 {
    NEXT_ID.with(|c| {
        let v = c.get();
        c.set(v.wrapping_add(1));
        v
    })
}

const MAX_DEPTH: usize = 128;

/// `Liquid::Expression.parse`.
pub fn parse(markup: &str) -> Result<Expr> {
    parse_depth(markup, 0)
}

fn parse_depth(markup: &str, depth: usize) -> Result<Expr> {
    if depth > MAX_DEPTH {
        return Err(Error::nesting());
    }
    let m = ruby_strip(markup);
    let b = m.as_bytes();
    if !b.is_empty() && ((b[0] == b'"' && b[b.len() - 1] == b'"') || (b[0] == b'\'' && b[b.len() - 1] == b'\'')) {
        let inner = if m.len() >= 2 { &m[1..m.len() - 1] } else { "" };
        return Ok(Expr::Lit(Value::str(inner)));
    }
    match m {
        "" | "nil" | "null" => return Ok(Expr::Lit(Value::Nil)),
        "true" => return Ok(Expr::Lit(Value::Bool(true))),
        "false" => return Ok(Expr::Lit(Value::Bool(false))),
        "blank" | "empty" => return Ok(Expr::Lit(Value::str(""))),
        _ => {}
    }
    if b[0] == b'(' && b[b.len() - 1] == b')' {
        if let Some((s, e)) = match_range(m) {
            return parse_range(s, e, m, depth);
        }
    }
    if let Some(v) = parse_number(m) {
        return Ok(Expr::Lit(v));
    }
    parse_variable(m, depth)
}

/// `RANGES_REGEX = /\A\(\s*(?>(\S+)\s*\.\.)\s*(\S+)\s*\)\z/`
fn match_range(m: &str) -> Option<(&str, &str)> {
    let b = m.as_bytes();
    let n = b.len();
    let mut i = 1;
    while i < n && is_space(b[i]) {
        i += 1;
    }
    let start = i;
    let mut q = start;
    while q < n && !is_space(b[q]) {
        q += 1;
    }
    if q == start {
        return None;
    }
    // Atomic group: the longest \S+ followed by \s*".." wins.
    let mut found = None;
    let mut e = q;
    while e > start {
        let mut k = e;
        if e == q {
            while k < n && is_space(b[k]) {
                k += 1;
            }
        }
        if k + 1 < n && b[k] == b'.' && b[k + 1] == b'.' {
            found = Some((e, k + 2));
            break;
        }
        e -= 1;
    }
    let (end1, mut j) = found?;
    while j < n && is_space(b[j]) {
        j += 1;
    }
    // (\S+)\s*\)\z
    if n == 0 || b[n - 1] != b')' || j >= n - 1 {
        return None;
    }
    let mut f = n - 1;
    while f > j && is_space(b[f - 1]) {
        f -= 1;
    }
    if f <= j {
        return None;
    }
    if b[j..f].iter().any(|&c| is_space(c)) {
        return None;
    }
    Some((&m[start..end1], &m[j..f]))
}

fn parse_range(s: &str, e: &str, markup: &str, depth: usize) -> Result<Expr> {
    let a = parse_depth(s, depth + 1)?;
    let b = parse_depth(e, depth + 1)?;
    if a.is_constant() && b.is_constant() {
        let x = const_to_i(a.as_lit().unwrap(), s)?;
        let y = const_to_i(b.as_lit().unwrap(), e)?;
        return Ok(Expr::Lit(Value::Range(x, y)));
    }
    Ok(Expr::Range(Rc::new((a, b)), Rc::from(markup)))
}

/// `obj.to_i` for constant range ends.
fn const_to_i(v: &Value, markup: &str) -> Result<i64> {
    match v {
        Value::Int(i) => Ok(*i),
        Value::BigInt(b) => Ok(if b.is_negative() { i64::MIN } else { i64::MAX }),
        Value::Float(f) => Ok(num::float_to_i64_saturating(*f)),
        Value::Str(s) => Ok(num::clamp_bigint(&num::ruby_str_to_i(s))),
        Value::Nil => Ok(0),
        _ => Err(Error::syntax(format!("Invalid expression type '{}' in range expression", markup))),
    }
}

/// `Expression.parse_number`.
fn parse_number(m: &str) -> Option<Value> {
    let b = m.as_bytes();
    let (neg, digits) = if b[0] == b'-' { (true, &b[1..]) } else { (false, b) };
    let _ = neg;
    if digits.is_empty() || !digits[0].is_ascii_digit() {
        return None;
    }
    if digits.iter().all(|c| c.is_ascii_digit()) {
        return Some(match m.parse::<i64>() {
            Ok(i) => Value::Int(i),
            Err(_) => Value::from_bigint(BigInt::parse(m)?),
        });
    }
    if !digits.iter().all(|&c| c.is_ascii_digit() || c == b'.') {
        return None;
    }
    Some(Value::Float(num::ruby_str_to_f(m)))
}

/// `VariableLookup.new(markup)`: scans `VariableParser` matches.
fn parse_variable(m: &str, depth: usize) -> Result<Expr> {
    let parts = scan_variable_parts(m);
    let mut iter = parts.into_iter();
    let name = match iter.next() {
        None => VarName::None,
        Some(p) => {
            if p.starts_with('[') && p.ends_with(']') {
                VarName::Expr(parse_depth(&p[1..p.len() - 1], depth + 1)?)
            } else {
                VarName::Ident(Rc::from(p))
            }
        }
    };
    let mut lookups = Vec::new();
    for p in iter {
        if p.starts_with('[') && p.ends_with(']') {
            lookups.push(Lookup::Index(parse_depth(&p[1..p.len() - 1], depth + 1)?));
        } else if p == "size" || p == "first" || p == "last" {
            lookups.push(Lookup::Command(Rc::from(p)));
        } else {
            lookups.push(Lookup::Key(Rc::from(p)));
        }
    }
    Ok(Expr::Var(Rc::new(VarLookup { name, lookups, id: next_id() })))
}

/// `markup.scan(/\[(?>[^\[\]]+|\g<0>)*\]|[\w-]+\??/)`
fn scan_variable_parts(m: &str) -> Vec<&str> {
    let b = m.as_bytes();
    let n = b.len();
    let mut out = Vec::new();
    let mut i = 0;
    while i < n {
        let c = b[i];
        if c == b'[' {
            let mut depth = 0usize;
            let mut j = i;
            let mut end = None;
            while j < n {
                match b[j] {
                    b'[' => depth += 1,
                    b']' => {
                        depth -= 1;
                        if depth == 0 {
                            end = Some(j + 1);
                            break;
                        }
                    }
                    _ => {}
                }
                j += 1;
            }
            if let Some(e) = end {
                out.push(&m[i..e]);
                i = e;
                continue;
            }
        } else if c.is_ascii_alphanumeric() || c == b'_' || c == b'-' {
            let mut j = i;
            while j < n && (b[j].is_ascii_alphanumeric() || b[j] == b'_' || b[j] == b'-') {
                j += 1;
            }
            if j < n && b[j] == b'?' {
                j += 1;
            }
            out.push(&m[i..j]);
            i = j;
            continue;
        }
        // Advance one character.
        i += 1;
        while i < n && !m.is_char_boundary(i) {
            i += 1;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ranges() {
        assert_eq!(match_range("(1..5)"), Some(("1", "5")));
        assert_eq!(match_range("(a.b..c)"), Some(("a.b", "c")));
        assert_eq!(match_range("( 1 .. 5 )"), Some(("1", "5")));
        assert_eq!(match_range("('a b'..2)"), None);
        assert_eq!(match_range("((1..2)..3)"), Some(("(1..2)", "3")));
    }

    #[test]
    fn scan() {
        assert_eq!(scan_variable_parts("a.b[\"c\"].d?"), vec!["a", "b", "[\"c\"]", "d?"]);
        assert_eq!(scan_variable_parts("a[\"]\"]"), vec!["a", "[\"]"]);
        assert_eq!(scan_variable_parts("a[b[c]]"), vec!["a", "[b[c]]"]);
    }
}
