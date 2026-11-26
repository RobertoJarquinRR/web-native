//! `--emit-header`: `ui_data.h`, byte-for-byte identical to `write_header()` in
//! `src/emitter/python/emit_ui.py`.
//!
//! The input is the same `NodeOut` tree `--emit-ui` serializes (there is no
//! re-parse of `ui.json`). Every value is derived with the normalizations the
//! Python emitter applies when it reads `ui.json`; `emit::blob` already has the
//! shared helpers for colors, font weights, grid and image sizes.
//!
//! Layout notes (`emit_ui.py` lines around `write_header`):
//! - `UI_STYLE_EXTRAS` and the E1 fields only appear when a node uses them;
//! - v2 (grid/scroll/images) and v3 (`<input>`) fields are only written under
//!   the detailed schema (the equivalent of `ui.json` schema `/2` or `/3`);
//! - there is no atlas, no glyph/run table and no blob section here.

use std::fmt::Write as _;
use std::fs;
use std::path::Path;

use crate::emit::blob::{
    action_code, border_radius, border_width, first_shadow, font_weight_code, grid_columns,
    grid_rows, image_size, margin_auto_code, object_fit_code, overflow_code, text_align_code,
};
use crate::emit::ui_tree::{InputOut, NodeOut, KIND_TEXT};
use crate::style::color::argb;

const DEFAULT_FAMILY: &str = "Segoe UI";

/// Python `f"{value:.4f}f"` (with `-0.0000f` folded to `0.0000f`).
fn c_float(value: f64) -> String {
    if value.is_nan() {
        return "nanf".to_string();
    }
    if value.is_infinite() {
        return if value < 0.0 { "-inff" } else { "inff" }.to_string();
    }
    let text = format!("{value:.4}");
    let text = if text == "-0.0000" { "0.0000" } else { &text };
    format!("{text}f")
}

/// `emit_ui.c_string`: quotes/backslashes/`\n` escaped and every other
/// non-printable or non-ASCII character as three-digit UTF-8 octal escapes.
fn c_string(value: Option<&str>) -> String {
    let Some(value) = value else {
        return "NULL".to_string();
    };
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            ch if (32..127).contains(&(ch as u32)) => out.push(ch),
            ch => {
                let mut buffer = [0u8; 4];
                for byte in ch.encode_utf8(&mut buffer).as_bytes() {
                    let _ = write!(out, "\\{byte:03o}");
                }
            }
        }
    }
    out.push('"');
    out
}

/// `max(0.0, value)` as Python evaluates it (NaN and `-0.0` become `0.0`).
fn nonneg(value: f64) -> f64 {
    if value > 0.0 {
        value
    } else {
        0.0
    }
}

/// One node exactly as `write_header()` consumes it: the Python dict values
/// `emit_from_ui_json` builds before printing.
struct HeaderNode {
    parent: i64,
    kind: u32,
    direction: u32,
    justify: u32,
    align: u32,
    wrap: u32,
    width: f64,
    height: f64,
    width_pct: f64,
    height_pct: f64,
    margin: [f64; 4],
    padding: [f64; 4],
    background: u32,
    text: Option<String>,
    hover_bg: u32,
    press_bg: u32,
    selected_bg: u32,
    action: i32,
    gap: f64,
    font_size: f64,
    text_color: u32,
    grid_mode: i32,
    grid_column_px: f64,
    grid_auto_rows: f64,
    row_gap: f64,
    column_gap: f64,
    overflow: i32,
    image_width: f64,
    image_height: f64,
    image_src: Option<String>,
    image_alt: Option<String>,
    object_fit: i32,
    border_width: f64,
    border_color: u32,
    border_radius: f64,
    shadow_x: f64,
    shadow_y: f64,
    shadow_blur: f64,
    shadow_spread: f64,
    shadow_color: u32,
    text_align: i32,
    line_height: f64,
    margin_auto: i32,
    box_sizing: i32,
    flex_grow: f64,
    flex_shrink: f64,
    flex_basis: f64,
    font_family: String,
    font_weight: i32,
}

