//! CSS -> rules. Uses `cssparser` to split blocks and raw values.
//!
//! Supported selector subset (the one in the SPEC): type, `.class`, `#id`,
//! compounds, descendant (` `) and child (`>`), and comma-separated lists.
//! Any unsupported construct drops that rule (it never panics).

use cssparser::{Delimiter, ParseError, Parser, ParserInput, Token};

use crate::parse::dom::Dom;
use crate::style::color::color_with_alpha;
use crate::style::style_ext::{parse_rel_length, split_components};

#[derive(Debug, Clone)]
pub(crate) struct Declaration {
    pub(crate) name: String,
    pub(crate) value: String,
    pub(crate) important: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct Rule {
    pub(crate) selector: Selector,
    pub(crate) declarations: Vec<Declaration>,
}

/// State pseudo-classes are semantic in blob v6: the cascade matches them
/// against a state mask instead of dropping the rule. Bits are local to this
/// module; the blob/runtime use their own state ordering.
pub(crate) const STATE_HOVER: u8 = 1 << 0;
pub(crate) const STATE_ACTIVE: u8 = 1 << 1;
pub(crate) const STATE_FOCUS: u8 = 1 << 2;
pub(crate) const STATE_DISABLED: u8 = 1 << 3;
/// `:focus-visible`: keyboard/programmatic focus only. The runtime carries the
/// bit separately; a focus-visible entry also folds in `:focus` declarations.
pub(crate) const STATE_FOCUS_VISIBLE: u8 = 1 << 4;
/// `:focus-within`: the element or one of its descendants is focused. The
/// runtime maintains the bit on the focused node and every ancestor.
pub(crate) const STATE_FOCUS_WITHIN: u8 = 1 << 5;

fn pseudo_state(name: &str) -> Option<u8> {
    match name {
        "hover" => Some(STATE_HOVER),
        "active" => Some(STATE_ACTIVE),
        "focus" => Some(STATE_FOCUS),
        "disabled" => Some(STATE_DISABLED),
        "focus-visible" => Some(STATE_FOCUS_VISIBLE),
        "focus-within" => Some(STATE_FOCUS_WITHIN),
        _ => None,
    }
}

#[derive(Debug, Clone, PartialEq)]
enum Simple {
    Type(String),
    Universal,
    Class(String),
    Id(String),
    Attr { name: String, value: Option<String> },
    Pseudo(String),
    /// `:nth-child(an+b)` / `:nth-of-type(an+b)` as static structural matching.
    Nth { of_type: bool, a: i64, b: i64 },
    Never,
}

#[derive(Debug, Clone)]
struct Compound {
    simples: Vec<Simple>,
    spec: (u32, u32, u32),
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum Combinator {
    Descendant,
    Child,
}

#[derive(Debug, Clone)]
pub(crate) struct Selector {
    /// `parts[i].0` is the combinator joining `parts[i-1]` to `parts[i]`
    /// (the one in `parts[0]` is unused).
    parts: Vec<(Combinator, Compound)>,
    spec: (u32, u32, u32),
}

impl Selector {
    pub(crate) fn specificity(&self) -> (u32, u32, u32) {
        self.spec
    }

    /// Matches with the given state bits active. A rule only matches when
    /// every state pseudo-class in the selector is present in `state_mask`:
    /// `:hover:active` needs both bits, so it is part of the press style.
    pub(crate) fn matches_with_state(&self, dom: &Dom, node: usize, state_mask: u8) -> bool {
        self.matches_at(dom, node, self.parts.len() - 1, state_mask)
    }

    /// State pseudo-classes present in the selector (OR of `STATE_*`).
    pub(crate) fn state_mask(&self) -> u8 {
        let mut mask = 0u8;
        for (_, compound) in &self.parts {
            for simple in &compound.simples {
                if let Simple::Pseudo(name) = simple {
                    if let Some(bit) = pseudo_state(name) {
                        mask |= bit;
                    }
                }
            }
        }
        mask
    }

    pub(crate) fn has_state_pseudo(&self) -> bool {
        self.state_mask() != 0
    }

