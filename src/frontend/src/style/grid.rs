//! v2 CSS values for grid (`docs/ui-tree-v2.md`).
//!
//! Only the mode covered by the contract:
//!   - `grid-template-columns: repeat(auto-fill, <px>)`
//!   - `grid-auto-rows: <px> | auto`
//!
//! There is no `fr`, `minmax()`, `auto-fit`, `span` or nested grid. Parsing is
//! tolerant of spaces and case but strict about the shape: any other construct
//! returns `None` and the subset diagnostic rejects it.

use crate::style::color::length;

/// `repeat(auto-fill, <px>)` already normalized for the blob.
#[derive(Debug, Clone, PartialEq)]
pub struct GridTemplateColumns {
    /// Always `"auto_fill"` in v2.
    pub mode: String,
    pub size_px: f64,
}

/// `grid-auto-rows: <px> | auto`.
#[derive(Debug, Clone, PartialEq)]
pub enum GridAutoRows {
    Px(f64),
    /// `auto` = maximum height of the children (computed by the runtime).
    Auto,
}

/// `repeat(auto-fill, 180px)` -> `{ mode: "auto_fill", size_px: 180.0 }`.
pub fn parse_grid_template_columns(value: &str) -> Option<GridTemplateColumns> {
    let text = value.trim();
    let open = text.find('(')?;
    let close = text.rfind(')')?;
    if close <= open || !text[close + 1..].trim().is_empty() {
        return None;
    }
    if !text[..open].trim().eq_ignore_ascii_case("repeat") {
        return None;
    }
    let parts = split_top_level_commas(&text[open + 1..close]);
    if parts.len() != 2 || !parts[0].trim().eq_ignore_ascii_case("auto-fill") {
        return None;
    }
    let size_px = length(Some(parts[1].trim()))?;
    if !size_px.is_finite() || size_px <= 0.0 {
        return None;
    }
    Some(GridTemplateColumns {
        mode: "auto_fill".to_string(),
        size_px,
    })
}

/// `240px` -> `Px(240.0)`; `auto` -> `Auto`.
pub fn parse_grid_auto_rows(value: &str) -> Option<GridAutoRows> {
    let text = value.trim();
    if text.eq_ignore_ascii_case("auto") {
        return Some(GridAutoRows::Auto);
    }
    let px = length(Some(text))?;
    if !px.is_finite() || px < 0.0 {
        return None;
    }
    Some(GridAutoRows::Px(px))
}

/// Top-level commas (respects parentheses and quotes).
fn split_top_level_commas(text: &str) -> Vec<&str> {
    let bytes = text.as_bytes();
    let mut out = Vec::new();
    let mut start = 0usize;
    let mut depth = 0i32;
    let mut quote: Option<u8> = None;
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
                    out.push(text[start..index].trim());
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
    out.push(text[start..].trim());
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_auto_fill_with_optional_spaces() {
        for value in [
            "repeat(auto-fill, 180px)",
            "repeat(auto-fill,180px)",
            "  REPEAT( Auto-Fill , 180px )  ",
            "repeat(auto-fill, 180)",
        ] {
            let parsed = parse_grid_template_columns(value).unwrap_or_else(|| panic!("{value}"));
            assert_eq!(parsed.mode, "auto_fill");
            assert_eq!(parsed.size_px, 180.0);
        }
    }

    #[test]
    fn rejects_other_grid_forms() {
        for value in [
            "repeat(auto-fit, 180px)",
            "repeat(auto-fill, 1fr)",
            "repeat(auto-fill, minmax(100px, 1fr))",
            "180px",
            "repeat(auto-fill, 180px) 180px",
            "repeat(auto-fill, 0px)",
            "repeat(auto-fill, -180px)",
        ] {
            assert!(
                parse_grid_template_columns(value).is_none(),
                "deberia rechazar {value}"
            );
        }
    }

    #[test]
    fn parses_auto_rows() {
        assert_eq!(parse_grid_auto_rows("auto"), Some(GridAutoRows::Auto));
        assert_eq!(parse_grid_auto_rows("AUTO"), Some(GridAutoRows::Auto));
        assert_eq!(parse_grid_auto_rows("240px"), Some(GridAutoRows::Px(240.0)));
        assert_eq!(parse_grid_auto_rows("240"), Some(GridAutoRows::Px(240.0)));
        assert_eq!(parse_grid_auto_rows("0"), Some(GridAutoRows::Px(0.0)));
        assert_eq!(parse_grid_auto_rows("1fr"), None);
        assert_eq!(parse_grid_auto_rows("-10px"), None);
        assert_eq!(parse_grid_auto_rows("minmax(10px, 20px)"), None);
    }
}
