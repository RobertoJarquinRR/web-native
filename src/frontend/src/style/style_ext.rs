//! v3 CSS style extensions: borders, radius, shadows and relative units.
//!
//! The frontend **parses, cascades and emits** them in the `.wnui` blob
//! (`wnui/2`); layout and painting remain the runtime's responsibility. This
//! module only resolves the px the document carries.
//!
//! Supported relative units: `em`, `rem`, `vh`, `vw`. Resolution to px uses
//! the build context (`LengthCtx`): the node's computed font-size, the root
//! element's font-size and the compile-time viewport. The declared value is
//! kept in `RelLength` so the runtime can recompute it when the viewport
//! changes (`width_em`, `width_rem`, ...).
//!
//! CSS-aligned semantics:
//! - `em` uses the node's own `font-size`;
//! - `rem` uses the root element's `font-size` (`html`);
//! - `vh`/`vw` are viewport percentages (1vh = height/100).
//!
//! Borders are only considered visible with `border-style: solid` and width > 0
//! (`border-style: none` is the initial value). The initial color is
//! `currentColor`, as in CSS.

use crate::style::color::{color_with_alpha, length};

/// Unit of a v3 CSS length.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Unit {
    Px,
    Em,
    Rem,
    Vh,
    Vw,
}

impl Unit {
    /// Suffix used by the relative-unit fields (`width_em`, `width_rem`, ...).
    pub fn suffix(self) -> &'static str {
        match self {
            Unit::Px => "px",
            Unit::Em => "em",
            Unit::Rem => "rem",
            Unit::Vh => "vh",
            Unit::Vw => "vw",
        }
    }
}

/// Declared length: numeric value + unit.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RelLength {
    pub value: f64,
    pub unit: Unit,
}

impl RelLength {
    /// Declared value if the unit matches (`2em` -> `Some(2.0)` for `Em`).
    pub fn in_unit(self, unit: Unit) -> Option<f64> {
        if self.unit == unit {
            Some(self.value)
        } else {
            None
        }
    }
}

/// Context for resolving to px at build time.
#[derive(Debug, Clone, Copy)]
pub struct LengthCtx {
    /// Node's computed `font-size` (px).
    pub font_size: f64,
    /// Computed `font-size` of `html` (px).
    pub root_font_size: f64,
    pub viewport_width: f64,
    pub viewport_height: f64,
}

impl Default for LengthCtx {
    fn default() -> Self {
        LengthCtx {
            font_size: 16.0,
            root_font_size: 16.0,
            viewport_width: 0.0,
            viewport_height: 0.0,
        }
    }
}

impl LengthCtx {
    pub fn resolve(self, rel: RelLength) -> f64 {
        match rel.unit {
            Unit::Px => rel.value,
            Unit::Em => rel.value * self.font_size,
            Unit::Rem => rel.value * self.root_font_size,
            Unit::Vh => rel.value * self.viewport_height / 100.0,
            Unit::Vw => rel.value * self.viewport_width / 100.0,
        }
    }
}

/// Simple CSS number (`-?(\d+\.?\d*|\.\d+)`), no exponents and no units.
pub fn parse_number(text: &str) -> Option<f64> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return None;
    }
    let mut chars = trimmed.chars().peekable();
    if matches!(chars.peek(), Some('+') | Some('-')) {
        chars.next();
    }
    let mut digits_before = 0usize;
    let mut digits_after = 0usize;
    let mut seen_dot = false;
    while let Some(&character) = chars.peek() {
        if character.is_ascii_digit() {
            if seen_dot {
                digits_after += 1;
            } else {
                digits_before += 1;
            }
            chars.next();
        } else if character == '.' && !seen_dot {
            seen_dot = true;
            chars.next();
        } else {
            return None;
        }
    }
    if digits_before == 0 && digits_after == 0 {
        return None;
    }
    trimmed.parse::<f64>().ok()
}

