//! Helpers de valores CSS con la misma semantica que `cascade.length` /
//! `cascade.parse_color` del frontend Python.

use crate::emit::round_py;

/// `px` o numero sin unidad; `auto`/`none`/`normal`/porcentajes -> `None`.
pub(crate) fn length(value: Option<&str>) -> Option<f64> {
    let raw = value?;
    let text = raw.trim().to_ascii_lowercase();
    if text.is_empty() || text == "auto" || text == "none" || text == "normal" {
        return None;
    }
    let number = text.strip_suffix("px").unwrap_or(&text);
    if !is_css_number(number) {
        return None;
    }
    number.parse::<f64>().ok()
}

/// `-?(\d+\.?\d*|\.\d+)` (el mismo `_LEN` del Python, sin exponentes).
fn is_css_number(text: &str) -> bool {
    let mut chars = text.chars().peekable();
    if chars.peek() == Some(&'-') {
        chars.next();
    }
    let mut digits_before = 0usize;
    let mut digits_after = 0usize;
    let mut seen_dot = false;
    while let Some(&c) = chars.peek() {
        if c.is_ascii_digit() {
            if seen_dot {
                digits_after += 1;
            } else {
                digits_before += 1;
            }
            chars.next();
        } else if c == '.' && !seen_dot {
            seen_dot = true;
            chars.next();
        } else {
            return false;
        }
    }
    if digits_before > 0 {
        true
    } else {
        seen_dot && digits_after > 0
    }
}

/// Porcentaje CSS (`30%`) -> `30.0`; cualquier otra cosa -> `0.0`.
///
/// Misma semantica que `_PCT` de `emit_ui.py`: sin signo `+`, sin exponentes y
/// con `%` obligatorio al final.
pub(crate) fn percentage(value: Option<&str>) -> f64 {
    let Some(raw) = value else { return 0.0 };
    let text = raw.trim().to_ascii_lowercase();
    let Some(number) = text.strip_suffix('%') else {
        return 0.0;
    };
    let number = number.trim_end();
    if is_css_number(number) {
        number.parse::<f64>().unwrap_or(0.0)
    } else {
        0.0
    }
}

/// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()` y una tabla corta
/// de nombres. Alfa 0 -> sin fondo (`None`), como el Python.
pub(crate) fn parse_color(value: Option<&str>) -> Option<String> {
    let raw = value?;
    let text = raw.trim().to_ascii_lowercase();
    if text.is_empty() {
        return None;
    }
    if let Some(hex) = text.strip_prefix('#') {
        let digits: Vec<char> = hex.chars().collect();
        let (red, green, blue, alpha) = match digits.len() {
            3 => (
                hex_pair(&digits, 0),
                hex_pair(&digits, 1),
                hex_pair(&digits, 2),
                255,
            ),
            4 => (
                hex_pair(&digits, 0),
                hex_pair(&digits, 1),
                hex_pair(&digits, 2),
                hex_pair(&digits, 3),
            ),
            6 => (
                hex_byte(&digits, 0)?,
                hex_byte(&digits, 2)?,
                hex_byte(&digits, 4)?,
                255,
            ),
            8 => (
                hex_byte(&digits, 0)?,
                hex_byte(&digits, 2)?,
                hex_byte(&digits, 4)?,
                hex_byte(&digits, 6)?,
            ),
            _ => return None,
        };
        if alpha == 0 {
            return None;
        }
        return Some(format!("#{red:02x}{green:02x}{blue:02x}"));
    }
    if let Some(inner) = text.strip_prefix("rgb(").and_then(|s| s.strip_suffix(')')) {
        return rgb_inner(inner);
    }
    if let Some(inner) = text.strip_prefix("rgba(").and_then(|s| s.strip_suffix(')')) {
        return rgb_inner(inner);
    }
    named_color(&text).map(str::to_string)
}

/// Color normalizado conservando el alfa en formato `#rrggbb` / `#rrggbbaa`.
///
/// `parse_color` descarta el alfa (y el color entero si es 0) porque el
/// contrato v1 solo emite `background`/`color` opacos. Las sombras y los bordes
/// v3 necesitan el alfa (`rgba(0,0,0,0.4)` -> `#00000066`) y `transparent`
/// (`#00000000`), asi que este helper no colapsa esos casos.
pub(crate) fn color_with_alpha(value: Option<&str>) -> Option<String> {
    let raw = value?;
    let text = raw.trim().to_ascii_lowercase();
    if text.is_empty() {
        return None;
    }
    if text == "transparent" {
        return Some("#00000000".to_string());
    }
    if let Some(hex) = text.strip_prefix('#') {
        let digits: Vec<char> = hex.chars().collect();
        let (red, green, blue, alpha) = match digits.len() {
            3 => (
                hex_pair(&digits, 0),
                hex_pair(&digits, 1),
                hex_pair(&digits, 2),
                255,
            ),
            4 => (
                hex_pair(&digits, 0),
                hex_pair(&digits, 1),
                hex_pair(&digits, 2),
                hex_pair(&digits, 3),
            ),
            6 => (
                hex_byte(&digits, 0)?,
                hex_byte(&digits, 2)?,
                hex_byte(&digits, 4)?,
                255,
            ),
            8 => (
                hex_byte(&digits, 0)?,
                hex_byte(&digits, 2)?,
                hex_byte(&digits, 4)?,
                hex_byte(&digits, 6)?,
            ),
            _ => return None,
        };
        return Some(format_rgba(red, green, blue, alpha));
    }
    if let Some(inner) = text.strip_prefix("rgb(").and_then(|s| s.strip_suffix(')')) {
        return rgb_inner_alpha(inner);
    }
    if let Some(inner) = text.strip_prefix("rgba(").and_then(|s| s.strip_suffix(')')) {
        return rgb_inner_alpha(inner);
    }
    named_color(&text).map(str::to_string)
}

