//! The standard filters (`Liquid::StandardFilters`) plus `json`.

use crate::date::{self, Timestamp};
use crate::error::{Error, ErrorKind, Result};
use crate::expression::{ruby_lstrip, ruby_rstrip, ruby_strip};
use crate::num::{self, to_number, Num};
use crate::render::{ruby_to_s, to_integer, to_liquid, Context, MAX_ARRAY_LEN, MAX_STRING_BYTES};
use crate::value::{self, format_float, is_ruby_space, ruby_cmp, ruby_eq, to_s, Hash, RcStr, Value};
use std::cmp::Ordering;
use std::fmt::Write as _;
use std::rc::Rc;

fn arity(name: &str) -> Option<(usize, usize)> {
    Some(match name {
        "size" | "downcase" | "upcase" | "capitalize" | "escape" | "h" | "escape_once" | "url_encode" | "url_decode"
        | "base64_encode" | "base64_decode" | "base64_url_safe_encode" | "base64_url_safe_decode" | "squish" | "strip"
        | "lstrip" | "rstrip" | "strip_html" | "strip_newlines" | "reverse" | "newline_to_br" | "first" | "last"
        | "abs" | "ceil" | "floor" | "json" => (0, 0),
        "slice" => (1, 1),
        "truncate" | "truncatewords" | "default" => (0, 2),
        "split" | "map" | "remove" | "remove_first" | "remove_last" | "append" | "concat" | "prepend" | "date"
        | "plus" | "minus" | "times" | "divided_by" | "modulo" | "at_least" | "at_most" => (1, 0),
        "join" | "sort" | "sort_natural" | "uniq" | "compact" | "round" | "sum" => (0, 1),
        "where" | "reject" | "has" | "find" | "find_index" | "replace" | "replace_first" => (1, 1),
        "replace_last" => (2, 0),
        _ => return None,
    })
}

fn too_long() -> Error {
    Error::memory()
}

fn check_len(n: usize) -> Result<()> {
    if n > MAX_STRING_BYTES { Err(too_long()) } else { Ok(()) }
}

fn arg(args: &[Value], i: usize) -> Option<&Value> {
    args.get(i)
}

