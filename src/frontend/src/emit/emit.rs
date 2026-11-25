//! JSON helpers matching Python's `json.dumps(..., indent=2)` output.

use std::fmt::Write;

/// Python's `round()` for floats: round half to even (banker's rounding).
pub(crate) fn round_py(value: f64) -> i64 {
    if !value.is_finite() {
        return 0;
    }
    value.round_ties_even() as i64
}

/// Float formatted like Python's `repr()` (always keeps `.0` for integral
/// values, because the original value was a float).
pub(crate) fn py_float(value: f64) -> String {
    let mut text = format!("{value}");
    if !text.contains('.') && !text.contains('e') && !text.contains('E') {
        text.push_str(".0");
    }
    text
}

pub(crate) fn push_json_string(out: &mut String, value: &str) {
    out.push('"');
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            ch if (ch as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", ch as u32);
            }
            ch => out.push(ch),
        }
    }
    out.push('"');
}
