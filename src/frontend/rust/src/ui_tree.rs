//! `--emit-ui`: arbol de estilos **pre-layout** para el runtime R2.
//!
//! El IR de R1 es un display list post-layout (rects y text runs ya
//! posicionados) y no sirve para recalcular a otro viewport. R2 necesita el
//! arbol tal cual sale de la cascada, con los valores computados y sin
//! geometria: `emit_ui.py` lo consume y lo convierte en `ui_data.h`.
//!
//! El aplanado reproduce `Emitter` de `src/emitter/python/emit_ui.py`:
//! pre-orden, texto propio antes de los hijos, nombres `tag#id.clase` con
//! sufijo `[n]` para duplicados, y `%` conservados como `width_pct`.
//!
//! Salida (junto al `ui.json`):
//!   - `ui.json`     arbol + estilos + data-* + texto con glyphs pre-shapeados
//!   - `atlas.bin`   R8 256x256 (solo si el documento tiene texto)
//!   - `atlas.json`  dimensiones, sha256 y conteos del atlas
//!
//! No ejecuta el pase de layout: no hay x/y/w/h de cajas, solo el estilo.
//! Emite `webnative.ui-tree/1` si la pagina no usa features v2 y
//! `webnative.ui-tree/2` si emite grid, scroll (`overflow: auto|scroll`) o
//! imagenes (`docs/ui-tree-v2.md`). `webnative.ui-tree/3` es aditivo: la pagina
//! usa un `<input type="text">` y anade `kind:"input"` + `input{}` con la
//! piscina ASCII imprimible horneada en el atlas.
//!
//! E1 (fidelidad visual) es tambien aditivo y por pagina: si algun nodo usa
//! `text-align`, `line-height` o `margin: auto`, cada nodo emite ademas
//! `text_align`, `line_height` y `margin_auto`. Las paginas que no los usan no
//! cambian ni un byte (y `UI_STYLE_EXTRAS` solo se define en esos headers).

use std::collections::HashMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use crate::cascade::ComputedStyle;
use crate::color::parse_color;
use crate::dom::{Dom, Node, NodeId};
use crate::emit::{push_json_string, py_float};
use crate::grid::{GridAutoRows, GridTemplateColumns};
use crate::sha256;
use crate::style_ext::{BorderOut, BoxShadow, RelLength, StyleV3, Unit};
use crate::text::{
    parse_font_weight, Atlas, BakedInputPool, BakedUiText, TextEngine, UiGlyphOut, UiInputGlyphOut,
    UiTextBaker, UI_INPUT_ATLAS_SIZE,
};

/// Schema sin features v2 (grid/scroll/imagenes). Identico al contrato previo.
pub const SCHEMA_V1: &str = "webnative.ui-tree/1";
/// Schema v2 cuando la pagina usa grid, scroll o imagenes.
pub const SCHEMA_V2: &str = "webnative.ui-tree/2";
/// Schema v3 (aditivo) cuando la pagina usa `<input type="text">`.
pub const SCHEMA_V3: &str = "webnative.ui-tree/3";

const KIND_BLOCK: u32 = 0;
const KIND_FLEX: u32 = 1;
const KIND_TEXT: u32 = 2;
const KIND_GRID: u32 = 3;
const KIND_IMAGE: u32 = 4;
const KIND_INPUT: u32 = 5;

/// Limite duro del buffer de edicion; `maxlength` del HTML puede ser menor.
pub const INPUT_MAX_BYTES: usize = 255;

const DEFAULT_FAMILY: &str = "Segoe UI";
const DEFAULT_SIZE: f64 = 16.0;

fn justify_code(name: &str) -> u32 {
    match name {
        "flex-start" | "start" | "normal" => 0,
        "center" => 1,
        "flex-end" | "end" => 2,
        "space-between" => 3,
        _ => 0,
    }
}

fn align_code(name: &str) -> u32 {
    match name {
        "flex-start" | "start" | "normal" => 0,
        "center" => 1,
        "flex-end" | "end" => 2,
        "stretch" => 3,
        // El oraculo usa `ALIGN.get(..., 3)`: desconocido = stretch.
        _ => 3,
    }
}

fn wrap_code(name: &str) -> u32 {
    if name == "wrap" || name == "wrap-reverse" {
        1
    } else {
        0
    }
}

fn kind_name(kind: u32) -> &'static str {
    match kind {
        KIND_FLEX => "flex",
        KIND_TEXT => "text",
        KIND_GRID => "grid",
        KIND_IMAGE => "image",
        KIND_INPUT => "input",
        _ => "block",
    }
}

/// `argb()` del oraculo para un color ya normalizado a `#rrggbb`.
fn argb_from_hex(value: Option<&str>) -> u32 {
    let Some(text) = value else { return 0 };
    let Some(hex) = text.strip_prefix('#') else {
        return 0;
    };
    if hex.len() != 6 {
        return 0;
    }
    match u32::from_str_radix(hex, 16) {
        Ok(rgb) => 0xFF00_0000 | rgb,
        Err(_) => 0,
    }
}

/// `argb()` sobre un atributo CSS crudo (`data-hover="#2a3140"`).
fn argb_from_attr(value: Option<&str>) -> u32 {
    argb_from_hex(parse_color(value).as_deref())
}

