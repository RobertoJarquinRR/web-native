//! CSS value helpers with the same semantics as `cascade.length` /
//! `cascade.parse_color` in the Python frontend.
//!
//! The color parser is a faithful port of
//! `tinycss2.color3.parse_color` **1.5.1** (legacy CSS Color Level 3 syntax
//! only), because that function is the oracle behind `emit_ui.argb()`:
//!
//! * `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa` (4 digits = rgba),
//! * every CSS3 named color (147 entries, including `grey` aliases) plus
//!   `transparent`; `rebeccapurple` is *not* a CSS3 color and parses to `None`,
//! * `rgb()`/`rgba()` with **mandatory commas** and 3/4 arguments; channels are
//!   integer numbers or percentages, `+` signs are allowed, exponents and
//!   decimals are rejected (`is_integer` in tinycss2's tokenizer),
//! * `hsl()`/`hsla()` with a unitless number hue (negative allowed, exponents
//!   allowed because the hue only needs a `number` token) and percentage
//!   saturation/lightness. `deg`/`turn`/`rad`/`grad` are **not** accepted by
//!   1.5.1 (`hsl(0deg, …)` is a dimension token, not a number),
//! * anything else (`rgb(255 0 0 / 50%)`, `#ggg`, `2.55e2`, `none`) -> `None`.
//!
//! Two tinycss2 quirks are replicated on purpose:
//!
//! * `rgb()`/`rgba()` channels are *not* clamped: `rgb(300,0,0)` yields
//!   `RGBA(1.176…)` and `rgb(-10,0,0)` a negative channel. Only the alpha
//!   channel is clipped to `0..1`. [`argb`] therefore masks the 32-bit result
//!   instead of clamping; [`parse_color`]/[`color_with_alpha`], which write a
//!   normalized `#rrggbb[aa]` string, clamp to `0..255` because Python's
//!   `"%02x" % value` would otherwise produce invalid CSS.
//! * `currentcolor` is returned by tinycss2 as the sentinel string
//!   `'currentColor'`; the Python `argb()` then raises `AttributeError`
//!   (a bug in the oracle). Rust returns 0 and never crashes.
//!
//! Channel -> byte conversion uses Python's `round()` (round-half-even), the
//! same path as `emit_ui.argb()`.

use crate::emit::round_py;

/// `px` or a unitless number; `auto`/`none`/`normal`/percentages -> `None`.
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

/// `-?(\d+\.?\d*|\.\d+)` (the same `_LEN` as Python, no exponents).
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

/// CSS percentage (`30%`) -> `30.0`; anything else -> `0.0`.
///
/// Same semantics as `_PCT` in `emit_ui.py`: no `+` sign, no exponents and a
/// mandatory trailing `%`.
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

// ---------------------------------------------------------------- the oracle

/// `tinycss2.color3.RGBA`: four floats in `0..1` (channels may be out of range).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Rgba {
    pub(crate) red: f64,
    pub(crate) green: f64,
    pub(crate) blue: f64,
    pub(crate) alpha: f64,
}

/// Result of `tinycss2.color3.parse_color`: a color or the `currentColor`
/// sentinel. Invalid input is `None`.
#[derive(Clone, Copy, Debug, PartialEq)]
enum ParsedColor {
    Rgba(Rgba),
    CurrentColor,
}

/// `emit_ui.argb()`: `0xAARRGGBB`, or 0 when the value is absent, invalid or
/// fully transparent.
///
/// This is the **single** ARGB entry point shared by `ui_tree`, `header` and
/// `blob`; it accepts both raw CSS (`data-hover="rgba(255,0,0,.5)"`) and the
/// normalized `#rrggbb[aa]` strings written into `ui.json`.
pub(crate) fn argb(value: Option<&str>) -> u32 {
    let Some(raw) = value else { return 0 };
    if raw.is_empty() {
        return 0;
    }
    let Some(ParsedColor::Rgba(color)) = parse_tinycss(raw) else {
        // `currentColor`: the Python oracle crashes here; stay at 0.
        return 0;
    };
    // Python checks the float alpha *before* rounding, so a very small alpha
    // can still produce an alpha byte of 0 (0x00RRGGBB) instead of 0.
    if color.alpha <= 0.0 {
        return 0;
    }
    let a = round_py(color.alpha * 255.0) as u32;
    let r = round_py(color.red * 255.0) as u32;
    let g = round_py(color.green * 255.0) as u32;
    let b = round_py(color.blue * 255.0) as u32;
    // Out-of-range channels are not clamped by tinycss2. Python keeps them in
    // unbounded ints; the C field is 32 bits, so mask exactly like a cast.
    (a << 24) | (r << 16) | (g << 8) | b
}

/// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb[a]()`, `hsl[a]()` and the
/// CSS3 names, normalized to an **opaque** `#rrggbb`.
///
/// This is `cascade.parse_color` (the v1 `background`/`color` contract): alpha
/// is dropped and a fully transparent color becomes `None` (no background).
pub(crate) fn parse_color(value: Option<&str>) -> Option<String> {
    let raw = value?;
    // Python: `css_color(value) if value else None` -- only "" is falsy.
    if raw.is_empty() {
        return None;
    }
    let Some(ParsedColor::Rgba(color)) = parse_tinycss(raw) else {
        return None;
    };
    if color.alpha <= 0.0 {
        return None;
    }
    Some(format!(
        "#{:02x}{:02x}{:02x}",
        to_byte(color.red),
        to_byte(color.green),
        to_byte(color.blue)
    ))
}

/// Normalized color keeping alpha in `#rrggbb` / `#rrggbbaa` form.
///
/// Used by the v3 border/shadow implementation (Rust-only extension), not by
/// the v1 `background`/`color` contract. `transparent` and `rgba(…,0)` become
/// `#00000000`/`#rrggbb00`, exactly like the rest of the v3 code, and [`argb`]
/// maps those strings back to 0.
pub(crate) fn color_with_alpha(value: Option<&str>) -> Option<String> {
    let raw = value?;
    let Some(ParsedColor::Rgba(color)) = parse_tinycss(raw) else {
        return None;
    };
    Some(format_rgba(
        to_byte(color.red),
        to_byte(color.green),
        to_byte(color.blue),
        to_byte(color.alpha),
    ))
}

fn format_rgba(red: u8, green: u8, blue: u8, alpha: u8) -> String {
    if alpha == 255 {
        format!("#{red:02x}{green:02x}{blue:02x}")
    } else {
        format!("#{red:02x}{green:02x}{blue:02x}{alpha:02x}")
    }
}

/// Python `round(channel * 255)`, clamped for the normalized string form.
fn to_byte(channel: f64) -> u8 {
    round_py(channel * 255.0).clamp(0, 255) as u8
}

fn parse_tinycss(input: &str) -> Option<ParsedColor> {
    let chars: Vec<char> = input.chars().collect();
    let mut lexer = Lexer {
        chars: &chars,
        pos: 0,
    };
    // `parse_one_component_value(input, skip_comments=True)`: exactly one
    // significant component value, otherwise a ParseError (`None` here).
    let token = lexer.next_token()?;
    if lexer.next_token().is_some() {
        return None;
    }
    component_color(token)
}

fn component_color(token: Token) -> Option<ParsedColor> {
    match token {
        Token::Ident(name) => keyword_color(&name),
        Token::Hash(value) => hex_color(&value),
        Token::Function { name, arguments } => function_color(&name, &arguments),
        _ => None,
    }
}

fn keyword_color(name: &str) -> Option<ParsedColor> {
    match name {
        "currentcolor" => Some(ParsedColor::CurrentColor),
        "transparent" => Some(ParsedColor::Rgba(Rgba {
            red: 0.0,
            green: 0.0,
            blue: 0.0,
            alpha: 0.0,
        })),
        _ => NAMED_COLORS
            .binary_search_by(|(keyword, _)| (*keyword).cmp(name))
            .ok()
            .map(|index| {
                let rgb = NAMED_COLORS[index].1;
                ParsedColor::Rgba(Rgba {
                    red: f64::from((rgb >> 16) & 0xFF) / 255.0,
                    green: f64::from((rgb >> 8) & 0xFF) / 255.0,
                    blue: f64::from(rgb & 0xFF) / 255.0,
                    alpha: 1.0,
                })
            }),
    }
}