fn format_rgba(red: u8, green: u8, blue: u8, alpha: u8) -> String {
    if alpha == 255 {
        format!("#{red:02x}{green:02x}{blue:02x}")
    } else {
        format!("#{red:02x}{green:02x}{blue:02x}{alpha:02x}")
    }
}

fn rgb_inner_alpha(inner: &str) -> Option<String> {
    let tokens: Vec<&str> = inner
        .split(|c: char| c == ',' || c == '/' || c.is_whitespace())
        .filter(|token| !token.is_empty())
        .collect();
    if tokens.len() < 3 || tokens.len() > 4 {
        return None;
    }
    let red = channel(tokens[0])?;
    let green = channel(tokens[1])?;
    let blue = channel(tokens[2])?;
    let alpha = match tokens.get(3) {
        Some(token) => alpha_value(token)?,
        None => 1.0,
    };
    let alpha_byte = round_py(alpha.clamp(0.0, 1.0) * 255.0).clamp(0, 255) as u8;
    Some(format_rgba(
        round_py(red).clamp(0, 255) as u8,
        round_py(green).clamp(0, 255) as u8,
        round_py(blue).clamp(0, 255) as u8,
        alpha_byte,
    ))
}

/// Cuerpo de `rgb()`/`rgba()`: canales separados por coma o espacios.
fn rgb_inner(inner: &str) -> Option<String> {
    let tokens: Vec<&str> = inner
        .split(|c: char| c == ',' || c == '/' || c.is_whitespace())
        .filter(|token| !token.is_empty())
        .collect();
    if tokens.len() < 3 || tokens.len() > 4 {
        return None;
    }
    let red = channel(tokens[0])?;
    let green = channel(tokens[1])?;
    let blue = channel(tokens[2])?;
    let alpha = match tokens.get(3) {
        Some(token) => alpha_value(token)?,
        None => 1.0,
    };
    if alpha <= 0.0 {
        return None;
    }
    Some(format!(
        "#{:02x}{:02x}{:02x}",
        round_py(red).clamp(0, 255) as u8,
        round_py(green).clamp(0, 255) as u8,
        round_py(blue).clamp(0, 255) as u8
    ))
}

fn hex_pair(digits: &[char], index: usize) -> u8 {
    let value = digits[index].to_digit(16).unwrap_or(0) as u8;
    value * 17
}

fn hex_byte(digits: &[char], index: usize) -> Option<u8> {
    let high = digits.get(index)?.to_digit(16)? as u8;
    let low = digits.get(index + 1)?.to_digit(16)? as u8;
    Some(high * 16 + low)
}

fn channel(token: &str) -> Option<f64> {
    if let Some(percent) = token.strip_suffix('%') {
        let value = percent.parse::<f64>().ok()?;
        Some(value * 255.0 / 100.0)
    } else {
        let value = token.parse::<f64>().ok()?;
        Some(value.clamp(0.0, 255.0))
    }
}

fn alpha_value(token: &str) -> Option<f64> {
    if let Some(percent) = token.strip_suffix('%') {
        let value = percent.parse::<f64>().ok()?;
        Some(value / 100.0)
    } else {
        Some(token.parse::<f64>().ok()?.clamp(0.0, 1.0))
    }
}

/// Tabla corta: el subconjunto del benchmark solo usa hex.
fn named_color(text: &str) -> Option<&'static str> {
    Some(match text {
        "black" => "#000000",
        "silver" => "#c0c0c0",
        "gray" | "grey" => "#808080",
        "white" => "#ffffff",
        "maroon" => "#800000",
        "red" => "#ff0000",
        "purple" => "#800080",
        "fuchsia" | "magenta" => "#ff00ff",
        "green" => "#008000",
        "lime" => "#00ff00",
        "olive" => "#808000",
        "yellow" => "#ffff00",
        "navy" => "#000080",
        "blue" => "#0000ff",
        "teal" => "#008080",
        "aqua" | "cyan" => "#00ffff",
        "orange" => "#ffa500",
        "rebeccapurple" => "#663399",
        "darkgray" | "darkgrey" => "#a9a9a9",
        "lightgray" | "lightgrey" => "#d3d3d3",
        "whitesmoke" => "#f5f5f5",
        "transparent" => return None,
        _ => return None,
    })
}
