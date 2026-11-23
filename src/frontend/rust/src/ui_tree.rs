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

use std::collections::HashMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use crate::cascade::ComputedStyle;
use crate::color::parse_color;
use crate::dom::{Dom, NodeId};
use crate::emit::{push_json_string, py_float};
use crate::sha256;
use crate::text::{Atlas, BakedUiText, TextEngine, UiGlyphOut, UiTextBaker};

pub const SCHEMA: &str = "webnative.ui-tree/1";

const KIND_BLOCK: u32 = 0;
const KIND_FLEX: u32 = 1;
const KIND_TEXT: u32 = 2;

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

/// `argb()` del oraculo para un color ya normalizado a `#rrggbb`.
fn argb_from_hex(value: Option<&str>) -> u32 {
    let Some(text) = value else { return 0 };
    let Some(hex) = text.strip_prefix('#') else { return 0 };
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

struct StyleOut {
    display: Option<&'static str>,
    direction: u32,
    justify: u32,
    align: u32,
    wrap: u32,
    flex_direction: String,
    justify_content: String,
    align_items: String,
    flex_wrap: String,
    gap: f64,
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
}

/// Resumen para el log del build.
pub struct UiSummary {
    pub nodes: usize,
    pub glyphs: usize,
    pub runs: usize,
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
    glyph_count: usize,
    run_count: usize,
}

impl<'a> Builder<'a> {
    fn new(dom: &'a Dom, styles: &'a [Option<ComputedStyle>], engine: &'a TextEngine) -> Self {
        Builder {
            dom,
            styles,
            engine,
            baker: UiTextBaker::new(),
            nodes: Vec::new(),
            name_counts: HashMap::new(),
            glyph_count: 0,
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
        let mut width_px = style.width.unwrap_or(-1.0);
        let mut width_pct = style.width_pct;
        let mut height_px = style.height.unwrap_or(-1.0);
        let mut height_pct = style.height_pct;
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
        if style.display == "none" || style.display == "inline" {
            // El oraculo filtra por block/flex antes de llamar; los nodos no
            // renderizables de la raiz son un error de fixture.
            if is_root {
                return Err("body is not renderable".to_string());
            }
            return Err(format!("nodo {} no renderizable", id));
        }
        let kind = if style.display == "flex" {
            KIND_FLEX
        } else {
            KIND_BLOCK
        };
        let action = style
            .data_action
            .as_deref()
            .map(|value| value.trim().to_ascii_lowercase());
        let index = self.nodes.len();
        self.nodes.push(NodeOut {
            parent,
            children: Vec::new(),
            kind,
            name: name.to_string(),
            tag: self.dom.nodes[id].tag.clone(),
            classes: self.dom.nodes[id]
                .classes()
                .map(str::to_string)
                .collect(),
            element_id: self.dom.nodes[id].attr("id").map(str::to_string),
            style: StyleOut {
                display: Some(if kind == KIND_FLEX { "flex" } else { "block" }),
                direction: if style.flex_direction == "column" { 1 } else { 0 },
                justify: justify_code(&style.justify_content),
                align: align_code(&style.align_items),
                wrap: wrap_code(&style.flex_wrap),
                flex_direction: style.flex_direction.clone(),
                justify_content: style.justify_content.clone(),
                align_items: style.align_items.clone(),
                flex_wrap: style.flex_wrap.clone(),
                gap: style.gap,
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
        });

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
            .filter(|child| {
                matches!(
                    self.styles[*child].as_ref().map(|style| style.display.as_str()),
                    Some("block") | Some("flex")
                )
            })
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
        let baked = self.baker.bake(self.engine, text, &family, size)?;
        let BakedUiText {
            glyphs,
            width,
            ascender,
            descender,
        } = baked;
        let node_width = width.ceil();
        let node_height = (ascender + descender).ceil();
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
                baseline: ascender,
                glyphs,
            }),
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

fn push_style(out: &mut String, style: &StyleOut) {
    out.push_str("      \"style\": {\n");
    out.push_str("        \"display\": ");
    push_opt_string(out, style.display);
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
    out.push_str("\n      }");
}

fn push_nodes_json(out: &mut String, nodes: &[NodeOut]) {
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
        push_json_string(
            out,
            match node.kind {
                KIND_FLEX => "flex",
                KIND_TEXT => "text",
                _ => "block",
            },
        );
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
        push_style(out, &node.style);
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
    if body_style.display != "block" && body_style.display != "flex" {
        return Err("body is not renderable".to_string());
    }

    let mut builder = Builder::new(dom, styles, engine);
    builder.emit_element(body, -1, "body.root", true)?;
    let Builder {
        nodes,
        baker,
        glyph_count,
        run_count,
        ..
    } = builder;

    let atlas: Option<AtlasRef<'_>> = if run_count > 0 {
        Some(AtlasRef {
            atlas: baker.atlas(),
            file: "atlas.bin",
            json: "atlas.json",
        })
    } else {
        None
    };

    let mut out = String::with_capacity(4096 + nodes.len() * 768 + glyph_count * 160);
    out.push_str("{\n  \"schema\": ");
    push_json_string(&mut out, SCHEMA);
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
    push_nodes_json(&mut out, &nodes);
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
        atlas: atlas.as_ref().map(|item| (item.atlas.width(), item.atlas.height())),
        atlas_path,
    })
}