/// `_HASH_REGEXPS`: 4 and 8 hex digits keep the alpha; 3 and 6 do not.
fn hex_color(value: &str) -> Option<ParsedColor> {
    let digits: Vec<u8> = value
        .chars()
        .map(|ch| ch.to_digit(16).map(|digit| digit as u8))
        .collect::<Option<Vec<u8>>>()?;
    let (red, green, blue, alpha) = match digits.len() {
        3 => (digits[0] * 17, digits[1] * 17, digits[2] * 17, 255),
        4 => (
            digits[0] * 17,
            digits[1] * 17,
            digits[2] * 17,
            digits[3] * 17,
        ),
        6 => (
            digits[0] * 16 + digits[1],
            digits[2] * 16 + digits[3],
            digits[4] * 16 + digits[5],
            255,
        ),
        8 => (
            digits[0] * 16 + digits[1],
            digits[2] * 16 + digits[3],
            digits[4] * 16 + digits[5],
            digits[6] * 16 + digits[7],
        ),
        _ => return None,
    };
    Some(ParsedColor::Rgba(Rgba {
        red: f64::from(red) / 255.0,
        green: f64::from(green) / 255.0,
        blue: f64::from(blue) / 255.0,
        alpha: f64::from(alpha) / 255.0,
    }))
}

fn function_color(name: &str, arguments: &[Token]) -> Option<ParsedColor> {
    // `_parse_comma_separated`: mandatory commas, one token per argument.
    let args = comma_separated(arguments)?;
    match name {
        // `rgb` requires exactly 3 args; a 4th makes `_parse_rgb` fail.
        "rgb" if args.len() == 3 => rgb_channels(&args, 1.0),
        "rgba" if args.len() == 4 => {
            let alpha = alpha_value(&args[3])?;
            rgb_channels(&args[..3], alpha)
        }
        // `hsl` requires exactly 3 args; `hsla` exactly 4.
        "hsl" if args.len() == 3 => hsl_channels(&args, 1.0),
        "hsla" if args.len() == 4 => {
            let alpha = alpha_value(&args[3])?;
            hsl_channels(&args[..3], alpha)
        }
        _ => None,
    }
}

fn rgb_channels(args: &[Token], alpha: f64) -> Option<ParsedColor> {
    let channel = |token: &Token| match token {
        // `types == ['number'; 3] and all(a.is_integer)`.
        Token::Number {
            int_value: Some(value),
            ..
        } => Some(*value as f64 / 255.0),
        _ => None,
    };
    // All three channels are number or all three percentage.
    if let (Some(red), Some(green), Some(blue)) =
        (channel(&args[0]), channel(&args[1]), channel(&args[2]))
    {
        return Some(ParsedColor::Rgba(Rgba {
            red,
            green,
            blue,
            alpha,
        }));
    }
    let percent = |token: &Token| match token {
        Token::Percentage { value } => Some(value / 100.0),
        _ => None,
    };
    match (percent(&args[0]), percent(&args[1]), percent(&args[2])) {
        (Some(red), Some(green), Some(blue)) => Some(ParsedColor::Rgba(Rgba {
            red,
            green,
            blue,
            alpha,
        })),
        _ => None,
    }
}

/// `_parse_alpha`: one bare number (fractional and exponent forms included),
/// clipped to `0..1`. Percentages are rejected.
fn alpha_value(token: &Token) -> Option<f64> {
    match token {
        Token::Number { value, .. } => Some(value.clamp(0.0, 1.0)),
        _ => None,
    }
}

