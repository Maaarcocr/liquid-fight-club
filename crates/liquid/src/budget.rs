//! Work accounting for operations that run outside the renderer's loop
//! (big-integer arithmetic, collection flattening, string building).
//!
//! Costs accumulate in a thread-local counter that the renderer drains into
//! its step budget, so hostile templates cannot burn unbounded CPU inside a
//! single filter call repeated many times.

use std::cell::Cell;

thread_local! {
    static WORK: Cell<u64> = const { Cell::new(0) };
}

/// Records `n` units of work (roughly: one unit per element or per 16 bytes).
#[inline]
pub(crate) fn charge(n: u64) {
    WORK.with(|w| w.set(w.get().saturating_add(n)));
}

/// Returns and resets the accumulated work.
#[inline]
pub(crate) fn take() -> u64 {
    WORK.with(|w| w.replace(0))
}