/// Invokes a filter. Unknown filters return the input unchanged.
pub(crate) fn invoke(ctx: &mut Context, name: &str, input: Value, args: &[Value]) -> Result<Value> {
    let Some((req, opt)) = arity(name) else {
        return Ok(input);
    };
    if args.len() < req || args.len() > req + opt {
        let expected = if opt == 0 { format!("{}", req + 1) } else { format!("{}..{}", req + 1, req + opt + 1) };
        return Err(Error::argument(format!(
            "wrong number of arguments (given {}, expected {})",
            args.len() + 1,
            expected
        )));
    }
    let a0 = arg(args, 0);
    let a1 = arg(args, 1);
    Ok(match name {
        "size" => size(&input),
        "downcase" => Value::from(to_s(&input).to_lowercase()),
        "upcase" => Value::from(to_s(&input).to_uppercase()),
        "capitalize" => Value::from(capitalize(&to_s(&input))),
        "escape" | "h" => {
            if input.is_nil() {
                Value::Nil
            } else {
                Value::from(escape_html(&to_s(&input)))
            }
        }
        "escape_once" => Value::from(escape_once(&to_s(&input))),
        "url_encode" => {
            if input.is_nil() {
                Value::Nil
            } else {
                Value::from(url_encode(&to_s(&input)))
            }
        }
        "url_decode" => {
            if input.is_nil() {
                Value::Nil
            } else {
                let bytes = url_decode(&to_s(&input));
                match String::from_utf8(bytes) {
                    Ok(s) => Value::from(s),
                    Err(_) => return Err(Error::argument("invalid byte sequence in UTF-8")),
                }
            }
        }
        "base64_encode" => Value::from(base64_encode(to_s(&input).as_bytes(), false)),
        "base64_url_safe_encode" => Value::from(base64_encode(to_s(&input).as_bytes(), true)),
        "base64_decode" => match base64_decode(&to_s(&input), false) {
            Some(b) => Value::from(String::from_utf8_lossy(&b).into_owned()),
            None => return Err(Error::argument("invalid base64 provided to base64_decode")),
        },
        "base64_url_safe_decode" => match base64_decode(&to_s(&input), true) {
            Some(b) => Value::from(String::from_utf8_lossy(&b).into_owned()),
            None => return Err(Error::argument("invalid base64 provided to base64_url_safe_decode")),
        },
        "slice" => slice(&input, a0.unwrap(), a1)?,
        "truncate" => {
            if input.is_nil() {
                return Ok(Value::Nil);
            }
            let len = match a0 {
                Some(v) => to_integer(v)?,
                None => 50,
            };
            let ts = match a1 {
                Some(v) => to_s(v),
                None => Rc::from("..."),
            };
            truncate(&to_s(&input), len, &ts)
        }
        "truncatewords" => {
            if input.is_nil() {
                return Ok(Value::Nil);
            }
            let words = match a0 {
                Some(v) => to_integer(v)?,
                None => 15,
            };
            let ts = match a1 {
                Some(v) => to_s(v),
                None => Rc::from("..."),
            };
            truncatewords(&to_s(&input), words, &ts)
        }
        "split" => {
            let pat = to_s(a0.unwrap());
            let s = to_s(&input);
            Value::from(split(&s, &pat)?)
        }
        "squish" => {
            if input.is_nil() {
                return Ok(Value::Nil);
            }
            Value::from(squish(&to_s(&input)))
        }
        "strip" => Value::str(ruby_strip(&to_s(&input))),
        "lstrip" => Value::str(ruby_lstrip(&to_s(&input))),
        "rstrip" => Value::str(ruby_rstrip(&to_s(&input))),
        "strip_html" => Value::from(strip_html(&to_s(&input))),
        "strip_newlines" => Value::from(to_s(&input).replace("\r\n", "").replace('\n', "")),
        "newline_to_br" => Value::from(newline_to_br(&to_s(&input))?),
        "join" => {
            let glue = match a0 {
                Some(v) => to_s(v),
                None => Rc::from(" "),
            };
            let items = input_iterator(&input)?;
            let mut out = String::new();
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push_str(&glue);
                }
                value::write_to_s(item, &mut out);
                check_len(out.len())?;
            }
            Value::from(out)
        }
        "sort" => sort(&input, a0, false)?,
        "sort_natural" => sort(&input, a0, true)?,
        "where" => filter_array(&input, a0.unwrap(), a1, FilterMode::Select)?,
        "reject" => filter_array(&input, a0.unwrap(), a1, FilterMode::Reject)?,
        "has" => filter_array(&input, a0.unwrap(), a1, FilterMode::Any)?,
        "find" => filter_array(&input, a0.unwrap(), a1, FilterMode::Find)?,
        "find_index" => filter_array(&input, a0.unwrap(), a1, FilterMode::FindIndex)?,
        "uniq" => uniq(&input, a0)?,
        "reverse" => {
            let mut items = input_iterator(&input)?;
            items.reverse();
            Value::from(items)
        }
        "map" => map(&input, a0.unwrap())?,
        "compact" => compact(&input, a0)?,
        "replace" => {
            let pat = to_s(a0.unwrap());
            let rep = match a1 {
                Some(v) => to_s(v),
                None => Rc::from(""),
            };
            Value::from(gsub(&to_s(&input), &pat, &rep, true)?)
        }
        "replace_first" => {
            let pat = to_s(a0.unwrap());
            let rep = match a1 {
                Some(v) => to_s(v),
                None => Rc::from(""),
            };
            Value::from(gsub(&to_s(&input), &pat, &rep, false)?)
        }
        "replace_last" => Value::from(replace_last(&to_s(&input), &to_s(a0.unwrap()), &to_s(a1.unwrap()))?),
        "remove" => Value::from(gsub(&to_s(&input), &to_s(a0.unwrap()), "", true)?),
        "remove_first" => Value::from(gsub(&to_s(&input), &to_s(a0.unwrap()), "", false)?),
        "remove_last" => Value::from(replace_last(&to_s(&input), &to_s(a0.unwrap()), "")?),
        "append" => {
            let a = to_s(&input);
            let b = to_s(a0.unwrap());
            check_len(a.len() + b.len())?;
            Value::from(format!("{}{}", a, b))
        }
        "prepend" => {
            let a = to_s(&input);
            let b = to_s(a0.unwrap());
            check_len(a.len() + b.len())?;
            Value::from(format!("{}{}", b, a))
        }
        "concat" => {
            let other = a0.unwrap();
            let Value::Array(arr) = other else {
                return Err(Error::argument("concat filter requires an array argument"));
            };
            let mut items = input_iterator(&input)?;
            if items.len() + arr.len() > MAX_ARRAY_LEN {
                return Err(too_long());
            }
            items.extend(arr.iter().cloned());
            Value::from(items)
        }
        "date" => date_filter(ctx, input, a0.unwrap())?,
        "first" => match &input {
            Value::Str(s) => Value::from(s.chars().next().map(String::from).unwrap_or_default()),
            Value::Array(a) => a.first().cloned().unwrap_or(Value::Nil),
            Value::Hash(h) => match h.first() {
                Some((k, v)) => Value::from(vec![Value::Str(k.clone()), v.clone()]),
                None => Value::Nil,
            },
            Value::Range(a, _) => Value::Int(*a),
            Value::Drop(d) => {
                if let Some(items) = d.iterate() {
                    items?.into_iter().next().unwrap_or(Value::Nil)
                } else if d.responds_to("first") {
                    d.get(&Value::str("first"))?
                } else {
                    Value::Nil
                }
            }
            _ => Value::Nil,
        },
        "last" => match &input {
            Value::Str(s) => Value::from(s.chars().last().map(String::from).unwrap_or_default()),
            Value::Array(a) => a.last().cloned().unwrap_or(Value::Nil),
            Value::Range(_, b) => Value::Int(*b),
            Value::Drop(d) => {
                if d.responds_to("last") {
                    d.get(&Value::str("last"))?
                } else {
                    Value::Nil
                }
            }
            _ => Value::Nil,
        },
        "abs" => to_number(&input).abs()?.into_value(),
        "plus" => to_number(&input).add(&to_number(a0.unwrap()))?.into_value(),
        "minus" => to_number(&input).sub(&to_number(a0.unwrap()))?.into_value(),
        "times" => to_number(&input).mul(&to_number(a0.unwrap()))?.into_value(),
        "divided_by" => to_number(&input).div(&to_number(a0.unwrap()))?.into_value(),
        "modulo" => to_number(&input).modulo(&to_number(a0.unwrap()))?.into_value(),
        "round" => {
            let n_val = a0.cloned().unwrap_or(Value::Int(0));
            let n = match to_number(&n_val) {
                Num::Int(i) => i,
                Num::Big(b) => num::clamp_bigint(&b),
                Num::Dec(d) => {
                    let f = d.to_f64();
                    if f.is_finite() { f.trunc() as i64 } else { 0 }
                }
            };
            let r = to_number(&input).round(n)?;
            let is_zero = matches!(n_val, Value::Int(0)) || matches!(n_val, Value::Float(f) if f == 0.0);
            let v = r.into_value();
            match (is_zero, &v) {
                (true, Value::Float(f)) => {
                    if f.is_finite() {
                        Value::Int(f.trunc() as i64)
                    } else {
                        return Err(Error::new(ErrorKind::FloatDomain, format_float(*f)));
                    }
                }
                _ => v,
            }
        }
        "ceil" => to_number(&input).ceil()?.into_value(),
        "floor" => to_number(&input).floor()?.into_value(),
        "at_least" => {
            let min = to_number(a0.unwrap());
            let r = to_number(&input);
            let r = if min.cmp(&r) == Some(Ordering::Greater) { min } else { r };
            r.into_value()
        }
        "at_most" => {
            let max = to_number(a0.unwrap());
            let r = to_number(&input);
            let r = if max.cmp(&r) == Some(Ordering::Less) { max } else { r };
            r.into_value()
        }
        "default" => {
            let dv = a0.cloned().unwrap_or_else(|| Value::str(""));
            let allow_false = match a1 {
                Some(Value::Hash(h)) => h.get("allow_false").is_some_and(|v| v.is_truthy()),
                _ => false,
            };
            let false_check = if allow_false { input.is_nil() } else { !input.to_liquid_value().is_truthy() };
            let empty = match &input {
                Value::Str(s) => s.is_empty(),
                Value::Array(a) => a.is_empty(),
                Value::Hash(h) => h.is_empty(),
                _ => false,
            };
            if false_check || empty { dv } else { input }
        }
        "sum" => sum(&input, a0)?,
        "json" => Value::from(json(&input)?),
        _ => input,
    })
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn size(v: &Value) -> Value {
    match v {
        Value::Str(s) => Value::Int(s.chars().count() as i64),
        Value::Array(a) => Value::Int(a.len() as i64),
        Value::Hash(h) => Value::Int(h.len() as i64),
        Value::Int(_) => Value::Int(8),
        Value::BigInt(b) => Value::Int(((b.bit_length().div_ceil(8) as i64) + 7) / 8 * 8),
        Value::Range(a, b) => Value::Int(Value::range_len(*a, *b) as i64),
        Value::Drop(d) => {
            if d.responds_to("size") {
                d.get(&Value::str("size")).unwrap_or(Value::Int(0))
            } else if let Some(Ok(items)) = d.iterate() {
                Value::Int(items.len() as i64)
            } else {
                Value::Int(0)
            }
        }
        _ => Value::Int(0),
    }
}