fn hsl_channels(args: &[Token], alpha: f64) -> Option<ParsedColor> {
    let hue = match args[0] {
        Token::Number { value, .. } => value,
        _ => return None,
    };
    let saturation = match args[1] {
        Token::Percentage { value } => value / 100.0,
        _ => return None,
    };
    let lightness = match args[2] {
        Token::Percentage { value } => value / 100.0,
        _ => return None,
    };
    let (red, green, blue) = hls_to_rgb(hue / 360.0, lightness, saturation);
    Some(ParsedColor::Rgba(Rgba {
        red,
        green,
        blue,
        alpha,
    }))
}

/// `colorsys.hls_to_rgb` (CPython), same operation order so the rounding path
/// of `argb()` is bit-identical.
fn hls_to_rgb(hue: f64, lightness: f64, saturation: f64) -> (f64, f64, f64) {
    const ONE_THIRD: f64 = 1.0 / 3.0;

    fn value(m1: f64, m2: f64, mut hue: f64) -> f64 {
        const ONE_SIXTH: f64 = 1.0 / 6.0;
        const TWO_THIRD: f64 = 2.0 / 3.0;
        // Python's `%` on floats has the sign of the divisor (always + here).
        hue %= 1.0;
        if hue < 0.0 {
            hue += 1.0;
        }
        if hue < ONE_SIXTH {
            m1 + (m2 - m1) * hue * 6.0
        } else if hue < 0.5 {
            m2
        } else if hue < TWO_THIRD {
            m1 + (m2 - m1) * (TWO_THIRD - hue) * 6.0
        } else {
            m1
        }
    }

    if saturation == 0.0 {
        return (lightness, lightness, lightness);
    }
    let m2 = if lightness <= 0.5 {
        lightness * (1.0 + saturation)
    } else {
        lightness + saturation - (lightness * saturation)
    };
    let m1 = 2.0 * lightness - m2;
    (
        value(m1, m2, hue + ONE_THIRD),
        value(m1, m2, hue),
        value(m1, m2, hue - ONE_THIRD),
    )
}

fn comma_separated(tokens: &[Token]) -> Option<Vec<Token>> {
    // Whitespace/comment tokens are already dropped by the lexer.
    if tokens.is_empty() {
        // tinycss2 returns `[]`, which is falsy for `if args:`.
        return None;
    }
    if tokens.len() % 2 == 1
        && tokens
            .iter()
            .skip(1)
            .step_by(2)
            .all(|token| matches!(token, Token::Literal(',')))
    {
        return Some(tokens.iter().step_by(2).cloned().collect());
    }
    None
}

// ------------------------------------------------------------------ lexer

/// The subset of CSS Syntax tokens `parse_color` needs. Whitespace and
/// comments never reach the parser (tinycss2 filters them the same way).
#[derive(Clone, Debug)]
enum Token {
    Ident(String),
    Hash(String),
    Number {
        value: f64,
        /// `Some` only when the source had no `.` and no exponent
        /// (`tinycss2.ast.NumberToken.is_integer`).
        int_value: Option<i64>,
    },
    Percentage {
        value: f64,
    },
    Function {
        name: String,
        arguments: Vec<Token>,
    },
    Literal(char),
    /// Any other component value (dimension, url, string, block, delim…):
    /// never a valid color argument.
    Other,
}

struct Lexer<'a> {
    chars: &'a [char],
    pos: usize,
}

impl<'a> Lexer<'a> {
    /// Next significant token, or `None` at end of input.
    fn next_token(&mut self) -> Option<Token> {
        loop {
            let ch = *self.chars.get(self.pos)?;
            if is_css_whitespace(ch) {
                self.pos += 1;
                continue;
            }
            if ch == '/' && self.peek(1) == Some('*') {
                self.pos += 2;
                while self.pos < self.chars.len()
                    && !(self.chars[self.pos] == '*' && self.peek(1) == Some('/'))
                {
                    self.pos += 1;
                }
                self.pos = (self.pos + 2).min(self.chars.len());
                continue;
            }
            return Some(self.token_at());
        }
    }

    fn peek(&self, offset: usize) -> Option<char> {
        self.chars.get(self.pos + offset).copied()
    }

