//! **Pre-layout** style tree consumed by the blob writer.
//!
//! The tree comes out of the cascade with computed values and no geometry.
//! Text travels as a string plus style; the runtime measures, shapes and
//! rasterizes it.
//!
//! Flattening mirrors the historical `Emitter` in
//! `src/emitter/python/emit_ui.py`: pre-order, own text before children,
//! `tag#id.class` names with an `[n]` suffix for duplicates, and `%` kept as
//! `width_pct`.
//!
//! No layout pass runs: there are no box x/y/w/h, only style. The `.wnui` blob
//! always carries every field; `detailed` records whether the page needs the
//! v2/v3 fields (grid, scroll, images, CSS v3, input). The blob writer uses it
//! to keep v1 pages byte-identical.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::parse::dom::{Dom, Node, NodeId};
use crate::style::cascade::ComputedStyle;
use crate::style::color::argb;
use crate::style::grid::{GridAutoRows, GridTemplateColumns};
use crate::style::style_ext::StyleV3;

/// Node kinds stored in the blob and exposed by the library tree.
pub const KIND_BLOCK: u32 = 0;
pub const KIND_FLEX: u32 = 1;
pub const KIND_TEXT: u32 = 2;
pub const KIND_GRID: u32 = 3;
pub const KIND_IMAGE: u32 = 4;
pub const KIND_INPUT: u32 = 5;

/// Hard cap of the edit buffer; the HTML `maxlength` may be lower.
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
        // The oracle uses `ALIGN.get(..., 3)`: unknown means stretch.
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

fn normalize_text(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// `<input type="text">` is the only v3 input widget. Without `type` it is
/// assumed to be `text` (like the browser); any other type is out of scope.
fn input_type_is_text(node: &Node) -> bool {
    match node.attr("type") {
        None => true,
        Some(value) => {
            let kind = value.trim();
            kind.is_empty() || kind.eq_ignore_ascii_case("text")
        }
    }
}

/// `maxlength` -> 0 (no limit) or 1..=255. Invalid values are ignored.
fn input_max_length(value: Option<&str>) -> u32 {
    value
        .and_then(|text| text.trim().parse::<u32>().ok())
        .map(|limit| limit.min(INPUT_MAX_BYTES as u32))
        .unwrap_or(0)
}

/// Printable ASCII only (32..126), without trimming words or normalizing
/// spaces. `limit == 0` does not truncate; the rest is dropped.
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

#[derive(Debug, Clone)]
pub struct StyleOut {
    pub display: Option<String>,
    pub direction: u32,
    pub justify: u32,
    pub align: u32,
    pub wrap: u32,
    pub flex_direction: String,
    pub justify_content: String,
    pub align_items: String,
    pub flex_wrap: String,
    pub gap: f64,
    /// v2 (only serialized with schema `/2`).
    pub row_gap: f64,
    pub column_gap: f64,
    pub grid_template_columns: Option<GridTemplateColumns>,
    pub grid_auto_rows: Option<GridAutoRows>,
    pub overflow: Option<String>,
    pub width_px: f64,
    pub width_pct: f64,
    pub height_px: f64,
    pub height_pct: f64,
    pub margin: [f64; 4],
    pub padding: [f64; 4],
    pub background: Option<String>,
    pub color: String,
    pub font_size: f64,
    pub font_family: String,
    pub font_weight: String,
    /// E1: horizontal text alignment and line box (only serialized when the
    /// page uses the feature).
    pub text_align: String,
    pub line_height: f64,
    pub margin_auto: [bool; 4],
    /// `box-sizing: border-box` (A). false = content-box (the CSS default).
    pub box_sizing_border: bool,
    /// Flex growth (E8): grow, shrink, basis px (-1 = auto).
    pub flex_grow: f64,
    pub flex_shrink: f64,
    pub flex_basis: f64,
    /// v3: borders, radius, shadows and relative units (only schema `/2`).
    pub css3: StyleV3,
}

#[derive(Debug, Clone)]
pub struct DataOut {
    pub hover: Option<String>,
    pub press: Option<String>,
    pub selected: Option<String>,
    pub action: Option<String>,
}

#[derive(Debug, Clone)]
pub struct ArgbOut {
    pub background: u32,
    pub text: u32,
    pub hover: u32,
    pub press: u32,
    pub selected: u32,
}

#[derive(Debug, Clone)]
pub struct TextOut {
    pub content: String,
}

/// Content of a `kind:"image"` node (only schema `/2`).
#[derive(Debug, Clone)]
pub struct ImageOut {
    pub src: Option<String>,
    pub alt: Option<String>,
    pub object_fit: String,
}

/// Content of a `kind:"input"` node (schema `/3`).
#[derive(Debug, Clone)]
pub struct InputOut {
    pub value: String,
    pub placeholder: String,
    pub max_length: u32,
    pub password: bool,
}

#[derive(Debug, Clone)]
pub struct NodeOut {
    pub parent: i64,
    pub children: Vec<usize>,
    pub kind: u32,
    pub name: String,
    pub tag: String,
    pub classes: Vec<String>,
    pub element_id: Option<String>,
    pub style: StyleOut,
    pub data: DataOut,
    pub argb: ArgbOut,
    pub text: Option<TextOut>,
    pub image: Option<ImageOut>,
    pub input: Option<InputOut>,
}

struct Builder<'a> {
    dom: &'a Dom,
    styles: &'a [Option<ComputedStyle>],
    nodes: Vec<NodeOut>,
    name_counts: HashMap<String, usize>,
    run_count: usize,
}

