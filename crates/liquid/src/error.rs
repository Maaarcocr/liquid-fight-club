//! Liquid errors.
//!
//! Errors carry the same pieces as Ruby's `Liquid::Error`: a kind (which
//! decides the "Liquid syntax error" / "Liquid error" prefix), a message, an
//! optional line number, template name and markup context.

use std::fmt;
use std::rc::Rc;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorKind {
    /// `Liquid::SyntaxError` (parse errors).
    Syntax,
    /// `Liquid::ArgumentError`.
    Argument,
    /// Plain `Liquid::Error` and other render errors.
    Standard,
    ZeroDivision,
    FloatDomain,
    /// "Nesting too deep".
    StackLevel,
    /// "Memory limits exceeded" (aborts the whole render).
    Memory,
    FileSystem,
    Disabled,
    Internal,
    TemplateEncoding,
}

#[derive(Debug, Clone)]
pub struct Error {
    pub kind: ErrorKind,
    pub message: String,
    pub line: Option<u32>,
    pub template_name: Option<Rc<str>>,
    pub markup_context: Option<String>,
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    pub fn new(kind: ErrorKind, message: impl Into<String>) -> Error {
        Error { kind, message: message.into(), line: None, template_name: None, markup_context: None }
    }

    pub fn syntax(message: impl Into<String>) -> Error {
        Error::new(ErrorKind::Syntax, message)
    }

    pub fn argument(message: impl Into<String>) -> Error {
        Error::new(ErrorKind::Argument, message)
    }

    pub fn standard(message: impl Into<String>) -> Error {
        Error::new(ErrorKind::Standard, message)
    }

    pub fn memory() -> Error {
        Error::new(ErrorKind::Memory, "Memory limits exceeded")
    }

    pub fn nesting() -> Error {
        Error::new(ErrorKind::StackLevel, "Nesting too deep")
    }

    pub fn is_syntax(&self) -> bool {
        self.kind == ErrorKind::Syntax
    }

    pub fn with_line(mut self, line: Option<u32>) -> Error {
        if self.line.is_none() {
            self.line = line;
        }
        self
    }

    /// The prefix Ruby puts in front of the message, e.g. `Liquid error (line 3): `.
    pub fn prefix(&self) -> String {
        let mut s = String::from(if self.kind == ErrorKind::Syntax { "Liquid syntax error" } else { "Liquid error" });
        if let Some(line) = self.line {
            s.push_str(" (");
            if let Some(name) = &self.template_name {
                s.push_str(name);
                s.push(' ');
            }
            s.push_str("line ");
            s.push_str(&line.to_string());
            s.push(')');
        }
        s.push_str(": ");
        s
    }

    /// The message without the prefix but with the markup context.
    pub fn message_with_context(&self) -> String {
        let mut s = self.message.clone();
        if let Some(ctx) = &self.markup_context {
            s.push(' ');
            s.push_str(ctx);
        }
        s
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.prefix())?;
        f.write_str(&self.message_with_context())
    }
}

impl std::error::Error for Error {}