    fn token_at(&mut self) -> Token {
        let ch = self.chars[self.pos];
        match ch {
            '#' => {
                self.pos += 1;
                let start = self.pos;
                while self
                    .chars
                    .get(self.pos)
                    .is_some_and(|value| is_name_char(*value))
                {
                    self.pos += 1;
                }
                if self.pos == start {
                    Token::Literal('#')
                } else {
                    Token::Hash(self.chars[start..self.pos].iter().collect())
                }
            }
            ',' => {
                self.pos += 1;
                Token::Literal(',')
            }
            '(' => {
                self.consume_block('(', ')');
                Token::Other
            }
            '[' => {
                self.consume_block('[', ']');
                Token::Other
            }
            '{' => {
                self.consume_block('{', '}');
                Token::Other
            }
            '"' | '\'' => {
                self.consume_string(ch);
                Token::Other
            }
            _ if is_number_start(self.chars, self.pos) => self.number_token(),
            _ if is_ident_start(self.chars, self.pos) => self.ident_token(),
            _ => {
                self.pos += 1;
                Token::Literal(ch)
            }
        }
    }

    fn number_token(&mut self) -> Token {
        let start = self.pos;
        if matches!(self.chars[self.pos], '+' | '-') {
            self.pos += 1;
        }
        while self.chars.get(self.pos).is_some_and(char::is_ascii_digit) {
            self.pos += 1;
        }
        if self.chars.get(self.pos) == Some(&'.')
            && self.peek(1).is_some_and(|value| value.is_ascii_digit())
        {
            self.pos += 1;
            while self.chars.get(self.pos).is_some_and(char::is_ascii_digit) {
                self.pos += 1;
            }
        }
        let mut has_exponent = false;
        if matches!(self.peek(0), Some('e') | Some('E')) {
            let mut lookahead = self.pos + 1;
            if matches!(self.chars.get(lookahead), Some('+') | Some('-')) {
                lookahead += 1;
            }
            if self
                .chars
                .get(lookahead)
                .is_some_and(|value| value.is_ascii_digit())
            {
                has_exponent = true;
                self.pos = lookahead + 1;
                while self.chars.get(self.pos).is_some_and(char::is_ascii_digit) {
                    self.pos += 1;
                }
            }
        }
        let representation: String = self.chars[start..self.pos].iter().collect();
        let value = representation.parse::<f64>().unwrap_or(f64::NAN);
        let int_value = if has_exponent || representation.contains('.') {
            None
        } else {
            representation.parse::<i64>().ok()
        };
        if self.chars.get(self.pos) == Some(&'%') {
            self.pos += 1;
            return Token::Percentage { value };
        }
        if is_ident_start(self.chars, self.pos) {
            // Dimension token (`0deg`, `1px`, …): never a valid color part.
            self.consume_ident();
            return Token::Other;
        }
        Token::Number { value, int_value }
    }

    fn ident_token(&mut self) -> Token {
        let name = self.consume_ident().to_ascii_lowercase();
        if self.chars.get(self.pos) != Some(&'(') {
            return Token::Ident(name);
        }
        self.pos += 1;
        let mut arguments = Vec::new();
        loop {
            let Some(token) = self.next_token() else {
                break;
            };
            if matches!(token, Token::Literal(')')) {
                break;
            }
            arguments.push(token);
        }
        Token::Function { name, arguments }
    }

    fn consume_ident(&mut self) -> String {
        let start = self.pos;
        if self.chars.get(self.pos).is_some_and(|value| *value == '-') {
            self.pos += 1;
        }
        while self
            .chars
            .get(self.pos)
            .is_some_and(|value| is_ident_char(*value))
        {
            self.pos += 1;
        }
        self.chars[start..self.pos].iter().collect()
    }

    /// Consumes a balanced block; the whole block is one `Other` component.
    fn consume_block(&mut self, open: char, close: char) {
        self.pos += 1;
        let mut depth = 1usize;
        while self.pos < self.chars.len() {
            let ch = self.chars[self.pos];
            self.pos += 1;
            if ch == open {
                depth += 1;
            } else if ch == close {
                depth -= 1;
                if depth == 0 {
                    return;
                }
            }
        }
    }

