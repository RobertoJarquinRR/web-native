//! `--emit-blob`: the `.wnui` v2 document (same sections as `write_blob` in
//! `src/emitter/python/emit_ui.py`).
//!
//! The tree is already parsed into `NodeOut`; this module only reproduces the
//! normalizations the Python emitter applies when reading `ui.json`, so the
//! blob is byte-for-byte identical. `detailed == false` (schema `/1`) implies
//! `ui.json` carries no v2 section: the contract there is to use `gap` for
//! `row_gap`/`column_gap`, grid at zero and `overflow` at 0.

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use crate::emit::ui_tree::{NodeOut, KIND_TEXT};
use crate::style::grid::{GridAutoRows, GridTemplateColumns};

const BLOB_MAGIC: u32 = 0x4955_4E57; // "WNUI"
const BLOB_VERSION: u32 = 2;
const BLOB_HEADER_SIZE: usize = 32;
const BLOB_SECTION_SIZE: usize = 16;

const SECTION_NODES: u32 = 1;
const SECTION_NAMES: u32 = 2;
const SECTION_STRINGS: u32 = 3;
const SECTION_INPUTS: u32 = 4;

/// Blob string pool: byte 0 is NUL, deduped in first-use order.
struct StringPool {
    data: Vec<u8>,
    offsets: HashMap<String, u32>,
}

impl StringPool {
    fn new() -> Self {
        StringPool {
            data: vec![0],
            offsets: HashMap::new(),
        }
    }