impl<'a> Builder<'a> {
    fn new(dom: &'a Dom, styles: &'a [Option<ComputedStyle>]) -> Self {
        Builder {
            dom,
            styles,
            nodes: Vec::new(),
            name_counts: HashMap::new(),
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
        // `<input type="text">` (or no `type`) is an editable v3 widget.
        // `password`, `checkbox`, etc. are not part of this contract and are
        // emitted as a normal block (the subset validator warns).
        let is_input = self.dom.nodes[id].tag == "input" && input_type_is_text(&self.dom.nodes[id]);
        let mut width_px = style.width.unwrap_or(-1.0);
        let mut width_pct = style.width_pct;
        let mut height_px = style.height.unwrap_or(-1.0);
        let mut height_pct = style.height_pct;
        // Image: the HTML `width`/`height` are presentational hints. They are
        // only used if CSS did not fix the dimension (CSS always wins; an
        // explicit `auto` also wins and lets the runtime measure the intrinsic
        // size).
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
        // Root: `%` against the viewport, same as the oracle.
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
        // An inline `<img>`/`<input>` is emitted; other `inline` nodes are not.
        if style.display == "none" || (style.display == "inline" && !is_image && !is_input) {
            // The oracle filters by block/flex before calling; a non-renderable
            // root node is a fixture error.
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
                flex_grow: style.flex_grow,
                flex_shrink: style.flex_shrink,
                flex_basis: style.flex_basis,
                css3: style.css3.clone(),
            },
            data: DataOut {
                hover: style.data_hover.clone(),
                press: style.data_press.clone(),
                selected: style.data_selected.clone(),
                action,
            },
            argb: ArgbOut {
                // `background`/`color` are already normalized to `#rrggbb`;
                // `data-*` travel raw so alpha and every tinycss2 form survive.
                background: argb(style.background.as_deref()),
                text: text_color(style),
                hover: argb(style.data_hover.as_deref()),
                press: argb(style.data_press.as_deref()),
                selected: argb(style.data_selected.as_deref()),
            },
            text: None,
            image,
            input,
        });

        // `<img>` and `<input>` are void: they have no own text and no children.
        if is_image || is_input {
            return Ok(index);
        }