    fn consume_string(&mut self, quote: char) {
        self.pos += 1;
        while self.pos < self.chars.len() {
            let ch = self.chars[self.pos];
            self.pos += 1;
            if ch == '\\' {
                self.pos += 1;
            } else if ch == quote {
                return;
            } else if ch == '\n' {
                return;
            }
        }
    }
}

fn is_css_whitespace(ch: char) -> bool {
    matches!(ch, ' ' | '\n' | '\t' | '\r' | '\u{0c}')
}

fn is_name_char(ch: char) -> bool {
    ch.is_ascii_alphanumeric() || ch == '_' || ch == '-' || !ch.is_ascii()
}

fn is_ident_char(ch: char) -> bool {
    ch.is_ascii_alphanumeric() || ch == '_' || ch == '-' || !ch.is_ascii()
}

fn is_ident_start(chars: &[char], pos: usize) -> bool {
    let Some(&ch) = chars.get(pos) else {
        return false;
    };
    if ch.is_ascii_alphabetic() || ch == '_' || !ch.is_ascii() {
        return true;
    }
    if ch == '-' {
        return matches!(chars.get(pos + 1), Some(next) if is_ident_start(chars, pos + 1) || *next == '-');
    }
    // CSS escapes (`\72 ed`) are not covered: they never reach `argb()` in the
    // current corpus and would need the full tokenizer/unescape pipeline.
    false
}

fn is_number_start(chars: &[char], pos: usize) -> bool {
    let Some(&ch) = chars.get(pos) else {
        return false;
    };
    if ch.is_ascii_digit() {
        return true;
    }
    if ch == '.' {
        return chars
            .get(pos + 1)
            .is_some_and(|value| value.is_ascii_digit());
    }
    if ch == '+' || ch == '-' {
        let next = chars.get(pos + 1);
        if next.is_some_and(|value| value.is_ascii_digit()) {
            return true;
        }
        return next == Some(&'.')
            && chars
                .get(pos + 2)
                .is_some_and(|value| value.is_ascii_digit());
    }
    false
}

// ------------------------------------------------------------- named colors

