//! A Liquid template engine compatible with Shopify's Ruby reference
//! implementation (Liquid 5.13, strict2 parse mode).
//!
//! ```
//! use liquid::{Template, RenderOptions, Value};
//! let t = Template::parse("Hello {{ name | upcase }}!").unwrap();
//! let mut assigns = liquid::Hash::new();
//! assigns.insert("name".into(), Value::str("world"));
//! let out = t.render(assigns, &RenderOptions::default()).unwrap();
//! assert_eq!(out.output, "Hello WORLD!");
//! ```

pub mod bigint;
pub mod convert;
pub mod date;
pub mod drop;
pub mod error;
pub mod expression;
mod filters;
pub mod json;
mod lexer;
pub mod num;
mod parse;
mod render;
pub mod test_drops;
pub mod value;

pub use date::Timestamp;
pub use drop::Drop;
pub use error::{Error, ErrorKind, Result};
pub use render::{BlankFileSystem, FileSystem, Limits, RenderOptions, RenderResult};
pub use value::{Hash, Value};

use std::rc::Rc;

/// Maximum template source size accepted by the parser.
pub const MAX_SOURCE_BYTES: usize = 64 * 1024 * 1024;

/// A parsed template. Rendering never mutates it, so it can be rendered any
/// number of times.
pub struct Template {
    inner: parse::Template,
}

impl Template {
    /// Parses a template in strict2 mode with line numbers.
    pub fn parse(source: &str) -> Result<Template> {
        if source.len() > MAX_SOURCE_BYTES {
            return Err(Error::memory());
        }
        let inner = parse::parse_template(source, true, false)?;
        Ok(Template { inner })
    }

    /// Parses raw bytes; invalid UTF-8 is a template encoding error.
    pub fn parse_bytes(source: &[u8]) -> Result<Template> {
        match std::str::from_utf8(source) {
            Ok(s) => Template::parse(s),
            Err(_) => Err(Error::new(ErrorKind::TemplateEncoding, "Invalid template encoding")),
        }
    }

    /// Renders with the given variables.
    ///
    /// With `opts.strict_errors` the first render error is returned as `Err`;
    /// otherwise errors are rendered inline (`Liquid error (line N): ...`) and
    /// also listed in [`RenderResult::errors`].
    pub fn render(&self, assigns: Hash, opts: &RenderOptions) -> Result<RenderResult> {
        self.render_shared(Rc::new(assigns), opts)
    }

    /// Like [`Template::render`] but borrows shared assigns.
    pub fn render_shared(&self, assigns: Rc<Hash>, opts: &RenderOptions) -> Result<RenderResult> {
        render::render(&self.inner, assigns, opts)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn r(src: &str) -> String {
        let t = match Template::parse(src) {
            Ok(t) => t,
            Err(e) => return e.to_string(),
        };
        let opts = RenderOptions { strict_errors: false, ..RenderOptions::default() };
        t.render(Hash::new(), &opts).unwrap().output
    }

    #[test]
    fn basics() {
        assert_eq!(r("a{{ 'b' | upcase }}c"), "aBc");
        assert_eq!(r("{% assign x = 1 | plus: 2 %}{{ x }}"), "3");
        assert_eq!(r("{% for i in (1..3) %}{{ i }}{% endfor %}"), "123");
        assert_eq!(r("{% if 1 < 2 and 3 > 2 %}y{% else %}n{% endif %}"), "y");
        assert_eq!(r("{{ 1 | divided_by: 0 }}"), "Liquid error (line 1): divided by 0");
        assert_eq!(r("{% assign x = x ) %}"), "Liquid syntax error (line 1): Expected end_of_string but found close_round in \"x = x )\"");
        assert_eq!(r("{{ 0.1 | plus: 0.2 }}"), "0.3");
        assert_eq!(r("{% case 'a' %}{% when 'a' %}1{% when 'a' %}2{% endcase %}"), "12");
    }
}
