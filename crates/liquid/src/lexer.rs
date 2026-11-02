//! Expression lexer and the strict token parser used inside tags and outputs.
//!
//! This mirrors `Liquid::Lexer` and `Liquid::Parser`. Like the reference,
//! [`Parser::expression`] returns the *markup string* of an expression, which
//! [`crate::expression::parse`] then turns into an expression tree. Keeping
//! that round trip makes odd inputs behave exactly like the reference.

use crate::error::{Error, Result};
use crate::value::write_string_inspect;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tok {
    Comparison,
    Id,
    Number,
    Str,
    Dot,
    DotDot,
    Colon,
    Comma,
    OpenSquare,
    CloseSquare,
    OpenRound,
    CloseRound,
    Pipe,
    Question,
    Dash,
    Eos,
}

impl Tok {
    pub fn name(self) -> &'static str {
        match self {
            Tok::Comparison => "comparison",
            Tok::Id => "id",
            Tok::Number => "number",
            Tok::Str => "string",
            Tok::Dot => "dot",
            Tok::DotDot => "dotdot",
            Tok::Colon => "colon",
            Tok::Comma => "comma",
            Tok::OpenSquare => "open_square",
            Tok::CloseSquare => "close_square",
            Tok::OpenRound => "open_round",
            Tok::CloseRound => "close_round",
            Tok::Pipe => "pipe",
            Tok::Question => "question",
            Tok::Dash => "dash",
            Tok::Eos => "end_of_string",
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Token<'a> {
    pub kind: Tok,
    pub text: &'a str,
}

impl Token<'_> {
    /// Ruby's `token.inspect`, e.g. `[:comma, ","]` or `[:end_of_string]`.
    pub fn inspect(&self) -> String {
        if self.kind == Tok::Eos {
            return "[:end_of_string]".to_string();
        }
        let mut s = format!("[:{}, ", self.kind.name());
        write_string_inspect(self.text, &mut s);
        s.push(']');
        s
    }
}

/// Ruby's `\s` (no vertical tab handling differences: Onigmo includes \v).
#[inline]
pub fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\r' | b'\n' | 0x0c | 0x0b)
}

#[inline]
pub fn is_word(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// Tokenizes expression markup (`Liquid::Lexer.tokenize`).
pub fn tokenize(src: &str) -> Result<Vec<Token<'_>>> {
    let s = src.as_bytes();
    let mut out: Vec<Token> = Vec::new();
    let mut i = 0;
    let n = s.len();
    while i < n {
        while i < n && is_space(s[i]) {
            i += 1;
        }
        if i >= n {
            break;
        }
        let start = i;
        let c = s[i];
        let simple = |k: Tok| Some(k);
        let special = match c {
            b'|' => simple(Tok::Pipe),
            b'.' => simple(Tok::Dot),
            b':' => simple(Tok::Colon),
            b',' => simple(Tok::Comma),
            b'[' => simple(Tok::OpenSquare),
            b']' => simple(Tok::CloseSquare),
            b'(' => simple(Tok::OpenRound),
            b')' => simple(Tok::CloseRound),
            b'?' => simple(Tok::Question),
            b'-' => simple(Tok::Dash),
            _ => None,
        };
        if let Some(kind) = special {
            i += 1;
            if kind == Tok::Dot && i < n && s[i] == b'.' {
                i += 1;
                out.push(Token { kind: Tok::DotDot, text: &src[start..i] });
            } else if kind == Tok::Dash && i < n && s[i].is_ascii_digit() {
                i = scan_number(s, start);
                out.push(Token { kind: Tok::Number, text: &src[start..i] });
            } else {
                out.push(Token { kind, text: &src[start..i] });
            }
            continue;
        }
        match c {
            b'=' | b'!' => {
                if i + 1 < n && s[i + 1] == b'=' {
                    i += 2;
                    out.push(Token { kind: Tok::Comparison, text: &src[start..i] });
                } else {
                    return Err(unexpected_char(src, start));
                }
            }
            b'<' => {
                if i + 1 < n && (s[i + 1] == b'=' || s[i + 1] == b'>') {
                    i += 2;
                } else {
                    i += 1;
                }
                out.push(Token { kind: Tok::Comparison, text: &src[start..i] });
            }
            b'>' => {
                if i + 1 < n && s[i + 1] == b'=' {
                    i += 2;
                } else {
                    i += 1;
                }
                out.push(Token { kind: Tok::Comparison, text: &src[start..i] });
            }
            b'a'..=b'z' | b'A'..=b'Z' | b'_' => {
                i += 1;
                while i < n && (is_word(s[i]) || s[i] == b'-') {
                    i += 1;
                }
                if i < n && s[i] == b'?' {
                    i += 1;
                }
                let text = &src[start..i];
                if text == "contains" && out.last().is_none_or(|t| t.kind != Tok::Dot) {
                    out.push(Token { kind: Tok::Comparison, text });
                } else {
                    out.push(Token { kind: Tok::Id, text });
                }
            }
            b'0'..=b'9' => {
                i = scan_number(s, start);
                out.push(Token { kind: Tok::Number, text: &src[start..i] });
            }
            b'\'' | b'"' => {
                let q = c;
                let mut j = i + 1;
                while j < n && s[j] != q {
                    j += 1;
                }
                if j >= n {
                    return Err(unexpected_char(src, start));
                }
                i = j + 1;
                out.push(Token { kind: Tok::Str, text: &src[start..i] });
            }
            _ => return Err(unexpected_char(src, start)),
        }
    }
    out.push(Token { kind: Tok::Eos, text: "" });
    Ok(out)
}

