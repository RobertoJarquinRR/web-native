//! Cascada: selector -> estilo computado por nodo, con herencia y `style=""`.
//!
//! Reproduce `cascade.compute_styles` del Python: clave de prioridad
//! `(!important, inline, especificidad, orden de regla, indice de declaracion)`.

use std::collections::HashMap;

use crate::parse::css::{parse_declarations, Rule};
use crate::parse::dom::{ua_display, Dom, NodeId};
use crate::style::color::{length, parse_color, percentage};
use crate::style::grid::{
    parse_grid_auto_rows, parse_grid_template_columns, GridAutoRows, GridTemplateColumns,
};
use crate::style::style_ext::{
    parse_box_shadows, parse_number, parse_rel_length, resolve_border_radius, resolve_border_side,
    BorderOut, LengthCtx, RelLength, StyleV3,
};

const SIDES: [&str; 4] = ["top", "right", "bottom", "left"];

const INHERITED: [&str; 7] = [
    "color",
    "font-family",
    "font-size",
    "font-weight",
    "line-height",
    "text-align",
    "white-space",
];

type Key = (u8, u8, (u32, u32, u32), u32, u32);

/// Campos `color`/`font_size`/`font_family` se calculan para mantener la
/// paridad de la cascada aunque esta ronda no emita texto.
#[allow(dead_code)]
pub(crate) struct ComputedStyle {
    pub(crate) raw: HashMap<String, String>,
    pub(crate) display: String,
    /// top, right, bottom, left
    pub(crate) margin: [f64; 4],
    pub(crate) padding: [f64; 4],
    pub(crate) width: Option<f64>,
    pub(crate) height: Option<f64>,
    /// `width: 30%` -> 30.0; 0.0 si no hay porcentaje (contrato de `UiNode`).
    pub(crate) width_pct: f64,
    pub(crate) height_pct: f64,
    pub(crate) background: Option<String>,
    pub(crate) color: String,
    pub(crate) font_size: f64,
    pub(crate) font_family: String,
    pub(crate) font_weight: String,
    /// `left | center | right` (E1). Heredado.
    pub(crate) text_align: String,
    /// `line-height` resuelto a px; 0.0 = `normal` (metricas de la fuente).
    pub(crate) line_height: f64,
    /// `margin-*: auto` por lado (top, right, bottom, left). E1.
    pub(crate) margin_auto: [bool; 4],
    /// `box-sizing: border-box` (E8/A). false = content-box, el default de CSS.
    pub(crate) box_sizing_border: bool,
    /// `flex-grow` / `flex-shrink` / `flex-basis` (px, -1 = auto). E8.
    pub(crate) flex_grow: f64,
    pub(crate) flex_shrink: f64,
    pub(crate) flex_basis: f64,
    pub(crate) flex_direction: String,
    pub(crate) justify_content: String,
    pub(crate) align_items: String,
    pub(crate) flex_wrap: String,
    pub(crate) gap: f64,
    /// v2: `gap` resuelto por eje (`row-gap`/`column-gap` ganan a `gap`).
    pub(crate) row_gap: f64,
    pub(crate) column_gap: f64,
    /// v2: solo `repeat(auto-fill, <px>)`; el resto queda `None`.
    pub(crate) grid_template_columns: Option<GridTemplateColumns>,
    /// v2: `<px>` o `auto`.
    pub(crate) grid_auto_rows: Option<GridAutoRows>,
    /// v2: `hidden | auto | scroll`; `visible`/desconocido -> `None`.
    pub(crate) overflow: Option<String>,
    /// v2: `fill | cover | contain`; se emite dentro de `image.object_fit`.
    pub(crate) object_fit: Option<String>,
    /// `data-hover` / `data-press` / `data-selected` / `data-action` crudos.
    /// La cascada los transporta porque el runtime R2 los necesita por nodo.
    pub(crate) data_hover: Option<String>,
    pub(crate) data_press: Option<String>,
    pub(crate) data_selected: Option<String>,
    pub(crate) data_action: Option<String>,
    /// Extensiones CSS v3 (bordes, radio, sombras, unidades relativas).
    pub(crate) css3: StyleV3,
}

