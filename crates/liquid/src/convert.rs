//! Conversion from JSON documents to Liquid values.

use crate::bigint::BigInt;
use crate::json::Json;
use crate::value::{Hash, Value};
use std::rc::Rc;

/// A hook that may turn special JSON objects (drop markers) into values.
pub type MarkerHook<'a> = dyn FnMut(&Json) -> Option<Value> + 'a;

/// Converts JSON into a Liquid value. Objects for which `hook` returns a
/// value are replaced by it (used for drop markers).
pub fn json_to_value(j: &Json, hook: &mut MarkerHook) -> Value {
    match j {
        Json::Null => Value::Nil,
        Json::Bool(b) => Value::Bool(*b),
        Json::Int(i) => Value::Int(*i),
        Json::BigInt(s) => match BigInt::parse(s) {
            Some(b) => Value::from_bigint(b),
            None => Value::Float(s.parse().unwrap_or(0.0)),
        },
        Json::Float(f) => Value::Float(*f),
        Json::Str(s) => Value::str(s),
        Json::Array(items) => Value::Array(Rc::new(items.iter().map(|x| json_to_value(x, hook)).collect())),
        Json::Object(members) => {
            if let Some(v) = hook(j) {
                return v;
            }
            let mut h = Hash::with_capacity(members.len());
            for (k, v) in members {
                h.insert(Rc::from(k.as_str()), json_to_value(v, hook));
            }
            Value::Hash(Rc::new(h))
        }
    }
}

/// Converts a JSON object into the top-level assigns hash.
pub fn json_to_assigns(j: &Json, hook: &mut MarkerHook) -> Option<Hash> {
    match json_to_value(j, hook) {
        Value::Hash(h) => Some(Rc::try_unwrap(h).unwrap_or_else(|rc| (*rc).clone())),
        Value::Nil => Some(Hash::new()),
        _ => None,
    }
}

/// Converts a Liquid value into JSON (drops become their string form).
pub fn value_to_json(v: &Value, depth: usize) -> Json {
    if depth > 100 {
        return Json::Null;
    }
    match v {
        Value::Nil => Json::Null,
        Value::Bool(b) => Json::Bool(*b),
        Value::Int(i) => Json::Int(*i),
        Value::BigInt(b) => Json::BigInt(b.to_string()),
        Value::Float(f) => Json::Float(*f),
        Value::Str(s) => Json::Str(s.to_string()),
        Value::Array(a) => Json::Array(a.iter().map(|x| value_to_json(x, depth + 1)).collect()),
        Value::Hash(h) => Json::Object(h.iter().map(|(k, x)| (k.to_string(), value_to_json(x, depth + 1))).collect()),
        Value::Range(a, b) => Json::Str(format!("{}..{}", a, b)),
        Value::Drop(d) => Json::Str(d.to_s()),
    }
}