fn capitalize(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    if let Some(c) = chars.next() {
        out.extend(c.to_uppercase());
    }
    for c in chars {
        out.extend(c.to_lowercase());
    }
    out
}

fn escape_html(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            c => out.push(c),
        }
    }
    out
}

/// `/["><']|&(?!([a-zA-Z]+|(#\d+));)/`
fn escape_once(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = String::with_capacity(s.len() + 8);
    let mut i = 0;
    let mut start = 0;
    while i < b.len() {
        let rep = match b[i] {
            b'"' => Some("&quot;"),
            b'>' => Some("&gt;"),
            b'<' => Some("&lt;"),
            b'\'' => Some("&#39;"),
            b'&' => {
                let mut j = i + 1;
                let entity = if j < b.len() && b[j].is_ascii_alphabetic() {
                    while j < b.len() && b[j].is_ascii_alphabetic() {
                        j += 1;
                    }
                    j < b.len() && b[j] == b';'
                } else if j < b.len() && b[j] == b'#' {
                    j += 1;
                    let ds = j;
                    while j < b.len() && b[j].is_ascii_digit() {
                        j += 1;
                    }
                    j > ds && j < b.len() && b[j] == b';'
                } else {
                    false
                };
                if entity { None } else { Some("&amp;") }
            }
            _ => None,
        };
        if let Some(r) = rep {
            out.push_str(&s[start..i]);
            out.push_str(r);
            start = i + 1;
        }
        i += 1;
    }
    out.push_str(&s[start..]);
    out
}

/// `CGI.escape`.
fn url_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    for &b in s.as_bytes() {
        match b {
            b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'_' | b'.' | b'-' | b'~' => out.push(b as char),
            b' ' => out.push('+'),
            _ => {
                let _ = write!(out, "%{:02X}", b);
            }
        }
    }
    out
}