pub(crate) fn compute_styles(
    dom: &Dom,
    rules: &[Rule],
    viewport: (f64, f64),
) -> Vec<Option<ComputedStyle>> {
    let mut styles: Vec<Option<ComputedStyle>> = Vec::with_capacity(dom.nodes.len());
    styles.resize_with(dom.nodes.len(), || None);
    // `rem` se resuelve contra el font-size computado de `html`. El preorden
    // garantiza que la raiz se calcula antes que body y sus descendientes.
    let mut root_font_size = 16.0f64;

    for id in dom.preorder() {
        let mut raw: HashMap<String, String> = HashMap::new();
        let mut parent_font_size = 16.0f64;
        if let Some(parent) = dom.nodes[id].parent {
            if let Some(parent_style) = styles[parent].as_ref() {
                parent_font_size = parent_style.font_size;
                for name in INHERITED {
                    // `font-size` se hereda como valor computado, no como token:
                    // un hijo sin font-size propio no debe re-resolver el `em`
                    // del padre contra si mismo.
                    if name == "font-size" {
                        continue;
                    }
                    if let Some(value) = parent_style.raw.get(name) {
                        raw.insert(name.to_string(), value.clone());
                    }
                }
            }
        }

        let mut winners: HashMap<String, (Key, String)> = HashMap::new();
        for (order, rule) in rules.iter().enumerate() {
            if !rule.selector.matches(dom, id) {
                continue;
            }
            let specificity = rule.selector.specificity();
            for (declaration_index, declaration) in rule.declarations.iter().enumerate() {
                let key: Key = (
                    declaration.important as u8,
                    0,
                    specificity,
                    order as u32,
                    declaration_index as u32,
                );
                offer(&mut winners, &declaration.name, &declaration.value, key);
            }
        }

        if let Some(inline) = dom.attr(id, "style") {
            for (declaration_index, declaration) in parse_declarations(inline).iter().enumerate() {
                let key: Key = (
                    declaration.important as u8,
                    1,
                    (0, 0, 0),
                    0,
                    declaration_index as u32,
                );
                offer(&mut winners, &declaration.name, &declaration.value, key);
            }
        }

        for (name, (_, value)) in winners {
            raw.insert(name, value);
        }
        let style = resolve(dom, id, raw, parent_font_size, root_font_size, viewport);
        if dom.nodes[id].tag == "html" {
            root_font_size = style.font_size;
        }
        styles[id] = Some(style);
    }
    styles
}

fn offer(map: &mut HashMap<String, (Key, String)>, name: &str, value: &str, key: Key) {
    match map.get(name) {
        Some((current, _)) if *current > key => {}
        _ => {
            map.insert(name.to_string(), (key, value.to_string()));
        }
    }
}