impl HeaderNode {
    fn from_node(node: &NodeOut, ui_dir: &Path, detailed: bool) -> Self {
        let style = &node.style;
        let is_text = node.kind == KIND_TEXT;

        let gap = if style.gap == 0.0 { 0.0 } else { style.gap };
        let (grid_mode, grid_column_px, grid_auto_rows) = if detailed {
            let (mode, column) = grid_columns(style.grid_template_columns.as_ref());
            (mode, column, grid_rows(style.grid_auto_rows.as_ref()))
        } else {
            // Under schema `/1` the keys are absent: `grid_columns(None)` and
            // `auto_rows(None)`.
            (0, 0.0, -1.0)
        };
        let (row_gap, column_gap) = if detailed {
            (style.row_gap, style.column_gap)
        } else {
            (gap, gap)
        };
        let image_src = node.image.as_ref().and_then(|image| image.src.clone());
        let image_alt = node.image.as_ref().and_then(|image| image.alt.clone());
        let object_fit = node
            .image
            .as_ref()
            .map(|image| object_fit_code(Some(image.object_fit.as_str())))
            .unwrap_or(0);
        let (image_width, image_height) = image_size(image_src.as_deref(), ui_dir);

        // CSS core v1 travels in `ui.json` only under the detailed schema.
        let (
            border_width,
            border_color,
            border_radius,
            shadow_x,
            shadow_y,
            shadow_blur,
            shadow_spread,
            shadow_color,
        ) = if detailed {
            let shadow = first_shadow(&style.css3);
            let (shadow_x, shadow_y, shadow_blur, shadow_spread, shadow_color) = match shadow {
                Some(shadow) => (
                    shadow.offset_x_px,
                    shadow.offset_y_px,
                    nonneg(shadow.blur_px),
                    shadow.spread_px,
                    argb(Some(shadow.color.as_str())),
                ),
                None => (0.0, 0.0, 0.0, 0.0, 0),
            };
            (
                border_width(&style.css3),
                argb(style.css3.border.visible_color()),
                border_radius(&style.css3),
                shadow_x,
                shadow_y,
                shadow_blur,
                shadow_spread,
                shadow_color,
            )
        } else {
            (0.0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0)
        };

        let family = if style.font_family.is_empty() {
            DEFAULT_FAMILY.to_string()
        } else {
            style.font_family.clone()
        };
        // `write_header()` reads `node.get("flex_shrink") or 1.0`, so a zero
        // shrink is emitted as 1.0 (and does not trigger the extras block).
        let flex_shrink = {
            let value = nonneg(style.flex_shrink);
            if value == 0.0 {
                1.0
            } else {
                value
            }
        };

        HeaderNode {
            parent: node.parent,
            kind: node.kind,
            direction: style.direction,
            justify: style.justify,
            align: style.align,
            wrap: style.wrap,
            // Text nodes carry the baked size in the tree but not in the
            // document; `emit_from_ui_json` resets them to `-1`.
            width: if is_text { -1.0 } else { style.width_px },
            height: if is_text { -1.0 } else { style.height_px },
            width_pct: style.width_pct,
            height_pct: style.height_pct,
            margin: style.margin,
            padding: style.padding,
            background: node.argb.background,
            text: node.text.as_ref().map(|text| text.content.clone()),
            hover_bg: node.argb.hover,
            press_bg: node.argb.press,
            selected_bg: node.argb.selected,
            action: action_code(node.data.action.as_deref()),
            gap,
            font_size: style.font_size,
            text_color: node.argb.text,
            grid_mode,
            grid_column_px,
            grid_auto_rows,
            row_gap,
            column_gap,
            overflow: if detailed {
                overflow_code(style.overflow.as_deref())
            } else {
                0
            },
            image_width,
            image_height,
            image_src,
            image_alt,
            object_fit,
            border_width,
            border_color,
            border_radius,
            shadow_x,
            shadow_y,
            shadow_blur,
            shadow_spread,
            shadow_color,
            text_align: text_align_code(&style.text_align),
            line_height: nonneg(style.line_height),
            margin_auto: margin_auto_code(&style.margin_auto),
            box_sizing: i32::from(style.box_sizing_border),
            flex_grow: nonneg(style.flex_grow),
            flex_shrink,
            flex_basis: style.flex_basis,
            font_family: family,
            font_weight: font_weight_code(&style.font_weight),
        }
    }

