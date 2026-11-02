//! Native versions of liquid-spec's portable test drops (`docs/test_drops.md`).
//! The JSON-RPC harness sends them as `{"_instantiate": Name, "params": {...}}`.

use crate::drop::Drop;
use crate::error::{Error, ErrorKind, Result};
use crate::value::{self, Value};
use std::cmp::Ordering;
use std::rc::Rc;

struct BooleanDrop(bool);
struct NumberDrop(Value);
struct StringDrop(Value);
struct MethodDrop;
struct IndexDrop;
struct SequenceDrop;
struct NilDrop;
struct OpaqueDrop;
struct ErrorDrop;
struct NestedDrop(Value);

impl Drop for BooleanDrop {
    fn class_name(&self) -> &str {
        "StandardBooleanDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        Ok(match key.as_str() {
            Some("to_liquid_value") => Value::Bool(self.0),
            _ => Value::Nil,
        })
    }
    fn to_liquid_value(&self) -> Option<Value> {
        Some(Value::Bool(self.0))
    }
    fn to_s(&self) -> String {
        if self.0 { "true".into() } else { "false".into() }
    }
}

impl Drop for NumberDrop {
    fn class_name(&self) -> &str {
        "StandardNumberDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        Ok(match key.as_str() {
            Some("to_liquid_value") | Some("to_number") => self.0.clone(),
            _ => Value::Nil,
        })
    }
    fn to_liquid_value(&self) -> Option<Value> {
        Some(self.0.clone())
    }
    fn to_number(&self) -> Option<Value> {
        Some(self.0.clone())
    }
    fn to_s(&self) -> String {
        value::to_s_string(&self.0)
    }
}

impl Drop for StringDrop {
    fn class_name(&self) -> &str {
        "StandardStringDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        Ok(match key.as_str() {
            Some("to_liquid_value") => self.0.clone(),
            _ => Value::Nil,
        })
    }
    fn to_liquid_value(&self) -> Option<Value> {
        Some(self.0.clone())
    }
    fn to_s(&self) -> String {
        value::to_s_string(&self.0)
    }
    fn compare(&self, other: &Value) -> Option<Ordering> {
        let o = value::to_s_string(other);
        match &self.0 {
            Value::Str(s) => Some(s.as_bytes().cmp(o.as_bytes())),
            _ => None,
        }
    }
    fn is_comparable(&self) -> bool {
        true
    }
}

impl Drop for MethodDrop {
    fn class_name(&self) -> &str {
        "StandardMethodDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        let name = value::to_s_string(key);
        if let Some((op, n)) = name.split_once('_') {
            if !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()) {
                let n: i128 = n.parse().unwrap_or(0);
                let r = match op {
                    "echo" => Some(n),
                    "square" => n.checked_mul(n),
                    "double" => n.checked_mul(2),
                    _ => None,
                };
                if let Some(r) = r {
                    return Ok(Value::from(r.to_string()));
                }
            }
        }
        Ok(Value::Nil)
    }
}

impl Drop for IndexDrop {
    fn class_name(&self) -> &str {
        "StandardIndexDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        const WORDS: [&str; 6] = ["zero", "one", "two", "three", "four", "five"];
        Ok(match key {
            Value::Int(i) => {
                let idx = if *i < 0 { 6 + i } else { *i };
                Value::str(if (0..6).contains(&idx) { WORDS[idx as usize] } else { "unknown" })
            }
            Value::Str(s) => Value::Str(s.clone()),
            _ => Value::str("unknown"),
        })
    }
}

const SEQUENCE: [&str; 3] = ["first", "second", "third"];

impl Drop for SequenceDrop {
    fn class_name(&self) -> &str {
        "StandardSequenceDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        let items = || Value::from(SEQUENCE.iter().map(|s| Value::str(s)).collect::<Vec<_>>());
        Ok(match key.as_str() {
            Some("size") | Some("count") => Value::Int(3),
            Some("first") | Some("min") => Value::str("first"),
            Some("last") | Some("max") => Value::str("third"),
            Some("sort") => items(),
            _ => Value::Nil,
        })
    }
    fn iterate(&self) -> Option<Result<Vec<Value>>> {
        Some(Ok(SEQUENCE.iter().map(|s| Value::str(s)).collect()))
    }
    fn responds_to(&self, method: &str) -> bool {
        matches!(method, "size" | "first" | "last")
    }
}

impl Drop for NilDrop {
    fn class_name(&self) -> &str {
        "StandardNilDrop"
    }
    fn get(&self, _key: &Value) -> Result<Value> {
        Ok(Value::Nil)
    }
    fn to_liquid(&self) -> Option<Value> {
        Some(Value::Nil)
    }
}

impl Drop for OpaqueDrop {
    fn class_name(&self) -> &str {
        "StandardOpaqueDrop"
    }
    fn get(&self, _key: &Value) -> Result<Value> {
        Ok(Value::Nil)
    }
    fn to_s(&self) -> String {
        "opaque".into()
    }
}

impl Drop for ErrorDrop {
    fn class_name(&self) -> &str {
        "StandardErrorDrop"
    }
    fn get(&self, _key: &Value) -> Result<Value> {
        Err(Error::new(ErrorKind::Standard, "ErrorDrop: access triggered an error"))
    }
}

impl Drop for NestedDrop {
    fn class_name(&self) -> &str {
        "StandardNestedDrop"
    }
    fn get(&self, key: &Value) -> Result<Value> {
        Ok(match key.as_str() {
            Some("to_liquid_value") => self.0.clone(),
            Some("nested") => Value::Drop(Rc::new(NestedDrop(self.0.clone()))),
            Some("square") => {
                let n = crate::num::to_number(&self.0);
                Value::Drop(Rc::new(NestedDrop(n.mul(&n)?.into_value())))
            }
            _ => Value::Nil,
        })
    }
    fn to_liquid_value(&self) -> Option<Value> {
        Some(self.0.clone())
    }
    fn to_s(&self) -> String {
        value::to_s_string(&self.0)
    }
}

/// Creates a standard test drop by its portable name.
pub fn instantiate(name: &str, params: Option<&Value>) -> Option<Value> {
    let param = |k: &str| -> Option<Value> {
        match params {
            Some(Value::Hash(h)) => h.get(k).cloned(),
            _ => None,
        }
    };
    let truthy_or = |k: &str, d: Value| -> Value {
        match param(k) {
            Some(v) if v.is_truthy() => v,
            _ => d,
        }
    };
    let d: Rc<dyn Drop> = match name {
        "BooleanDrop" => Rc::new(BooleanDrop(truthy_or("value", Value::Bool(false)).is_truthy())),
        "NumberDrop" => Rc::new(NumberDrop(truthy_or("value", Value::Int(0)))),
        "StringDrop" => Rc::new(StringDrop(truthy_or("value", Value::Nil))),
        "MethodDrop" => Rc::new(MethodDrop),
        "IndexDrop" => Rc::new(IndexDrop),
        "SequenceDrop" => Rc::new(SequenceDrop),
        "NilDrop" => Rc::new(NilDrop),
        "OpaqueDrop" => Rc::new(OpaqueDrop),
        "ErrorDrop" => Rc::new(ErrorDrop),
        "NestedDrop" => Rc::new(NestedDrop(truthy_or("value", Value::Int(3)))),
        _ => return None,
    };
    Some(Value::Drop(d))
}