fn normalize_text(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// `<input type="text">` es el unico widget de entrada de v3. Sin `type` se
/// asume `text` (como el navegador); cualquier otro tipo queda fuera.
fn input_type_is_text(node: &Node) -> bool {
    match node.attr("type") {
        None => true,
        Some(value) => {
            let kind = value.trim();
            kind.is_empty() || kind.eq_ignore_ascii_case("text")
        }
    }
}

/// `maxlength` -> 0 (sin limite) o 1..=255. Valores invalidos se ignoran.
fn input_max_length(value: Option<&str>) -> u32 {
    value
        .and_then(|text| text.trim().parse::<u32>().ok())
        .map(|limit| limit.min(INPUT_MAX_BYTES as u32))
        .unwrap_or(0)
}

/// Solo ASCII imprimible (32..126), sin recortar palabras ni normalizar
/// espacios. `limit == 0` no recorta; el resto se descarta.
fn sanitize_input_text(text: &str, limit: u32) -> String {
    let max = if limit == 0 {
        INPUT_MAX_BYTES
    } else {
        (limit as usize).min(INPUT_MAX_BYTES)
    };
    text.chars()
        .filter(|ch| ch.is_ascii() && (32..=126).contains(&(*ch as u32)))
        .take(max)
        .collect()
}

struct StyleOut {
    display: Option<String>,
    direction: u32,
    justify: u32,
    align: u32,
    wrap: u32,
    flex_direction: String,
    justify_content: String,
    align_items: String,
    flex_wrap: String,
    gap: f64,
    /// v2 (solo se serializan con schema `/2`).
    row_gap: f64,
    column_gap: f64,
    grid_template_columns: Option<GridTemplateColumns>,
    grid_auto_rows: Option<GridAutoRows>,
    overflow: Option<String>,
    width_px: f64,
    width_pct: f64,
    height_px: f64,
    height_pct: f64,
    margin: [f64; 4],
    padding: [f64; 4],
    background: Option<String>,
    color: String,
    font_size: f64,
    font_family: String,
    font_weight: String,
    /// E1: alineacion horizontal del texto y caja de linea (solo se
    /// serializan cuando la pagina usa la feature).
    text_align: String,
    line_height: f64,
    margin_auto: [bool; 4],
    /// `box-sizing: border-box` (A). false = content-box (CSS default).
    box_sizing_border: bool,
    /// v3: bordes, radio, sombras y unidades relativas (solo schema `/2`).
    css3: StyleV3,
}

struct DataOut {
    hover: Option<String>,
    press: Option<String>,
    selected: Option<String>,
    action: Option<String>,
}

struct ArgbOut {
    background: u32,
    text: u32,
    hover: u32,
    press: u32,
    selected: u32,
}

struct TextOut {
    content: String,
    baseline: f64,
    glyphs: Vec<UiGlyphOut>,
}

/// Contenido de un nodo `kind:"image"` (solo schema `/2`).
struct ImageOut {
    src: Option<String>,
    alt: Option<String>,
    object_fit: String,
}

/// Contenido de un nodo `kind:"input"` (schema `/3`).
struct InputOut {
    value: String,
    placeholder: String,
    max_length: u32,
    password: bool,
    baseline: f64,
    line_height: f64,
    glyphs: Vec<UiInputGlyphOut>,
}

struct NodeOut {
    parent: i64,
    children: Vec<usize>,
    kind: u32,
    name: String,
    tag: String,
    classes: Vec<String>,
    element_id: Option<String>,
    style: StyleOut,
    data: DataOut,
    argb: ArgbOut,
    text: Option<TextOut>,
    image: Option<ImageOut>,
    input: Option<InputOut>,
}

/// Resumen para el log del build.
pub struct UiSummary {
    pub nodes: usize,
    pub glyphs: usize,
    pub runs: usize,
    pub input_glyphs: usize,
    pub atlas: Option<(u32, u32)>,
    pub atlas_path: Option<PathBuf>,
}

struct Builder<'a> {
    dom: &'a Dom,
    styles: &'a [Option<ComputedStyle>],
    engine: &'a TextEngine,
    baker: UiTextBaker,
    nodes: Vec<NodeOut>,
    name_counts: HashMap<String, usize>,
    /// Piscina ASCII cacheada por (familia, tamano redondeado, peso).
    input_pools: HashMap<(String, i32, u16), BakedInputPool>,
    glyph_count: usize,
    input_glyph_count: usize,
    run_count: usize,
}

impl<'a> Builder<'a> {
    fn new(
        dom: &'a Dom,
        styles: &'a [Option<ComputedStyle>],
        engine: &'a TextEngine,
        atlas_size: u32,
    ) -> Self {
        Builder {
            dom,
            styles,
            engine,
            baker: UiTextBaker::with_size(atlas_size),
            nodes: Vec::new(),
            name_counts: HashMap::new(),
            input_pools: HashMap::new(),
            glyph_count: 0,
            input_glyph_count: 0,
            run_count: 0,
        }
    }

    fn base_name(&self, id: NodeId) -> String {
        let node = &self.dom.nodes[id];
        let mut base = node.tag.clone();
        if let Some(element_id) = node.attr("id") {
            if !element_id.is_empty() {
                base.push('#');
                base.push_str(element_id);
            }
        }
        if let Some(first) = node.classes().next() {
            base.push('.');
            base.push_str(first);
        }
        base
    }

    fn unique_name(&mut self, base: &str) -> String {
        let count = self.name_counts.entry(base.to_string()).or_insert(0);
        let name = if *count == 0 {
            base.to_string()
        } else {
            format!("{base}[{count}]")
        };
        *count += 1;
        name
    }