/// `2em`, `1.5rem`, `12px`, `10` -> `RelLength`; `%`, `auto` and garbage -> `None`.
///
/// Suffix order matters: `rem` before `em`.
pub fn parse_rel_length(token: &str) -> Option<RelLength> {
    let text = token.trim().to_ascii_lowercase();
    if text.is_empty() {
        return None;
    }
    let (number, unit) = if let Some(number) = text.strip_suffix("px") {
        (number, Unit::Px)
    } else if let Some(number) = text.strip_suffix("rem") {
        (number, Unit::Rem)
    } else if let Some(number) = text.strip_suffix("em") {
        (number, Unit::Em)
    } else if let Some(number) = text.strip_suffix("vh") {
        (number, Unit::Vh)
    } else if let Some(number) = text.strip_suffix("vw") {
        (number, Unit::Vw)
    } else {
        (text.as_str(), Unit::Px)
    };
    let value = parse_number(number)?;
    Some(RelLength { value, unit })
}

/// `true` if the token is a valid relative unit (`em/rem/vh/vw`).
pub fn is_relative_length(token: &str) -> bool {
    matches!(
        parse_rel_length(token),
        Some(RelLength {
            unit: Unit::Em | Unit::Rem | Unit::Vh | Unit::Vw,
            ..
        })
    )
}

/// Splits on whitespace, respecting parentheses, brackets and quotes.
/// Keeps `rgb(0, 0, 0)` / `rgba(0, 0, 0, .4)` as a single component.
pub fn split_components(value: &str) -> Vec<String> {
    let bytes = value.as_bytes();
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut quote: Option<u8> = None;
    let mut start: Option<usize> = None;
    let mut index = 0usize;
    while index < bytes.len() {
        let byte = bytes[index];
        if let Some(active) = quote {
            if byte == b'\\' {
                index += 2;
                continue;
            }
            if byte == active {
                quote = None;
            }
            index += 1;
            continue;
        }
        match byte {
            b'"' | b'\'' => {
                quote = Some(byte);
                if start.is_none() {
                    start = Some(index);
                }
            }
            b'(' | b'[' => {
                depth += 1;
                if start.is_none() {
                    start = Some(index);
                }
            }
            b')' | b']' => {
                depth -= 1;
                if start.is_none() {
                    start = Some(index);
                }
            }
            b'\\' => {
                if start.is_none() {
                    start = Some(index);
                }
                index += 2;
                continue;
            }
            _ if byte.is_ascii_whitespace() && depth == 0 => {
                if let Some(begin) = start.take() {
                    out.push(value[begin..index].to_string());
                }
            }
            _ => {
                if start.is_none() {
                    start = Some(index);
                }
            }
        }
        index += 1;
    }
    if let Some(begin) = start {
        out.push(value[begin..].to_string());
    }
    out
}

fn split_commas(value: &str) -> Vec<String> {
    let bytes = value.as_bytes();
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut quote: Option<u8> = None;
    let mut start = 0usize;
    let mut index = 0usize;
    while index < bytes.len() {
        let byte = bytes[index];
        if let Some(active) = quote {
            if byte == b'\\' {
                index += 2;
                continue;
            }
            if byte == active {
                quote = None;
            }
        } else {
            match byte {
                b'"' | b'\'' => quote = Some(byte),
                b'(' | b'[' => depth += 1,
                b')' | b']' => depth -= 1,
                b',' if depth <= 0 => {
                    out.push(value[start..index].trim().to_string());
                    start = index + 1;
                }
                b'\\' => {
                    index += 2;
                    continue;
                }
                _ => {}
            }
        }
        index += 1;
    }
    out.push(value[start..].trim().to_string());
    out.into_iter().filter(|item| !item.is_empty()).collect()
}

// ------------------------------------------------------------------ borders

/// An already-computed border side. `width_px` is the build-time resolution and
/// `width_rel` keeps the declared unit (if it was `em`/`rem`/`vh`/`vw`).
#[derive(Debug, Clone)]
pub struct BorderSide {
    pub width_px: f64,
    pub width_rel: Option<RelLength>,
    /// `solid` or `none` (any other CSS value is ignored -> `none`).
    pub style: String,
    /// Normalized color; `#rrggbb` or `#rrggbbaa`. `None` if it could not be parsed.
    pub color: Option<String>,
}

