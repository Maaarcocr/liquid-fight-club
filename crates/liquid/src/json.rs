//! A small, self-contained JSON parser and serializer.
//!
//! Objects keep their members in source order (Liquid hashes are ordered).
//! Integers that do not fit in an `i64` are kept as their decimal digits so
//! callers can turn them into big integers. Nesting depth is bounded so that
//! hostile input cannot overflow the stack.

use std::fmt::Write as _;

/// Maximum nesting depth accepted by the parser.
pub const MAX_DEPTH: usize = 512;

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    /// An integer outside the `i64` range, as an optional `-` followed by digits.
    BigInt(String),
    Float(f64),
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    /// Looks up a member of an object (the last one wins on duplicates).
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(members) => members.iter().rev().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Json::Int(i) => Some(*i),
            Json::Float(f) if f.fract() == 0.0 && f.abs() < 9.0e18 => Some(*f as i64),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Json::Null)
    }

    /// Serializes to compact JSON text.
    pub fn to_string(&self) -> String {
        let mut out = String::new();
        self.write_to(&mut out);
        out
    }

    pub fn write_to(&self, out: &mut String) {
        // Iterative to keep stack use bounded regardless of depth.
        enum Step<'a> {
            Value(&'a Json),
            Raw(&'static str),
            Key(&'a str),
        }
        let mut stack: Vec<Step> = vec![Step::Value(self)];
        while let Some(step) = stack.pop() {
            match step {
                Step::Raw(s) => out.push_str(s),
                Step::Key(k) => {
                    write_string(out, k);
                    out.push(':');
                }
                Step::Value(v) => match v {
                    Json::Null => out.push_str("null"),
                    Json::Bool(true) => out.push_str("true"),
                    Json::Bool(false) => out.push_str("false"),
                    Json::Int(i) => {
                        let _ = write!(out, "{}", i);
                    }
                    Json::BigInt(s) => out.push_str(s),
                    Json::Float(f) => write_float(out, *f),
                    Json::Str(s) => write_string(out, s),
                    Json::Array(items) => {
                        out.push('[');
                        stack.push(Step::Raw("]"));
                        for (i, item) in items.iter().enumerate().rev() {
                            stack.push(Step::Value(item));
                            if i > 0 {
                                stack.push(Step::Raw(","));
                            }
                        }
                    }
                    Json::Object(members) => {
                        out.push('{');
                        stack.push(Step::Raw("}"));
                        for (i, (k, val)) in members.iter().enumerate().rev() {
                            stack.push(Step::Value(val));
                            stack.push(Step::Key(k));
                            if i > 0 {
                                stack.push(Step::Raw(","));
                            }
                        }
                    }
                },
            }
        }
    }
}

fn write_float(out: &mut String, f: f64) {
    if !f.is_finite() {
        out.push_str("null");
    } else if f.fract() == 0.0 && f.abs() < 1e16 {
        let _ = write!(out, "{:.1}", f);
    } else {
        let _ = write!(out, "{}", f);
    }
}

/// Writes `s` as a JSON string literal.
pub fn write_string(out: &mut String, s: &str) {
    out.push('"');
    let bytes = s.as_bytes();
    let mut start = 0;
    for (i, &b) in bytes.iter().enumerate() {
        let esc: Option<&str> = match b {
            b'"' => Some("\\\""),
            b'\\' => Some("\\\\"),
            b'\n' => Some("\\n"),
            b'\r' => Some("\\r"),
            b'\t' => Some("\\t"),
            0x08 => Some("\\b"),
            0x0c => Some("\\f"),
            _ => None,
        };
        if esc.is_some() || b < 0x20 {
            out.push_str(&s[start..i]);
            match esc {
                Some(e) => out.push_str(e),
                None => {
                    let _ = write!(out, "\\u{:04x}", b);
                }
            }
            start = i + 1;
        }
    }
    out.push_str(&s[start..]);
    out.push('"');
}

#[derive(Debug, Clone, PartialEq)]
pub struct ParseError {
    pub message: String,
    pub offset: usize,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} at byte {}", self.message, self.offset)
    }
}

/// Parses a complete JSON document from bytes (must be valid UTF-8).
pub fn parse_bytes(input: &[u8]) -> Result<Json, ParseError> {
    match std::str::from_utf8(input) {
        Ok(s) => parse(s),
        Err(e) => Err(ParseError { message: "invalid UTF-8".into(), offset: e.valid_up_to() }),
    }
}

/// Parses a complete JSON document.
pub fn parse(input: &str) -> Result<Json, ParseError> {
    let mut p = Parser { s: input.as_bytes(), src: input, pos: 0 };
    p.skip_ws();
    let v = p.value(0)?;
    p.skip_ws();
    if p.pos != p.s.len() {
        return Err(p.err("trailing characters"));
    }
    Ok(v)
}

struct Parser<'a> {
    s: &'a [u8],
    src: &'a str,
    pos: usize,
}

impl<'a> Parser<'a> {
    fn err(&self, msg: &str) -> ParseError {
        ParseError { message: msg.to_string(), offset: self.pos }
    }

    fn skip_ws(&mut self) {
        while self.pos < self.s.len() && matches!(self.s[self.pos], b' ' | b'\t' | b'\n' | b'\r') {
            self.pos += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.pos).copied()
    }