    fn emit_element(
        &mut self,
        id: NodeId,
        parent: i64,
        name: &str,
        is_root: bool,
    ) -> Result<usize, String> {
        let style = self.styles[id]
            .as_ref()
            .ok_or_else(|| format!("nodo {} sin estilo computado", id))?;
        let is_image = self.dom.nodes[id].tag == "img";
        // `<input type="text">` (o sin `type`) es un widget editable de v3.
        // `password`, `checkbox`, etc. no forman parte de este contrato y se
        // emiten como bloque normal (subset avisa).
        let is_input = self.dom.nodes[id].tag == "input" && input_type_is_text(&self.dom.nodes[id]);
        let mut width_px = style.width.unwrap_or(-1.0);
        let mut width_pct = style.width_pct;
        let mut height_px = style.height.unwrap_or(-1.0);
        let mut height_pct = style.height_pct;
        // Imagen: `width`/`height` del HTML son presentational hints. Solo se
        // usan si el CSS no fijo la dimension (el CSS siempre gana; `auto`
        // explicito tambien gana y deja que el runtime mida el intrinseco).
        if is_image {
            if width_px < 0.0 && width_pct == 0.0 && !css_auto(style, "width") {
                if let Some(value) = attribute_length(self.dom.nodes[id].attr("width")) {
                    width_px = value;
                }
            }
            if height_px < 0.0 && height_pct == 0.0 && !css_auto(style, "height") {
                if let Some(value) = attribute_length(self.dom.nodes[id].attr("height")) {
                    height_px = value;
                }
            }
        }
        // Raiz: `%` contra el viewport, igual que el oraculo.
        if is_root {
            if width_pct == 0.0 && width_px < 0.0 {
                width_px = -1.0;
                width_pct = 100.0;
            }
            if height_pct == 0.0 && height_px < 0.0 {
                height_px = -1.0;
                height_pct = 100.0;
            }
        }
        // Un `<img>`/`<input>` inline si se emite; el resto de `inline` no.
        if style.display == "none" || (style.display == "inline" && !is_image && !is_input) {
            // El oraculo filtra por block/flex antes de llamar; los nodos no
            // renderizables de la raiz son un error de fixture.
            if is_root {
                return Err("body is not renderable".to_string());
            }
            return Err(format!("nodo {} no renderizable", id));
        }
        let kind = if is_image {
            KIND_IMAGE
        } else if is_input {
            KIND_INPUT
        } else if style.display == "grid" {
            KIND_GRID
        } else if style.display == "flex" {
            KIND_FLEX
        } else {
            KIND_BLOCK
        };
        let action = style
            .data_action
            .as_deref()
            .map(|value| value.trim().to_ascii_lowercase());
        let image = if is_image {
            Some(ImageOut {
                src: self.dom.nodes[id].attr("src").map(str::to_string),
                alt: self.dom.nodes[id].attr("alt").map(str::to_string),
                object_fit: style
                    .object_fit
                    .clone()
                    .unwrap_or_else(|| "fill".to_string()),
            })
        } else {
            None
        };
        let input = if is_input {
            let family = if style.font_family.is_empty() {
                DEFAULT_FAMILY.to_string()
            } else {
                style.font_family.clone()
            };
            let size = if style.font_size > 0.0 {
                style.font_size
            } else {
                DEFAULT_SIZE
            };
            let weight = parse_font_weight(&style.font_weight);
            let key_size = (size.round_ties_even() as i32).max(1);
            let key = (family.clone(), key_size, weight);
            let pool = if let Some(pool) = self.input_pools.get(&key) {
                pool.clone()
            } else {
                let baked = self
                    .baker
                    .bake_input_pool(self.engine, &family, size, weight)?;
                self.input_pools.insert(key, baked.clone());
                baked
            };
            let max_length = input_max_length(self.dom.nodes[id].attr("maxlength"));
            let value = sanitize_input_text(
                self.dom.nodes[id].attr("value").unwrap_or(""),
                max_length,
            );
            let placeholder = sanitize_input_text(
                self.dom.nodes[id].attr("placeholder").unwrap_or(""),
                INPUT_MAX_BYTES as u32,
            );
            Some(InputOut {
                value,
                placeholder,
                max_length,
                password: false,
                baseline: pool.ascender,
                line_height: pool.ascender + pool.descender,
                glyphs: pool.glyphs,
            })
        } else {
            None
        };
        let index = self.nodes.len();
        self.nodes.push(NodeOut {
            parent,
            children: Vec::new(),
            kind,
            name: name.to_string(),
            tag: self.dom.nodes[id].tag.clone(),
            classes: self.dom.nodes[id].classes().map(str::to_string).collect(),
            element_id: self.dom.nodes[id].attr("id").map(str::to_string),
            style: StyleOut {
                display: Some(style.display.clone()),
                direction: if style.flex_direction == "column" {
                    1
                } else {
                    0
                },
                justify: justify_code(&style.justify_content),
                align: align_code(&style.align_items),
                wrap: wrap_code(&style.flex_wrap),
                flex_direction: style.flex_direction.clone(),
                justify_content: style.justify_content.clone(),
                align_items: style.align_items.clone(),
                flex_wrap: style.flex_wrap.clone(),
                gap: style.gap,
                row_gap: style.row_gap,
                column_gap: style.column_gap,
                grid_template_columns: style.grid_template_columns.clone(),
                grid_auto_rows: style.grid_auto_rows.clone(),
                overflow: style.overflow.clone(),
                width_px,
                width_pct,
                height_px,
                height_pct,
                margin: style.margin,
                padding: style.padding,
                background: style.background.clone(),
                color: style.color.clone(),
                font_size: style.font_size,
                font_family: style.font_family.clone(),
                font_weight: style.font_weight.clone(),
                text_align: style.text_align.clone(),
                line_height: style.line_height,
                margin_auto: style.margin_auto,
                box_sizing_border: style.box_sizing_border,
                css3: style.css3.clone(),
            },
            data: DataOut {
                hover: style.data_hover.clone(),
                press: style.data_press.clone(),
                selected: style.data_selected.clone(),
                action,
            },
            argb: ArgbOut {
                background: argb_from_hex(style.background.as_deref()),
                text: text_color(style),
                hover: argb_from_attr(style.data_hover.as_deref()),
                press: argb_from_attr(style.data_press.as_deref()),
                selected: argb_from_attr(style.data_selected.as_deref()),
            },
            text: None,
            image,
            input,
        });

        // `<img>` y `<input>` son void: no tienen texto propio ni hijos.
        if is_image || is_input {
            self.input_glyph_count += self.nodes[index]
                .input
                .as_ref()
                .map(|input| input.glyphs.len())
                .unwrap_or(0);
            return Ok(index);
        }

        // Texto propio primero, como `Emitter._emit_element`.
        let text = normalize_text(&self.dom.nodes[id].text);
        if !text.is_empty() {
            let text_index = self.emit_text(style, index, name, &text)?;
            self.nodes[index].children.push(text_index);
        }

        let child_ids: Vec<NodeId> = self.dom.nodes[id]
            .children
            .iter()
            .copied()
            .filter(|child| self.is_renderable(*child))
            .collect();
        for child in child_ids {
            let base = self.base_name(child);
            let child_name = self.unique_name(&base);
            let full = format!("{name}/{child_name}");
            let child_index = self.emit_element(child, index as i64, &full, false)?;
            self.nodes[index].children.push(child_index);
        }
        Ok(index)
    }