impl Default for BorderSide {
    fn default() -> Self {
        BorderSide {
            width_px: 0.0,
            width_rel: None,
            style: "none".to_string(),
            color: None,
        }
    }
}

impl BorderSide {
    /// A border only paints if it is `solid` and wider than 0.
    pub fn visible(&self) -> bool {
        self.style == "solid" && self.width_px > 0.0
    }
}

#[derive(Debug, Clone, Default)]
pub struct BorderOut {
    pub top: BorderSide,
    pub right: BorderSide,
    pub bottom: BorderSide,
    pub left: BorderSide,
}

impl BorderOut {
    pub fn sides(&self) -> [(&'static str, &BorderSide); 4] {
        [
            ("top", &self.top),
            ("right", &self.right),
            ("bottom", &self.bottom),
            ("left", &self.left),
        ]
    }

    /// `true` if CSS declared anything about borders (even if invisible).
    pub fn any_declared(&self) -> bool {
        self.sides().iter().any(|(_, side)| {
            side.width_px != 0.0
                || side.width_rel.is_some()
                || side.style == "solid"
                || side.color.is_some()
        })
    }

    pub fn any_visible(&self) -> bool {
        self.sides().iter().any(|(_, side)| side.visible())
    }

    /// Effective uniform width (maximum across visible sides) and, if all four
    /// sides share exactly the same relative unit, that declaration.
    pub fn uniform_width(&self) -> (f64, Option<RelLength>) {
        let mut width = 0.0f64;
        let mut rel: Option<RelLength> = None;
        let mut first = true;
        for (_, side) in self.sides() {
            if !side.visible() {
                continue;
            }
            width = width.max(side.width_px);
            if first {
                rel = side.width_rel.filter(|value| value.unit != Unit::Px);
                first = false;
            } else if rel.is_none() || side.width_rel != rel {
                // Unit missing, different or mixed: there is no uniform field.
                rel = None;
            }
        }
        (width, rel)
    }

    pub fn style_name(&self) -> &'static str {
        if self.any_visible() {
            "solid"
        } else {
            "none"
        }
    }

    /// Color of the first visible side (top, right, bottom, left).
    pub fn visible_color(&self) -> Option<&str> {
        self.sides()
            .iter()
            .find(|(_, side)| side.visible())?
            .1
            .color
            .as_deref()
    }
}

fn border_width_token(token: Option<&String>) -> (f64, Option<RelLength>) {
    let Some(raw) = token else {
        return (0.0, None);
    };
    let text = raw.trim().to_ascii_lowercase();
    match text.as_str() {
        // CSS nominal widths (`medium` = 3px); they are not relative units.
        "thin" => return (1.0, None),
        "medium" => return (3.0, None),
        "thick" => return (5.0, None),
        "" => return (0.0, None),
        _ => {}
    }
    if let Some(px) = length(Some(raw)) {
        return (px, None);
    }
    if let Some(rel) = parse_rel_length(raw) {
        return (0.0, Some(rel));
    }
    (0.0, None)
}

/// Computes a border side from the already-cascaded values (longhands).
pub fn resolve_border_side(
    width: Option<&String>,
    style: Option<&String>,
    color: Option<&String>,
    ctx: &LengthCtx,
    current_color: &str,
) -> BorderSide {
    let declared = width.is_some() || style.is_some() || color.is_some();
    let (mut width_px, width_rel) = border_width_token(width);
    if let Some(rel) = width_rel {
        width_px = ctx.resolve(rel);
    }
    let style = match style.map(|value| value.trim().to_ascii_lowercase()) {
        Some(value) if value == "solid" => "solid".to_string(),
        _ => "none".to_string(),
    };
    // CSS initial width (`medium` = 3px) when `border-style: solid` comes
    // without an explicit `border-width`.
    if width.is_none() && style == "solid" {
        width_px = 3.0;
    }
    // The CSS initial color is `currentColor`; an invalid declaration also
    // falls back to currentColor (the subset diagnoses it separately). If
    // nothing border-related was declared, the side stays neutral (`None`) so
    // `BorderOut::any_declared()` does not flag every node.
    let color = if !declared {
        None
    } else {
        match color.map(|value| value.trim()) {
            Some(value) if value.eq_ignore_ascii_case("currentcolor") => {
                Some(current_color.to_string())
            }
            Some(value) if !value.is_empty() => {
                Some(color_with_alpha(Some(value)).unwrap_or_else(|| current_color.to_string()))
            }
            _ => Some(current_color.to_string()),
        }
    };
    BorderSide {
        width_px,
        width_rel,
        style,
        color,
    }
}