    fn matches_at(&self, dom: &Dom, node: usize, index: usize, state_mask: u8) -> bool {
        // State pseudos are only semantic on the subject compound. An ancestor
        // state (`.card:hover .label`) cannot be applied by this runtime, so
        // matching it with the subject's mask would silently compile a rule
        // that never fires. Ancestor compounds therefore match with no state
        // bits active: the rule never applies, and the subset reports it.
        let compound_mask = if index == self.parts.len() - 1 {
            state_mask
        } else {
            0
        };
        if !compound_matches(dom, node, &self.parts[index].1, compound_mask) {
            return false;
        }
        if index == 0 {
            return true;
        }
        match self.parts[index].0 {
            Combinator::Child => dom.nodes[node].parent.map_or(false, |parent| {
                self.matches_at(dom, parent, index - 1, state_mask)
            }),
            Combinator::Descendant => {
                let mut cursor = dom.nodes[node].parent;
                while let Some(parent) = cursor {
                    if self.matches_at(dom, parent, index - 1, state_mask) {
                        return true;
                    }
                    cursor = dom.nodes[parent].parent;
                }
                false
            }
        }
    }
}

fn compound_matches(dom: &Dom, node: usize, compound: &Compound, state_mask: u8) -> bool {
    let element = &dom.nodes[node];
    for simple in &compound.simples {
        let ok = match simple {
            Simple::Type(tag) => element.tag == *tag,
            Simple::Universal => true,
            Simple::Class(class) => element.classes().any(|item| item == class),
            Simple::Id(id) => element.attr("id") == Some(id.as_str()),
            Simple::Attr { name, value } => match element.attr(name) {
                None => false,
                Some(actual) => value
                    .as_ref()
                    .map_or(true, |expected| actual == expected.as_str()),
            },
            Simple::Pseudo(name) => pseudo_matches(dom, node, name, state_mask),
            Simple::Nth { of_type, a, b } => nth_matches(dom, node, *of_type, *a, *b),
            Simple::Never => false,
        };
        if !ok {
            return false;
        }
    }
    true
}

fn pseudo_matches(dom: &Dom, node: usize, name: &str, state_mask: u8) -> bool {
    if let Some(bit) = pseudo_state(name) {
        return state_mask & bit != 0;
    }
    match name {
        "root" => dom.nodes[node].parent.is_none(),
        "first-child" => match dom.nodes[node].parent {
            None => false,
            Some(parent) => dom.nodes[parent].children.first() == Some(&node),
        },
        "last-child" => match dom.nodes[node].parent {
            None => false,
            Some(parent) => dom.nodes[parent].children.last() == Some(&node),
        },
        "only-child" => match dom.nodes[node].parent {
            None => false,
            Some(parent) => dom.nodes[parent].children.len() == 1,
        },
        _ => false,
    }
}

/// CSS `:nth-child(an+b)` / `:nth-of-type(an+b)` static match.
///
/// The index is 1-based among the parent's children (elements only: this DOM
/// drops text/comment nodes). `of_type` restricts the count to siblings with
/// the same tag. A matched index must be `a*n + b` for an integer `n >= 0`.
fn nth_matches(dom: &Dom, node: usize, of_type: bool, a: i64, b: i64) -> bool {
    let Some(parent) = dom.nodes[node].parent else {
        return false;
    };
    let tag = dom.nodes[node].tag.as_str();
    let mut index = 0i64;
    for child in &dom.nodes[parent].children {
        if of_type && dom.nodes[*child].tag.as_str() != tag {
            continue;
        }
        index += 1;
        if *child == node {
            return nth_index_matches(index, a, b);
        }
    }
    false
}

fn nth_index_matches(index: i64, a: i64, b: i64) -> bool {
    if a == 0 {
        return index == b;
    }
    let diff = index - b;
    diff % a == 0 && diff / a >= 0
}

/// Parses the An+B microsyntax: `odd`, `even`, `<integer>`, `an+b` with
/// optional signs and whitespace around the `b` sign. `:nth-child(2n of .x)`
/// and every other unsupported form return `None` (the caller drops the rule
/// and the subset layer reports it).
pub(crate) fn parse_an_plus_b(text: &str) -> Option<(i64, i64)> {
    let lowered = text.trim().to_ascii_lowercase();
    match lowered.as_str() {
        "odd" => return Some((2, 1)),
        "even" => return Some((2, 0)),
        _ => {}
    }
    if let Some(position) = lowered.find('n') {
        // `3 n` / `3\t n` are invalid: whitespace cannot separate the
        // coefficient from the `n`.
        if lowered[..position].trim_end().len() != position {
            return None;
        }
        let coefficient = lowered[..position].trim();
        let a = match coefficient {
            "" | "+" => 1,
            "-" => -1,
            _ => parse_nth_integer(coefficient)?,
        };
        let rest = lowered[position + 1..].trim();
        let b = if rest.is_empty() {
            0
        } else {
            if !rest.starts_with('+') && !rest.starts_with('-') {
                return None;
            }
            let compact: String = rest.chars().filter(|c| !c.is_whitespace()).collect();
            parse_nth_integer(&compact)?
        };
        return Some((a, b));
    }
    // A bare integer has no internal whitespace.
    Some((0, parse_nth_integer(lowered.as_str())?))
}

/// Optional sign followed by one or more ASCII digits (no units, no decimals).
fn parse_nth_integer(text: &str) -> Option<i64> {
    let value = text.trim();
    if value.is_empty() {
        return None;
    }
    let digits = value.strip_prefix(['+', '-']).unwrap_or(value);
    if digits.is_empty() || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    value.parse::<i64>().ok()
}

// ---------------------------------------------------------------- stylesheet

fn consume_all<'i, 't>(parser: &mut Parser<'i, 't>) -> Result<(), ParseError<'i, ()>> {
    while parser.next_including_whitespace_and_comments().is_ok() {}
    Ok(())
}

fn capture_block<'i, 't>(parser: &mut Parser<'i, 't>) -> Result<String, ParseError<'i, ()>> {
    let start = parser.position();
    while parser.next_including_whitespace_and_comments().is_ok() {}
    Ok(parser.slice_from(start).to_string())
}