// @generated from tinycss2.color3._COLOR_KEYWORDS (excluding currentcolor and
// transparent, matched explicitly above). 147 CSS3 names, sorted for the
// binary search. Do not hand-edit: src/frontend/tests/generate_color_corpus.py
// with `--table` prints this block from the installed tinycss2.
const NAMED_COLORS: &[(&str, u32)] = &[
    ("aliceblue", 0xF0F8FF),
    ("antiquewhite", 0xFAEBD7),
    ("aqua", 0x00FFFF),
    ("aquamarine", 0x7FFFD4),
    ("azure", 0xF0FFFF),
    ("beige", 0xF5F5DC),
    ("bisque", 0xFFE4C4),
    ("black", 0x000000),
    ("blanchedalmond", 0xFFEBCD),
    ("blue", 0x0000FF),
    ("blueviolet", 0x8A2BE2),
    ("brown", 0xA52A2A),
    ("burlywood", 0xDEB887),
    ("cadetblue", 0x5F9EA0),
    ("chartreuse", 0x7FFF00),
    ("chocolate", 0xD2691E),
    ("coral", 0xFF7F50),
    ("cornflowerblue", 0x6495ED),
    ("cornsilk", 0xFFF8DC),
    ("crimson", 0xDC143C),
    ("cyan", 0x00FFFF),
    ("darkblue", 0x00008B),
    ("darkcyan", 0x008B8B),
    ("darkgoldenrod", 0xB8860B),
    ("darkgray", 0xA9A9A9),
    ("darkgreen", 0x006400),
    ("darkgrey", 0xA9A9A9),
    ("darkkhaki", 0xBDB76B),
    ("darkmagenta", 0x8B008B),
    ("darkolivegreen", 0x556B2F),
    ("darkorange", 0xFF8C00),
    ("darkorchid", 0x9932CC),
    ("darkred", 0x8B0000),
    ("darksalmon", 0xE9967A),
    ("darkseagreen", 0x8FBC8F),
    ("darkslateblue", 0x483D8B),
    ("darkslategray", 0x2F4F4F),
    ("darkslategrey", 0x2F4F4F),
    ("darkturquoise", 0x00CED1),
    ("darkviolet", 0x9400D3),
    ("deeppink", 0xFF1493),
    ("deepskyblue", 0x00BFFF),
    ("dimgray", 0x696969),
    ("dimgrey", 0x696969),
    ("dodgerblue", 0x1E90FF),
    ("firebrick", 0xB22222),
    ("floralwhite", 0xFFFAF0),
    ("forestgreen", 0x228B22),
    ("fuchsia", 0xFF00FF),
    ("gainsboro", 0xDCDCDC),
    ("ghostwhite", 0xF8F8FF),
    ("gold", 0xFFD700),
    ("goldenrod", 0xDAA520),
    ("gray", 0x808080),
    ("green", 0x008000),
    ("greenyellow", 0xADFF2F),
    ("grey", 0x808080),
    ("honeydew", 0xF0FFF0),
    ("hotpink", 0xFF69B4),
    ("indianred", 0xCD5C5C),
    ("indigo", 0x4B0082),
    ("ivory", 0xFFFFF0),
    ("khaki", 0xF0E68C),
    ("lavender", 0xE6E6FA),
    ("lavenderblush", 0xFFF0F5),
    ("lawngreen", 0x7CFC00),
    ("lemonchiffon", 0xFFFACD),
    ("lightblue", 0xADD8E6),
    ("lightcoral", 0xF08080),
    ("lightcyan", 0xE0FFFF),
    ("lightgoldenrodyellow", 0xFAFAD2),
    ("lightgray", 0xD3D3D3),
    ("lightgreen", 0x90EE90),
    ("lightgrey", 0xD3D3D3),
    ("lightpink", 0xFFB6C1),
    ("lightsalmon", 0xFFA07A),
    ("lightseagreen", 0x20B2AA),
    ("lightskyblue", 0x87CEFA),
    ("lightslategray", 0x778899),
    ("lightslategrey", 0x778899),
    ("lightsteelblue", 0xB0C4DE),
    ("lightyellow", 0xFFFFE0),
    ("lime", 0x00FF00),
    ("limegreen", 0x32CD32),
    ("linen", 0xFAF0E6),
    ("magenta", 0xFF00FF),
    ("maroon", 0x800000),
    ("mediumaquamarine", 0x66CDAA),
    ("mediumblue", 0x0000CD),
    ("mediumorchid", 0xBA55D3),
    ("mediumpurple", 0x9370DB),
    ("mediumseagreen", 0x3CB371),
    ("mediumslateblue", 0x7B68EE),
    ("mediumspringgreen", 0x00FA9A),
    ("mediumturquoise", 0x48D1CC),
    ("mediumvioletred", 0xC71585),
    ("midnightblue", 0x191970),
    ("mintcream", 0xF5FFFA),
    ("mistyrose", 0xFFE4E1),
    ("moccasin", 0xFFE4B5),
    ("navajowhite", 0xFFDEAD),
    ("navy", 0x000080),
    ("oldlace", 0xFDF5E6),
    ("olive", 0x808000),
    ("olivedrab", 0x6B8E23),
    ("orange", 0xFFA500),
    ("orangered", 0xFF4500),
    ("orchid", 0xDA70D6),
    ("palegoldenrod", 0xEEE8AA),
    ("palegreen", 0x98FB98),
    ("paleturquoise", 0xAFEEEE),
    ("palevioletred", 0xDB7093),
    ("papayawhip", 0xFFEFD5),
    ("peachpuff", 0xFFDAB9),
    ("peru", 0xCD853F),
    ("pink", 0xFFC0CB),
    ("plum", 0xDDA0DD),
    ("powderblue", 0xB0E0E6),
    ("purple", 0x800080),
    ("red", 0xFF0000),
    ("rosybrown", 0xBC8F8F),
    ("royalblue", 0x4169E1),
    ("saddlebrown", 0x8B4513),
    ("salmon", 0xFA8072),
    ("sandybrown", 0xF4A460),
    ("seagreen", 0x2E8B57),
    ("seashell", 0xFFF5EE),
    ("sienna", 0xA0522D),
    ("silver", 0xC0C0C0),
    ("skyblue", 0x87CEEB),
    ("slateblue", 0x6A5ACD),
    ("slategray", 0x708090),
    ("slategrey", 0x708090),
    ("snow", 0xFFFAFA),
    ("springgreen", 0x00FF7F),
    ("steelblue", 0x4682B4),
    ("tan", 0xD2B48C),
    ("teal", 0x008080),
    ("thistle", 0xD8BFD8),
    ("tomato", 0xFF6347),
    ("turquoise", 0x40E0D0),
    ("violet", 0xEE82EE),
    ("wheat", 0xF5DEB3),
    ("white", 0xFFFFFF),
    ("whitesmoke", 0xF5F5F5),
    ("yellow", 0xFFFF00),
    ("yellowgreen", 0x9ACD32),
];

