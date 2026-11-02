#![allow(non_camel_case_types)]
//! C ABI (`harness/liquid_fc.h`) over the `liquid` library.
//!
//! Every entry point catches panics so nothing unwinds into C, validates its
//! pointers, and treats inputs as untrusted bytes.

use liquid::convert::json_to_value;
use liquid::json::{self, Json};
use liquid::{Error, FileSystem, Hash, MemoryFileSystem, RenderOptions, Template, Value};
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;

#[repr(C)]
pub struct lfc_string {
    pub ptr: *mut c_char,
    pub len: usize,
}

impl lfc_string {
    fn empty() -> lfc_string {
        lfc_string { ptr: std::ptr::null_mut(), len: 0 }
    }

    fn from_string(s: String) -> lfc_string {
        if s.is_empty() {
            return lfc_string::empty();
        }
        let b: Box<[u8]> = s.into_bytes().into_boxed_slice();
        let len = b.len();
        let ptr = Box::into_raw(b) as *mut u8 as *mut c_char;
        lfc_string { ptr, len }
    }
}

/// Opaque parsed template.
pub struct lfc_template {
    template: Template,
}

/// Opaque render inputs.
pub struct lfc_context {
    assigns: Rc<Hash>,
    fs: MemoryFileSystem,
}

unsafe fn bytes<'a>(ptr: *const c_char, len: usize) -> &'a [u8] {
    if ptr.is_null() || len == 0 {
        &[]
    } else {
        // SAFETY: the caller guarantees `ptr` points to `len` readable bytes.
        unsafe { std::slice::from_raw_parts(ptr as *const u8, len) }
    }
}

unsafe fn set(out: *mut lfc_string, s: String) {
    if !out.is_null() {
        // SAFETY: `out` is a valid, writable lfc_string supplied by the caller.
        unsafe { out.write(lfc_string::from_string(s)) };
    }
}

unsafe fn clear(out: *mut lfc_string) {
    if !out.is_null() {
        // SAFETY: as above.
        unsafe { out.write(lfc_string::empty()) };
    }
}

fn marker(j: &Json) -> Option<Value> {
    let name = j.get("_instantiate")?.as_str()?;
    let params = j.get("params").map(|p| json_to_value(p, &mut marker));
    liquid::test_drops::instantiate(name, params.as_ref()).or(Some(Value::Nil))
}

/// Parses a template. Returns NULL on a syntax error, with the message in `*error`.
///
/// # Safety
/// `source` must point to `source_len` readable bytes (or be NULL with length 0);
/// `error` must be NULL or point to a writable `lfc_string`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lfc_parse(source: *const c_char, source_len: usize, error: *mut lfc_string) -> *mut lfc_template {
    // SAFETY: forwarded caller guarantees.
    unsafe { clear(error) };
    let r = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: forwarded caller guarantees.
        let src = unsafe { bytes(source, source_len) };
        Template::parse_bytes(src)
    }));
    match r {
        Ok(Ok(t)) => Box::into_raw(Box::new(lfc_template { template: t })),
        Ok(Err(e)) => {
            // SAFETY: forwarded caller guarantees.
            unsafe { set(error, e.to_string()) };
            std::ptr::null_mut()
        }
        Err(_) => {
            // SAFETY: forwarded caller guarantees.
            unsafe { set(error, "Liquid error: internal".to_string()) };
            std::ptr::null_mut()
        }
    }
}

fn build_context(assigns: &[u8], partials: &[u8]) -> Result<lfc_context, String> {
    let aj = json::parse_bytes(assigns).map_err(|e| format!("invalid assigns JSON: {}", e))?;
    let Json::Object(_) = aj else {
        return Err("invalid assigns JSON: expected an object".into());
    };
    let assigns = match json_to_value(&aj, &mut marker) {
        Value::Hash(h) => h,
        _ => return Err("invalid assigns JSON: expected an object".into()),
    };
    let mut fs = MemoryFileSystem::new();
    if !partials.is_empty() {
        let pj = json::parse_bytes(partials).map_err(|e| format!("invalid partials JSON: {}", e))?;
        match pj {
            Json::Object(members) => {
                for (k, v) in members {
                    match v {
                        Json::Str(src) => fs.insert(&k, src),
                        _ => return Err(format!("invalid partials JSON: value for {:?} is not a string", k)),
                    }
                }
            }
            Json::Null => {}
            _ => return Err("invalid partials JSON: expected an object".into()),
        }
    }
    Ok(lfc_context { assigns, fs })
}