        // Own text first, like `Emitter._emit_element`.
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

    /// Children kept by the v2/v3 tree: block/flex/grid, `<img>` and
    /// renderable `<input>`. `inline`, `none` and everything else is dropped.
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
        // Text is shaped, measured and rasterized by the runtime; the document
        // only carries the string, the family, the size, the weight and an
        // optional line box.
        let node_height = if style.line_height > 0.0 {
            style.line_height.ceil()
        } else {
            0.0
        };
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
                width_px: 0.0,
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
                flex_grow: style.flex_grow,
                flex_shrink: style.flex_shrink,
                flex_basis: style.flex_basis,
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
            }),
            image: None,
            input: None,
        });
        self.run_count += 1;
        Ok(index)
    }
}

fn text_color(style: &ComputedStyle) -> u32 {
    // `argb(style.color) or 0xFF000000` in `emit_ui.py`.
    let value = argb(Some(style.color.as_str()));
    if value == 0 {
        0xFF00_0000
    } else {
        value
    }
}

/// `width="180"` / `height="240px"` -> px; `%`/`auto`/garbage -> `None`.
fn attribute_length(value: Option<&str>) -> Option<f64> {
    let px = crate::style::color::length(value)?;
    if px.is_finite() && px >= 0.0 {
        Some(px)
    } else {
        None
    }
}

/// `true` if CSS explicitly declared `auto` for `width`/`height`.
fn css_auto(style: &ComputedStyle, name: &str) -> bool {
    style
        .raw
        .get(name)
        .is_some_and(|value| value.trim().eq_ignore_ascii_case("auto"))
}

/// A page uses the v2 blob fields if any node of the tree uses grid, scroll or
/// an image. Decided on the already-emitted nodes (not on dead CSS): an `<img>`
/// inside `display:none` does not count. `overflow: hidden` does not force v2:
/// it already existed in v1 and adds no scroll.
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

/// The in-memory pipeline result: the pre-layout style tree plus the data the
/// blob writer (and the tests) need. Nothing here has been laid out.
#[derive(Debug, Clone)]
pub struct Tree {
    pub nodes: Vec<NodeOut>,
    pub viewport: (u32, u32),
    /// Directory the document's asset references (image `src`) resolve
    /// against. It comes from the pipeline input (the HTML file's directory),
    /// not from where the blob is written: moving the output never changes the
    /// baked intrinsic sizes.
    pub asset_dir: PathBuf,
    pub rule_count: usize,
    pub text_run_count: usize,
    /// `true` when the blob carries the v2 sections (grid, scroll, images and
    /// the CSS v3 fields). This is the historical schema `/2` decision, kept
    /// because the blob layout depends on it.
    pub detailed: bool,
}

impl Tree {
    /// Writes the canonical `.wnui` v2 document for this tree. Assets resolve
    /// from [`Tree::asset_dir`], so `path` may live anywhere.
    pub fn write_blob(&self, path: &Path) -> Result<(), String> {
        crate::emit::blob::write_blob(
            path,
            &self.nodes,
            self.viewport,
            self.detailed,
            &self.asset_dir,
        )
    }
}

/// Builds the pre-layout tree from the DOM and the already-computed cascade.
pub(crate) fn emit_tree(
    dom: &Dom,
    styles: &[Option<ComputedStyle>],
    viewport: (u32, u32),
    asset_dir: &Path,
    rule_count: usize,
) -> Result<Tree, String> {
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

    let mut builder = Builder::new(dom, styles);
    builder.emit_element(body, -1, "body.root", true)?;
    let Builder {
        nodes, run_count, ..
    } = builder;

    let detailed = nodes_use_v2(&nodes) || nodes.iter().any(|node| node.input.is_some());

    Ok(Tree {
        nodes,
        viewport,
        asset_dir: asset_dir.to_path_buf(),
        rule_count,
        text_run_count: run_count,
        detailed,
    })
}