/// `CGI.unescape`.
fn url_decode(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    let h = |c: u8| (c as char).to_digit(16);
    while i < b.len() {
        let c = b[i];
        if c == b'+' {
            out.push(b' ');
            i += 1;
        } else if c == b'%' && i + 2 < b.len() && h(b[i + 1]).is_some() && h(b[i + 2]).is_some() {
            out.push((h(b[i + 1]).unwrap() * 16 + h(b[i + 2]).unwrap()) as u8);
            i += 3;
        } else {
            out.push(c);
            i += 1;
        }
    }
    out
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const B64URL: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

fn base64_encode(data: &[u8], url: bool) -> String {
    let table = if url { B64URL } else { B64 };
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let n = match chunk.len() {
            3 => (chunk[0] as u32) << 16 | (chunk[1] as u32) << 8 | chunk[2] as u32,
            2 => (chunk[0] as u32) << 16 | (chunk[1] as u32) << 8,
            _ => (chunk[0] as u32) << 16,
        };
        out.push(table[(n >> 18) as usize & 63] as char);
        out.push(table[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { table[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { table[n as usize & 63] as char } else { '=' });
    }
    out
}

/// `Base64.strict_decode64` / `urlsafe_decode64`.
fn base64_decode(s: &str, url: bool) -> Option<Vec<u8>> {
    let mut s: String = s.to_string();
    if url {
        if !s.ends_with('=') && s.len() % 4 != 0 {
            while s.len() % 4 != 0 {
                s.push('=');
            }
        }
        s = s.replace('-', "+").replace('_', "/");
    }
    let b = s.as_bytes();
    if b.len() % 4 != 0 {
        return None;
    }
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => return None,
        } as u32)
    };
    let mut out = Vec::with_capacity(b.len() / 4 * 3);
    let chunks = b.len() / 4;
    for (ci, chunk) in b.chunks(4).enumerate() {
        let last = ci + 1 == chunks;
        let pad = chunk.iter().rev().take_while(|&&c| c == b'=').count();
        if pad > 2 || (pad > 0 && !last) {
            return None;
        }
        let mut n = 0u32;
        for (k, &c) in chunk.iter().enumerate() {
            let v = if k >= 4 - pad { 0 } else { val(c)? };
            n = n << 6 | v;
        }
        out.push((n >> 16) as u8);
        if pad < 2 {
            out.push((n >> 8) as u8);
        }
        if pad < 1 {
            out.push(n as u8);
        }
        // Strict: unused bits must be zero.
        if pad == 1 && (n & 0xff) != 0 {
            return None;
        }
        if pad == 2 && (n & 0xffff) != 0 {
            return None;
        }
    }
    Some(out)
}

/// Ruby `Array#slice(start, len)` index math on a sequence of `n` items.
fn slice_range(n: usize, start: i64, len: i64) -> Option<(usize, usize)> {
    if len < 0 {
        return None;
    }
    let n = n as i64;
    let mut s = start;
    if s < 0 {
        s += n;
        if s < 0 {
            return None;
        }
    }
    if s > n {
        return None;
    }
    let e = s.saturating_add(len).min(n);
    Some((s as usize, (e - s) as usize))
}

fn slice(input: &Value, offset: &Value, length: Option<&Value>) -> Result<Value> {
    let offset = to_integer(offset)?;
    let length = match length {
        Some(v) if !v.is_nil() => to_integer(v)?,
        _ => 1,
    };
    if let Value::Array(a) = input {
        return Ok(match slice_range(a.len(), offset, length) {
            Some((s, n)) => Value::from(a[s..s + n].to_vec()),
            None => Value::from(Vec::new()),
        });
    }
    let s = to_s(input);
    let chars: Vec<char> = s.chars().collect();
    Ok(match slice_range(chars.len(), offset, length) {
        Some((st, n)) => Value::from(chars[st..st + n].iter().collect::<String>()),
        None => Value::str(""),
    })
}

fn truncate(s: &str, length: i64, ts: &str) -> Value {
    let n = s.chars().count() as i64;
    if n <= length {
        return Value::str(s);
    }
    let l = (length - ts.chars().count() as i64).max(0) as usize;
    let mut out: String = s.chars().take(l).collect();
    out.push_str(ts);
    Value::from(out)
}