    /// Hijos que el arbol v2/v3 conserva: block/flex/grid, `<img>` e
    /// `<input>` renderizables. `inline`, `none` y el resto se descartan.
    fn is_renderable(&self, child: NodeId) -> bool {
        let Some(style) = self.styles[child].as_ref() else {
            return false;
        };
        if style.display == "none" {
            return false;
        }
        if self.dom.nodes[child].tag == "img" {
            return true;
        }
        if self.dom.nodes[child].tag == "input" && input_type_is_text(&self.dom.nodes[child]) {
            return true;
        }
        matches!(style.display.as_str(), "block" | "flex" | "grid")
    }

    fn emit_text(
        &mut self,
        style: &ComputedStyle,
        parent: usize,
        parent_name: &str,
        text: &str,
    ) -> Result<usize, String> {
        let family = if style.font_family.is_empty() {
            DEFAULT_FAMILY.to_string()
        } else {
            style.font_family.clone()
        };
        let size = if style.font_size > 0.0 {
            style.font_size
        } else {
            DEFAULT_SIZE
        };
        let weight = parse_font_weight(&style.font_weight);
        let baked = self
            .baker
            .bake(self.engine, text, &family, size, weight)?;
        let BakedUiText {
            mut glyphs,
            width,
            ascender,
            descender,
        } = baked;
        // E1: `line-height` define la caja de linea. El texto se centra en ella
        // repartiendo el leading (half-leading de CSS) por arriba y por abajo;
        // los glyphs y el baseline se desplazan con el leading superior.
        let natural = ascender + descender;
        let line_height = if style.line_height > 0.0 {
            style.line_height
        } else {
            natural
        };
        let leading = (line_height - natural) * 0.5;
        if leading != 0.0 {
            for glyph in &mut glyphs {
                glyph.y += leading;
            }
        }
        let node_width = width.ceil();
        let node_height = line_height.ceil();
        let baseline = ascender + leading;
        let text_name = self.unique_name("text");
        let index = self.nodes.len();
        self.nodes.push(NodeOut {
            parent: parent as i64,
            children: Vec::new(),
            kind: KIND_TEXT,
            name: format!("{parent_name}/{text_name}"),
            tag: "#text".to_string(),
            classes: Vec::new(),
            element_id: None,
            style: StyleOut {
                display: None,
                direction: 0,
                justify: 0,
                align: 0,
                wrap: 0,
                flex_direction: "row".to_string(),
                justify_content: "flex-start".to_string(),
                align_items: "stretch".to_string(),
                flex_wrap: "nowrap".to_string(),
                gap: 0.0,
                row_gap: 0.0,
                column_gap: 0.0,
                grid_template_columns: None,
                grid_auto_rows: None,
                overflow: None,
                width_px: node_width,
                width_pct: 0.0,
                height_px: node_height,
                height_pct: 0.0,
                margin: [0.0; 4],
                padding: [0.0; 4],
                background: None,
                color: style.color.clone(),
                font_size: size,
                font_family: family,
                font_weight: style.font_weight.clone(),
                text_align: style.text_align.clone(),
                line_height: style.line_height,
                margin_auto: [false; 4],
                box_sizing_border: style.box_sizing_border,
                css3: StyleV3::default(),
            },
            data: DataOut {
                hover: None,
                press: None,
                selected: None,
                action: None,
            },
            argb: ArgbOut {
                background: 0,
                text: text_color(style),
                hover: 0,
                press: 0,
                selected: 0,
            },
            text: Some(TextOut {
                content: text.to_string(),
                baseline,
                glyphs,
            }),
            image: None,
            input: None,
        });
        self.glyph_count += self.nodes[index]
            .text
            .as_ref()
            .map(|text| text.glyphs.len())
            .unwrap_or(0);
        self.run_count += 1;
        Ok(index)
    }
}

fn text_color(style: &ComputedStyle) -> u32 {
    let value = argb_from_hex(Some(style.color.as_str()));
    if value == 0 {
        0xFF00_0000
    } else {
        value
    }
}

// ------------------------------------------------------------------- JSON

fn push_float(out: &mut String, value: f64) {
    out.push_str(&py_float(value));
}

fn push_float_array4(out: &mut String, values: &[f64; 4]) {
    out.push('[');
    for (index, value) in values.iter().enumerate() {
        if index > 0 {
            out.push_str(", ");
        }
        push_float(out, *value);
    }
    out.push(']');
}

fn push_opt_string(out: &mut String, value: Option<&str>) {
    match value {
        Some(text) => push_json_string(out, text),
        None => out.push_str("null"),
    }
}

/// Escribe `"<prefix>_em"`, `"<prefix>_rem"`, `"<prefix>_vh"` y
/// `"<prefix>_vw"` (float o null) para una longitud declarada.
fn push_unit_fields(out: &mut String, prefix: &str, rel: Option<RelLength>) {
    for unit in [Unit::Em, Unit::Rem, Unit::Vh, Unit::Vw] {
        let _ = write!(out, ",\n        \"{prefix}_{}\": ", unit.suffix());
        match rel.and_then(|value| value.in_unit(unit)) {
            Some(value) => push_float(out, value),
            None => out.push_str("null"),
        }
    }
}