pub(crate) fn parse_stylesheets(texts: &[String]) -> Vec<Rule> {
    let mut rules = Vec::new();
    for text in texts {
        parse_stylesheet(text, &mut rules);
    }
    rules
}

pub(crate) fn parse_stylesheet(text: &str, rules: &mut Vec<Rule>) {
    let mut input = ParserInput::new(text);
    let mut parser = Parser::new(&mut input);
    loop {
        if parser.is_exhausted() {
            break;
        }
        let start = parser.position();
        let _ = parser.parse_until_before(
            Delimiter::Semicolon | Delimiter::CurlyBracketBlock,
            consume_all,
        );
        let end = parser.position();
        let prelude = text[start.byte_index()..end.byte_index()]
            .trim()
            .to_string();

        match parser.next_including_whitespace_and_comments() {
            Ok(Token::CurlyBracketBlock) => {
                let block = parser.parse_nested_block(capture_block).unwrap_or_default();
                if !prelude.is_empty() && !prelude.starts_with('@') {
                    push_rule(&prelude, &block, rules);
                }
            }
            Ok(_) => {}
            Err(_) => break,
        }
    }
}

fn push_rule(prelude: &str, block: &str, rules: &mut Vec<Rule>) {
    let declarations = parse_declarations(block);
    if declarations.is_empty() {
        return;
    }
    let prelude = strip_comments(prelude);
    for selector_text in split_top_level_commas(&prelude) {
        if let Some(selector) = parse_selector(selector_text) {
            rules.push(Rule {
                selector,
                declarations: declarations.clone(),
            });
        }
    }
}

/// Removes `/* ... */` comments outside strings. The oracle (tinycss2) does
/// not include them in the prelude; `cssparser` keeps them when slicing by
/// offsets.
fn strip_comments(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    let mut quote: Option<char> = None;
    while let Some(character) = chars.next() {
        match quote {
            Some(active) => {
                out.push(character);
                if character == '\\' {
                    if let Some(next) = chars.next() {
                        out.push(next);
                    }
                } else if character == active {
                    quote = None;
                }
            }
            None => {
                if character == '"' || character == '\'' {
                    quote = Some(character);
                    out.push(character);
                } else if character == '/' && chars.peek() == Some(&'*') {
                    chars.next();
                    let mut previous = '\0';
                    for next in chars.by_ref() {
                        if previous == '*' && next == '/' {
                            break;
                        }
                        previous = next;
                    }
                } else {
                    out.push(character);
                }
            }
        }
    }
    out
}