#[cfg(test)]
mod tests {
    use super::{argb, color_with_alpha, parse_color};

    // Checked-in oracle corpus: `(input, ARGB)` generated with tinycss2 1.5.1
    // by `src/frontend/tests/generate_color_corpus.py`.
    include!("../../tests/corpus/color_corpus.rs");

    #[test]
    fn argb_matches_the_python_oracle_for_every_corpus_entry() {
        assert!(CASES.len() >= 200, "corpus too small: {}", CASES.len());
        for (input, expected) in CASES {
            assert_eq!(
                argb(Some(input)),
                *expected,
                "argb({input:?}) differs from tinycss2/emit_ui.argb"
            );
        }
    }

    #[test]
    fn v1_normalization_matches_the_python_oracle() {
        assert!(NORMALIZED_CASES.len() >= 150, "{}", NORMALIZED_CASES.len());
        for (input, expected) in NORMALIZED_CASES {
            assert_eq!(
                parse_color(Some(input)).as_deref(),
                Some(*expected),
                "parse_color({input:?}) differs from cascade.parse_color"
            );
        }
    }

    #[test]
    fn currentcolor_returns_zero_instead_of_the_python_crash() {
        // tinycss2 returns the string 'currentColor'; emit_ui.argb() then
        // raises AttributeError on `color.alpha`. We document the bug and do
        // not reproduce it.
        assert_eq!(argb(Some("currentcolor")), 0);
        assert_eq!(argb(Some("currentColor")), 0);
        assert_eq!(parse_color(Some("currentcolor")), None);
        assert_eq!(color_with_alpha(Some("currentcolor")), None);
    }

    #[test]
    fn v1_contract_is_opaque_and_drops_zero_alpha() {
        assert_eq!(
            parse_color(Some("rgba(255,0,0,.5)")).as_deref(),
            Some("#ff0000")
        );
        assert_eq!(parse_color(Some("#ff000080")).as_deref(), Some("#ff0000"));
        assert_eq!(parse_color(Some("transparent")), None);
        assert_eq!(parse_color(Some("#00000000")), None);
        assert_eq!(parse_color(Some("RED")).as_deref(), Some("#ff0000"));
    }

    #[test]
    fn v3_contract_keeps_alpha() {
        assert_eq!(
            color_with_alpha(Some("rgba(255,0,0,.5)")).as_deref(),
            Some("#ff000080")
        );
        assert_eq!(
            color_with_alpha(Some("transparent")).as_deref(),
            Some("#00000000")
        );
        assert_eq!(argb(Some("#ff000080")), 0x80FF_0000);
        assert_eq!(argb(Some("rgba(255,0,0,.5)")), 0x80FF_0000);
        assert_eq!(argb(Some("hsl(0,100%,50%)")), 0xFFFF_0000);
    }
}