/// Ruby `str.split(" ", limit)` (awk mode), returning at most `limit` fields.
fn awk_split(s: &str, limit: Option<usize>) -> Vec<&str> {
    let mut out = Vec::new();
    let b = s.as_bytes();
    let mut i = 0;
    let n = b.len();
    let is_ws = |c: u8| matches!(c, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r');
    loop {
        while i < n && is_ws(b[i]) {
            i += 1;
        }
        if i >= n {
            break;
        }
        if let Some(l) = limit {
            if out.len() + 1 == l {
                out.push(&s[i..]);
                return out;
            }
        }
        let st = i;
        while i < n && !is_ws(b[i]) {
            i += 1;
        }
        out.push(&s[st..i]);
    }
    out
}

fn truncatewords(s: &str, words: i64, ts: &str) -> Value {
    let words = if words <= 0 { 1 } else { words };
    if words >= i32::MAX as i64 {
        return Value::str(s);
    }
    let words = words as usize;
    let list = awk_split(s, Some(words + 1));
    if list.len() <= words {
        return Value::str(s);
    }
    let mut out = list[..words].join(" ");
    out.push_str(ts);
    Value::from(out)
}

fn split(s: &str, pat: &str) -> Result<Vec<Value>> {
    let mut parts: Vec<Value> = if pat == " " {
        awk_split(s, None).into_iter().map(Value::str).collect()
    } else if pat.is_empty() {
        let mut buf = [0u8; 4];
        s.chars().map(|c| Value::str(c.encode_utf8(&mut buf))).collect()
    } else {
        s.split(pat).map(Value::str).collect()
    };
    while matches!(parts.last(), Some(Value::Str(x)) if x.is_empty()) {
        parts.pop();
    }
    if parts.len() > MAX_ARRAY_LEN {
        return Err(too_long());
    }
    Ok(parts)
}

fn squish(s: &str) -> String {
    let t = ruby_strip(s);
    let mut out = String::with_capacity(t.len());
    let mut in_ws = false;
    for c in t.chars() {
        if is_ruby_space(c) {
            if !in_ws {
                out.push(' ');
                in_ws = true;
            }
        } else {
            out.push(c);
            in_ws = false;
        }
    }
    out
}

fn strip_html(s: &str) -> String {
    // Remove <script...</script>, <!--...-->, <style...</style> blocks first.
    let mut tmp = String::with_capacity(s.len());
    let mut rest = s;
    loop {
        let candidates = [("<script", "</script>"), ("<!--", "-->"), ("<style", "</style>")];
        let mut best: Option<(usize, &str)> = None;
        for (open, close) in candidates {
            if let Some(p) = rest.find(open) {
                if rest[p + open.len()..].contains(close) && best.is_none_or(|(bp, _)| p < bp) {
                    best = Some((p, close));
                }
            }
        }
        // A block opener without a closer can still match a later opener.
        match best {
            Some((p, close)) => {
                let open_len = if close == "-->" { 4 } else if close == "</script>" { 7 } else { 6 };
                tmp.push_str(&rest[..p]);
                let after = &rest[p + open_len..];
                let e = after.find(close).unwrap();
                rest = &after[e + close.len()..];
            }
            None => {
                tmp.push_str(rest);
                break;
            }
        }
    }
    // Then <.*?>
    let mut out = String::with_capacity(tmp.len());
    let mut rest = tmp.as_str();
    while let Some(p) = rest.find('<') {
        match rest[p + 1..].find('>') {
            Some(e) => {
                out.push_str(&rest[..p]);
                rest = &rest[p + 1 + e + 1..];
            }
            None => break,
        }
    }
    out.push_str(rest);
    out
}

fn newline_to_br(s: &str) -> Result<String> {
    let mut out = String::with_capacity(s.len());
    let b = s.as_bytes();
    let mut start = 0;
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\n' {
            let end = if i > start && b[i - 1] == b'\r' { i - 1 } else { i };
            out.push_str(&s[start..end]);
            out.push_str("<br />\n");
            check_len(out.len())?;
            start = i + 1;
        }
        i += 1;
    }
    out.push_str(&s[start..]);
    Ok(out)
}

/// Expands Ruby's `gsub`/`sub` replacement escapes (`\0`, `\&`, `` \` ``, `\'`, `\\`).
fn expand_replacement(rep: &str, matched: &str, pre: &str, post: &str, out: &mut String) {
    if !rep.contains('\\') {
        out.push_str(rep);
        return;
    }
    let b = rep.as_bytes();
    let mut i = 0;
    let mut start = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 1 < b.len() {
            let r: Option<&str> = match b[i + 1] {
                b'0' | b'&' => Some(matched),
                b'1'..=b'9' => Some(""),
                b'`' => Some(pre),
                b'\'' => Some(post),
                b'\\' => Some("\\"),
                _ => None,
            };
            if let Some(r) = r {
                out.push_str(&rep[start..i]);
                out.push_str(r);
                i += 2;
                start = i;
                continue;
            }
        }
        i += 1;
    }
    out.push_str(&rep[start..]);
}

/// `String#gsub(str, repl)` / `String#sub(str, repl)` with a literal pattern.
fn gsub(s: &str, pat: &str, rep: &str, global: bool) -> Result<String> {
    let mut out = String::with_capacity(s.len());
    if pat.is_empty() {
        // Matches at every character boundary.
        let mut first = true;
        for (i, c) in s.char_indices() {
            if global || first {
                expand_replacement(rep, "", &s[..i], &s[i..], &mut out);
                check_len(out.len())?;
            }
            first = false;
            out.push(c);
        }
        if global || s.is_empty() {
            expand_replacement(rep, "", s, "", &mut out);
        }
        return Ok(out);
    }
    let mut last = 0;
    let mut pos = 0;
    while let Some(p) = s[pos..].find(pat) {
        let at = pos + p;
        out.push_str(&s[last..at]);
        expand_replacement(rep, pat, &s[..at], &s[at + pat.len()..], &mut out);
        check_len(out.len())?;
        last = at + pat.len();
        pos = last;
        if !global {
            break;
        }
    }
    out.push_str(&s[last..]);
    Ok(out)
}

fn replace_last(s: &str, pat: &str, rep: &str) -> Result<String> {
    match s.rfind(pat) {
        Some(p) => {
            check_len(s.len() + rep.len())?;
            let mut out = String::with_capacity(s.len() + rep.len());
            out.push_str(&s[..p]);
            out.push_str(rep);
            out.push_str(&s[p + pat.len()..]);
            Ok(out)
        }
        None => Ok(s.to_string()),
    }
}

/// `StandardFilters::InputIterator`: arrays are flattened, hashes are one item,
/// ranges and enumerable drops expand, nil is empty, anything else is one item.
fn input_iterator(v: &Value) -> Result<Vec<Value>> {
    fn flatten(items: &[Value], out: &mut Vec<Value>, depth: usize) -> Result<()> {
        if depth > 128 {
            return Err(Error::argument("tried to flatten recursive array"));
        }
        for i in items {
            match i {
                Value::Array(a) => flatten(a, out, depth + 1)?,
                other => out.push(to_liquid(other.clone())),
            }
            if out.len() > MAX_ARRAY_LEN {
                return Err(too_long());
            }
        }
        Ok(())
    }
    Ok(match v {
        Value::Array(a) => {
            let mut out = Vec::with_capacity(a.len());
            flatten(a, &mut out, 0)?;
            out
        }
        Value::Nil => Vec::new(),
        Value::Range(a, b) => {
            let n = Value::range_len(*a, *b);
            if n > MAX_ARRAY_LEN {
                return Err(too_long());
            }
            (0..n).map(|i| Value::Int(a + i as i64)).collect()
        }
        Value::Drop(d) => match d.iterate() {
            Some(items) => items?.into_iter().map(to_liquid).collect(),
            None => vec![v.clone()],
        },
        other => vec![other.clone()],
    })
}