/// Cuatro arrays (`<name>_em`, `<name>_rem`, `<name>_vh`, `<name>_vw`) con la
/// unidad declarada por lado; `null` en los lados que no usan esa unidad.
fn push_unit_arrays4(out: &mut String, name: &str, values: &[Option<RelLength>; 4]) {
    for unit in [Unit::Em, Unit::Rem, Unit::Vh, Unit::Vw] {
        let _ = write!(out, ",\n        \"{name}_{}\": [", unit.suffix());
        for (index, rel) in values.iter().enumerate() {
            if index > 0 {
                out.push_str(", ");
            }
            match rel.and_then(|value| value.in_unit(unit)) {
                Some(value) => push_float(out, value),
                None => out.push_str("null"),
            }
        }
        out.push(']');
    }
}

fn push_border(out: &mut String, border: &BorderOut) {
    let (width, width_rel) = border.uniform_width();
    out.push_str(",\n        \"border_width_px\": ");
    push_float(out, width);
    push_unit_fields(out, "border_width", width_rel);
    out.push_str(",\n        \"border_color\": ");
    push_opt_string(out, border.visible_color());
    out.push_str(",\n        \"border_style\": ");
    push_json_string(out, border.style_name());
    out.push_str(",\n        \"border_sides\": {");
    for (index, (name, side)) in border.sides().into_iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        let _ = write!(out, "\n          \"{name}\": {{ \"width_px\": ");
        push_float(out, side.width_px);
        for unit in [Unit::Em, Unit::Rem, Unit::Vh, Unit::Vw] {
            let _ = write!(out, ", \"width_{}\": ", unit.suffix());
            match side.width_rel.and_then(|value| value.in_unit(unit)) {
                Some(value) => push_float(out, value),
                None => out.push_str("null"),
            }
        }
        out.push_str(", \"color\": ");
        push_opt_string(out, side.color.as_deref());
        out.push_str(", \"style\": ");
        push_json_string(out, &side.style);
        out.push_str(" }");
    }
    out.push_str("\n        }");
}

fn push_border_radius(out: &mut String, radius: &[f64; 4], rel: Option<RelLength>) {
    let uniform = radius.iter().copied().fold(0.0f64, f64::max);
    out.push_str(",\n        \"border_radius_px\": ");
    push_float(out, uniform);
    push_unit_fields(out, "border_radius", rel);
    out.push_str(",\n        \"border_radii_px\": [");
    for (index, value) in radius.iter().enumerate() {
        if index > 0 {
            out.push_str(", ");
        }
        push_float(out, *value);
    }
    out.push(']');
}

fn push_box_shadow(out: &mut String, shadows: &[BoxShadow]) {
    out.push_str(",\n        \"box_shadow\": ");
    if shadows.is_empty() {
        out.push_str("null");
        return;
    }
    out.push('[');
    for (index, shadow) in shadows.iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        out.push_str("\n          { \"offset_x_px\": ");
        push_float(out, shadow.offset_x_px);
        out.push_str(", \"offset_y_px\": ");
        push_float(out, shadow.offset_y_px);
        out.push_str(", \"blur_px\": ");
        push_float(out, shadow.blur_px);
        out.push_str(", \"spread_px\": ");
        push_float(out, shadow.spread_px);
        out.push_str(", \"color\": ");
        push_json_string(out, &shadow.color);
        out.push_str(", \"inset\": ");
        out.push_str(if shadow.inset { "true" } else { "false" });
        out.push_str(" }");
    }
    out.push_str("\n        ]");
}

fn push_style_v3(out: &mut String, css3: &StyleV3) {
    out.push_str(",\n        \"opacity\": ");
    push_float(out, css3.opacity);

    push_unit_fields(out, "width", css3.width_rel);
    push_unit_fields(out, "height", css3.height_rel);

    out.push_str(",\n        \"min_width_px\": ");
    push_float(out, css3.min_width.unwrap_or(-1.0));
    out.push_str(",\n        \"min_width_pct\": ");
    push_float(out, css3.min_width_pct);
    push_unit_fields(out, "min_width", css3.min_width_rel);
    out.push_str(",\n        \"max_width_px\": ");
    push_float(out, css3.max_width.unwrap_or(-1.0));
    out.push_str(",\n        \"max_width_pct\": ");
    push_float(out, css3.max_width_pct);
    push_unit_fields(out, "max_width", css3.max_width_rel);
    out.push_str(",\n        \"min_height_px\": ");
    push_float(out, css3.min_height.unwrap_or(-1.0));
    out.push_str(",\n        \"min_height_pct\": ");
    push_float(out, css3.min_height_pct);
    push_unit_fields(out, "min_height", css3.min_height_rel);
    out.push_str(",\n        \"max_height_px\": ");
    push_float(out, css3.max_height.unwrap_or(-1.0));
    out.push_str(",\n        \"max_height_pct\": ");
    push_float(out, css3.max_height_pct);
    push_unit_fields(out, "max_height", css3.max_height_rel);

    push_unit_arrays4(out, "margin", &css3.margin_rel);
    push_unit_arrays4(out, "padding", &css3.padding_rel);
    push_unit_fields(out, "gap", css3.gap_rel);
    push_unit_fields(out, "row_gap", css3.row_gap_rel);
    push_unit_fields(out, "column_gap", css3.column_gap_rel);

    push_border(out, &css3.border);
    push_border_radius(out, &css3.border_radius, css3.border_radius_rel);
    push_box_shadow(out, &css3.box_shadow);
}