fn resolve(
    dom: &Dom,
    id: NodeId,
    raw: HashMap<String, String>,
    parent_font_size: f64,
    root_font_size: f64,
    viewport: (f64, f64),
) -> ComputedStyle {
    let tag = dom.nodes[id].tag.as_str();
    let font_size = raw
        .get("font-size")
        .and_then(|value| resolve_font_size(value, parent_font_size, root_font_size))
        .unwrap_or(parent_font_size);
    // Contexto de resolucion de `em`/`rem`/`vh`/`vw` a px en build time.
    let ctx = LengthCtx {
        font_size,
        root_font_size,
        viewport_width: viewport.0,
        viewport_height: viewport.1,
    };

    let mut margin = [0.0f64; 4];
    let mut padding = [0.0f64; 4];
    let mut margin_rel = [None; 4];
    let mut padding_rel = [None; 4];
    let mut margin_auto = [false; 4];
    for (index, side) in SIDES.iter().enumerate() {
        let (margin_px, _, margin_unit) =
            resolve_box_length(raw.get(&format!("margin-{side}")), &ctx);
        let (padding_px, _, padding_unit) =
            resolve_box_length(raw.get(&format!("padding-{side}")), &ctx);
        margin[index] = margin_px.unwrap_or(0.0);
        padding[index] = padding_px.unwrap_or(0.0);
        margin_rel[index] = margin_unit;
        padding_rel[index] = padding_unit;
        margin_auto[index] = raw
            .get(&format!("margin-{side}"))
            .is_some_and(|value| value.trim().eq_ignore_ascii_case("auto"));
    }

    let (width, width_pct, width_rel) = resolve_box_length(raw.get("width"), &ctx);
    let (height, height_pct, height_rel) = resolve_box_length(raw.get("height"), &ctx);
    let box_sizing_border = raw
        .get("box-sizing")
        .is_some_and(|value| value.trim().eq_ignore_ascii_case("border-box"));
    let (flex_grow, flex_shrink, flex_basis) = resolve_flex(&raw);
    let (min_width, min_width_pct, min_width_rel) = resolve_box_length(raw.get("min-width"), &ctx);
    let (max_width, max_width_pct, max_width_rel) = resolve_box_length(raw.get("max-width"), &ctx);
    let (min_height, min_height_pct, min_height_rel) =
        resolve_box_length(raw.get("min-height"), &ctx);
    let (max_height, max_height_pct, max_height_rel) =
        resolve_box_length(raw.get("max-height"), &ctx);

    // `gap` con el mismo cortocircuito "truthy" que el Python (0.0 es falsy).
    // `row_gap`/`column_gap` son los valores efectivos por eje: el longhand
    // gana al shorthand (CSS), y `gap` sigue calculandose igual que en v1.
    let (gap_direct, _, gap_rel) = resolve_box_length(raw.get("gap"), &ctx);
    let (row_gap_decl, _, row_gap_rel_decl) = resolve_box_length(raw.get("row-gap"), &ctx);
    let (column_gap_decl, _, column_gap_rel_decl) = resolve_box_length(raw.get("column-gap"), &ctx);
    let gap = match gap_direct {
        Some(value) => value,
        None => {
            let row = row_gap_decl.filter(|v| *v != 0.0);
            let column = column_gap_decl.filter(|v| *v != 0.0);
            row.or(column).unwrap_or(0.0)
        }
    };
    let row_gap = row_gap_decl.or(gap_direct).unwrap_or(0.0);
    let column_gap = column_gap_decl.or(gap_direct).unwrap_or(0.0);
    // La unidad relativa efectiva sigue la misma precedencia que el px.
    let row_gap_rel = if row_gap_decl.is_some() {
        row_gap_rel_decl
    } else {
        gap_rel
    };
    let column_gap_rel = if column_gap_decl.is_some() {
        column_gap_rel_decl
    } else {
        gap_rel
    };

    let overflow = keyword_value(raw.get("overflow"), &["hidden", "auto", "scroll"]);
    let object_fit = keyword_value(raw.get("object-fit"), &["fill", "cover", "contain"]);
    let grid_template_columns = raw
        .get("grid-template-columns")
        .and_then(|value| parse_grid_template_columns(value));
    let grid_auto_rows = raw
        .get("grid-auto-rows")
        .and_then(|value| parse_grid_auto_rows(value));

    let display = match raw.get("display") {
        Some(value) if !value.is_empty() => value.trim().to_ascii_lowercase(),
        _ => ua_display(tag).to_string(),
    };

    let background_value = match raw.get("background-color") {
        Some(value) if !value.is_empty() => Some(value.as_str()),
        _ => raw.get("background").map(String::as_str),
    };
    let background = parse_color(background_value);

    let color =
        parse_color(raw.get("color").map(String::as_str)).unwrap_or_else(|| "#000000".to_string());

    let side = |name: &str| resolve_side(&raw, name, &ctx, &color);
    let border = BorderOut {
        top: side("top"),
        right: side("right"),
        bottom: side("bottom"),
        left: side("left"),
    };

    let (border_radius, border_radius_rel) =
        resolve_border_radius(raw.get("border-radius").map(String::as_str), &ctx);
    let box_shadow = raw
        .get("box-shadow")
        .and_then(|value| parse_box_shadows(value, &ctx, &color))
        .unwrap_or_default();
    let opacity = raw
        .get("opacity")
        .and_then(|value| parse_number(value))
        .map(|value| value.clamp(0.0, 1.0))
        .unwrap_or(1.0);

    // E1: text-align es discreto y heredado; line-height se resuelve a px
    // (`normal` -> 0.0 para que el runtime use ascender + descender).
    let text_align = keyword_value(raw.get("text-align"), &["left", "center", "right"])
        .unwrap_or_else(|| "left".to_string());
    let line_height = resolve_line_height(raw.get("line-height"), font_size, root_font_size);

    ComputedStyle {
        color,
        font_family: raw
            .get("font-family")
            .cloned()
            .unwrap_or_else(|| "sans-serif".to_string()),
        font_weight: raw
            .get("font-weight")
            .filter(|value| !value.trim().is_empty())
            .cloned()
            .unwrap_or_else(|| "400".to_string()),
        text_align,
        line_height,
        margin_auto,
        box_sizing_border,
        flex_grow,
        flex_shrink,
        flex_basis,
        flex_direction: keyword(&raw, "flex-direction", "row"),
        justify_content: keyword(&raw, "justify-content", "flex-start"),
        align_items: keyword(&raw, "align-items", "stretch"),
        flex_wrap: keyword(&raw, "flex-wrap", "nowrap"),
        display,
        margin,
        padding,
        width,
        height,
        width_pct,
        height_pct,
        background,
        font_size,
        gap,
        row_gap,
        column_gap,
        grid_template_columns,
        grid_auto_rows,
        overflow,
        object_fit,
        data_hover: dom.attr(id, "data-hover").map(str::to_string),
        data_press: dom.attr(id, "data-press").map(str::to_string),
        data_selected: dom.attr(id, "data-selected").map(str::to_string),
        data_action: dom.attr(id, "data-action").map(str::to_string),
        css3: StyleV3 {
            width_rel,
            height_rel,
            min_width,
            min_width_pct,
            min_width_rel,
            max_width,
            max_width_pct,
            max_width_rel,
            min_height,
            min_height_pct,
            min_height_rel,
            max_height,
            max_height_pct,
            max_height_rel,
            margin_rel,
            padding_rel,
            gap_rel,
            row_gap_rel,
            column_gap_rel,
            border,
            border_radius,
            border_radius_rel,
            box_shadow,
            opacity,
        },
        raw,
    }
}