/// The outcome of Ruby's `item[property]` on a filter item.
enum Prop {
    Val(Value),
    /// TypeError (e.g. indexing an array with a string).
    TypeError,
    /// NoMethodError: the item has no `[]`.
    NoMethod,
}

fn item_property(item: &Value, prop: &Value) -> Result<Prop> {
    Ok(match item {
        Value::Hash(h) => Prop::Val(match prop {
            Value::Str(k) => h.get(k).cloned().unwrap_or(Value::Nil),
            _ => Value::Nil,
        }),
        Value::Array(a) => match prop {
            Value::Int(i) => {
                let idx = if *i < 0 { a.len() as i64 + i } else { *i };
                Prop::Val(if idx >= 0 && (idx as usize) < a.len() { a[idx as usize].clone() } else { Value::Nil })
            }
            Value::Float(f) => {
                let i = f.trunc() as i64;
                let idx = if i < 0 { a.len() as i64 + i } else { i };
                Prop::Val(if idx >= 0 && (idx as usize) < a.len() { a[idx as usize].clone() } else { Value::Nil })
            }
            Value::Range(..) => Prop::TypeError,
            _ => Prop::TypeError,
        },
        Value::Str(s) => match prop {
            Value::Str(p) => Prop::Val(if s.contains(&**p) { Value::Str(p.clone()) } else { Value::Nil }),
            Value::Int(i) => {
                let n = s.chars().count() as i64;
                let idx = if *i < 0 { n + i } else { *i };
                Prop::Val(if idx >= 0 && idx < n {
                    Value::from(s.chars().nth(idx as usize).unwrap().to_string())
                } else {
                    Value::Nil
                })
            }
            Value::Nil => Prop::TypeError,
            _ => Prop::TypeError,
        },
        Value::Int(i) => match prop {
            Value::Int(bit) => Prop::Val(Value::Int(if *bit < 0 { 0 } else if *bit >= 64 { (*i < 0) as i64 } else { (i >> bit) & 1 })),
            _ => Prop::TypeError,
        },
        Value::BigInt(_) => match prop {
            Value::Int(_) => Prop::Val(Value::Int(0)),
            _ => Prop::TypeError,
        },
        Value::Drop(d) => Prop::Val(to_liquid(d.get(prop)?)),
        _ => Prop::NoMethod,
    })
}

fn property_error(prop: &Value) -> Error {
    Error::argument(format!("cannot select the property '{}'", to_s(prop)))
}

fn responds_to_index(v: &Value) -> bool {
    matches!(v, Value::Hash(_) | Value::Array(_) | Value::Str(_) | Value::Int(_) | Value::BigInt(_) | Value::Drop(_))
}

fn nil_safe_compare(a: &Value, b: &Value) -> Result<Ordering> {
    match ruby_cmp(a, b) {
        Some(o) => Ok(o),
        None => {
            if a.is_nil() {
                Ok(Ordering::Greater)
            } else if b.is_nil() {
                Ok(Ordering::Less)
            } else {
                Err(Error::argument("cannot sort values of incompatible types"))
            }
        }
    }
}

fn casecmp(a: &str, b: &str) -> Ordering {
    let x = a.bytes().map(|c| c.to_ascii_lowercase());
    let y = b.bytes().map(|c| c.to_ascii_lowercase());
    x.cmp(y)
}

fn nil_safe_casecmp(a: &Value, b: &Value) -> Ordering {
    match (a.is_nil(), b.is_nil()) {
        (false, false) => casecmp(&ruby_to_s(a), &ruby_to_s(b)),
        (true, true) => Ordering::Equal,
        (true, false) => Ordering::Greater,
        (false, true) => Ordering::Less,
    }
}

/// Sorts with a fallible comparator, propagating the first error.
fn try_sort<T>(items: &mut [T], mut cmp: impl FnMut(&T, &T) -> Result<Ordering>) -> Result<()> {
    let mut err: Option<Error> = None;
    items.sort_by(|a, b| {
        if err.is_some() {
            return Ordering::Equal;
        }
        match cmp(a, b) {
            Ok(o) => o,
            Err(e) => {
                err = Some(e);
                Ordering::Equal
            }
        }
    });
    match err {
        Some(e) => Err(e),
        None => Ok(()),
    }
}