fn push_style(out: &mut String, style: &StyleOut, detailed: bool, extras: bool) {
    out.push_str("      \"style\": {\n");
    out.push_str("        \"display\": ");
    push_opt_string(out, style.display.as_deref());
    let _ = write!(
        out,
        ",\n        \"direction\": {},\n        \"justify\": {},\n        \"align\": {},\n        \"wrap\": {},\n",
        style.direction,
        style.justify,
        style.align,
        style.wrap
    );
    out.push_str("        \"flex_direction\": ");
    push_json_string(out, &style.flex_direction);
    out.push_str(",\n        \"justify_content\": ");
    push_json_string(out, &style.justify_content);
    out.push_str(",\n        \"align_items\": ");
    push_json_string(out, &style.align_items);
    out.push_str(",\n        \"flex_wrap\": ");
    push_json_string(out, &style.flex_wrap);
    out.push_str(",\n        \"gap\": ");
    push_float(out, style.gap);
    if detailed {
        // Campos v2 (`docs/ui-tree-v2.md`). En `/1` no se escriben para que
        // las paginas existentes sigan siendo byte a byte identicas. `/3` los
        // hereda de `/2` (es aditivo).
        out.push_str(",\n        \"row_gap\": ");
        push_float(out, style.row_gap);
        out.push_str(",\n        \"column_gap\": ");
        push_float(out, style.column_gap);
        out.push_str(",\n        \"grid_template_columns\": ");
        match &style.grid_template_columns {
            None => out.push_str("null"),
            Some(grid) => {
                out.push_str("{ \"mode\": ");
                push_json_string(out, &grid.mode);
                out.push_str(", \"size_px\": ");
                push_float(out, grid.size_px);
                out.push_str(" }");
            }
        }
        out.push_str(",\n        \"grid_auto_rows\": ");
        match &style.grid_auto_rows {
            None => out.push_str("null"),
            Some(GridAutoRows::Auto) => out.push_str("\"auto\""),
            Some(GridAutoRows::Px(px)) => {
                out.push_str("{ \"px\": ");
                push_float(out, *px);
                out.push_str(" }");
            }
        }
        out.push_str(",\n        \"overflow\": ");
        push_opt_string(out, style.overflow.as_deref());
    }
    out.push_str(",\n        \"width_px\": ");
    push_float(out, style.width_px);
    out.push_str(",\n        \"width_pct\": ");
    push_float(out, style.width_pct);
    out.push_str(",\n        \"height_px\": ");
    push_float(out, style.height_px);
    out.push_str(",\n        \"height_pct\": ");
    push_float(out, style.height_pct);
    out.push_str(",\n        \"margin\": ");
    push_float_array4(out, &style.margin);
    out.push_str(",\n        \"padding\": ");
    push_float_array4(out, &style.padding);
    out.push_str(",\n        \"background\": ");
    push_opt_string(out, style.background.as_deref());
    out.push_str(",\n        \"color\": ");
    push_json_string(out, &style.color);
    out.push_str(",\n        \"font_size\": ");
    push_float(out, style.font_size);
    out.push_str(",\n        \"font_family\": ");
    push_json_string(out, &style.font_family);
    out.push_str(",\n        \"font_weight\": ");
    push_json_string(out, &style.font_weight);
    if extras {
        // E1 (`text-align`, `line-height`, `margin:auto`). Solo se escriben si
        // la pagina usa alguna: las paginas existentes siguen byte a byte.
        out.push_str(",\n        \"text_align\": ");
        push_json_string(out, &style.text_align);
        out.push_str(",\n        \"line_height\": ");
        push_float(out, style.line_height);
        out.push_str(",\n        \"margin_auto\": [");
        for (index, value) in style.margin_auto.iter().enumerate() {
            if index > 0 {
                out.push_str(", ");
            }
            out.push_str(if *value { "1" } else { "0" });
        }
        out.push(']');
        out.push_str(",\n        \"box_sizing\": ");
        push_json_string(
            out,
            if style.box_sizing_border {
                "border-box"
            } else {
                "content-box"
            },
        );
    }
    if detailed {
        push_style_v3(out, &style.css3);
    }
    out.push_str("\n      }");
}