fn resolve_side(
    raw: &HashMap<String, String>,
    side: &str,
    ctx: &LengthCtx,
    color: &str,
) -> crate::style::style_ext::BorderSide {
    resolve_border_side(
        raw.get(&format!("border-{side}-width")),
        raw.get(&format!("border-{side}-style")),
        raw.get(&format!("border-{side}-color")),
        ctx,
        color,
    )
}

/// Resuelve una propiedad de caja a `(px, pct, unidad declarada)`.
///
/// - `30%` -> `(None, 30.0, None)` (el contrato v1 conserva el porcentaje).
/// - `2em` -> `(px resuelto, 0.0, Some(2em))`.
/// - `12px`/`12` -> `(Some(12.0), 0.0, None)`.
/// - `auto`/`none`/desconocido -> `(None, 0.0, None)`.
fn resolve_box_length(
    value: Option<&String>,
    ctx: &LengthCtx,
) -> (Option<f64>, f64, Option<RelLength>) {
    let Some(raw) = value else {
        return (None, 0.0, None);
    };
    let text = raw.trim();
    if text.is_empty() || text.eq_ignore_ascii_case("auto") || text.eq_ignore_ascii_case("none") {
        return (None, 0.0, None);
    }
    if text.ends_with('%') {
        return (None, percentage(Some(text)), None);
    }
    if let Some(px) = length(Some(text)) {
        return (Some(px), 0.0, None);
    }
    if let Some(rel) = parse_rel_length(text) {
        return (Some(ctx.resolve(rel)), 0.0, Some(rel));
    }
    (None, 0.0, None)
}

/// `font-size` computado: px/numero, `em` (padre), `rem` (raiz) o `%` (padre).
fn resolve_font_size(value: &str, parent: f64, root: f64) -> Option<f64> {
    let text = value.trim().to_ascii_lowercase();
    if text.is_empty() {
        return None;
    }
    if let Some(px) = length(Some(&text)) {
        return Some(px);
    }
    if let Some(number) = text.strip_suffix('%') {
        return parse_number(number).map(|value| parent * value / 100.0);
    }
    if let Some(number) = text.strip_suffix("rem") {
        return parse_number(number).map(|value| root * value);
    }
    if let Some(number) = text.strip_suffix("em") {
        return parse_number(number).map(|value| parent * value);
    }
    None
}

fn keyword(raw: &HashMap<String, String>, name: &str, default: &str) -> String {
    match raw.get(name) {
        Some(value) if !value.is_empty() => value.trim().to_ascii_lowercase(),
        _ => default.to_string(),
    }
}