fn sort(input: &Value, prop: Option<&Value>, natural: bool) -> Result<Value> {
    let mut items = input_iterator(input)?;
    if items.is_empty() {
        return Ok(Value::from(Vec::new()));
    }
    match prop.filter(|p| !p.is_nil()) {
        None => {
            if natural {
                items.sort_by(nil_safe_casecmp);
            } else {
                try_sort(&mut items, nil_safe_compare)?;
            }
            Ok(Value::from(items))
        }
        Some(p) => {
            if !items.iter().all(responds_to_index) {
                return Ok(Value::Nil);
            }
            let mut keyed = Vec::with_capacity(items.len());
            for it in items {
                let k = match item_property(&it, p)? {
                    Prop::Val(v) => v,
                    Prop::TypeError => return Err(property_error(p)),
                    Prop::NoMethod => Value::Nil,
                };
                keyed.push((k, it));
            }
            if natural {
                keyed.sort_by(|a, b| nil_safe_casecmp(&a.0, &b.0));
            } else {
                try_sort(&mut keyed, |a, b| nil_safe_compare(&a.0, &b.0))?;
            }
            Ok(Value::from(keyed.into_iter().map(|(_, v)| v).collect::<Vec<_>>()))
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
enum FilterMode {
    Select,
    Reject,
    Any,
    Find,
    FindIndex,
}

fn filter_array(input: &Value, prop: &Value, target: Option<&Value>, mode: FilterMode) -> Result<Value> {
    let items = input_iterator(input)?;
    if items.is_empty() {
        return Ok(match mode {
            FilterMode::Select | FilterMode::Reject => Value::from(Vec::new()),
            FilterMode::Any => Value::Bool(false),
            _ => Value::Nil,
        });
    }
    let target = target.filter(|t| !t.is_nil());
    let mut selected = Vec::new();
    for (idx, item) in items.iter().enumerate() {
        let v = match item_property(item, prop)? {
            Prop::Val(v) => v,
            Prop::TypeError => return Err(property_error(prop)),
            Prop::NoMethod => return Ok(Value::Nil),
        };
        let hit = match target {
            None => v.is_truthy(),
            Some(t) => ruby_eq(&v, t),
        };
        match mode {
            FilterMode::Select => {
                if hit {
                    selected.push(item.clone());
                }
            }
            FilterMode::Reject => {
                if !hit {
                    selected.push(item.clone());
                }
            }
            FilterMode::Any => {
                if hit {
                    return Ok(Value::Bool(true));
                }
            }
            FilterMode::Find => {
                if hit {
                    return Ok(item.clone());
                }
            }
            FilterMode::FindIndex => {
                if hit {
                    return Ok(Value::Int(idx as i64));
                }
            }
        }
    }
    Ok(match mode {
        FilterMode::Select | FilterMode::Reject => Value::from(selected),
        FilterMode::Any => Value::Bool(false),
        _ => Value::Nil,
    })
}

/// A hashable key with Ruby `eql?` semantics for `uniq`.
fn eql_key(v: &Value) -> String {
    match v {
        Value::Str(s) => format!("s{}", s),
        Value::Int(i) => format!("i{}", i),
        Value::BigInt(b) => format!("i{}", b),
        Value::Float(f) => format!("f{}", format_float(*f)),
        Value::Nil => "n".into(),
        Value::Bool(b) => format!("b{}", b),
        Value::Range(a, b) => format!("r{}..{}", a, b),
        Value::Array(a) => {
            let mut s = String::from("a[");
            for x in a.iter() {
                s.push_str(&eql_key(x));
                s.push(',');
            }
            s.push(']');
            s
        }
        Value::Hash(h) => {
            let mut keys: Vec<String> = h.iter().map(|(k, v)| format!("{}={}", k, eql_key(v))).collect();
            keys.sort();
            format!("h{{{}}}", keys.join(","))
        }
        Value::Drop(d) => format!("d{:p}", Rc::as_ptr(d) as *const u8),
    }
}

fn uniq(input: &Value, prop: Option<&Value>) -> Result<Value> {
    let items = input_iterator(input)?;
    let prop = prop.filter(|p| !p.is_nil());
    if prop.is_some() && items.is_empty() {
        return Ok(Value::from(Vec::new()));
    }
    let mut seen = std::collections::HashSet::new();
    let mut out = Vec::new();
    for item in items {
        let base = item.to_liquid_value();
        let key = match prop {
            None => eql_key(&base),
            Some(p) => match item_property(&base, p)? {
                Prop::Val(v) => eql_key(&v),
                Prop::TypeError => return Err(property_error(p)),
                Prop::NoMethod => return Ok(Value::Nil),
            },
        };
        if seen.insert(key) {
            out.push(item);
        }
    }
    Ok(Value::from(out))
}

fn map(input: &Value, prop: &Value) -> Result<Value> {
    let items = input_iterator(input)?;
    let mut out = Vec::with_capacity(items.len());
    for item in items {
        if prop.as_str() == Some("to_liquid") {
            out.push(item);
            continue;
        }
        if !responds_to_index(&item) {
            out.push(Value::Nil);
            continue;
        }
        match item_property(&item, prop)? {
            Prop::Val(v) => out.push(v),
            Prop::TypeError => return Err(property_error(prop)),
            Prop::NoMethod => out.push(Value::Nil),
        }
    }
    Ok(Value::from(out))
}

fn compact(input: &Value, prop: Option<&Value>) -> Result<Value> {
    let items = input_iterator(input)?;
    let prop = prop.filter(|p| !p.is_nil());
    match prop {
        None => Ok(Value::from(items.into_iter().filter(|v| !v.is_nil()).collect::<Vec<_>>())),
        Some(p) => {
            let mut out = Vec::new();
            for item in items {
                match item_property(&item, p)? {
                    Prop::Val(v) => {
                        if !v.is_nil() {
                            out.push(item);
                        }
                    }
                    Prop::TypeError => return Err(property_error(p)),
                    Prop::NoMethod => return Ok(Value::Nil),
                }
            }
            Ok(Value::from(out))
        }
    }
}

fn sum(input: &Value, prop: Option<&Value>) -> Result<Value> {
    let items = input_iterator(input)?;
    if items.is_empty() {
        return Ok(Value::Int(0));
    }
    let prop = prop.filter(|p| !p.is_nil());
    let mut values = Vec::with_capacity(items.len());
    for item in items {
        match prop {
            None => values.push(item),
            Some(p) => {
                if responds_to_index(&item) {
                    match item_property(&item, p)? {
                        Prop::Val(v) => values.push(v),
                        Prop::TypeError => return Err(property_error(p)),
                        Prop::NoMethod => values.push(Value::Int(0)),
                    }
                } else {
                    values.push(Value::Int(0));
                }
            }
        }
    }
    let values = input_iterator(&Value::from(values))?;
    let mut total = Num::Int(0);
    for v in &values {
        total = total.add(&to_number(v))?;
    }
    Ok(total.into_value())
}

fn date_filter(ctx: &Context, input: Value, format: &Value) -> Result<Value> {
    let fmt = to_s(format);
    if fmt.is_empty() {
        return Ok(input);
    }
    let now = ctx.now().unwrap_or_else(Timestamp::now);
    let ts = match &input {
        Value::Int(i) => Some(Timestamp::from_unix(*i)),
        Value::Str(s) => {
            if s.is_empty() {
                None
            } else {
                let l = s.to_lowercase();
                if l == "now" || l == "today" {
                    Some(now)
                } else if l.bytes().all(|b| b.is_ascii_digit()) {
                    match l.parse::<i64>() {
                        Ok(i) => Some(Timestamp::from_unix(i)),
                        Err(_) => None,
                    }
                } else {
                    date::parse_time(s, now)
                }
            }
        }
        _ => None,
    };
    match ts {
        Some(t) => {
            // Keep years within a range strftime can print sanely.
            if t.secs.abs() > 100_000_000_000_000 {
                return Ok(input);
            }
            Ok(Value::from(date::strftime(&t, &fmt)))
        }
        None => Ok(input),
    }
}

// ---------------------------------------------------------------------------
// json
// ---------------------------------------------------------------------------

fn json(v: &Value) -> Result<String> {
    let mut out = String::new();
    write_json(v, &mut out, 0)?;
    Ok(out)
}

fn write_json(v: &Value, out: &mut String, depth: usize) -> Result<()> {
    if depth > 100 {
        return Err(Error::argument("nesting of 101 is too deep"));
    }
    match v {
        Value::Nil => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Int(i) => {
            let _ = write!(out, "{}", i);
        }
        Value::BigInt(b) => {
            let _ = write!(out, "{}", b);
        }
        Value::Float(f) => {
            if !f.is_finite() {
                return Err(Error::new(ErrorKind::Internal, "internal"));
            }
            out.push_str(&format_float(*f));
        }
        Value::Str(s) => json_string(s, out),
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_json(&to_liquid(x.clone()), out, depth + 1)?;
            }
            out.push(']');
        }
        Value::Hash(h) => {
            out.push('{');
            for (i, (k, x)) in h.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                json_string(k, out);
                out.push(':');
                write_json(&to_liquid(x.clone()), out, depth + 1)?;
            }
            out.push('}');
        }
        Value::Range(a, b) => {
            let n = Value::range_len(*a, *b);
            if n > MAX_ARRAY_LEN {
                return Err(too_long());
            }
            out.push('[');
            for i in 0..n {
                if i > 0 {
                    out.push(',');
                }
                let _ = write!(out, "{}", a + i as i64);
            }
            out.push(']');
        }
        Value::Drop(d) => match d.iterate() {
            Some(items) => {
                let items = items?;
                write_json(&Value::from(items), out, depth + 1)?;
            }
            None => out.push_str("{\"error\":\"json not allowed for this object\"}"),
        },
    }
    check_len(out.len())
}

