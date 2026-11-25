//! Serializacion JSON del IR, del atlas y de los timings, con el mismo
//! formato que `json.dumps(..., indent=2)` del frontend Python.

use std::fmt::Write;

use crate::layout::RectOp;
use crate::sha256;
use crate::text::{Atlas, TextOp};

/// `round()` de Python para floats: redondeo a par mas cercano (banker's).
pub fn round_py(value: f64) -> i64 {
    if !value.is_finite() {
        return 0;
    }
    value.round_ties_even() as i64
}

/// `round(value, digits)` de Python: cuantizacion decimal con par mas cercano.
pub fn round_digits(value: f64, digits: i32) -> f64 {
    if !value.is_finite() {
        return 0.0;
    }
    let factor = 10f64.powi(digits);
    (value * factor).round_ties_even() / factor
}

/// Float con la representacion corta de `repr()` de Python (siempre con `.0`
/// si es integral, porque el valor original era float).
pub fn py_float(value: f64) -> String {
    let mut text = format!("{value}");
    if !text.contains('.') && !text.contains('e') && !text.contains('E') {
        text.push_str(".0");
    }
    text
}

fn py_float_round(value: f64, digits: i32) -> String {
    py_float(round_digits(value, digits))
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

pub enum IrOp {
    Rect(RectOp),
    Text(TextOp),
}

pub fn ir_json(viewport_width: u32, viewport_height: u32, ops: &[IrOp]) -> String {
    let mut out = String::with_capacity(256 + ops.len() * 256);
    let _ = write!(
        out,
        "{{\n  \"viewport\": {{\n    \"width\": {},\n    \"height\": {}\n  }},\n  \"ops\": [",
        viewport_width, viewport_height
    );
    for (index, op) in ops.iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        match op {
            IrOp::Rect(op) => {
                let _ = write!(
                    out,
                    "\n    {{\n      \"type\": \"rect\",\n      \"x\": {},\n      \"y\": {},\n      \"w\": {},\n      \"h\": {},\n      \"fill\": \"{}\"\n    }}",
                    round_py(op.x),
                    round_py(op.y),
                    round_py(op.w),
                    round_py(op.h),
                    op.fill
                );
            }
            IrOp::Text(op) => {
                out.push_str("\n    {\n      \"type\": \"text_run\",\n      \"x\": ");
                out.push_str(&py_float_round(op.x, 3));
                out.push_str(",\n      \"y\": ");
                out.push_str(&py_float_round(op.y, 3));
                out.push_str(",\n      \"text\": ");
                push_json_string(&mut out, &op.text);
                out.push_str(",\n      \"font\": ");
                push_json_string(&mut out, &op.font);
                out.push_str(",\n      \"font_file\": ");
                push_json_string(&mut out, &op.font_file);
                out.push_str(",\n      \"size\": ");
                out.push_str(&py_float_round(op.size, 3));
                out.push_str(",\n      \"color\": ");
                push_json_string(&mut out, &op.color);
                out.push_str(",\n      \"glyphs\": [");
                for (glyph_index, glyph) in op.glyphs.iter().enumerate() {
                    if glyph_index > 0 {
                        out.push(',');
                    }
                    let _ = write!(
                        out,
                        "\n        {{\n          \"gid\": {},\n          \"x\": {},\n          \"y\": {},\n          \"w\": {},\n          \"h\": {},\n          \"u0\": {},\n          \"v0\": {},\n          \"u1\": {},\n          \"v1\": {},\n          \"adv\": {}\n        }}",
                        glyph.gid,
                        py_float_round(glyph.x, 3),
                        py_float_round(glyph.y, 3),
                        glyph.w,
                        glyph.h,
                        py_float_round(glyph.u0, 6),
                        py_float_round(glyph.v0, 6),
                        py_float_round(glyph.u1, 6),
                        py_float_round(glyph.v1, 6),
                        py_float_round(glyph.adv, 4)
                    );
                }
                if op.glyphs.is_empty() {
                    out.push_str("]\n    }");
                } else {
                    out.push_str("\n      ]\n    }");
                }
            }
        }
    }
    if ops.is_empty() {
        out.push_str("]\n}\n");
    } else {
        out.push_str("\n  ]\n}\n");
    }
    out
}

pub fn atlas_json(atlas: &Atlas) -> String {
    let hash = sha256::hex(&sha256::digest(&atlas.data));
    format!(
        "{{\n  \"width\": {},\n  \"height\": {},\n  \"format\": \"r8unorm\",\n  \"glyph_count\": {},\n  \"size_bytes\": {},\n  \"sha256\": \"{}\",\n  \"pixels_nonzero\": {}\n}}\n",
        atlas.width(),
        atlas.height(),
        atlas.glyph_count(),
        atlas.data.len(),
        hash,
        atlas.pixels_nonzero()
    )
}

pub struct Timings {
    pub html: f64,
    pub css: f64,
    pub cascade: f64,
    pub fonts: f64,
    pub layout: f64,
    pub text: f64,
    pub emit: f64,
    pub pipeline: f64,
    pub ops: usize,
    pub peak_rss_kb: u64,
}

/// Float con el estilo corto de Python (`0.044`, `37.115`, `12.0`).
fn timing_float(value: f64) -> String {
    let mut text = format!("{value:.3}");
    if text.contains('.') {
        while text.ends_with('0') {
            text.pop();
        }
        if text.ends_with('.') {
            text.push('0');
        }
    }
    text
}

pub fn timings_json(timings: &Timings) -> String {
    let mut out = String::new();
    // `html`/`css` son los nombres del frontend Python; `parse` es el alias que
    // usa el SPEC para la etapa de parseo HTML.
    let fields: [(&str, String); 11] = [
        ("parse", timing_float(timings.html)),
        ("html", timing_float(timings.html)),
        ("css", timing_float(timings.css)),
        ("cascade", timing_float(timings.cascade)),
        ("fonts", timing_float(timings.fonts)),
        ("layout", timing_float(timings.layout)),
        ("text", timing_float(timings.text)),
        ("emit", timing_float(timings.emit)),
        ("pipeline", timing_float(timings.pipeline)),
        ("ops", timings.ops.to_string()),
        ("peak_rss_kb", timings.peak_rss_kb.to_string()),
    ];
    out.push_str("{\n");
    for (index, (name, value)) in fields.iter().enumerate() {
        let comma = if index + 1 < fields.len() { "," } else { "" };
        let _ = write!(out, "  \"{name}\": {value}{comma}\n");
    }
    out.push_str("}\n");
    out
}

pub fn timings_inline(timings: &Timings) -> String {
    format!(
        "{{\"html\": {}, \"css\": {}, \"cascade\": {}, \"fonts\": {}, \"layout\": {}, \"text\": {}, \"emit\": {}, \"pipeline\": {}, \"ops\": {}, \"peak_rss_kb\": {}}}",
        timing_float(timings.html),
        timing_float(timings.css),
        timing_float(timings.cascade),
        timing_float(timings.fonts),
        timing_float(timings.layout),
        timing_float(timings.text),
        timing_float(timings.emit),
        timing_float(timings.pipeline),
        timings.ops,
        timings.peak_rss_kb
    )
}