fn expand4<T: Copy>(values: &[T]) -> [T; 4] {
    // 1-4 values with CSS semantics (top, right, bottom, left).
    let first = values[0];
    if values.len() == 1 {
        return [first; 4];
    }
    let second = values[1];
    let third = values.get(2).copied().unwrap_or(first);
    let fourth = values.get(3).copied().unwrap_or(second);
    [first, second, third, fourth]
}

/// `border-radius` (uniform or 1-4 corners). Returns px per corner and the
/// uniform relative declaration if the four corners use the same unit.
/// The `a / b` syntax (elliptical radii) only uses the horizontal part: the
/// subset rejects it, and the v3 runtime only supports circular radii.
pub fn resolve_border_radius(
    value: Option<&str>,
    ctx: &LengthCtx,
) -> ([f64; 4], Option<RelLength>) {
    let mut px = [0.0f64; 4];
    let Some(raw) = value else {
        return (px, None);
    };
    let horizontal = raw.split('/').next().unwrap_or(raw);
    let components = split_components(horizontal);
    if components.is_empty() || components.len() > 4 {
        return (px, None);
    }
    let mut rels = Vec::with_capacity(components.len());
    for component in &components {
        match parse_rel_length(component) {
            Some(rel) => rels.push(rel),
            None => return (px, None),
        }
    }
    let expanded = expand4(&rels);
    for (index, rel) in expanded.iter().enumerate() {
        px[index] = ctx.resolve(*rel);
    }
    let uniform = if expanded.iter().all(|rel| *rel == expanded[0]) {
        Some(expanded[0]).filter(|rel| rel.unit != Unit::Px)
    } else {
        None
    };
    (px, uniform)
}

// ------------------------------------------------------------------ shadows

#[derive(Debug, Clone)]
pub struct BoxShadow {
    pub offset_x_px: f64,
    pub offset_y_px: f64,
    pub blur_px: f64,
    pub spread_px: f64,
    /// `#rrggbb` or `#rrggbbaa` (alpha preserved).
    pub color: String,
    pub inset: bool,
}

