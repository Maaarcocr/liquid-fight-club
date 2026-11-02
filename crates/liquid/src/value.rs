//! The Liquid value model.
//!
//! Every question Liquid asks about a value (how it prints, how it compares,
//! whether it is truthy, empty or blank) is answered here, following the
//! Ruby reference implementation's semantics.

use crate::bigint::BigInt;
use crate::drop::Drop;
use std::cell::OnceCell;
use std::cmp::Ordering;
use std::collections::HashMap;
use std::fmt::Write as _;
use std::rc::Rc;

pub type RcStr = Rc<str>;

#[derive(Clone)]
pub enum Value {
    Nil,
    Bool(bool),
    Int(i64),
    BigInt(Rc<BigInt>),
    Float(f64),
    Str(RcStr),
    Array(Rc<Vec<Value>>),
    Hash(Rc<Hash>),
    /// An inclusive integer range `(a..b)`.
    Range(i64, i64),
    Drop(Rc<dyn Drop>),
}

impl std::fmt::Debug for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&inspect(self))
    }
}

impl Default for Value {
    fn default() -> Self {
        Value::Nil
    }
}

impl From<&str> for Value {
    fn from(s: &str) -> Value {
        Value::Str(Rc::from(s))
    }
}

impl From<String> for Value {
    fn from(s: String) -> Value {
        Value::Str(Rc::from(s))
    }
}

impl From<i64> for Value {
    fn from(i: i64) -> Value {
        Value::Int(i)
    }
}

impl From<bool> for Value {
    fn from(b: bool) -> Value {
        Value::Bool(b)
    }
}

impl From<f64> for Value {
    fn from(f: f64) -> Value {
        Value::Float(f)
    }
}

impl From<Vec<Value>> for Value {
    fn from(v: Vec<Value>) -> Value {
        Value::Array(Rc::new(v))
    }
}

impl From<Hash> for Value {
    fn from(h: Hash) -> Value {
        Value::Hash(Rc::new(h))
    }
}

impl Value {
    pub fn str(s: &str) -> Value {
        Value::Str(Rc::from(s))
    }

    /// Builds an integer, normalizing big integers that fit in an i64.
    pub fn from_bigint(b: BigInt) -> Value {
        match b.to_i64() {
            Some(i) => Value::Int(i),
            None => Value::BigInt(Rc::new(b)),
        }
    }

    pub fn is_nil(&self) -> bool {
        matches!(self, Value::Nil)
    }

    /// Ruby truthiness: only nil and false are falsy.
    pub fn is_truthy(&self) -> bool {
        !matches!(self, Value::Nil | Value::Bool(false))
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_int(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i),
            _ => None,
        }
    }

    pub fn as_float(&self) -> Option<f64> {
        match self {
            Value::Float(f) => Some(*f),
            _ => None,
        }
    }

    pub fn is_integer(&self) -> bool {
        matches!(self, Value::Int(_) | Value::BigInt(_))
    }

    pub fn is_number(&self) -> bool {
        matches!(self, Value::Int(_) | Value::BigInt(_) | Value::Float(_))
    }

    /// The Ruby class name, used in error messages.
    pub fn class_name(&self) -> &str {
        match self {
            Value::Nil => "nil",
            Value::Bool(true) => "true",
            Value::Bool(false) => "false",
            Value::Int(_) | Value::BigInt(_) => "Integer",
            Value::Float(_) => "Float",
            Value::Str(_) => "String",
            Value::Array(_) => "Array",
            Value::Hash(_) => "Hash",
            Value::Range(..) => "Range",
            Value::Drop(d) => d.class_name(),
        }
    }

    /// `to_liquid_value`: lets drops stand in for primitive values.
    pub fn to_liquid_value(&self) -> Value {
        match self {
            Value::Drop(d) => match d.to_liquid_value() {
                Some(v) => v,
                None => self.clone(),
            },
            _ => self.clone(),
        }
    }

    pub fn range_len(a: i64, b: i64) -> usize {
        if b < a { 0 } else { (b as i128 - a as i128 + 1).min(usize::MAX as i128) as usize }
    }
}