// -------------------------------------------------------------- declarations

pub(crate) fn parse_declarations(text: &str) -> Vec<Declaration> {
    let cleaned = strip_comments(text);
    let text = cleaned.as_str();
    let mut out = Vec::new();
    let mut input = ParserInput::new(text);
    let mut parser = Parser::new(&mut input);
    loop {
        if parser.is_exhausted() {
            break;
        }
        let start = parser.position();
        let _ = parser.parse_until_before(Delimiter::Semicolon, consume_all);
        let end = parser.position();
        let raw = text[start.byte_index()..end.byte_index()].trim();
        if !raw.is_empty() {
            if let Some((name, value, important)) = split_declaration(raw) {
                for (expanded_name, expanded_value) in expand_shorthand(&name, &value) {
                    out.push(Declaration {
                        name: expanded_name,
                        value: expanded_value,
                        important,
                    });
                }
            }
        }
        match parser.next_including_whitespace_and_comments() {
            Ok(Token::Semicolon) => continue,
            Ok(_) => continue,
            Err(_) => break,
        }
    }
    out
}

fn split_declaration(raw: &str) -> Option<(String, String, bool)> {
    let colon = raw.find(':')?;
    let name = raw[..colon].trim().to_ascii_lowercase();
    if name.is_empty() {
        return None;
    }
    let mut value = raw[colon + 1..].trim().to_string();
    let mut important = false;
    let lowered = value.to_ascii_lowercase();
    let trimmed = lowered.trim_end();
    if trimmed.ends_with("important") {
        let without = trimmed[..trimmed.len() - "important".len()].trim_end();
        if let Some(stripped) = without.strip_suffix('!') {
            important = true;
            value = stripped.trim_end().to_string();
        }
    }
    Some((name, value, important))
}

/// Expands shorthands into longhands, same as `css_parse._expand` plus the v3
/// extensions (per-side borders). v3 properties that are not shorthands
/// (`border-radius`, `box-shadow`, ...) pass through unchanged.
fn expand_shorthand(name: &str, value: &str) -> Vec<(String, String)> {
    match name {
        "margin" | "padding" => {
            let parts = split_components(value);
            let Some(expanded) = expand_four(&parts) else {
                return Vec::new();
            };
            ["top", "right", "bottom", "left"]
                .iter()
                .zip(expanded)
                .map(|(side, item)| (format!("{name}-{side}"), item))
                .collect()
        }
        "border-width" | "border-style" | "border-color" => {
            let parts = split_components(value);
            let Some(expanded) = expand_four(&parts) else {
                return Vec::new();
            };
            let property = &name["border-".len()..];
            ["top", "right", "bottom", "left"]
                .iter()
                .zip(expanded)
                .map(|(side, item)| (format!("border-{side}-{property}"), item))
                .collect()
        }
        "border" => expand_border(name, value, None),
        "border-top" | "border-right" | "border-bottom" | "border-left" => {
            let side = &name["border-".len()..];
            expand_border(name, value, Some(side))
        }
        _ => vec![(name.to_string(), value.to_string())],
    }
}

/// 1-4 values with CSS semantics (top, right, bottom, left).
fn expand_four(parts: &[String]) -> Option<[String; 4]> {
    let first = parts.first()?.clone();
    if parts.len() > 4 {
        return None;
    }
    let second = parts.get(1).cloned().unwrap_or_else(|| first.clone());
    let third = parts.get(2).cloned().unwrap_or_else(|| first.clone());
    let fourth = parts.get(3).cloned().unwrap_or_else(|| second.clone());
    Some([first, second, third, fourth])
}