fn push_nodes_json(out: &mut String, nodes: &[NodeOut], detailed: bool, extras: bool) {
    for (index, node) in nodes.iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        out.push_str("\n    {\n");
        let _ = write!(
            out,
            "      \"id\": {},\n      \"parent\": {},\n      \"children\": [",
            index, node.parent
        );
        for (child_index, child) in node.children.iter().enumerate() {
            if child_index > 0 {
                out.push_str(", ");
            }
            let _ = write!(out, "{child}");
        }
        out.push_str("],\n      \"kind\": ");
        push_json_string(out, kind_name(node.kind));
        out.push_str(",\n      \"name\": ");
        push_json_string(out, &node.name);
        out.push_str(",\n      \"tag\": ");
        push_json_string(out, &node.tag);
        out.push_str(",\n      \"classes\": [");
        for (class_index, class) in node.classes.iter().enumerate() {
            if class_index > 0 {
                out.push_str(", ");
            }
            push_json_string(out, class);
        }
        out.push_str("],\n      \"element_id\": ");
        push_opt_string(out, node.element_id.as_deref());
        out.push_str(",\n");
        push_style(out, &node.style, detailed, extras);
        out.push_str(",\n      \"data\": {\n        \"hover\": ");
        push_opt_string(out, node.data.hover.as_deref());
        out.push_str(",\n        \"press\": ");
        push_opt_string(out, node.data.press.as_deref());
        out.push_str(",\n        \"selected\": ");
        push_opt_string(out, node.data.selected.as_deref());
        out.push_str(",\n        \"action\": ");
        push_opt_string(out, node.data.action.as_deref());
        out.push_str("\n      },\n      \"argb\": {\n        \"background\": ");
        let _ = write!(out, "{}", node.argb.background);
        out.push_str(",\n        \"text\": ");
        let _ = write!(out, "{}", node.argb.text);
        out.push_str(",\n        \"hover\": ");
        let _ = write!(out, "{}", node.argb.hover);
        out.push_str(",\n        \"press\": ");
        let _ = write!(out, "{}", node.argb.press);
        out.push_str(",\n        \"selected\": ");
        let _ = write!(out, "{}", node.argb.selected);
        out.push_str("\n      },\n      \"text\": ");
        match &node.text {
            None => out.push_str("null"),
            Some(text) => {
                out.push_str("{\n        \"content\": ");
                push_json_string(out, &text.content);
                out.push_str(",\n        \"baseline\": ");
                push_float(out, text.baseline);
                out.push_str(",\n        \"glyphs\": [");
                for (glyph_index, glyph) in text.glyphs.iter().enumerate() {
                    if glyph_index > 0 {
                        out.push(',');
                    }
                    let _ = write!(
                        out,
                        "\n          {{ \"gid\": {}, \"advance\": {}, \"x\": {}, \"y\": {}, \"w\": {}, \"h\": {}, \"u0\": {}, \"v0\": {}, \"u1\": {}, \"v1\": {} }}",
                        glyph.gid,
                        py_float(glyph.advance),
                        py_float(glyph.x),
                        py_float(glyph.y),
                        glyph.w,
                        glyph.h,
                        py_float(glyph.u0),
                        py_float(glyph.v0),
                        py_float(glyph.u1),
                        py_float(glyph.v1)
                    );
                }
                out.push_str("\n        ]\n      }");
            }
        }
        // Solo los nodos input llevan `input` (schema `/3`).
        if let Some(input) = &node.input {
            out.push_str(",\n      \"input\": {\n        \"value\": ");
            push_json_string(out, &input.value);
            out.push_str(",\n        \"placeholder\": ");
            push_json_string(out, &input.placeholder);
            let _ = write!(
                out,
                ",\n        \"max_length\": {},\n        \"password\": {},\n        \"baseline\": {},\n        \"line_height\": {},\n        \"glyphs\": [",
                input.max_length,
                if input.password { "true" } else { "false" },
                py_float(input.baseline),
                py_float(input.line_height)
            );
            for (glyph_index, glyph) in input.glyphs.iter().enumerate() {
                if glyph_index > 0 {
                    out.push(',');
                }
                let _ = write!(
                    out,
                    "\n          {{ \"cp\": {}, \"gid\": {}, \"advance\": {}, \"x\": {}, \"y\": {}, \"w\": {}, \"h\": {}, \"u0\": {}, \"v0\": {}, \"u1\": {}, \"v1\": {} }}",
                    glyph.cp,
                    glyph.gid,
                    py_float(glyph.advance),
                    py_float(glyph.x),
                    py_float(glyph.y),
                    glyph.w,
                    glyph.h,
                    py_float(glyph.u0),
                    py_float(glyph.v0),
                    py_float(glyph.u1),
                    py_float(glyph.v1)
                );
            }
            out.push_str("\n        ]\n      }");
        }
        // Solo los nodos imagen llevan `image`; asi `/1` no cambia.
        if let Some(image) = &node.image {
            out.push_str(",\n      \"image\": {\n        \"src\": ");
            push_opt_string(out, image.src.as_deref());
            out.push_str(",\n        \"alt\": ");
            push_opt_string(out, image.alt.as_deref());
            out.push_str(",\n        \"object_fit\": ");
            push_json_string(out, &image.object_fit);
            out.push_str("\n      }");
        }
        out.push_str("\n    }");
    }
}

struct AtlasRef<'a> {
    atlas: &'a Atlas,
    file: &'a str,
    json: &'a str,
}

fn push_atlas_json(out: &mut String, reference: Option<&AtlasRef<'_>>) {
    match reference {
        None => out.push_str("null"),
        Some(item) => {
            let atlas = item.atlas;
            let hash = sha256::hex(&sha256::digest(&atlas.data));
            out.push_str("{\n      \"file\": ");
            push_json_string(out, item.file);
            out.push_str(",\n      \"json\": ");
            push_json_string(out, item.json);
            let _ = write!(
                out,
                ",\n      \"width\": {},\n      \"height\": {},\n      \"format\": \"r8unorm\",\n      \"size_bytes\": {},\n      \"sha256\": \"{}\",\n      \"glyph_count\": {},\n      \"pixels_nonzero\": {}\n    }}",
                atlas.width(),
                atlas.height(),
                atlas.data.len(),
                hash,
                atlas.glyph_count(),
                atlas.pixels_nonzero()
            );
        }
    }
}

// ------------------------------------------------------------------ driver

/// `width="180"` / `height="240px"` -> px; `%`/`auto`/basura -> `None`.
fn attribute_length(value: Option<&str>) -> Option<f64> {
    let px = crate::color::length(value)?;
    if px.is_finite() && px >= 0.0 {
        Some(px)
    } else {
        None
    }
}

/// `true` si el CSS declaro `auto` explicitamente para `width`/`height`.
fn css_auto(style: &ComputedStyle, name: &str) -> bool {
    style
        .raw
        .get(name)
        .is_some_and(|value| value.trim().eq_ignore_ascii_case("auto"))
}

/// Una pagina emite `/2` si algun nodo del arbol usa grid, scroll o imagen.
/// Se decide sobre los nodos ya emitidos (no sobre CSS muerto): un `<img>`
/// dentro de `display:none` no cuenta. `overflow: hidden` no fuerza v2: ya
/// existia en v1 y no anade scroll.
fn nodes_use_v2(nodes: &[NodeOut]) -> bool {
    nodes.iter().any(|node| {
        node.kind == KIND_GRID
            || node.kind == KIND_IMAGE
            || matches!(
                node.style.overflow.as_deref(),
                Some("auto") | Some("scroll")
            )
            || node.style.grid_template_columns.is_some()
            || node.style.grid_auto_rows.is_some()
            || node.style.css3.declares_style()
    })
}