    /// String offset in the pool; `None` and `""` are both 0 (NULL).
    fn offset(&mut self, value: Option<&str>) -> u32 {
        let Some(text) = value else {
            return 0;
        };
        if text.is_empty() {
            return 0;
        }
        if let Some(offset) = self.offsets.get(text) {
            return *offset;
        }
        let offset = self.data.len() as u32;
        self.data.extend_from_slice(text.as_bytes());
        self.data.push(0);
        self.offsets.insert(text.to_string(), offset);
        offset
    }
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_i32(out: &mut Vec<u8>, value: i32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_f32(out: &mut Vec<u8>, value: f64) {
    out.extend_from_slice(&(value as f32).to_le_bytes());
}

/// Python's `argb()` for a color already normalized to `#rrggbb[aa]`.
pub(crate) fn argb_hex(value: Option<&str>) -> u32 {
    let Some(text) = value else {
        return 0;
    };
    let Some(hex) = text.strip_prefix('#') else {
        return 0;
    };
    let channels = match hex.len() {
        3 => {
            let mut expanded = String::with_capacity(6);
            for ch in hex.chars() {
                expanded.push(ch);
                expanded.push(ch);
            }
            expanded
        }
        6 | 8 => hex.to_string(),
        _ => return 0,
    };
    let mut bytes = [0u8; 4];
    bytes[3] = 0xFF;
    for index in 0..channels.len() / 2 {
        let pair = &channels[index * 2..index * 2 + 2];
        match u8::from_str_radix(pair, 16) {
            Ok(byte) => bytes[index] = byte,
            Err(_) => return 0,
        }
    }
    let (r, g, b, a) = (bytes[0], bytes[1], bytes[2], bytes[3]);
    if a == 0 {
        return 0;
    }
    (u32::from(a) << 24) | (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b)
}

pub(crate) fn action_code(value: Option<&str>) -> i32 {
    match value {
        Some(text) if text.trim().eq_ignore_ascii_case("select") => 1,
        _ => 0,
    }
}

pub(crate) fn overflow_code(value: Option<&str>) -> i32 {
    match value.map(|text| text.trim().to_ascii_lowercase()) {
        Some(text) if text == "auto" || text == "scroll" => 2,
        Some(text) if text == "hidden" => 1,
        _ => 0,
    }
}

pub(crate) fn object_fit_code(value: Option<&str>) -> i32 {
    match value.map(|text| text.trim().to_ascii_lowercase()) {
        Some(text) if text == "cover" => 1,
        Some(text) if text == "contain" => 2,
        _ => 0,
    }
}

pub(crate) fn text_align_code(value: &str) -> i32 {
    match value.trim().to_ascii_lowercase().as_str() {
        "center" => 1,
        "right" => 2,
        _ => 0,
    }
}

/// Python's `font_weight_value()`: keyword, number 100..900, or 400.
pub(crate) fn font_weight_code(value: &str) -> i32 {
    let text = value.trim().to_ascii_lowercase();
    match text.as_str() {
        "normal" => 400,
        "bold" | "bolder" => 700,
        "lighter" => 300,
        _ => match text.parse::<f64>() {
            Ok(number) => (number as i32).clamp(100, 900),
            Err(_) => 400,
        },
    }
}

pub(crate) fn margin_auto_code(flags: &[bool; 4]) -> i32 {
    let mut mask = 0i32;
    for (index, flag) in flags.iter().enumerate() {
        if *flag {
            mask |= 1 << index;
        }
    }
    mask
}

pub(crate) fn grid_columns(columns: Option<&GridTemplateColumns>) -> (i32, f64) {
    match columns {
        Some(grid) if grid.mode == "auto_fill" && grid.size_px > 0.0 => (1, grid.size_px),
        _ => (0, 0.0),
    }
}

pub(crate) fn grid_rows(rows: Option<&GridAutoRows>) -> f64 {
    match rows {
        Some(GridAutoRows::Px(px)) => px.max(0.0),
        Some(GridAutoRows::Auto) | None => -1.0,
    }
}

/// `border_width_value()`: uniform width across the visible sides.
pub(crate) fn border_width(css3: &crate::style::style_ext::StyleV3) -> f64 {
    let (width, _) = css3.border.uniform_width();
    width.max(0.0)
}

/// `first_box_shadow()`: first non-`inset` shadow.
pub(crate) fn first_shadow(
    css3: &crate::style::style_ext::StyleV3,
) -> Option<&crate::style::style_ext::BoxShadow> {
    css3.box_shadow.iter().find(|shadow| !shadow.inset)
}

pub(crate) fn border_radius(css3: &crate::style::style_ext::StyleV3) -> f64 {
    css3.border_radius
        .iter()
        .copied()
        .fold(0.0f64, f64::max)
        .max(0.0)
}

fn png_size(data: &[u8]) -> Option<(u32, u32)> {
    if data.len() >= 24
        && data[..8] == [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1a, b'\n']
        && &data[12..16] == b"IHDR"
    {
        let width = u32::from_be_bytes([data[16], data[17], data[18], data[19]]);
        let height = u32::from_be_bytes([data[20], data[21], data[22], data[23]]);
        if width > 0 && height > 0 {
            return Some((width, height));
        }
    }
    None
}

fn jpeg_size(data: &[u8]) -> Option<(u32, u32)> {
    if data.len() < 4 || data[0..2] != [0xFF, 0xD8] {
        return None;
    }
    let mut index = 2usize;
    while index + 9 < data.len() {
        if data[index] != 0xFF {
            index += 1;
            continue;
        }
        let marker = data[index + 1];
        index += 2;
        while index < data.len() && data[index] == 0xFF {
            index += 1;
        }
        if index >= data.len() || marker == 0xD8 || marker == 0xD9 {
            continue;
        }
        if index + 2 > data.len() {
            return None;
        }
        let length = usize::from(u16::from_be_bytes([data[index], data[index + 1]]));
        if length < 2 {
            return None;
        }
        if (0xC0..=0xCF).contains(&marker) && !matches!(marker, 0xC4 | 0xC8 | 0xCC) {
            if index + 7 > data.len() {
                return None;
            }
            let height = u32::from(u16::from_be_bytes([data[index + 3], data[index + 4]]));
            let width = u32::from(u16::from_be_bytes([data[index + 5], data[index + 6]]));
            if width > 0 && height > 0 {
                return Some((width, height));
            }
            return None;
        }
        index += length;
    }
    None
}

/// Intrinsic PNG/JPEG size resolved against the `ui.json` directory, same as
/// the Python emitter's `image_size()`.
pub(crate) fn image_size(source: Option<&str>, base_dir: &Path) -> (f64, f64) {
    let Some(source) = source else {
        return (0.0, 0.0);
    };
    if source.is_empty() {
        return (0.0, 0.0);
    }
    let path = Path::new(source);
    let path = if path.is_absolute() {
        path.to_path_buf()
    } else {
        base_dir.join(path)
    };
    let Ok(data) = fs::read(&path) else {
        return (0.0, 0.0);
    };
    let data = &data[..data.len().min(65536)];
    match png_size(data).or_else(|| jpeg_size(data)) {
        Some((width, height)) => (f64::from(width), f64::from(height)),
        None => (0.0, 0.0),
    }
}

fn push_node_record(
    out: &mut Vec<u8>,
    node: &NodeOut,
    pool: &mut StringPool,
    detailed: bool,
    ui_dir: &Path,
) {
    let style = &node.style;
    let is_text = node.kind == KIND_TEXT;
    let (width, height) = if is_text {
        (-1.0, -1.0)
    } else {
        (style.width_px, style.height_px)
    };
    let (grid_mode, grid_column_px) = if detailed {
        grid_columns(style.grid_template_columns.as_ref())
    } else {
        (0, 0.0)
    };
    let grid_auto_rows = if detailed {
        grid_rows(style.grid_auto_rows.as_ref())
    } else {
        -1.0
    };
    let (row_gap, column_gap) = if detailed {
        (style.row_gap, style.column_gap)
    } else {
        (style.gap, style.gap)
    };
    let overflow = if detailed {
        overflow_code(style.overflow.as_deref())
    } else {
        0
    };
    let image_src = node.image.as_ref().and_then(|image| image.src.as_deref());
    let image_alt = node.image.as_ref().and_then(|image| image.alt.as_deref());
    let (image_width, image_height) = image_size(image_src, ui_dir);
    let object_fit = node
        .image
        .as_ref()
        .map(|image| object_fit_code(Some(image.object_fit.as_str())))
        .unwrap_or(0);
    let shadow = first_shadow(&style.css3);

    push_i32(out, node.parent as i32);
    push_i32(out, node.kind as i32);
    push_i32(out, style.direction as i32);
    push_i32(out, style.justify as i32);
    push_i32(out, style.align as i32);
    push_i32(out, style.wrap as i32);
    push_f32(out, width);
    push_f32(out, height);
    push_f32(out, style.width_pct);
    push_f32(out, style.height_pct);
    for value in style.margin {
        push_f32(out, value);
    }
    for value in style.padding {
        push_f32(out, value);
    }
    push_u32(out, node.argb.background);
    push_u32(
        out,
        pool.offset(node.text.as_ref().map(|text| text.content.as_str())),
    );
    push_u32(out, node.argb.hover);
    push_u32(out, node.argb.press);
    push_u32(out, node.argb.selected);
    push_i32(out, action_code(node.data.action.as_deref()));
    push_f32(out, style.gap);
    push_f32(out, style.font_size);
    push_u32(out, node.argb.text);
    push_i32(out, grid_mode);
    push_f32(out, grid_column_px);
    push_f32(out, grid_auto_rows);
    push_f32(out, row_gap);
    push_f32(out, column_gap);
    push_i32(out, overflow);
    push_f32(out, image_width);
    push_f32(out, image_height);
    push_u32(out, pool.offset(image_src));
    push_u32(out, pool.offset(image_alt));
    push_i32(out, object_fit);
    push_f32(out, border_width(&style.css3));
    push_u32(out, argb_hex(style.css3.border.visible_color()));
    push_f32(out, border_radius(&style.css3));
    let (shadow_x, shadow_y, shadow_blur, shadow_spread, shadow_color) = match shadow {
        Some(shadow) => (
            shadow.offset_x_px,
            shadow.offset_y_px,
            shadow.blur_px.max(0.0),
            shadow.spread_px,
            argb_hex(Some(shadow.color.as_str())),
        ),
        None => (0.0, 0.0, 0.0, 0.0, 0),
    };
    push_f32(out, shadow_x);
    push_f32(out, shadow_y);
    push_f32(out, shadow_blur);
    push_f32(out, shadow_spread);
    push_u32(out, shadow_color);
    push_i32(out, text_align_code(&style.text_align));
    push_f32(out, style.line_height);
    push_i32(out, margin_auto_code(&style.margin_auto));
    push_i32(out, i32::from(style.box_sizing_border));
    push_f32(out, style.flex_grow.max(0.0));
    push_f32(out, style.flex_shrink.max(0.0));
    push_f32(out, style.flex_basis);
    let family = if style.font_family.is_empty() {
        "Segoe UI"
    } else {
        style.font_family.as_str()
    };
    push_u32(out, pool.offset(Some(family)));
    push_i32(out, font_weight_code(&style.font_weight));
}

/// Writes the v2 `.wnui` with the same tree as `ui.json`.
pub(crate) fn write_blob(
    path: &Path,
    nodes: &[NodeOut],
    viewport: (u32, u32),
    ui_dir: &Path,
    detailed: bool,
) -> Result<(), String> {
    let mut pool = StringPool::new();

    let mut node_records = Vec::with_capacity(nodes.len() * 220);
    for node in nodes {
        push_node_record(&mut node_records, node, &mut pool, detailed, ui_dir);
    }

    let mut name_records = Vec::with_capacity(nodes.len() * 4);
    for node in nodes {
        let offset = pool.offset(Some(node.name.as_str()));
        push_u32(&mut name_records, offset);
    }

    let mut input_records = Vec::new();
    for (index, node) in nodes.iter().enumerate() {
        let Some(input) = &node.input else {
            continue;
        };
        push_i32(&mut input_records, index as i32);
        let value = pool.offset(Some(input.value.as_str()));
        push_u32(&mut input_records, value);
        let placeholder = pool.offset(Some(input.placeholder.as_str()));
        push_u32(&mut input_records, placeholder);
        push_i32(&mut input_records, input.max_length as i32);
        push_i32(&mut input_records, i32::from(input.password));
    }

    let mut sections: Vec<(u32, Vec<u8>)> = vec![
        (SECTION_NODES, node_records),
        (SECTION_NAMES, name_records),
        (SECTION_STRINGS, pool.data),
    ];
    if !input_records.is_empty() {
        sections.push((SECTION_INPUTS, input_records));
    }

    let table_size = BLOB_HEADER_SIZE + sections.len() * BLOB_SECTION_SIZE;
    let mut offset = (table_size + 7) & !7;
    let mut table = Vec::with_capacity(sections.len() * BLOB_SECTION_SIZE);
    let mut payload = Vec::new();
    for (section_id, data) in &sections {
        push_u32(&mut table, *section_id);
        push_u32(&mut table, offset as u32);
        push_u32(&mut table, data.len() as u32);
        push_u32(&mut table, 0);
        payload.extend_from_slice(data);
        offset += data.len();
        let padding = (8 - (offset & 7)) & 7;
        payload.resize(payload.len() + padding, 0);
        offset += padding;
    }

    let mut file = Vec::with_capacity(offset);
    push_u32(&mut file, BLOB_MAGIC);
    push_u32(&mut file, BLOB_VERSION);
    push_u32(&mut file, sections.len() as u32);
    push_u32(&mut file, offset as u32);
    push_u32(&mut file, nodes.len() as u32);
    push_u32(&mut file, viewport.0);
    push_u32(&mut file, viewport.1);
    push_u32(&mut file, 0);
    file.extend_from_slice(&table);
    file.extend_from_slice(&payload);

    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|err| format!("{}: {err}", parent.display()))?;
        }
    }
    fs::write(path, file).map_err(|err| format!("{}: {err}", path.display()))
}
