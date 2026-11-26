//! Python-compatible numeric helpers shared by the cascade and the blob writer.

/// Python's `round()` for floats: round half to even (banker's rounding).
pub(crate) fn round_py(value: f64) -> i64 {
    if !value.is_finite() {
        return 0;
    }
    value.round_ties_even() as i64
}