fn json_string(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

#[allow(dead_code)]
fn hash_from(pairs: Vec<(RcStr, Value)>) -> Hash {
    let mut h = Hash::new();
    for (k, v) in pairs {
        h.insert(k, v);
    }
    h
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn b64() {
        assert_eq!(base64_encode(b"hello", false), "aGVsbG8=");
        assert_eq!(base64_decode("aGVsbG8=", false).unwrap(), b"hello");
        assert!(base64_decode("aGVsbG8", false).is_none());
        assert_eq!(base64_decode("aGVsbG8", true).unwrap(), b"hello");
    }

    #[test]
    fn gsubs() {
        assert_eq!(gsub("abc", "", "-", true).unwrap(), "-a-b-c-");
        assert_eq!(gsub("abc", "", "-", false).unwrap(), "-abc");
        assert_eq!(gsub("aXbX", "X", "\\0\\0", true).unwrap(), "aXXbXX");
    }

    #[test]
    fn escapes() {
        assert_eq!(escape_once("&amp; & < &#39; &#x1;"), "&amp; &amp; &lt; &#39; &amp;#x1;");
        assert_eq!(url_encode("a b&é"), "a+b%26%C3%A9");
        assert_eq!(url_decode("a+b%26%C3%A9%zz%"), "a b&é%zz%".as_bytes());
    }

    #[test]
    fn html() {
        assert_eq!(strip_html("<p>Hi <b>there</b></p><script>x</script>!<!-- c -->"), "Hi there!");
    }
}