/// Una pagina emite `/3` (aditivo sobre `/2`) si algun nodo emitido es un
/// `<input type="text">`. El schema se decide sobre nodos ya emitidos: un
/// input dentro de `display:none` no cuenta.
fn nodes_have_input(nodes: &[NodeOut]) -> bool {
    nodes.iter().any(|node| node.input.is_some())
}

/// Emite `ui.json` (+ atlas) desde el DOM y la cascada ya calculados.
///
/// `ui_path` es el fichero `ui.json`; el atlas se escribe al lado con nombres
/// fijos `atlas.bin` / `atlas.json`, referenciados desde el JSON.
#[allow(clippy::too_many_arguments)]
pub fn emit_ui_tree(
    dom: &Dom,
    styles: &[Option<ComputedStyle>],
    engine: &TextEngine,
    viewport: (u32, u32),
    source_html: &str,
    source_css: Option<&str>,
    ui_path: &Path,
    rule_count: usize,
) -> Result<UiSummary, String> {
    let body = dom
        .preorder()
        .into_iter()
        .find(|id| dom.nodes[*id].tag == "body")
        .ok_or_else(|| "el documento no tiene <body>".to_string())?;
    let body_style = styles
        .get(body)
        .and_then(Option::as_ref)
        .ok_or_else(|| "body sin estilo computado".to_string())?;
    if !matches!(body_style.display.as_str(), "block" | "flex" | "grid") {
        return Err("body is not renderable".to_string());
    }

    // La piscina ASCII de un input necesita mas atlas; las paginas sin input
    // conservan 256x256 (atlas y UVs byte-identicos a la version anterior).
    // Un input dentro de display:none no llega a emitirse ni cuenta.
    let has_input = dom.preorder().into_iter().any(|id| {
        if dom.nodes[id].tag != "input" || !input_type_is_text(&dom.nodes[id]) {
            return false;
        }
        let mut cursor = Some(id);
        while let Some(node) = cursor {
            if let Some(style) = styles.get(node).and_then(Option::as_ref) {
                if style.display == "none" {
                    return false;
                }
            }
            cursor = dom.nodes[node].parent;
        }
        true
    });
    let atlas_size = if has_input {
        UI_INPUT_ATLAS_SIZE
    } else {
        crate::text::UI_ATLAS_SIZE
    };

    let mut builder = Builder::new(dom, styles, engine, atlas_size);
    builder.emit_element(body, -1, "body.root", true)?;
    let Builder {
        nodes,
        baker,
        glyph_count,
        input_glyph_count,
        run_count,
        ..
    } = builder;

    let v3 = nodes_have_input(&nodes);
    let v2 = nodes_use_v2(&nodes);
    let detailed = v2 || v3;
    // E1: la pagina usa text-align/line-height/margin:auto en algun nodo. Solo
    // entonces se serializan los campos nuevos (las paginas existentes no
    // cambian ni un byte).
    let extras = nodes.iter().any(|node| {
        node.style.text_align != "left"
            || node.style.line_height != 0.0
            || node.style.margin_auto.iter().any(|value| *value)
            || node.style.box_sizing_border
    });
    let schema = if v3 {
        SCHEMA_V3
    } else if v2 {
        SCHEMA_V2
    } else {
        SCHEMA_V1
    };
    let atlas: Option<AtlasRef<'_>> = if run_count > 0 || input_glyph_count > 0 {
        Some(AtlasRef {
            atlas: baker.atlas(),
            file: "atlas.bin",
            json: "atlas.json",
        })
    } else {
        None
    };

    let mut out = String::with_capacity(
        4096 + nodes.len() * 768 + (glyph_count + input_glyph_count) * 160,
    );
    out.push_str("{\n  \"schema\": ");
    push_json_string(&mut out, schema);
    out.push_str(",\n  \"generator\": \"frontend-rust\",\n  \"source\": {\n    \"html\": ");
    push_json_string(&mut out, source_html);
    out.push_str(",\n    \"css\": ");
    push_opt_string(&mut out, source_css);
    let _ = write!(
        out,
        "\n  }},\n  \"viewport\": {{ \"width\": {}, \"height\": {} }},\n  \"rule_count\": {},\n  \"node_count\": {},\n  \"text\": {{ \"glyph_count\": {}, \"text_run_count\": {} }},\n  \"atlas\": ",
        viewport.0, viewport.1, rule_count, nodes.len(), glyph_count, run_count
    );
    push_atlas_json(&mut out, atlas.as_ref());
    out.push_str(",\n  \"nodes\": [");
    push_nodes_json(&mut out, &nodes, detailed, extras);
    out.push_str("\n  ]\n}\n");

    if let Some(parent) = ui_path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|err| format!("{}: {err}", parent.display()))?;
        }
    }
    fs::write(ui_path, out).map_err(|err| format!("{}: {err}", ui_path.display()))?;

    let mut atlas_path = None;
    if let Some(item) = &atlas {
        let bin_path = ui_path.with_file_name(item.file);
        fs::write(&bin_path, &item.atlas.data)
            .map_err(|err| format!("{}: {err}", bin_path.display()))?;
        let json_path = ui_path.with_file_name(item.json);
        fs::write(&json_path, crate::emit::atlas_json(item.atlas))
            .map_err(|err| format!("{}: {err}", json_path.display()))?;
        atlas_path = Some(bin_path);
    } else {
        // Un documento sin texto no debe dejar un atlas viejo al lado del JSON.
        for name in ["atlas.bin", "atlas.json"] {
            let stale = ui_path.with_file_name(name);
            if stale.exists() {
                let _ = fs::remove_file(stale);
            }
        }
    }

    Ok(UiSummary {
        nodes: nodes.len(),
        glyphs: glyph_count,
        runs: run_count,
        input_glyphs: input_glyph_count,
        atlas: atlas
            .as_ref()
            .map(|item| (item.atlas.width(), item.atlas.height())),
        atlas_path,
    })
}