/// `line-height: normal | <px> | <numero> | <porcentaje>` -> px.
///
/// `normal` (o un valor no reconocido) devuelve 0.0: el runtime usa entonces
/// `ascender + descender` de la fuente. Un numero sin unidad es un multiplo del
/// `font-size` computado (CSS), aunque el subset solo acepte px|normal.
fn resolve_line_height(value: Option<&String>, font_size: f64, root_font_size: f64) -> f64 {
    let Some(raw) = value else { return 0.0 };
    let text = raw.trim().to_ascii_lowercase();
    if text.is_empty() || text == "normal" {
        return 0.0;
    }
    // `24px` es px absoluto; un numero suelto es multiplicador del font-size.
    if text.ends_with("px") {
        return match length(Some(&text)) {
            Some(px) if px > 0.0 => px,
            _ => 0.0,
        };
    }
    if let Some(number) = text.strip_suffix('%') {
        return parse_number(number)
            .map(|value| (font_size * value / 100.0).max(0.0))
            .unwrap_or(0.0);
    }
    if let Some((number, rest)) = parse_number_unitless(&text) {
        if rest.is_empty() {
            return (font_size * number).max(0.0);
        }
    }
    if let Some(px) = length(Some(&text)) {
        // Cualquier otra longitud absoluta que `length` acepte.
        return if px > 0.0 { px } else { 0.0 };
    }
    // `1.5em`/`2rem`: longitud relativa al font-size computado del nodo.
    if let Some(rel) = parse_rel_length(&text) {
        let ctx = LengthCtx {
            font_size,
            root_font_size,
            viewport_width: 0.0,
            viewport_height: 0.0,
        };
        return ctx.resolve(rel).max(0.0);
    }
    0.0
}

/// Numero CSS sin unidad (admite signo y decimales).
fn parse_number_unitless(text: &str) -> Option<(f64, &str)> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return None;
    }
    let mut end = 0usize;
    let bytes = trimmed.as_bytes();
    if bytes[0] == b'+' || bytes[0] == b'-' {
        end = 1;
    }
    let mut digits = 0usize;
    while end < bytes.len() && bytes[end].is_ascii_digit() {
        end += 1;
        digits += 1;
    }
    if end < bytes.len() && bytes[end] == b'.' {
        end += 1;
        while end < bytes.len() && bytes[end].is_ascii_digit() {
            end += 1;
            digits += 1;
        }
    }
    if digits == 0 {
        return None;
    }
    let value = trimmed[..end].parse::<f64>().ok()?;
    Some((value, &trimmed[end..]))
}

/// `flex-grow`, `flex-shrink` y `flex-basis` (px; -1 = auto), incluido el
/// shorthand `flex: <grow> [<shrink>] [<basis>]`.
fn resolve_flex(raw: &HashMap<String, String>) -> (f64, f64, f64) {
    let mut grow = raw
        .get("flex-grow")
        .and_then(|v| v.trim().parse::<f64>().ok())
        .unwrap_or(0.0);
    let mut shrink = raw
        .get("flex-shrink")
        .and_then(|v| v.trim().parse::<f64>().ok())
        .unwrap_or(1.0);
    let mut basis = flex_basis_px(raw.get("flex-basis")).unwrap_or(-1.0);

    if let Some(value) = raw.get("flex") {
        let text = value.trim();
        if !text.is_empty() && !text.eq_ignore_ascii_case("none") {
            let mut parts = text.split_whitespace();
            if let Some(first) = parts.next() {
                if let Some(number) = first.parse::<f64>().ok() {
                    grow = number;
                    // `flex: <grow>` alone means basis 0 (CSS one-value form).
                    basis = 0.0;
                    if let Some(second) = parts.next() {
                        if let Some(number) = second.parse::<f64>().ok() {
                            shrink = number;
                            if let Some(third) = parts.next() {
                                basis = flex_basis_px(Some(&third.to_string())).unwrap_or(basis);
                            }
                        } else {
                            basis = flex_basis_px(Some(&second.to_string())).unwrap_or(basis);
                        }
                    }
                } else {
                    basis = flex_basis_px(Some(&first.to_string())).unwrap_or(basis);
                }
            }
        }
    }
    (grow.max(0.0), shrink.max(0.0), basis)
}

fn flex_basis_px(value: Option<&String>) -> Option<f64> {
    let text = value?.trim();
    if text.is_empty() || text.eq_ignore_ascii_case("auto") {
        return None;
    }
    let (number, unit) = parse_number_unitless(text)?;
    let unit = unit.trim().to_ascii_lowercase();
    if unit.is_empty() || unit == "px" {
        Some(number)
    } else {
        None
    }
}

/// Palabra clave normalizada si pertenece a `allowed`; si no, `None`.
fn keyword_value(value: Option<&String>, allowed: &[&str]) -> Option<String> {
    let keyword = value?.trim().to_ascii_lowercase();
    if allowed.contains(&keyword.as_str()) {
        Some(keyword)
    } else {
        None
    }
}