    /// One `kUiNodes` entry, byte-for-byte like `write_header()`'s
    /// `format_text % values`.
    fn line(&self, extras: bool) -> String {
        let margin = self
            .margin
            .iter()
            .map(|value| c_float(*value))
            .collect::<Vec<_>>()
            .join(", ");
        let padding = self
            .padding
            .iter()
            .map(|value| c_float(*value))
            .collect::<Vec<_>>()
            .join(", ");
        let mut line = format!(
            "    {{ {parent}, {kind}, {direction}, {justify}, {align}, {wrap}, \
             {width}, {height}, {width_pct}, {height_pct}, {{ {margin} }}, \
             {{ {padding} }}, 0x{background:08X}u, {text}, 0x{hover_bg:08X}u, \
             0x{press_bg:08X}u, 0x{selected_bg:08X}u, {action}, {gap}, \
             {font_size}, 0x{text_color:08X}u, {grid_mode}, {grid_column_px}, \
             {grid_auto_rows}, {row_gap}, {column_gap}, {overflow}, \
             {image_width}, {image_height}, {image_src}, {image_alt}, \
             {object_fit}, {border_width}, 0x{border_color:08X}u, \
             {border_radius}, {shadow_x}, {shadow_y}, {shadow_blur}, \
             {shadow_spread}, 0x{shadow_color:08X}u",
            parent = self.parent,
            kind = self.kind,
            direction = self.direction,
            justify = self.justify,
            align = self.align,
            wrap = self.wrap,
            width = c_float(self.width),
            height = c_float(self.height),
            width_pct = c_float(self.width_pct),
            height_pct = c_float(self.height_pct),
            text = c_string(self.text.as_deref()),
            background = self.background,
            hover_bg = self.hover_bg,
            press_bg = self.press_bg,
            selected_bg = self.selected_bg,
            action = self.action,
            gap = c_float(self.gap),
            font_size = c_float(self.font_size),
            text_color = self.text_color,
            grid_mode = self.grid_mode,
            grid_column_px = c_float(self.grid_column_px),
            grid_auto_rows = c_float(self.grid_auto_rows),
            row_gap = c_float(self.row_gap),
            column_gap = c_float(self.column_gap),
            overflow = self.overflow,
            image_width = c_float(self.image_width),
            image_height = c_float(self.image_height),
            image_src = c_string(self.image_src.as_deref()),
            image_alt = c_string(self.image_alt.as_deref()),
            object_fit = self.object_fit,
            border_width = c_float(self.border_width),
            border_color = self.border_color,
            border_radius = c_float(self.border_radius),
            shadow_x = c_float(self.shadow_x),
            shadow_y = c_float(self.shadow_y),
            shadow_blur = c_float(self.shadow_blur),
            shadow_spread = c_float(self.shadow_spread),
            shadow_color = self.shadow_color,
        );
        if extras {
            line.push_str(&format!(
                ", {text_align}, {line_height}, {margin_auto}, {box_sizing}, \
                 {flex_grow}, {flex_shrink}, {flex_basis}",
                text_align = self.text_align,
                line_height = c_float(self.line_height),
                margin_auto = self.margin_auto,
                box_sizing = self.box_sizing,
                flex_grow = c_float(self.flex_grow),
                flex_shrink = c_float(self.flex_shrink),
                flex_basis = c_float(self.flex_basis),
            ));
        }
        line.push_str(&format!(
            ", {family}, {weight}",
            family = c_string(Some(self.font_family.as_str())),
            weight = self.font_weight,
        ));
        line.push_str(" },");
        line
    }
}