/// `border`/`border-<side>`: width + style + color with the CSS initial values
/// (`medium none currentcolor`). An invalid declaration is returned unexpanded
/// so the cascade drops it as a whole, like a browser would.
fn expand_border(name: &str, value: &str, side: Option<&str>) -> Vec<(String, String)> {
    let components = split_components(value);
    if components.is_empty() || components.len() > 3 {
        return vec![(name.to_string(), value.to_string())];
    }
    let mut width: Option<String> = None;
    let mut style: Option<String> = None;
    let mut color: Option<String> = None;
    for component in &components {
        let lowered = component.trim().to_ascii_lowercase();
        if lowered == "solid" || lowered == "none" {
            if style.is_some() {
                return vec![(name.to_string(), value.to_string())];
            }
            style = Some(component.clone());
        } else if matches!(lowered.as_str(), "thin" | "medium" | "thick")
            || parse_rel_length(component).is_some()
        {
            if width.is_some() {
                return vec![(name.to_string(), value.to_string())];
            }
            width = Some(component.clone());
        } else if lowered == "currentcolor" || color_with_alpha(Some(component)).is_some() {
            if color.is_some() {
                return vec![(name.to_string(), value.to_string())];
            }
            color = Some(component.clone());
        } else {
            return vec![(name.to_string(), value.to_string())];
        }
    }
    let width = width.unwrap_or_else(|| "medium".to_string());
    let style = style.unwrap_or_else(|| "none".to_string());
    let color = color.unwrap_or_else(|| "currentcolor".to_string());
    let sides: Vec<&str> = match side {
        Some(side) => vec![side],
        None => vec!["top", "right", "bottom", "left"],
    };
    let mut out = Vec::with_capacity(sides.len() * 3);
    for side in sides {
        out.push((format!("border-{side}-width"), width.clone()));
        out.push((format!("border-{side}-style"), style.clone()));
        out.push((format!("border-{side}-color"), color.clone()));
    }
    out
}

fn split_top_level_commas(text: &str) -> Vec<&str> {
    let bytes = text.as_bytes();
    let mut out = Vec::new();
    let mut start = 0usize;
    let mut depth = 0i32;
    let mut quote: Option<u8> = None;
    let mut i = 0usize;
    while i < bytes.len() {
        let byte = bytes[i];
        if let Some(active) = quote {
            if byte == b'\\' {
                i += 2;
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
                    out.push(text[start..i].trim());
                    start = i + 1;
                }
                b'\\' => {
                    i += 2;
                    continue;
                }
                _ => {}
            }
        }
        i += 1;
    }
    out.push(text[start..].trim());
    out.into_iter().filter(|item| !item.is_empty()).collect()
}

// ------------------------------------------------------------------ selectors

fn parse_selector(text: &str) -> Option<Selector> {
    let chars: Vec<char> = text.trim().chars().collect();
    if chars.is_empty() {
        return None;
    }
    let mut index = 0usize;
    let mut parts: Vec<(Combinator, Compound)> = Vec::new();
    let mut pending = Combinator::Descendant;
    while index < chars.len() {
        let before = index;
        skip_ws(&chars, &mut index);
        if index >= chars.len() {
            break;
        }
        let had_ws = index > before;
        match chars[index] {
            '>' => {
                if parts.is_empty() {
                    return None;
                }
                pending = Combinator::Child;
                index += 1;
                continue;
            }
            '+' | '~' => return None,
            _ => {}
        }
        if !parts.is_empty() && !had_ws && pending == Combinator::Descendant {
            return None;
        }
        let (compound, next) = parse_compound(&chars, index)?;
        index = next;
        if compound.simples.is_empty() {
            return None;
        }
        parts.push((pending, compound));
        pending = Combinator::Descendant;
    }
    if parts.is_empty() {
        return None;
    }
    let mut spec = (0u32, 0u32, 0u32);
    for (_, compound) in &parts {
        spec.0 += compound.spec.0;
        spec.1 += compound.spec.1;
        spec.2 += compound.spec.2;
    }
    Some(Selector { parts, spec })
}

fn skip_ws(chars: &[char], index: &mut usize) {
    while *index < chars.len() && chars[*index].is_whitespace() {
        *index += 1;
    }
}

fn is_name_char(c: char) -> bool {
    c.is_alphanumeric() || c == '-' || c == '_' || (c as u32) >= 0x80
}

fn is_name_start(c: char) -> bool {
    c.is_alphabetic() || c == '-' || c == '_' || (c as u32) >= 0x80
}

fn parse_ident(chars: &[char], mut index: usize) -> Option<(String, usize)> {
    if index >= chars.len() || !is_name_start(chars[index]) {
        return None;
    }
    let mut out = String::new();
    while index < chars.len() {
        let c = chars[index];
        if c == '\\' {
            index += 1;
            if index >= chars.len() {
                return None;
            }
            out.push(chars[index]);
            index += 1;
        } else if is_name_char(c) {
            out.push(c);
            index += 1;
        } else {
            break;
        }
    }
    if out.is_empty() {
        None
    } else {
        Some((out, index))
    }
}