/// Builds render inputs from JSON. Returns NULL on invalid input.
///
/// # Safety
/// Each buffer must point to its length in readable bytes (or be NULL with
/// length 0); `error` must be NULL or point to a writable `lfc_string`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lfc_context_new(
    assigns_json: *const c_char,
    assigns_len: usize,
    partials_json: *const c_char,
    partials_len: usize,
    error: *mut lfc_string,
) -> *mut lfc_context {
    // SAFETY: forwarded caller guarantees.
    unsafe { clear(error) };
    let r = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: forwarded caller guarantees.
        let (a, p) = unsafe { (bytes(assigns_json, assigns_len), bytes(partials_json, partials_len)) };
        build_context(a, p)
    }));
    match r {
        Ok(Ok(c)) => Box::into_raw(Box::new(c)),
        Ok(Err(msg)) => {
            // SAFETY: forwarded caller guarantees.
            unsafe { set(error, msg) };
            std::ptr::null_mut()
        }
        Err(_) => {
            // SAFETY: forwarded caller guarantees.
            unsafe { set(error, "internal error".to_string()) };
            std::ptr::null_mut()
        }
    }
}

/// Renders with errors raised (`strict_errors: true`). Returns 0 on success.
///
/// # Safety
/// `tpl` and `ctx` must be NULL or values returned by this library and not yet
/// freed; `output` and `error` must be NULL or point to writable `lfc_string`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lfc_render(
    tpl: *const lfc_template,
    ctx: *const lfc_context,
    output: *mut lfc_string,
    error: *mut lfc_string,
) -> c_int {
    // SAFETY: forwarded caller guarantees.
    unsafe {
        clear(output);
        clear(error);
    }
    if tpl.is_null() || ctx.is_null() {
        // SAFETY: forwarded caller guarantees.
        unsafe { set(error, "Liquid error: invalid argument".to_string()) };
        return 1;
    }
    // SAFETY: non-null pointers from lfc_parse / lfc_context_new.
    let (t, c) = unsafe { (&*tpl, &*ctx) };
    let r = catch_unwind(AssertUnwindSafe(|| {
        let opts = RenderOptions {
            strict_errors: true,
            file_system: Some(&c.fs as &dyn FileSystem),
            ..RenderOptions::default()
        };
        t.template.render_shared(c.assigns.clone(), &opts)
    }));
    match r {
        Ok(Ok(res)) => {
            // SAFETY: forwarded caller guarantees.
            unsafe { set(output, res.output) };
            0
        }
        Ok(Err(e)) => {
            // SAFETY: forwarded caller guarantees.
            unsafe { set(error, e.to_string()) };
            1
        }
        Err(_) => {
            let e: Error = Error::new(liquid::ErrorKind::Internal, "internal");
            // SAFETY: forwarded caller guarantees.
            unsafe { set(error, e.to_string()) };
            1
        }
    }
}

/// # Safety
/// `tpl` must be NULL or a template returned by `lfc_parse`, freed only once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lfc_template_free(tpl: *mut lfc_template) {
    if !tpl.is_null() {
        // SAFETY: allocated by Box::into_raw in lfc_parse.
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(tpl) })));
    }
}

/// # Safety
/// `ctx` must be NULL or a context returned by `lfc_context_new`, freed only once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lfc_context_free(ctx: *mut lfc_context) {
    if !ctx.is_null() {
        // SAFETY: allocated by Box::into_raw in lfc_context_new.
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(ctx) })));
    }
}

/// # Safety
/// `s` must be `{NULL, 0}` or a string returned by this library, freed only once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lfc_string_free(s: lfc_string) {
    if !s.ptr.is_null() && s.len > 0 {
        // SAFETY: allocated as a Box<[u8]> of exactly `len` bytes.
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(s.ptr as *mut u8, s.len)) })
        }));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        unsafe {
            let src = "Hi {{ name | upcase }}{% include 'p' %}";
            let mut err = lfc_string::empty();
            let t = lfc_parse(src.as_ptr() as *const c_char, src.len(), &mut err);
            assert!(!t.is_null());
            let a = r#"{"name":"bob"}"#;
            let p = r#"{"p":"!"}"#;
            let c = lfc_context_new(a.as_ptr() as _, a.len(), p.as_ptr() as _, p.len(), &mut err);
            assert!(!c.is_null());
            let mut out = lfc_string::empty();
            assert_eq!(lfc_render(t, c, &mut out, &mut err), 0);
            let s = std::slice::from_raw_parts(out.ptr as *const u8, out.len);
            assert_eq!(s, b"Hi BOB!");
            lfc_string_free(out);
            lfc_context_free(c);
            lfc_template_free(t);

            let bad = "{% if %}";
            let t = lfc_parse(bad.as_ptr() as _, bad.len(), &mut err);
            assert!(t.is_null());
            assert!(err.len > 0);
            lfc_string_free(err);
        }
    }
}
