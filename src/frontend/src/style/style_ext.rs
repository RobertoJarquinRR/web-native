//! Extensiones de estilo CSS v3: bordes, radio, sombras y unidades relativas.
//!
//! El frontend las **parsea, cascadea y emite** en `ui.json` (`webnative.ui-tree/2`);
//! el layout y el pintado siguen siendo responsabilidad del runtime R2. Este
//! modulo solo resuelve los px que el documento transporta.
//!
//! Unidades relativas soportadas: `em`, `rem`, `vh`, `vw`. La resolucion a px
//! usa el contexto de build (`LengthCtx`): font-size computado del nodo,
//! font-size del elemento raiz y viewport de compilacion. El valor declarado se
//! conserva en `RelLength` para que el runtime pueda recalcularlo al cambiar el
//! viewport (`width_em`, `width_rem`, ...).
//!
//! Semantica alineada con CSS:
//! - `em` usa el `font-size` del propio nodo;
//! - `rem` usa el `font-size` del elemento raiz (`html`);
//! - `vh`/`vw` son porcentajes del viewport (1vh = height/100).
//!
//! Los bordes solo se consideran visibles con `border-style: solid` y ancho > 0
//! (`border-style: none` es el valor inicial). El color inicial es
//! `currentColor`, igual que CSS.

use crate::style::color::{color_with_alpha, length};

/// Unidad de una longitud CSS v3.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Unit {
    Px,
    Em,
    Rem,
    Vh,
    Vw,
}

impl Unit {
    /// Sufijo usado en los campos JSON (`width_em`, `width_rem`, ...).
    pub(crate) fn suffix(self) -> &'static str {
        match self {
            Unit::Px => "px",
            Unit::Em => "em",
            Unit::Rem => "rem",
            Unit::Vh => "vh",
            Unit::Vw => "vw",
        }
    }
}

/// Longitud declarada: valor numerico + unidad.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct RelLength {
    pub(crate) value: f64,
    pub(crate) unit: Unit,
}

impl RelLength {
    /// Valor declarado si la unidad coincide (`2em` -> `Some(2.0)` para `Em`).
    pub(crate) fn in_unit(self, unit: Unit) -> Option<f64> {
        if self.unit == unit {
            Some(self.value)
        } else {
            None
        }
    }
}

/// Contexto de resolucion a px en build time.
#[derive(Debug, Clone, Copy)]
pub(crate) struct LengthCtx {
    /// `font-size` computado del nodo (px).
    pub(crate) font_size: f64,
    /// `font-size` computado de `html` (px).
    pub(crate) root_font_size: f64,
    pub(crate) viewport_width: f64,
    pub(crate) viewport_height: f64,
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
    pub(crate) fn resolve(self, rel: RelLength) -> f64 {
        match rel.unit {
            Unit::Px => rel.value,
            Unit::Em => rel.value * self.font_size,
            Unit::Rem => rel.value * self.root_font_size,
            Unit::Vh => rel.value * self.viewport_height / 100.0,
            Unit::Vw => rel.value * self.viewport_width / 100.0,
        }
    }
}