/// Renders `ui_data.h`. `detailed` is the schema decision already made for
/// `ui.json` (`/2` or `/3`): under `/1` the v2/v3 keys are absent and the
/// Python emitter falls back to the v1 defaults.
pub(crate) fn render_header(
    nodes: &[NodeOut],
    viewport: (u32, u32),
    ui_dir: &Path,
    detailed: bool,
) -> String {
    let out_nodes: Vec<HeaderNode> = nodes
        .iter()
        .map(|node| HeaderNode::from_node(node, ui_dir, detailed))
        .collect();
    let inputs: Vec<(usize, &InputOut)> = nodes
        .iter()
        .enumerate()
        .filter_map(|(index, node)| node.input.as_ref().map(|input| (index, input)))
        .collect();

    // Same predicate as `write_header()`: only pages that use one of the E1
    // fields define `UI_STYLE_EXTRAS` and append the seven values.
    //
    // Python reads `float(node.get("flex_basis") or -1.0) >= 0.0`: a zero basis
    // is falsy, so `flex-basis: 0` / `flex: 0 1 0` does *not* enable extras.
    // The printer, however, uses `is not None` and would write a zero basis.
    let extras = out_nodes.iter().any(|node| {
        node.text_align != 0
            || node.line_height > 0.0
            || node.margin_auto != 0
            || node.box_sizing != 0
            || node.flex_grow != 0.0
            || node.flex_shrink != 1.0
            || node.flex_basis > 0.0
    });

    let mut lines: Vec<String> = Vec::with_capacity(64 + out_nodes.len() * 2);
    lines.push("/* Generated by emit_ui.py. Do not edit. */".to_string());
    lines.push("#ifndef UI_DATA_H".to_string());
    lines.push("#define UI_DATA_H".to_string());
    lines.push(String::new());
    let mut node_kinds =
        "R2_NODE_BLOCK, R2_NODE_FLEX, R2_NODE_TEXT, R2_NODE_GRID, R2_NODE_IMAGE".to_string();
    if !inputs.is_empty() {
        // ui-tree/3: additive, only present on pages with <input>.
        node_kinds.push_str(", R2_NODE_INPUT");
    }
    lines.push(format!("typedef enum {{ {node_kinds} }} NodeKind;"));
    lines.push(
        "typedef enum { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_SPACE_BETWEEN } Justify;"
            .to_string(),
    );
    lines.push(
        "typedef enum { ALIGN_START, ALIGN_CENTER, ALIGN_END, ALIGN_STRETCH } Align;".to_string(),
    );
    lines.push("typedef enum { ACTION_NONE = 0, ACTION_SELECT = 1 } Action;".to_string());
    lines.push("typedef enum { R2_GRID_NONE = 0, R2_GRID_AUTO_FILL = 1 } GridMode;".to_string());
    lines.push(
        "typedef enum { R2_OVERFLOW_VISIBLE = 0, R2_OVERFLOW_HIDDEN = 1, R2_OVERFLOW_SCROLL = 2 } OverflowMode;"
            .to_string(),
    );
    lines.push(
        "typedef enum { R2_OBJECT_FIT_FILL = 0, R2_OBJECT_FIT_COVER = 1, R2_OBJECT_FIT_CONTAIN = 2 } ObjectFit;"
            .to_string(),
    );
    if extras {
        lines.push(String::new());
        lines.push("#define UI_STYLE_EXTRAS 1".to_string());
    }
    lines.push(String::new());
    lines.push("typedef struct {".to_string());
    lines.push("    int parent;                 /* -1 root */".to_string());
    lines.push("    int kind;".to_string());
    lines.push("    int direction;              /* 0 row, 1 column */".to_string());
    lines.push("    int justify, align, wrap;".to_string());
    lines.push("    float width, height;        /* px, -1 auto */".to_string());
    lines.push("    float width_pct, height_pct;/* 0 none, resolved at runtime */".to_string());
    lines.push("    float margin[4], padding[4];/* top,right,bottom,left */".to_string());
    lines.push(
        "    unsigned int background;    /* 0xAARRGGBB, 0 none */".to_string(),
    );
    lines.push("    const char *text;           /* NODE_TEXT only */".to_string());
    lines.push("    unsigned int hover_bg, press_bg, selected_bg;".to_string());
    lines.push("    int action;".to_string());
    lines.push("    /* R2 extensions (documented in README) */".to_string());
    lines.push("    float gap;                  /* main-axis gap, px */".to_string());
    lines.push(
        "    float font_size;            /* text nodes, build-time metric */".to_string(),
    );
    lines.push("    unsigned int text_color;    /* text nodes, 0xAARRGGBB */".to_string());
    lines.push("    /* ui-tree/2: grid, scroll and images */".to_string());
    lines.push("    int grid_mode;              /* R2_GRID_* */".to_string());
    lines.push("    float grid_column_px;       /* repeat(auto-fill, <px>) */".to_string());
    lines.push("    float grid_auto_rows;       /* px, -1 auto */".to_string());
    lines.push("    float row_gap, column_gap;  /* resolved gaps, px */".to_string());
    lines.push("    int overflow;               /* R2_OVERFLOW_* */".to_string());
    lines.push(
        "    float image_width, image_height; /* intrinsic px, 0 unknown */".to_string(),
    );
    lines.push("    const char *image_src;      /* local path, NODE_IMAGE only */".to_string());
    lines.push("    const char *image_alt;".to_string());
    lines.push("    int object_fit;             /* R2_OBJECT_FIT_* */".to_string());
    lines.push(
        "    /* CSS core v1: rounded background, uniform border, first shadow. */".to_string(),
    );
    lines.push("    float border_width;         /* px, 0 none */".to_string());
    lines.push("    unsigned int border_color;  /* 0xAARRGGBB, 0 none */".to_string());
    lines.push("    float border_radius;        /* uniform px, 0 square */".to_string());
    lines.push(
        "    float shadow_x, shadow_y;   /* first non-inset shadow offset, px */".to_string(),
    );
    lines.push("    float shadow_blur, shadow_spread;".to_string());
    lines.push("    unsigned int shadow_color;  /* 0xAARRGGBB, 0 none */".to_string());
    if extras {
        lines.push(
            "    /* E1: text alignment, line box, auto margins and box model (only when used). */"
                .to_string(),
        );
        lines.push("    int text_align;             /* 0 left, 1 center, 2 right */".to_string());
        lines.push("    float line_height;          /* px, 0 = font metrics */".to_string());
        lines.push(
            "    int margin_auto;            /* bit0 top, 1 right, 2 bottom, 3 left */".to_string(),
        );
        lines.push(
            "    int box_sizing;             /* 0 content-box (CSS default), 1 border-box */"
                .to_string(),
        );
        lines.push("    float flex_grow;            /* >= 0 */".to_string());
        lines.push("    float flex_shrink;          /* >= 0, default 1 */".to_string());
        lines.push("    float flex_basis;           /* px, -1 = auto */".to_string());
    }
    lines.push("    const char *font_family;    /* text nodes, CSS family */".to_string());
    lines.push("    int font_weight;            /* text nodes, 100..900 */".to_string());
    lines.push("} UiNode;".to_string());
    lines.push(String::new());
    lines.push(format!("#define UI_NODE_COUNT {}", out_nodes.len()));
    lines.push(format!("#define UI_VIEWPORT_WIDTH {}", viewport.0));
    lines.push(format!("#define UI_VIEWPORT_HEIGHT {}", viewport.1));
    lines.push(String::new());
    lines.push("static const UiNode kUiNodes[UI_NODE_COUNT] = {".to_string());
    for node in &out_nodes {
        lines.push(node.line(extras));
    }
    lines.push("};".to_string());
    lines.push(String::new());
    lines.push("static const char *const kUiNodeNames[UI_NODE_COUNT] = {".to_string());
    for node in nodes {
        lines.push(format!("    {},", c_string(Some(node.name.as_str()))));
    }
    lines.push("};".to_string());
    lines.push(String::new());

    if !inputs.is_empty() {
        // ui-tree/3: only the spec travels; the runtime shapes the value.
        lines.push("#include \"document/wn_document_format.h\"".to_string());
        lines.push(String::new());
        lines.push(format!("#define UI_INPUT_COUNT {}", inputs.len()));
        lines.push(String::new());
        lines.push(
            "static const UiInput kUiInputs[UI_INPUT_COUNT > 0 ? UI_INPUT_COUNT : 1] = {"
                .to_string(),
        );
        for (index, input) in &inputs {
            lines.push(format!(
                "    {{ {}, {}, {}, {}, {} }},",
                index,
                c_string(Some(input.value.as_str())),
                c_string(Some(input.placeholder.as_str())),
                input.max_length,
                i32::from(input.password),
            ));
        }
        lines.push("};".to_string());
        lines.push(String::new());
    }

    // No atlas: every glyph is rasterized by the runtime.
    lines.push("#endif /* UI_DATA_H */".to_string());
    lines.push(String::new());

    lines.join("\n")
}