fn parse_compound(chars: &[char], mut index: usize) -> Option<(Compound, usize)> {
    let start = index;
    let mut simples = Vec::new();
    let mut spec = (0u32, 0u32, 0u32);
    if index < chars.len() && chars[index] == '*' {
        simples.push(Simple::Universal);
        index += 1;
    } else if index < chars.len() && is_name_start(chars[index]) {
        let (name, next) = parse_ident(chars, index)?;
        simples.push(Simple::Type(name));
        spec.2 += 1;
        index = next;
    }
    loop {
        if index >= chars.len() {
            break;
        }
        match chars[index] {
            '.' => {
                index += 1;
                let (name, next) = parse_ident(chars, index)?;
                simples.push(Simple::Class(name));
                spec.1 += 1;
                index = next;
            }
            '#' => {
                index += 1;
                let (name, next) = parse_ident(chars, index)?;
                simples.push(Simple::Id(name));
                spec.0 += 1;
                index = next;
            }
            '[' => {
                index += 1;
                skip_ws(chars, &mut index);
                let (name, next) = parse_ident(chars, index)?;
                index = next;
                skip_ws(chars, &mut index);
                let mut value = None;
                if index < chars.len() && chars[index] == '=' {
                    index += 1;
                    skip_ws(chars, &mut index);
                    value = Some(parse_attr_value(chars, &mut index)?);
                    skip_ws(chars, &mut index);
                }
                if index >= chars.len() || chars[index] != ']' {
                    return None;
                }
                index += 1;
                simples.push(Simple::Attr { name, value });
                spec.1 += 1;
            }
            ':' => {
                index += 1;
                let doubled = index < chars.len() && chars[index] == ':';
                if doubled {
                    index += 1;
                }
                let (name, next) = parse_ident(chars, index)?;
                index = next;
                if index < chars.len() && chars[index] == '(' {
                    let open = index;
                    let after = skip_balanced(chars, index)?;
                    let argument: String = chars[open + 1..after.saturating_sub(1)]
                        .iter()
                        .collect();
                    if !doubled && matches!(name.as_str(), "nth-child" | "nth-of-type") {
                        if let Some((a, b)) = parse_an_plus_b(&argument) {
                            simples.push(Simple::Nth {
                                of_type: name == "nth-of-type",
                                a,
                                b,
                            });
                            spec.1 += 1;
                        } else {
                            simples.push(Simple::Never);
                        }
                    } else {
                        simples.push(Simple::Never);
                    }
                    index = after;
                } else if doubled
                    || !matches!(
                        name.as_str(),
                        "root"
                            | "first-child"
                            | "last-child"
                            | "only-child"
                            | "hover"
                            | "active"
                            | "focus"
                            | "focus-visible"
                            | "focus-within"
                            | "disabled"
                    )
                {
                    simples.push(Simple::Never);
                } else {
                    simples.push(Simple::Pseudo(name));
                    spec.1 += 1;
                }
            }
            _ => break,
        }
    }
    if index == start {
        return None;
    }
    Some((Compound { simples, spec }, index))
}

fn parse_attr_value(chars: &[char], index: &mut usize) -> Option<String> {
    if *index >= chars.len() {
        return None;
    }
    let quote = chars[*index];
    if quote == '"' || quote == '\'' {
        *index += 1;
        let mut out = String::new();
        while *index < chars.len() {
            let c = chars[*index];
            if c == quote {
                *index += 1;
                return Some(out);
            }
            if c == '\\' {
                *index += 1;
                if *index >= chars.len() {
                    return None;
                }
            }
            out.push(chars[*index]);
            *index += 1;
        }
        None
    } else {
        let (name, next) = parse_ident(chars, *index)?;
        *index = next;
        Some(name)
    }
}

fn skip_balanced(chars: &[char], mut index: usize) -> Option<usize> {
    let mut depth = 0i32;
    while index < chars.len() {
        match chars[index] {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(index + 1);
                }
            }
            _ => {}
        }
        index += 1;
    }
    None
}