// ---------------------------------------------------------------------------
// Ordered hash
// ---------------------------------------------------------------------------

/// An insertion-ordered map with string keys (Ruby Hash semantics for the
/// values Liquid can receive).
#[derive(Default)]
pub struct Hash {
    entries: Vec<(RcStr, Value)>,
    index: OnceCell<HashMap<RcStr, usize>>,
}

const INDEX_THRESHOLD: usize = 12;

impl Clone for Hash {
    fn clone(&self) -> Self {
        Hash { entries: self.entries.clone(), index: OnceCell::new() }
    }
}

impl Hash {
    pub fn new() -> Hash {
        Hash::default()
    }

    pub fn with_capacity(n: usize) -> Hash {
        Hash { entries: Vec::with_capacity(n), index: OnceCell::new() }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    fn position(&self, key: &str) -> Option<usize> {
        if self.entries.len() < INDEX_THRESHOLD {
            return self.entries.iter().position(|(k, _)| &**k == key);
        }
        let idx = self.index.get_or_init(|| {
            let mut m = HashMap::with_capacity(self.entries.len());
            for (i, (k, _)) in self.entries.iter().enumerate() {
                m.insert(k.clone(), i);
            }
            m
        });
        idx.get(key).copied()
    }

    pub fn get(&self, key: &str) -> Option<&Value> {
        self.position(key).map(|i| &self.entries[i].1)
    }

    pub fn contains_key(&self, key: &str) -> bool {
        self.position(key).is_some()
    }

    /// Inserts or replaces (keeping the original position, like Ruby).
    pub fn insert(&mut self, key: RcStr, value: Value) {
        if let Some(i) = self.position(&key) {
            self.entries[i].1 = value;
        } else {
            if let Some(idx) = self.index.get_mut() {
                idx.insert(key.clone(), self.entries.len());
            }
            self.entries.push((key, value));
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = (&RcStr, &Value)> {
        self.entries.iter().map(|(k, v)| (k, v))
    }

    pub fn first(&self) -> Option<(&RcStr, &Value)> {
        self.entries.first().map(|(k, v)| (k, v))
    }
}

// ---------------------------------------------------------------------------
// Number formatting (Ruby's Float#to_s)
// ---------------------------------------------------------------------------

/// Shortest round-trip decimal digits and decimal exponent of a finite,
/// non-zero float: value = 0.d1d2d3... * 10^decpt.
fn float_digits(f: f64) -> (String, i32) {
    let s = format!("{:e}", f.abs());
    let (mant, exp) = s.split_once('e').unwrap_or((&s, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    let digits: String = mant.chars().filter(|c| c.is_ascii_digit()).collect();
    let digits = digits.trim_end_matches('0');
    let digits = if digits.is_empty() { "0".to_string() } else { digits.to_string() };
    (digits, exp + 1)
}

/// Formats a float exactly like Ruby's `Float#to_s`.
pub fn format_float(f: f64) -> String {
    if f.is_nan() {
        return "NaN".into();
    }
    if f.is_infinite() {
        return if f > 0.0 { "Infinity".into() } else { "-Infinity".into() };
    }
    if f == 0.0 {
        return if f.is_sign_negative() { "-0.0".into() } else { "0.0".into() };
    }
    let (digits, decpt) = float_digits(f);
    let mut out = String::new();
    if f < 0.0 {
        out.push('-');
    }
    if !(-3..=15).contains(&decpt) {
        // Exponent form: d.ddde+XX
        out.push_str(&digits[..1]);
        out.push('.');
        if digits.len() > 1 {
            out.push_str(&digits[1..]);
        } else {
            out.push('0');
        }
        let e = decpt - 1;
        let _ = write!(out, "e{}{:02}", if e < 0 { '-' } else { '+' }, e.abs());
    } else if decpt <= 0 {
        out.push_str("0.");
        for _ in 0..(-decpt) {
            out.push('0');
        }
        out.push_str(&digits);
    } else {
        let d = decpt as usize;
        if d < digits.len() {
            out.push_str(&digits[..d]);
            out.push('.');
            out.push_str(&digits[d..]);
        } else {
            out.push_str(&digits);
            for _ in 0..(d - digits.len()) {
                out.push('0');
            }
            out.push_str(".0");
        }
    }
    out
}

// ---------------------------------------------------------------------------
// String conversion
// ---------------------------------------------------------------------------

/// `Liquid::Utils.to_s`: the string form used by filters and output.
pub fn to_s(v: &Value) -> RcStr {
    match v {
        Value::Str(s) => s.clone(),
        _ => Rc::from(to_s_string(v)),
    }
}

/// Like [`to_s`] but returns an owned String.
pub fn to_s_string(v: &Value) -> String {
    let mut out = String::new();
    write_to_s(v, &mut out);
    out
}

pub fn write_to_s(v: &Value, out: &mut String) {
    match v {
        Value::Nil => {}
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Int(i) => {
            let _ = write!(out, "{}", i);
        }
        Value::BigInt(b) => {
            let _ = write!(out, "{}", b);
        }
        Value::Float(f) => out.push_str(&format_float(*f)),
        Value::Str(s) => out.push_str(s),
        Value::Array(_) | Value::Hash(_) => write_inspect(v, out, 0),
        Value::Range(a, b) => {
            let _ = write!(out, "{}..{}", a, b);
        }
        Value::Drop(d) => out.push_str(&d.to_s()),
    }
}

/// Appends the value as `{{ value }}` renders it: arrays are flattened and
/// their elements rendered recursively, nil renders as nothing.
pub fn write_output(v: &Value, out: &mut String) {
    write_output_depth(v, out, 0)
}

fn write_output_depth(v: &Value, out: &mut String, depth: usize) {
    match v {
        Value::Nil => {}
        Value::Array(items) => {
            if depth > MAX_INSPECT_DEPTH {
                return;
            }
            for item in items.iter() {
                write_output_depth(item, out, depth + 1);
            }
        }
        _ => write_to_s(v, out),
    }
}

const MAX_INSPECT_DEPTH: usize = 256;

/// Ruby's `inspect`, as used for hashes and arrays converted to strings.
pub fn inspect(v: &Value) -> String {
    let mut out = String::new();
    write_inspect(v, &mut out, 0);
    out
}

pub fn write_inspect(v: &Value, out: &mut String, depth: usize) {
    if depth > MAX_INSPECT_DEPTH {
        out.push_str("...");
        return;
    }
    match v {
        Value::Nil => out.push_str("nil"),
        Value::Str(s) => write_string_inspect(s, out),
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push_str(", ");
                }
                write_inspect(item, out, depth + 1);
            }
            out.push(']');
        }
        Value::Hash(h) => {
            out.push('{');
            for (i, (k, val)) in h.iter().enumerate() {
                if i > 0 {
                    out.push_str(", ");
                }
                write_string_inspect(k, out);
                out.push_str("=>");
                write_inspect(val, out, depth + 1);
            }
            out.push('}');
        }
        Value::Drop(d) => out.push_str(&d.inspect()),
        _ => write_to_s(v, out),
    }
}

/// Ruby's `String#inspect` for UTF-8 strings.
pub fn write_string_inspect(s: &str, out: &mut String) {
    out.push('"');
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{c}' => out.push_str("\\f"),
            '\u{b}' => out.push_str("\\v"),
            '\u{8}' => out.push_str("\\b"),
            '\u{7}' => out.push_str("\\a"),
            '\u{1b}' => out.push_str("\\e"),
            '#' => {
                if matches!(chars.peek(), Some('{') | Some('$') | Some('@')) {
                    out.push_str("\\#");
                } else {
                    out.push('#');
                }
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => {
                let _ = write!(out, "\\u{:04X}", c as u32);
            }
            c if is_unprintable(c) => {
                let cp = c as u32;
                if cp > 0xFFFF {
                    let _ = write!(out, "\\u{{{:X}}}", cp);
                } else {
                    let _ = write!(out, "\\u{:04X}", cp);
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Approximation of Onigmo's "not printable" test for non-ASCII characters.
fn is_unprintable(c: char) -> bool {
    let cp = c as u32;
    (0x80..0xA0).contains(&cp)
        || cp == 0xAD
        || (0x2028..=0x2029).contains(&cp)
        || (0xD800..0xE000).contains(&cp)
        || (0xFFF0..=0xFFF8).contains(&cp)
        || cp == 0xFEFF
        || (0xE0000..0xE0080).contains(&cp)
}

// ---------------------------------------------------------------------------
// Equality and ordering
// ---------------------------------------------------------------------------

/// Compares two numbers exactly (no lossy int/float conversion).
pub fn num_cmp(a: &Value, b: &Value) -> Option<Ordering> {
    match (a, b) {
        (Value::Int(x), Value::Int(y)) => Some(x.cmp(y)),
        (Value::Float(x), Value::Float(y)) => x.partial_cmp(y),
        (Value::Int(x), Value::Float(y)) => int_float_cmp(*x as i128, *y),
        (Value::Float(x), Value::Int(y)) => int_float_cmp(*y as i128, *x).map(Ordering::reverse),
        (Value::BigInt(x), Value::BigInt(y)) => Some(x.as_ref().cmp(y)),
        (Value::BigInt(x), Value::Int(y)) => Some(x.as_ref().cmp(&BigInt::from_i64(*y))),
        (Value::Int(x), Value::BigInt(y)) => Some(BigInt::from_i64(*x).cmp(y)),
        (Value::BigInt(x), Value::Float(y)) => bigint_float_cmp(x, *y),
        (Value::Float(x), Value::BigInt(y)) => bigint_float_cmp(y, *x).map(Ordering::reverse),
        _ => None,
    }
}

fn int_float_cmp(i: i128, f: f64) -> Option<Ordering> {
    if f.is_nan() {
        return None;
    }
    if f >= 1.8e19 {
        return Some(Ordering::Less);
    }
    if f <= -1.8e19 {
        return Some(Ordering::Greater);
    }
    let fl = f.floor();
    let fi = fl as i128;
    match i.cmp(&fi) {
        Ordering::Equal => {
            if f > fl {
                Some(Ordering::Less)
            } else {
                Some(Ordering::Equal)
            }
        }
        o => Some(o),
    }
}

fn bigint_float_cmp(b: &BigInt, f: f64) -> Option<Ordering> {
    if f.is_nan() {
        return None;
    }
    if f.is_infinite() {
        return Some(if f > 0.0 { Ordering::Less } else { Ordering::Greater });
    }
    // Big integers are beyond i64; compare via the integer part of the float.
    let fl = f.floor();
    let fb = BigInt::parse(&format!("{:.0}", fl)).unwrap_or_else(BigInt::zero);
    match b.cmp(&fb) {
        Ordering::Equal => Some(if f > fl { Ordering::Less } else { Ordering::Equal }),
        o => Some(o),
    }
}

/// Ruby `==` between Liquid values.
pub fn ruby_eq(a: &Value, b: &Value) -> bool {
    ruby_eq_depth(a, b, 0)
}

fn ruby_eq_depth(a: &Value, b: &Value, depth: usize) -> bool {
    if depth > MAX_INSPECT_DEPTH {
        return false;
    }
    match (a, b) {
        (Value::Nil, Value::Nil) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Str(x), Value::Str(y)) => x == y,
        (Value::Range(a1, b1), Value::Range(a2, b2)) => a1 == a2 && b1 == b2,
        (Value::Array(x), Value::Array(y)) => {
            Rc::ptr_eq(x, y)
                || (x.len() == y.len() && x.iter().zip(y.iter()).all(|(p, q)| ruby_eq_depth(p, q, depth + 1)))
        }
        (Value::Hash(x), Value::Hash(y)) => {
            Rc::ptr_eq(x, y)
                || (x.len() == y.len()
                    && x.iter().all(|(k, v)| y.get(k).is_some_and(|w| ruby_eq_depth(v, w, depth + 1))))
        }
        (Value::Drop(x), Value::Drop(y)) => x.equals(b) || Rc::ptr_eq(x, y),
        (Value::Drop(x), _) => x.equals(b),
        (_, Value::Drop(y)) => {
            // Ruby dispatches `a == drop` on a; primitives are never equal to drops,
            // except a Comparable StringDrop on the right is still compared by its left.
            let _ = y;
            false
        }
        _ => {
            if a.is_number() && b.is_number() {
                num_cmp(a, b) == Some(Ordering::Equal)
            } else {
                false
            }
        }
    }
}

/// Ruby `<=>`; None when the values are not comparable.
pub fn ruby_cmp(a: &Value, b: &Value) -> Option<Ordering> {
    ruby_cmp_depth(a, b, 0)
}

fn ruby_cmp_depth(a: &Value, b: &Value, depth: usize) -> Option<Ordering> {
    if depth > MAX_INSPECT_DEPTH {
        return None;
    }
    match (a, b) {
        (Value::Str(x), Value::Str(y)) => Some(x.as_bytes().cmp(y.as_bytes())),
        (Value::Array(x), Value::Array(y)) => {
            for (p, q) in x.iter().zip(y.iter()) {
                match ruby_cmp_depth(p, q, depth + 1)? {
                    Ordering::Equal => continue,
                    o => return Some(o),
                }
            }
            Some(x.len().cmp(&y.len()))
        }
        (Value::Drop(d), _) => d.compare(b),
        _ if a.is_number() && b.is_number() => num_cmp(a, b),
        _ => {
            if ruby_eq(a, b) {
                Some(Ordering::Equal)
            } else {
                None
            }
        }
    }
}

// ---------------------------------------------------------------------------
// empty / blank
// ---------------------------------------------------------------------------

/// `value == empty`
pub fn is_empty(v: &Value) -> bool {
    match v {
        Value::Str(s) => s.is_empty(),
        Value::Array(a) => a.is_empty(),
        Value::Hash(h) => h.is_empty(),
        Value::Drop(d) => d.is_empty(),
        _ => false,
    }
}

/// Ruby whitespace for `blank?` (`/\A\s*\z/`).
pub fn is_ruby_space(c: char) -> bool {
    matches!(c, ' ' | '\t' | '\n' | '\u{b}' | '\u{c}' | '\r')
}

/// `value == blank`
pub fn is_blank(v: &Value) -> bool {
    match v {
        Value::Nil | Value::Bool(false) => true,
        Value::Bool(true) => false,
        Value::Int(_) | Value::BigInt(_) | Value::Float(_) => false,
        Value::Str(s) => s.chars().all(is_ruby_space),
        Value::Array(a) => a.is_empty(),
        Value::Hash(h) => h.is_empty(),
        Value::Range(..) => false,
        Value::Drop(d) => d.is_blank(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn floats() {
        assert_eq!(format_float(1e16), "1.0e+16");
        assert_eq!(format_float(1e15), "1.0e+15");
        assert_eq!(format_float(1e14), "100000000000000.0");
        assert_eq!(format_float(0.0001), "0.0001");
        assert_eq!(format_float(0.00001), "1.0e-05");
        assert_eq!(format_float(2.5e-5), "2.5e-05");
        assert_eq!(format_float(123456789.123456789), "123456789.12345679");
        assert_eq!(format_float(1.0 / 3.0), "0.3333333333333333");
        assert_eq!(format_float(1.234567890123456e15), "1.234567890123456e+15");
        assert_eq!(format_float(100.0), "100.0");
        assert_eq!(format_float(-1.5), "-1.5");
        assert_eq!(format_float(1e100), "1.0e+100");
    }

    #[test]
    fn inspects() {
        let mut s = String::new();
        write_string_inspect("a\"b\\#{x}\u{1}\u{7f}\u{1b}é", &mut s);
        assert_eq!(s, "\"a\\\"b\\\\\\#{x}\\u0001\\u007F\\eé\"");
    }
}