/// Writes `ui_data.h` with LF endings, like `Path.write_text(newline="\n")`.
pub(crate) fn write_header(
    path: &Path,
    nodes: &[NodeOut],
    viewport: (u32, u32),
    ui_dir: &Path,
    detailed: bool,
) -> Result<(), String> {
    let text = render_header(nodes, viewport, ui_dir, detailed);
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|err| format!("{}: {err}", parent.display()))?;
        }
    }
    fs::write(path, text).map_err(|err| format!("{}: {err}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::{c_float, c_string};

    #[test]
    fn float_matches_python_format() {
        assert_eq!(c_float(-1.0), "-1.0000f");
        assert_eq!(c_float(0.0), "0.0000f");
        assert_eq!(c_float(-0.0), "0.0000f");
        assert_eq!(c_float(12.34567), "12.3457f");
        assert_eq!(c_float(-0.00001), "0.0000f");
    }

    #[test]
    fn string_matches_python_c_string() {
        assert_eq!(c_string(None), "NULL");
        assert_eq!(c_string(Some("")), "\"\"");
        assert_eq!(c_string(Some("a\"b\\c\n")), "\"a\\\"b\\\\c\\n\"");
        // Non-ASCII becomes three-digit UTF-8 octal escapes, never `\xNN`.
        assert_eq!(c_string(Some("Caf\u{e9}")), "\"Caf\\303\\251\"");
        assert_eq!(c_string(Some("\u{1f600}")), "\"\\360\\237\\230\\200\"");
    }
}