    fn value(&mut self, depth: usize) -> Result<Json, ParseError> {
        if depth > MAX_DEPTH {
            return Err(self.err("nesting too deep"));
        }
        match self.peek() {
            None => Err(self.err("unexpected end of input")),
            Some(b'{') => {
                self.pos += 1;
                let mut members = Vec::new();
                self.skip_ws();
                if self.peek() == Some(b'}') {
                    self.pos += 1;
                    return Ok(Json::Object(members));
                }
                loop {
                    self.skip_ws();
                    if self.peek() != Some(b'"') {
                        return Err(self.err("expected object key"));
                    }
                    let key = self.string()?;
                    self.skip_ws();
                    if self.peek() != Some(b':') {
                        return Err(self.err("expected ':'"));
                    }
                    self.pos += 1;
                    self.skip_ws();
                    let v = self.value(depth + 1)?;
                    members.push((key, v));
                    self.skip_ws();
                    match self.peek() {
                        Some(b',') => self.pos += 1,
                        Some(b'}') => {
                            self.pos += 1;
                            return Ok(Json::Object(members));
                        }
                        _ => return Err(self.err("expected ',' or '}'")),
                    }
                }
            }
            Some(b'[') => {
                self.pos += 1;
                let mut items = Vec::new();
                self.skip_ws();
                if self.peek() == Some(b']') {
                    self.pos += 1;
                    return Ok(Json::Array(items));
                }
                loop {
                    self.skip_ws();
                    items.push(self.value(depth + 1)?);
                    self.skip_ws();
                    match self.peek() {
                        Some(b',') => self.pos += 1,
                        Some(b']') => {
                            self.pos += 1;
                            return Ok(Json::Array(items));
                        }
                        _ => return Err(self.err("expected ',' or ']'")),
                    }
                }
            }
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => self.literal("true", Json::Bool(true)),
            Some(b'f') => self.literal("false", Json::Bool(false)),
            Some(b'n') => self.literal("null", Json::Null),
            Some(b'-') | Some(b'0'..=b'9') => self.number(),
            Some(_) => Err(self.err("unexpected character")),
        }
    }

    fn literal(&mut self, word: &str, v: Json) -> Result<Json, ParseError> {
        if self.s[self.pos..].starts_with(word.as_bytes()) {
            self.pos += word.len();
            Ok(v)
        } else {
            Err(self.err("invalid literal"))
        }
    }

    fn number(&mut self) -> Result<Json, ParseError> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        let int_start = self.pos;
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.pos += 1;
        }
        if self.pos == int_start {
            return Err(self.err("invalid number"));
        }
        if self.s[int_start] == b'0' && self.pos - int_start > 1 {
            return Err(self.err("leading zeros not allowed"));
        }
        let mut is_float = false;
        if self.peek() == Some(b'.') {
            self.pos += 1;
            let frac_start = self.pos;
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
            if self.pos == frac_start {
                return Err(self.err("invalid number"));
            }
            is_float = true;
        }
        if matches!(self.peek(), Some(b'e') | Some(b'E')) {
            self.pos += 1;
            if matches!(self.peek(), Some(b'+') | Some(b'-')) {
                self.pos += 1;
            }
            let exp_start = self.pos;
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
            if self.pos == exp_start {
                return Err(self.err("invalid number"));
            }
            is_float = true;
        }
        let text = &self.src[start..self.pos];
        if is_float {
            text.parse::<f64>().map(Json::Float).map_err(|_| self.err("invalid number"))
        } else {
            match text.parse::<i64>() {
                Ok(i) => Ok(Json::Int(i)),
                Err(_) => Ok(Json::BigInt(text.to_string())),
            }
        }
    }

    fn hex4(&mut self) -> Result<u32, ParseError> {
        if self.pos + 4 > self.s.len() {
            return Err(self.err("invalid unicode escape"));
        }
        let mut v = 0u32;
        for i in 0..4 {
            let c = self.s[self.pos + i];
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => return Err(self.err("invalid unicode escape")),
            };
            v = v * 16 + d as u32;
        }
        self.pos += 4;
        Ok(v)
    }

    fn string(&mut self) -> Result<String, ParseError> {
        // Assumes current byte is '"'.
        self.pos += 1;
        let mut out = String::new();
        let mut start = self.pos;
        loop {
            match self.peek() {
                None => return Err(self.err("unterminated string")),
                Some(b'"') => {
                    out.push_str(&self.src[start..self.pos]);
                    self.pos += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    out.push_str(&self.src[start..self.pos]);
                    self.pos += 1;
                    let c = self.peek().ok_or_else(|| self.err("unterminated string"))?;
                    self.pos += 1;
                    match c {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let hi = self.hex4()?;
                            let cp = if (0xD800..0xDC00).contains(&hi) {
                                if self.s[self.pos..].starts_with(b"\\u") {
                                    let save = self.pos;
                                    self.pos += 2;
                                    let lo = self.hex4()?;
                                    if (0xDC00..0xE000).contains(&lo) {
                                        0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00)
                                    } else {
                                        self.pos = save;
                                        0xFFFD
                                    }
                                } else {
                                    0xFFFD
                                }
                            } else if (0xDC00..0xE000).contains(&hi) {
                                0xFFFD
                            } else {
                                hi
                            };
                            out.push(char::from_u32(cp).unwrap_or('\u{FFFD}'));
                        }
                        _ => return Err(self.err("invalid escape")),
                    }
                    start = self.pos;
                }
                Some(c) if c < 0x20 => return Err(self.err("control character in string")),
                Some(_) => self.pos += 1,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let src = r#"{"a":[1,2.5,"x\né😀"],"b":null,"c":true,"d":123456789012345678901234567890}"#;
        let v = parse(src).unwrap();
        assert_eq!(v.get("d"), Some(&Json::BigInt("123456789012345678901234567890".into())));
        let s = v.to_string();
        assert_eq!(parse(&s).unwrap(), v);
    }

    #[test]
    fn deep_nesting_rejected() {
        let s = "[".repeat(10_000);
        assert!(parse(&s).is_err());
    }
}
