//! Drops: host objects whose properties are computed on access.
//!
//! A drop behaves like Ruby's `Liquid::Drop`: `drop[key]` invokes a method,
//! `key?` is always true, and it may materialize to a primitive through
//! `to_liquid_value` / `to_liquid`. The default method bodies follow
//! `Liquid::Drop`'s behavior so implementations only override what they need.

use crate::error::Result;
use crate::value::Value;
use std::cmp::Ordering;

pub trait Drop {
    /// Ruby class name (used for the default `to_s`/`inspect` and error messages).
    fn class_name(&self) -> &str;

    /// `drop[key]` / `invoke_drop(key)`. Unknown keys return nil.
    fn get(&self, key: &Value) -> Result<Value>;

    /// `to_liquid`: drops that turn into another value (e.g. nil) return it here.
    fn to_liquid(&self) -> Option<Value> {
        None
    }

    /// `to_liquid_value`: lets the drop act as a primitive in conditions.
    fn to_liquid_value(&self) -> Option<Value> {
        None
    }

    /// `to_number`, used by numeric filters.
    fn to_number(&self) -> Option<Value> {
        None
    }

    fn to_s(&self) -> String {
        self.class_name().to_string()
    }

    fn inspect(&self) -> String {
        self.class_name().to_string()
    }

    /// Items when the drop is enumerable (`each`), or None.
    fn iterate(&self) -> Option<Result<Vec<Value>>> {
        None
    }

    /// Whether `drop.respond_to?(name)` for command-style lookups such as
    /// `size`, `first` and `last` (beyond what `get` already answers).
    fn responds_to(&self, _method: &str) -> bool {
        false
    }

    /// `drop == other`. Defaults to identity, decided by the caller.
    fn equals(&self, _other: &Value) -> bool {
        false
    }

    /// `drop <=> other`.
    fn compare(&self, _other: &Value) -> Option<Ordering> {
        None
    }

    /// Whether the drop defines comparison operators (Comparable).
    fn is_comparable(&self) -> bool {
        false
    }

    fn is_empty(&self) -> bool {
        false
    }

    fn is_blank(&self) -> bool {
        false
    }

    /// Downcast support for engine-internal drops.
    fn as_any(&self) -> Option<&dyn std::any::Any> {
        None
    }
}
