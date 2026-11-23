//! Cascada: selector -> estilo computado por nodo, con herencia y `style=""`.
//!
//! Reproduce `cascade.compute_styles` del Python: clave de prioridad
//! `(!important, inline, especificidad, orden de regla, indice de declaracion)`.

use std::collections::HashMap;

use crate::color::{length, parse_color};
use crate::css::{parse_declarations, Rule};
use crate::dom::{ua_display, Dom};

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
pub struct ComputedStyle {
    pub raw: HashMap<String, String>,
    pub display: String,
    /// top, right, bottom, left
    pub margin: [f64; 4],
    pub padding: [f64; 4],
    pub width: Option<f64>,
    pub height: Option<f64>,
    pub background: Option<String>,
    pub color: String,
    pub font_size: f64,
    pub font_family: String,
    pub flex_direction: String,
    pub justify_content: String,
    pub align_items: String,
    pub gap: f64,
}

pub fn compute_styles(dom: &Dom, rules: &[Rule]) -> Vec<Option<ComputedStyle>> {
    let mut styles: Vec<Option<ComputedStyle>> = Vec::with_capacity(dom.nodes.len());
    styles.resize_with(dom.nodes.len(), || None);

    for id in dom.preorder() {
        let mut raw: HashMap<String, String> = HashMap::new();
        if let Some(parent) = dom.nodes[id].parent {
            if let Some(parent_style) = styles[parent].as_ref() {
                for name in INHERITED {
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
        styles[id] = Some(resolve(&dom.nodes[id].tag, raw));
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

fn resolve(tag: &str, raw: HashMap<String, String>) -> ComputedStyle {
    let mut margin = [0.0f64; 4];
    let mut padding = [0.0f64; 4];
    for (index, side) in SIDES.iter().enumerate() {
        margin[index] = length(raw.get(&format!("margin-{side}")).map(String::as_str)).unwrap_or(0.0);
        padding[index] =
            length(raw.get(&format!("padding-{side}")).map(String::as_str)).unwrap_or(0.0);
    }

    let width = length(raw.get("width").map(String::as_str));
    let height = length(raw.get("height").map(String::as_str));
    let font_size = length(raw.get("font-size").map(String::as_str)).unwrap_or(16.0);

    // `gap` con el mismo cortocircuito "truthy" que el Python (0.0 es falsy).
    let gap = match length(raw.get("gap").map(String::as_str)) {
        Some(value) => value,
        None => {
            let row = length(raw.get("row-gap").map(String::as_str)).filter(|v| *v != 0.0);
            let column = length(raw.get("column-gap").map(String::as_str)).filter(|v| *v != 0.0);
            row.or(column).unwrap_or(0.0)
        }
    };

    let display = match raw.get("display") {
        Some(value) if !value.is_empty() => value.trim().to_ascii_lowercase(),
        _ => ua_display(tag).to_string(),
    };

    let background_value = match raw.get("background-color") {
        Some(value) if !value.is_empty() => Some(value.as_str()),
        _ => raw.get("background").map(String::as_str),
    };
    let background = parse_color(background_value);

    ComputedStyle {
        color: parse_color(raw.get("color").map(String::as_str))
            .unwrap_or_else(|| "#000000".to_string()),
        font_family: raw
            .get("font-family")
            .cloned()
            .unwrap_or_else(|| "sans-serif".to_string()),
        flex_direction: keyword(&raw, "flex-direction", "row"),
        justify_content: keyword(&raw, "justify-content", "flex-start"),
        align_items: keyword(&raw, "align-items", "stretch"),
        display,
        margin,
        padding,
        width,
        height,
        background,
        font_size,
        gap,
        raw,
    }
}

fn keyword(raw: &HashMap<String, String>, name: &str, default: &str) -> String {
    match raw.get(name) {
        Some(value) if !value.is_empty() => value.trim().to_ascii_lowercase(),
        _ => default.to_string(),
    }
}