/// `-?\d+(\.\d+)?` starting at `start`.
fn scan_number(s: &[u8], start: usize) -> usize {
    let n = s.len();
    let mut i = start;
    if i < n && s[i] == b'-' {
        i += 1;
    }
    while i < n && s[i].is_ascii_digit() {
        i += 1;
    }
    if i + 1 < n && s[i] == b'.' && s[i + 1].is_ascii_digit() {
        i += 1;
        while i < n && s[i].is_ascii_digit() {
            i += 1;
        }
    }
    i
}

fn unexpected_char(src: &str, pos: usize) -> Error {
    let ch = src[pos..].chars().next().unwrap_or(' ');
    Error::syntax(format!("Unexpected character {}", ch))
}

/// The strict token parser (`Liquid::Parser`).
pub struct Parser<'a> {
    tokens: Vec<Token<'a>>,
    p: usize,
    reject_bare_brackets: bool,
}

/// Maximum nesting of expressions (brackets/ranges) inside one markup.
const MAX_EXPR_DEPTH: usize = 64;

impl<'a> Parser<'a> {
    pub fn new(markup: &'a str) -> Result<Parser<'a>> {
        Ok(Parser { tokens: tokenize(markup)?, p: 0, reject_bare_brackets: true })
    }

    fn tok(&self, i: usize) -> Token<'a> {
        *self.tokens.get(i).unwrap_or(self.tokens.last().expect("eos"))
    }

    pub fn current(&self) -> Token<'a> {
        self.tok(self.p)
    }

    pub fn consume(&mut self, kind: Tok) -> Result<&'a str> {
        let t = self.tok(self.p);
        if t.kind != kind {
            return Err(Error::syntax(format!("Expected {} but found {}", kind.name(), t.kind.name())));
        }
        self.p += 1;
        Ok(t.text)
    }

    /// Consumes the current token whatever it is.
    pub fn consume_any(&mut self) -> &'a str {
        let t = self.tok(self.p);
        self.p += 1;
        t.text
    }

    pub fn consume_if(&mut self, kind: Tok) -> Option<&'a str> {
        let t = self.tok(self.p);
        if t.kind == kind && self.p < self.tokens.len() {
            self.p += 1;
            Some(t.text)
        } else {
            None
        }
    }

    /// Consumes an identifier with the given text.
    pub fn id_is(&mut self, word: &str) -> bool {
        let t = self.tok(self.p);
        if t.kind == Tok::Id && t.text == word {
            self.p += 1;
            true
        } else {
            false
        }
    }

    pub fn look(&self, kind: Tok) -> bool {
        self.look_ahead(kind, 0)
    }

    pub fn look_ahead(&self, kind: Tok, ahead: usize) -> bool {
        match self.tokens.get(self.p + ahead) {
            Some(t) => t.kind == kind,
            None => false,
        }
    }

    /// Parses one expression and returns its normalized markup.
    pub fn expression(&mut self) -> Result<String> {
        let mut out = String::new();
        self.expression_into(&mut out, 0)?;
        Ok(out)
    }

    fn expression_into(&mut self, out: &mut String, depth: usize) -> Result<()> {
        if depth > MAX_EXPR_DEPTH {
            return Err(Error::nesting());
        }
        let t = self.current();
        match t.kind {
            Tok::Id => {
                out.push_str(self.consume_any());
                self.variable_lookups(out, depth)?;
            }
            Tok::OpenSquare => {
                if self.reject_bare_brackets {
                    return Err(Error::syntax("Bare bracket access is not allowed. Use self['...'] instead"));
                }
                out.push_str(self.consume_any());
                self.expression_into(out, depth + 1)?;
                out.push_str(self.consume(Tok::CloseSquare)?);
                self.variable_lookups(out, depth)?;
            }
            Tok::Str | Tok::Number => out.push_str(self.consume_any()),
            Tok::OpenRound => {
                self.consume_any();
                let mut first = String::new();
                self.expression_into(&mut first, depth + 1)?;
                self.consume(Tok::DotDot)?;
                let mut last = String::new();
                self.expression_into(&mut last, depth + 1)?;
                self.consume(Tok::CloseRound)?;
                out.push('(');
                out.push_str(&first);
                out.push_str("..");
                out.push_str(&last);
                out.push(')');
            }
            _ => return Err(Error::syntax(format!("{} is not a valid expression", t.inspect()))),
        }
        Ok(())
    }

    fn variable_lookups(&mut self, out: &mut String, depth: usize) -> Result<()> {
        loop {
            if self.look(Tok::OpenSquare) {
                out.push_str(self.consume_any());
                self.expression_into(out, depth + 1)?;
                out.push_str(self.consume(Tok::CloseSquare)?);
            } else if self.look(Tok::Dot) {
                out.push_str(self.consume_any());
                out.push_str(self.consume(Tok::Id)?);
            } else {
                return Ok(());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lex() {
        let t = tokenize("a.b[0] | f: 'x', k: -1.5 contains x.contains").unwrap();
        let kinds: Vec<_> = t.iter().map(|t| t.kind).collect();
        assert_eq!(
            kinds,
            vec![
                Tok::Id,
                Tok::Dot,
                Tok::Id,
                Tok::OpenSquare,
                Tok::Number,
                Tok::CloseSquare,
                Tok::Pipe,
                Tok::Id,
                Tok::Colon,
                Tok::Str,
                Tok::Comma,
                Tok::Id,
                Tok::Colon,
                Tok::Number,
                Tok::Comparison,
                Tok::Id,
                Tok::Dot,
                Tok::Id,
                Tok::Eos
            ]
        );
    }

    #[test]
    fn expr_markup() {
        let mut p = Parser::new("a . b [ 'c' ] (1 .. x)").unwrap();
        assert_eq!(p.expression().unwrap(), "a.b['c']");
        assert_eq!(p.expression().unwrap(), "(1..x)");
    }
}