/// Parses `box-shadow: <x> <y> [blur] [spread] [color] [, ...]`.
///
/// Accepts `none`, comma-separated lists and `inset`. An omitted `color` uses
/// `currentColor` (the node's computed color). Returns `None` if any shadow is
/// invalid (the CSS declaration is dropped as a whole, as in CSS).
pub fn parse_box_shadows(
    value: &str,
    ctx: &LengthCtx,
    current_color: &str,
) -> Option<Vec<BoxShadow>> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return None;
    }
    if trimmed.eq_ignore_ascii_case("none") {
        return Some(Vec::new());
    }
    let mut out = Vec::new();
    for part in split_commas(trimmed) {
        let components = split_components(&part);
        if components.is_empty() {
            return None;
        }
        let mut inset = false;
        let mut lengths: Vec<RelLength> = Vec::new();
        let mut color: Option<String> = None;
        for component in components {
            if component.eq_ignore_ascii_case("inset") {
                if inset {
                    return None;
                }
                inset = true;
                continue;
            }
            if component.eq_ignore_ascii_case("currentcolor") {
                if color.is_some() {
                    return None;
                }
                color = Some(current_color.to_string());
                continue;
            }
            if let Some(rel) = parse_rel_length(&component) {
                lengths.push(rel);
                continue;
            }
            if let Some(parsed) = color_with_alpha(Some(&component)) {
                if color.is_some() {
                    return None;
                }
                color = Some(parsed);
                continue;
            }
            return None;
        }
        if lengths.len() < 2 || lengths.len() > 4 {
            return None;
        }
        let resolved: Vec<f64> = lengths.iter().map(|rel| ctx.resolve(*rel)).collect();
        let blur = resolved.get(2).copied().unwrap_or(0.0);
        if blur < 0.0 {
            return None;
        }
        out.push(BoxShadow {
            offset_x_px: resolved[0],
            offset_y_px: resolved[1],
            blur_px: blur,
            spread_px: resolved.get(3).copied().unwrap_or(0.0),
            color: color.unwrap_or_else(|| current_color.to_string()),
            inset,
        });
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

/// Validation without context (subset): the same parser with a neutral context.
pub fn box_shadow_is_valid(value: &str) -> bool {
    parse_box_shadows(value, &LengthCtx::default(), "#000000").is_some()
}

/// v3 style fields that accompany the `wnui/2` blob.
///
/// The resolved px (`width_px`, `margin`, ...) still live in `ComputedStyle`;
/// this block keeps what the runtime needs to re-resolve when the viewport
/// changes (declared units), plus borders, radius and shadows.
#[derive(Debug, Clone)]
pub struct StyleV3 {
    pub width_rel: Option<RelLength>,
    pub height_rel: Option<RelLength>,
    pub min_width: Option<f64>,
    pub min_width_pct: f64,
    pub min_width_rel: Option<RelLength>,
    pub max_width: Option<f64>,
    pub max_width_pct: f64,
    pub max_width_rel: Option<RelLength>,
    pub min_height: Option<f64>,
    pub min_height_pct: f64,
    pub min_height_rel: Option<RelLength>,
    pub max_height: Option<f64>,
    pub max_height_pct: f64,
    pub max_height_rel: Option<RelLength>,
    /// Relative unit declared per side (top, right, bottom, left).
    pub margin_rel: [Option<RelLength>; 4],
    pub padding_rel: [Option<RelLength>; 4],
    pub gap_rel: Option<RelLength>,
    pub row_gap_rel: Option<RelLength>,
    pub column_gap_rel: Option<RelLength>,
    pub border: BorderOut,
    pub border_radius: [f64; 4],
    /// Uniform relative `border-radius` declaration (if all 4 corners share
    /// the same unit).
    pub border_radius_rel: Option<RelLength>,
    pub box_shadow: Vec<BoxShadow>,
    pub opacity: f64,
}

impl Default for StyleV3 {
    fn default() -> Self {
        StyleV3 {
            width_rel: None,
            height_rel: None,
            min_width: None,
            min_width_pct: 0.0,
            min_width_rel: None,
            max_width: None,
            max_width_pct: 0.0,
            max_width_rel: None,
            min_height: None,
            min_height_pct: 0.0,
            min_height_rel: None,
            max_height: None,
            max_height_pct: 0.0,
            max_height_rel: None,
            margin_rel: [None; 4],
            padding_rel: [None; 4],
            gap_rel: None,
            row_gap_rel: None,
            column_gap_rel: None,
            border: BorderOut::default(),
            border_radius: [0.0; 4],
            border_radius_rel: None,
            box_shadow: Vec::new(),
            opacity: 1.0,
        }
    }
}

impl StyleV3 {
    /// `true` if the page uses the v3 layer and therefore needs schema `/2`
    /// (v3 fields are not written under `/1`).
    pub fn declares_style(&self) -> bool {
        self.width_rel.is_some()
            || self.height_rel.is_some()
            || self.min_width.is_some()
            || self.min_width_pct != 0.0
            || self.min_width_rel.is_some()
            || self.max_width.is_some()
            || self.max_width_pct != 0.0
            || self.max_width_rel.is_some()
            || self.min_height.is_some()
            || self.min_height_pct != 0.0
            || self.min_height_rel.is_some()
            || self.max_height.is_some()
            || self.max_height_pct != 0.0
            || self.max_height_rel.is_some()
            || self.margin_rel.iter().any(Option::is_some)
            || self.padding_rel.iter().any(Option::is_some)
            || self.gap_rel.is_some()
            || self.row_gap_rel.is_some()
            || self.column_gap_rel.is_some()
            || self.border.any_declared()
            || self.border_radius.iter().any(|value| *value != 0.0)
            || self.border_radius_rel.is_some()
            || !self.box_shadow.is_empty()
            || (self.opacity - 1.0).abs() > 1e-9
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relative_lengths_parse_and_resolve() {
        let ctx = LengthCtx {
            font_size: 20.0,
            root_font_size: 16.0,
            viewport_width: 800.0,
            viewport_height: 600.0,
        };
        let em = parse_rel_length("2em").unwrap();
        assert_eq!(
            em,
            RelLength {
                value: 2.0,
                unit: Unit::Em
            }
        );
        assert_eq!(ctx.resolve(em), 40.0);
        assert_eq!(ctx.resolve(parse_rel_length("1.5rem").unwrap()), 24.0);
        assert_eq!(ctx.resolve(parse_rel_length("10vh").unwrap()), 60.0);
        assert_eq!(ctx.resolve(parse_rel_length("50vw").unwrap()), 400.0);
        assert_eq!(ctx.resolve(parse_rel_length("12px").unwrap()), 12.0);
        assert_eq!(ctx.resolve(parse_rel_length("12").unwrap()), 12.0);
        assert!(parse_rel_length("50%").is_none());
        assert!(parse_rel_length("auto").is_none());
        assert!(is_relative_length("3REM"));
        assert!(!is_relative_length("3px"));
    }

    #[test]
    fn border_side_defaults_to_current_color_and_none_style() {
        let ctx = LengthCtx::default();
        let side = resolve_border_side(None, None, None, &ctx, "#123456");
        assert_eq!(side.style, "none");
        assert!(!side.visible());
        assert_eq!(side.color, None);
        let solid = resolve_border_side(
            Some(&"2px".to_string()),
            Some(&"solid".to_string()),
            Some(&"red".to_string()),
            &ctx,
            "#123456",
        );
        assert!(solid.visible());
        assert_eq!(solid.width_px, 2.0);
        assert_eq!(solid.color.as_deref(), Some("#ff0000"));
    }

    #[test]
    fn border_radius_expands_and_keeps_uniform_relative_unit() {
        let ctx = LengthCtx {
            font_size: 20.0,
            root_font_size: 16.0,
            viewport_width: 800.0,
            viewport_height: 600.0,
        };
        let (px, rel) = resolve_border_radius(Some("1em"), &ctx);
        assert_eq!(px, [20.0, 20.0, 20.0, 20.0]);
        assert_eq!(
            rel,
            Some(RelLength {
                value: 1.0,
                unit: Unit::Em
            })
        );
        let (px, rel) = resolve_border_radius(Some("1px 2px 3px 4px"), &ctx);
        assert_eq!(px, [1.0, 2.0, 3.0, 4.0]);
        assert!(rel.is_none());
    }

    #[test]
    fn box_shadow_parses_lists_and_alpha() {
        let ctx = LengthCtx::default();
        let shadows = parse_box_shadows(
            "0 4px 12px rgba(0, 0, 0, 0.4), inset 1px 2px 0 #ff000080",
            &ctx,
            "#000000",
        )
        .expect("valid shadows");
        assert_eq!(shadows.len(), 2);
        assert_eq!(shadows[0].offset_y_px, 4.0);
        assert_eq!(shadows[0].blur_px, 12.0);
        assert_eq!(shadows[0].color, "#00000066");
        assert!(!shadows[0].inset);
        assert!(shadows[1].inset);
        assert_eq!(shadows[1].offset_x_px, 1.0);
        assert_eq!(shadows[1].offset_y_px, 2.0);
        assert_eq!(shadows[1].color, "#ff000080");
        assert_eq!(parse_box_shadows("none", &ctx, "#000000").unwrap().len(), 0);
        assert!(parse_box_shadows("1px", &ctx, "#000000").is_none());
        assert!(parse_box_shadows("0 0 -1px #000", &ctx, "#000000").is_none());
    }
}