/// Numero CSS simple (`-?(\d+\.?\d*|\.\d+)`), sin exponentes ni unidades.
pub(crate) fn parse_number(text: &str) -> Option<f64> {
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

/// `2em`, `1.5rem`, `12px`, `10` -> `RelLength`; `%`, `auto` y basura -> `None`.
///
/// El orden de los sufijos importa: `rem` antes que `em`.
pub(crate) fn parse_rel_length(token: &str) -> Option<RelLength> {
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

/// `true` si el token es una unidad relativa valida (`em/rem/vh/vw`).
pub(crate) fn is_relative_length(token: &str) -> bool {
    matches!(
        parse_rel_length(token),
        Some(RelLength {
            unit: Unit::Em | Unit::Rem | Unit::Vh | Unit::Vw,
            ..
        })
    )
}

/// Divide por whitespace respetando parentesis, corchetes y comillas.
/// Mantiene `rgb(0, 0, 0)` / `rgba(0, 0, 0, .4)` como un solo componente.
pub(crate) fn split_components(value: &str) -> Vec<String> {
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

// ------------------------------------------------------------------- bordes

/// Un lado de borde ya computado. `width_px` es la resolucion en build time y
/// `width_rel` conserva la unidad declarada (si era `em`/`rem`/`vh`/`vw`).
#[derive(Debug, Clone)]
pub(crate) struct BorderSide {
    pub(crate) width_px: f64,
    pub(crate) width_rel: Option<RelLength>,
    /// `solid` o `none` (cualquier otro valor CSS se ignora -> `none`).
    pub(crate) style: String,
    /// Color normalizado; `#rrggbb` o `#rrggbbaa`. `None` si no se pudo parsear.
    pub(crate) color: Option<String>,
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
    /// Un borde solo pinta si es `solid` y tiene ancho > 0.
    pub(crate) fn visible(&self) -> bool {
        self.style == "solid" && self.width_px > 0.0
    }
}

#[derive(Debug, Clone, Default)]
pub(crate) struct BorderOut {
    pub(crate) top: BorderSide,
    pub(crate) right: BorderSide,
    pub(crate) bottom: BorderSide,
    pub(crate) left: BorderSide,
}

impl BorderOut {
    pub(crate) fn sides(&self) -> [(&'static str, &BorderSide); 4] {
        [
            ("top", &self.top),
            ("right", &self.right),
            ("bottom", &self.bottom),
            ("left", &self.left),
        ]
    }

    /// `true` si el CSS declaro algo de borde (aunque sea invisible).
    pub(crate) fn any_declared(&self) -> bool {
        self.sides().iter().any(|(_, side)| {
            side.width_px != 0.0
                || side.width_rel.is_some()
                || side.style == "solid"
                || side.color.is_some()
        })
    }

    pub(crate) fn any_visible(&self) -> bool {
        self.sides().iter().any(|(_, side)| side.visible())
    }

    /// Ancho efectivo uniforme (maximo de los lados visibles) y, si los cuatro
    /// lados comparten exactamente la misma unidad relativa, esa declaracion.
    pub(crate) fn uniform_width(&self) -> (f64, Option<RelLength>) {
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
                // Unidad ausente, distinta o mezclada: no hay campo uniforme.
                rel = None;
            }
        }
        (width, rel)
    }

    pub(crate) fn style_name(&self) -> &'static str {
        if self.any_visible() {
            "solid"
        } else {
            "none"
        }
    }

    /// Color del primer lado visible (top, right, bottom, left).
    pub(crate) fn visible_color(&self) -> Option<&str> {
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
        // Anchos nominales de CSS (`medium` = 3px); no son unidades relativas.
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

/// Computa un lado de borde con los valores ya cascadeados (longhands).
pub(crate) fn resolve_border_side(
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
    // Ancho inicial de CSS (`medium` = 3px) cuando `border-style: solid` no
    // trae un `border-width` explicito.
    if width.is_none() && style == "solid" {
        width_px = 3.0;
    }
    // El color inicial de CSS es `currentColor`; una declaracion invalida
    // tambien cae a currentColor (el subset la diagnostica aparte). Si no se
    // declaro nada de borde, el lado queda neutro (`None`) para que
    // `BorderOut::any_declared()` no marque todos los nodos.
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
    // 1-4 valores con la semantica CSS (top, right, bottom, left).
    let first = values[0];
    if values.len() == 1 {
        return [first; 4];
    }
    let second = values[1];
    let third = values.get(2).copied().unwrap_or(first);
    let fourth = values.get(3).copied().unwrap_or(second);
    [first, second, third, fourth]
}

/// `border-radius` (uniforme o 1-4 esquinas). Devuelve px por esquina y la
/// declaracion relativa uniforme si las cuatro esquinas usan la misma unidad.
/// La sintaxis `a / b` (radios elipticos) solo usa la parte horizontal: el
/// subset la rechaza, el runtime v3 solo soporta radios circulares.
pub(crate) fn resolve_border_radius(
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

// ------------------------------------------------------------------ sombras

#[derive(Debug, Clone)]
pub(crate) struct BoxShadow {
    pub(crate) offset_x_px: f64,
    pub(crate) offset_y_px: f64,
    pub(crate) blur_px: f64,
    pub(crate) spread_px: f64,
    /// `#rrggbb` o `#rrggbbaa` (alfa conservado).
    pub(crate) color: String,
    pub(crate) inset: bool,
}

/// Parsea `box-shadow: <x> <y> [blur] [spread] [color] [, ...]`.
///
/// Acepta `none`, listas separadas por coma y `inset`. Un `color` omitido usa
/// `currentColor` (el color computado del nodo). Devuelve `None` si alguna
/// sombra es invalida (la declaracion CSS se ignora entera, como en CSS).
pub(crate) fn parse_box_shadows(
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

/// Validacion sin contexto (subset): mismo parser con un contexto neutro.
pub(crate) fn box_shadow_is_valid(value: &str) -> bool {
    parse_box_shadows(value, &LengthCtx::default(), "#000000").is_some()
}

/// Campos de estilo v3 que acompanan a `webnative.ui-tree/2`.
///
/// Los px resueltos (`width_px`, `margin`, ...) siguen en `ComputedStyle`; este
/// bloque guarda lo que necesita el runtime para re-resolver al cambiar el
/// viewport (unidades declaradas), ademas de bordes, radio y sombras.
#[derive(Debug, Clone)]
pub(crate) struct StyleV3 {
    pub(crate) width_rel: Option<RelLength>,
    pub(crate) height_rel: Option<RelLength>,
    pub(crate) min_width: Option<f64>,
    pub(crate) min_width_pct: f64,
    pub(crate) min_width_rel: Option<RelLength>,
    pub(crate) max_width: Option<f64>,
    pub(crate) max_width_pct: f64,
    pub(crate) max_width_rel: Option<RelLength>,
    pub(crate) min_height: Option<f64>,
    pub(crate) min_height_pct: f64,
    pub(crate) min_height_rel: Option<RelLength>,
    pub(crate) max_height: Option<f64>,
    pub(crate) max_height_pct: f64,
    pub(crate) max_height_rel: Option<RelLength>,
    /// Unidad relativa declarada por lado (top, right, bottom, left).
    pub(crate) margin_rel: [Option<RelLength>; 4],
    pub(crate) padding_rel: [Option<RelLength>; 4],
    pub(crate) gap_rel: Option<RelLength>,
    pub(crate) row_gap_rel: Option<RelLength>,
    pub(crate) column_gap_rel: Option<RelLength>,
    pub(crate) border: BorderOut,
    pub(crate) border_radius: [f64; 4],
    /// Declaracion relativa uniforme de `border-radius` (si las 4 esquinas
    /// comparten la misma unidad).
    pub(crate) border_radius_rel: Option<RelLength>,
    pub(crate) box_shadow: Vec<BoxShadow>,
    pub(crate) opacity: f64,
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
    /// `true` si la pagina usa la capa v3 y por tanto necesita schema `/2`
    /// (los campos v3 no se escriben en `/1`).
    pub(crate) fn declares_style(&self) -> bool {
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
