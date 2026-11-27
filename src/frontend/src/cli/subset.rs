//! `subset v1` contract diagnostics (`docs/subset-v1.md`).
//!
//! It only inspects the input HTML/CSS: it takes no part in the cascade or the
//! emission, so the output never changes. Diagnostics carry file and line
//! (CSS via `cssparser`; HTML via a dedicated tag scanner) and a readable
//! context (selector, property or tag).
//!
//! Default mode: warnings and the pipeline continues (compatibility).
//! `--strict-subset`: errors and exit != 0 on any feature outside the
//! contract. `--pilot`: allows the v2 pilot extensions (`display: grid`,
//! `grid-*`, `overflow: auto|scroll`, `object-fit`) and the v3 ones
//! (`box-shadow`, per-side properties, `min/max-*` and `em/rem/vh/vw` units);
//! without the flag they are reported as extensions and `--strict-subset`
//! rejects them.

use std::fs;
use std::path::Path;

use cssparser::{Delimiter, ParseError, Parser, ParserInput, Token};

use crate::parse::css::parse_an_plus_b;
use crate::style::cascade::parse_font_weight;
use crate::style::color::{is_declared_color, parse_color};
use crate::style::grid::{parse_grid_auto_rows, parse_grid_template_columns};
use crate::style::style_ext::{box_shadow_is_valid, is_relative_length};

/// Tags of the v1 subset. `input` is the v2 pilot extension (text input
/// widget, `ui-tree/3`) and is diagnosed without `--pilot`.
const SUPPORTED_TAGS: [&str; 20] = [
    "html", "head", "body", "div", "span", "p", "h1", "h2", "h3", "h4", "h5", "h6", "button",
    "img", "input", "ul", "ol", "li", "link", "style",
];

/// Tags the contract explicitly names as outside v1.
const OUT_OF_V1_TAGS: [&str; 13] = [
    "script", "iframe", "form", "select", "table", "canvas", "video", "audio", "svg",
    "template", "slot", "object", "embed",
];

/// Attributes accepted on any supported tag.
const GLOBAL_ATTRIBUTES: [&str; 8] = [
    "id",
    "class",
    "style",
    "data-hover",
    "data-press",
    "data-selected",
    "data-action",
    "title",
];

/// Accepted pseudo-classes. `:root` and the structural ones are already
/// resolved by the engine; the first four are the states named by the
/// contract. `:nth-child()`/`:nth-of-type()` are validated separately because
/// their argument grammar decides support.
const ALLOWED_PSEUDO_CLASSES: [&str; 12] = [
    "hover",
    "active",
    "focus",
    "disabled",
    "root",
    "first-child",
    "last-child",
    "only-child",
    "focus-visible",
    "focus-within",
    "nth-child",
    "nth-of-type",
];

/// `:nth-of-type()` shares the `:nth-child()` argument grammar.
const NTH_OF_TYPE: &str = "nth-of-type";

/// State pseudo-classes supported by blob v6, local to the diagnostic.
const HOOK_STATE_HOVER: u8 = 1 << 0;
const HOOK_STATE_ACTIVE: u8 = 1 << 1;
const HOOK_STATE_FOCUS: u8 = 1 << 2;
const HOOK_STATE_DISABLED: u8 = 1 << 3;
const HOOK_STATE_FOCUS_VISIBLE: u8 = 1 << 4;
const HOOK_STATE_FOCUS_WITHIN: u8 = 1 << 5;

fn hook_state_bit(name: &str) -> Option<u8> {
    match name {
        "hover" => Some(HOOK_STATE_HOVER),
        "active" => Some(HOOK_STATE_ACTIVE),
        "focus" => Some(HOOK_STATE_FOCUS),
        "disabled" => Some(HOOK_STATE_DISABLED),
        "focus-visible" => Some(HOOK_STATE_FOCUS_VISIBLE),
        "focus-within" => Some(HOOK_STATE_FOCUS_WITHIN),
        _ => None,
    }
}

#[derive(Debug, Clone)]
pub struct Diagnostic {
    pub file: String,
    pub line: Option<u32>,
    pub context: Option<String>,
    pub message: String,
}

impl Diagnostic {
    fn new(file: &Path, line: u32, context: Option<String>, message: String) -> Self {
        Diagnostic {
            file: file.display().to_string(),
            line: Some(line),
            context,
            message,
        }
    }

    pub fn render(&self, strict: bool) -> String {
        let level = if strict { "error" } else { "warning" };
        let location = match self.line {
            Some(line) => format!("{}:{}", self.file, line),
            None => self.file.clone(),
        };
        let context = self
            .context
            .as_ref()
            .map(|value| format!(" [{value}]"))
            .unwrap_or_default();
        format!("{level}[subset] {location}: {}{context}", self.message)
    }
}

pub fn print_diagnostics(diagnostics: &[Diagnostic], strict: bool) {
    for diagnostic in diagnostics {
        eprintln!("{}", diagnostic.render(strict));
    }
    if !diagnostics.is_empty() {
        let what = if diagnostics.len() == 1 {
            "diagnostico"
        } else {
            "diagnosticos"
        };
        let mode = if strict {
            "modo estricto: exit != 0"
        } else {
            "solo warnings: se continua"
        };
        eprintln!("subset v1: {} {} ({})", diagnostics.len(), what, mode);
    }
}

/// Checks a full HTML document and every stylesheet it references.
/// Variant without the pilot flag (tests and callers that omit `--pilot`).
#[allow(dead_code)]
pub(crate) fn check_document(html: &str, html_path: &Path, base_dir: &Path) -> Vec<Diagnostic> {
    check_document_pilot(html, html_path, base_dir, false)
}

/// Same as [`check_document`] but with the v2 pilot extensions:
/// `display: grid`, scroll (`overflow: auto|scroll`), `grid-*` and `object-fit`
/// stop being diagnosed when `pilot` is `true`.
pub fn check_document_pilot(
    html: &str,
    html_path: &Path,
    base_dir: &Path,
    pilot: bool,
) -> Vec<Diagnostic> {
    let mut scanner = HtmlScanner {
        html,
        bytes: html.as_bytes(),
        path: html_path,
        base_dir,
        pilot,
        pos: 0,
        line: 1,
        diagnostics: Vec::new(),
    };
    scanner.scan();
    scanner.diagnostics
}

// ------------------------------------------------------------------- HTML

struct HtmlScanner<'a> {
    html: &'a str,
    bytes: &'a [u8],
    path: &'a Path,
    base_dir: &'a Path,
    /// v2 pilot extensions allowed (`--pilot`).
    pilot: bool,
    pos: usize,
    line: u32,
    diagnostics: Vec<Diagnostic>,
}

impl<'a> HtmlScanner<'a> {
    fn push(&mut self, line: u32, context: Option<String>, message: String) {
        self.diagnostics
            .push(Diagnostic::new(self.path, line, context, message));
    }

    fn starts_with(&self, needle: &[u8]) -> bool {
        self.pos + needle.len() <= self.bytes.len()
            && &self.bytes[self.pos..self.pos + needle.len()] == needle
    }

    fn advance_to(&mut self, end: usize) {
        let end = end.min(self.bytes.len());
        if end <= self.pos {
            return;
        }
        self.line += self.bytes[self.pos..end]
            .iter()
            .filter(|byte| **byte == b'\n')
            .count() as u32;
        self.pos = end;
    }

    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len() && self.bytes[self.pos].is_ascii_whitespace() {
            if self.bytes[self.pos] == b'\n' {
                self.line += 1;
            }
            self.pos += 1;
        }
    }

    fn scan(&mut self) {
        while self.pos < self.bytes.len() {
            if self.bytes[self.pos] != b'<' {
                let mut next = self.pos;
                while next < self.bytes.len() && self.bytes[next] != b'<' {
                    next += 1;
                }
                self.advance_to(next);
                continue;
            }
            if self.starts_with(b"<!--") {
                match find_bytes(self.bytes, b"-->", self.pos + 4) {
                    Some(end) => self.advance_to(end + 3),
                    None => self.advance_to(self.bytes.len()),
                }
                continue;
            }
            if self.starts_with(b"</") {
                match find_byte(self.bytes, b'>', self.pos + 2) {
                    Some(end) => self.advance_to(end + 1),
                    None => self.advance_to(self.bytes.len()),
                }
                continue;
            }
            if self.starts_with(b"<!") || self.starts_with(b"<?") {
                match find_byte(self.bytes, b'>', self.pos + 2) {
                    Some(end) => self.advance_to(end + 1),
                    None => self.advance_to(self.bytes.len()),
                }
                continue;
            }
            self.start_tag();
        }
    }

    fn start_tag(&mut self) {
        let tag_line = self.line;
        self.pos += 1; // '<'
        let name_start = self.pos;
        while self.pos < self.bytes.len() && !is_tag_name_end(self.bytes[self.pos]) {
            self.pos += 1;
        }
        let tag = self.html[name_start..self.pos].to_ascii_lowercase();
        let mut attrs: Vec<(String, String, u32)> = Vec::new();

        loop {
            self.skip_ws();
            if self.pos >= self.bytes.len() {
                break;
            }
            match self.bytes[self.pos] {
                b'>' => {
                    self.pos += 1;
                    break;
                }
                b'/' => {
                    self.pos += 1;
                    continue;
                }
                _ => {}
            }

            let attr_line = self.line;
            let attr_start = self.pos;
            while self.pos < self.bytes.len() && !is_attr_name_end(self.bytes[self.pos]) {
                self.pos += 1;
            }
            if self.pos == attr_start {
                // Odd byte (e.g. a stray quote): advance so the loop cannot spin.
                self.pos += 1;
                continue;
            }
            let name = self.html[attr_start..self.pos].to_ascii_lowercase();
            self.skip_ws();

            let mut value = String::new();
            if self.pos < self.bytes.len() && self.bytes[self.pos] == b'=' {
                self.pos += 1;
                self.skip_ws();
                if self.pos < self.bytes.len()
                    && (self.bytes[self.pos] == b'"' || self.bytes[self.pos] == b'\'')
                {
                    let quote = self.bytes[self.pos];
                    self.pos += 1;
                    let value_start = self.pos;
                    while self.pos < self.bytes.len() && self.bytes[self.pos] != quote {
                        if self.bytes[self.pos] == b'\n' {
                            self.line += 1;
                        }
                        self.pos += 1;
                    }
                    value = self.html[value_start..self.pos].to_string();
                    if self.pos < self.bytes.len() {
                        self.pos += 1;
                    }
                } else {
                    let value_start = self.pos;
                    while self.pos < self.bytes.len() {
                        let byte = self.bytes[self.pos];
                        if byte.is_ascii_whitespace() || byte == b'>' {
                            break;
                        }
                        self.pos += 1;
                    }
                    value = self.html[value_start..self.pos].to_string();
                }
            }
            attrs.push((name, value, attr_line));
        }

        self.check_tag(&tag, &attrs, tag_line);

        // In HTML the `/` flag is ignored on non-void elements: `<style />`,
        // `<script />`, `<iframe />`... still open raw-text content.
        if tag == "style" {
            self.consume_raw_text("style", true);
        } else if matches!(
            tag.as_str(),
            "script"
                | "textarea"
                | "title"
                | "iframe"
                | "noembed"
                | "noframes"
                | "xmp"
                | "plaintext"
        ) {
            self.consume_raw_text(&tag, false);
        }
    }

    fn consume_raw_text(&mut self, tag: &str, is_css: bool) {
        let content_start = self.pos;
        let content_line = self.line;
        let content_end =
            find_raw_text_end(self.bytes, tag, content_start).unwrap_or(self.bytes.len());
        if is_css {
            let content = self.html[content_start..content_end].to_string();
            check_stylesheet_pilot(
                &content,
                self.path,
                content_line,
                self.pilot,
                &mut self.diagnostics,
            );
        }
        self.advance_to(content_end);
    }

    fn check_tag(&mut self, tag: &str, attrs: &[(String, String, u32)], line: u32) {
        if !SUPPORTED_TAGS.contains(&tag) {
            let suffix = if OUT_OF_V1_TAGS.contains(&tag) {
                " (fuera de v1)"
            } else {
                ""
            };
            self.push(
                line,
                None,
                format!("etiqueta <{tag}> no soportada por subset v1{suffix}"),
            );
            // Attributes of an already-rejected tag add nothing.
            return;
        }

        if tag == "input" && !self.pilot {
            self.push(
                line,
                Some("<input>".to_string()),
                "<input> es una extension del piloto v2 (widget text input, ui-tree/3); usa --pilot para permitirla"
                    .to_string(),
            );
        }

        for (name, value, attr_line) in attrs {
            if !attribute_supported(tag, name) {
                self.push(
                    *attr_line,
                    Some(format!("<{tag}>")),
                    format!("atributo '{name}' no soportado por subset v1"),
                );
                continue;
            }
            match (tag, name.as_str()) {
                ("img", "src") => self.check_local_asset("src de <img>", value, *attr_line),
                ("input", "type") => {
                    if !value.trim().eq_ignore_ascii_case("text") {
                        self.push(
                            *attr_line,
                            Some("<input>".to_string()),
                            format!(
                                "input type '{}' no soportado: el widget v1 solo edita type=\"text\"",
                                value.trim()
                            ),
                        );
                    }
                }
                ("input", "value") | ("input", "placeholder") => {
                    if !value.is_ascii() {
                        self.push(
                            *attr_line,
                            Some(format!("<input {name}>")),
                            "el text input v1 solo compone ASCII imprimible (32..126); el resto se descarta"
                                .to_string(),
                        );
                    }
                }
                ("input", "maxlength") => {
                    if value.trim().parse::<u32>().is_err() {
                        self.push(
                            *attr_line,
                            Some("<input maxlength>".to_string()),
                            format!("maxlength '{value}' no es un entero positivo"),
                        );
                    }
                }
                ("link", "href") => {
                    self.check_local_asset("href de <link>", value, *attr_line);
                    let is_sheet = attrs.iter().find(|(attr, _, _)| attr == "rel").map_or(
                        false,
                        |(_, rel, _)| {
                            rel.split_whitespace()
                                .any(|item| item.eq_ignore_ascii_case("stylesheet"))
                        },
                    );
                    if is_sheet {
                        if let Some((text, path)) = read_linked_stylesheet(value, self.base_dir) {
                            check_stylesheet_pilot(
                                &text,
                                &path,
                                1,
                                self.pilot,
                                &mut self.diagnostics,
                            );
                        }
                    }
                }
                (_, "style") => {
                    let context = format!("style inline en <{tag}>");
                    check_inline_style_pilot(
                        value,
                        self.path,
                        *attr_line,
                        &context,
                        self.pilot,
                        &mut self.diagnostics,
                    );
                }
                _ => {}
            }
        }
    }

    fn check_local_asset(&mut self, label: &str, value: &str, line: u32) {
        let lowered = value.trim().to_ascii_lowercase();
        let remote = lowered.starts_with("http:")
            || lowered.starts_with("https:")
            || lowered.starts_with("//")
            || lowered.starts_with("data:");
        if remote {
            self.push(
                line,
                Some(label.to_string()),
                format!(
                    "recurso remoto/embebido no permitido en v1 (solo archivos locales): '{value}'"
                ),
            );
        }
    }
}

fn attribute_supported(tag: &str, name: &str) -> bool {
    GLOBAL_ATTRIBUTES.contains(&name)
        // `alt`, `width` and `height` are part of the v2 `kind:image` node: the
        // runtime needs the alternative text and the intrinsic size. They are
        // additive to subset v1 (which only names `src`).
        || (tag == "img" && matches!(name, "src" | "alt" | "width" | "height"))
        || (tag == "input"
            && matches!(
                name,
                "type" | "value" | "placeholder" | "maxlength" | "disabled" | "readonly"
            ))
        || (tag == "link" && (name == "href" || name == "rel"))
        || (tag == "button" && name == "disabled")
}

fn is_tag_name_end(byte: u8) -> bool {
    byte.is_ascii_whitespace() || byte == b'/' || byte == b'>' || byte == b'<'
}

fn is_attr_name_end(byte: u8) -> bool {
    byte.is_ascii_whitespace() || byte == b'=' || byte == b'>' || byte == b'/' || byte == b'<'
}

fn find_byte(bytes: &[u8], needle: u8, from: usize) -> Option<usize> {
    if from >= bytes.len() {
        return None;
    }
    bytes[from..]
        .iter()
        .position(|byte| *byte == needle)
        .map(|offset| from + offset)
}

fn find_bytes(bytes: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if needle.is_empty() || from >= bytes.len() || needle.len() > bytes.len() - from {
        return None;
    }
    let last = bytes.len() - needle.len();
    let mut index = from;
    while index <= last {
        if &bytes[index..index + needle.len()] == needle {
            return Some(index);
        }
        index += 1;
    }
    None
}

fn find_ci(bytes: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if needle.is_empty() || from >= bytes.len() || needle.len() > bytes.len() - from {
        return None;
    }
    let last = bytes.len() - needle.len();
    let mut index = from;
    while index <= last {
        if bytes[index..index + needle.len()].eq_ignore_ascii_case(needle) {
            return Some(index);
        }
        index += 1;
    }
    None
}

/// Finds `</tag` followed by whitespace, `/` or `>` (the HTML end-tag rule).
fn find_raw_text_end(bytes: &[u8], tag: &str, from: usize) -> Option<usize> {
    let needle = format!("</{tag}");
    let mut index = from;
    while let Some(found) = find_ci(bytes, needle.as_bytes(), index) {
        let after = found + needle.len();
        if after >= bytes.len() {
            return Some(found);
        }
        match bytes[after] {
            b'>' | b'/' => return Some(found),
            byte if byte.is_ascii_whitespace() => return Some(found),
            _ => index = found + 1,
        }
    }
    None
}

// -------------------------------------------------------------------- CSS

/// Validates a stylesheet. `base_line` is the 1-based line of the first
/// character of `text` inside `path`; for CSS files it is 1 and for `<style>`
/// blocks it is the HTML line where the content starts.
/// Variant without the pilot flag (tests).
#[allow(dead_code)]
pub(crate) fn check_stylesheet(
    text: &str,
    path: &Path,
    base_line: u32,
    diagnostics: &mut Vec<Diagnostic>,
) {
    check_stylesheet_pilot(text, path, base_line, false, diagnostics)
}

/// Same as [`check_stylesheet`] but allowing the v2 pilot extensions.
pub(crate) fn check_stylesheet_pilot(
    text: &str,
    path: &Path,
    base_line: u32,
    pilot: bool,
    diagnostics: &mut Vec<Diagnostic>,
) {
    let masked = mask_comments(text);
    let text = masked.as_str();
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
        let prelude_start = first_content_index(text, start.byte_index(), end.byte_index());
        let prelude = strip_comments(&text[prelude_start..end.byte_index()])
            .trim()
            .to_string();
        let prelude_line = line_at(text, base_line, prelude_start);

        match parser.next_including_whitespace_and_comments() {
            Ok(Token::CurlyBracketBlock) => {
                if prelude.starts_with('@') {
                    push_at_rule(&prelude, path, prelude_line, diagnostics);
                    skip_nested_block(&mut parser);
                } else if prelude.is_empty() {
                    skip_nested_block(&mut parser);
                } else if check_selector(&prelude, path, prelude_line, diagnostics) {
                    skip_nested_block(&mut parser);
                } else {
                    check_block(
                        &mut parser,
                        text,
                        path,
                        base_line,
                        &prelude,
                        selector_is_state_scoped(&prelude),
                        pilot,
                        diagnostics,
                    );
                }
            }
            Ok(Token::Semicolon) => {
                if prelude.starts_with('@') {
                    push_at_rule(&prelude, path, prelude_line, diagnostics);
                }
            }
            Ok(_) => {}
            Err(_) => break,
        }
    }
}

fn consume_all<'i, 't>(parser: &mut Parser<'i, 't>) -> Result<(), ParseError<'i, ()>> {
    while parser.next_including_whitespace_and_comments().is_ok() {}
    Ok(())
}

fn skip_nested_block<'i, 't>(parser: &mut Parser<'i, 't>) {
    let _ = parser.parse_nested_block(|nested| -> Result<(), ParseError<'i, ()>> {
        let _ = consume_all(nested);
        Ok(())
    });
}

#[allow(clippy::too_many_arguments)]
fn check_block<'i, 't>(
    parser: &mut Parser<'i, 't>,
    text: &str,
    path: &Path,
    base_line: u32,
    selector: &str,
    state_rule: bool,
    pilot: bool,
    diagnostics: &mut Vec<Diagnostic>,
) {
    let where_context = format!("selector '{selector}'");
    let _ = parser.parse_nested_block(|nested| -> Result<(), ParseError<'i, ()>> {
        loop {
            if nested.is_exhausted() {
                break;
            }
            let start = nested.position();
            let _ = nested.parse_until_before(Delimiter::Semicolon, consume_all);
            let end = nested.position();
            let decl_start = first_non_ws(text, start.byte_index(), end.byte_index());
            let raw = text[decl_start..end.byte_index()].trim();
            if !raw.is_empty() {
                if let Some((name, value, _important)) = split_declaration(raw) {
                    let line = line_at(text, base_line, decl_start);
                    check_property(
                        &name,
                        &value,
                        path,
                        line,
                        &where_context,
                        state_rule,
                        pilot,
                        diagnostics,
                    );
                }
            }
            if nested.next_including_whitespace_and_comments().is_err() {
                break;
            }
        }
        Ok(())
    });
}

/// Validates the content of an inline `style="..."`.
/// Variant without the pilot flag (tests).
#[allow(dead_code)]
pub(crate) fn check_inline_style(
    text: &str,
    path: &Path,
    base_line: u32,
    context: &str,
    diagnostics: &mut Vec<Diagnostic>,
) {
    check_inline_style_pilot(text, path, base_line, context, false, diagnostics)
}

/// Same as [`check_inline_style`] but allowing the v2 pilot extensions.
pub(crate) fn check_inline_style_pilot(
    text: &str,
    path: &Path,
    base_line: u32,
    context: &str,
    pilot: bool,
    diagnostics: &mut Vec<Diagnostic>,
) {
    let masked = mask_comments(text);
    let text = masked.as_str();
    let mut input = ParserInput::new(text);
    let mut parser = Parser::new(&mut input);
    loop {
        if parser.is_exhausted() {
            break;
        }
        let start = parser.position();
        let _ = parser.parse_until_before(Delimiter::Semicolon, consume_all);
        let end = parser.position();
        let decl_start = first_non_ws(text, start.byte_index(), end.byte_index());
        let raw = text[decl_start..end.byte_index()].trim();
        if !raw.is_empty() {
            if let Some((name, value, _important)) = split_declaration(raw) {
                let line = line_at(text, base_line, decl_start);
                check_property(&name, &value, path, line, context, false, pilot, diagnostics);
            }
        }
        if parser.next_including_whitespace_and_comments().is_err() {
            break;
        }
    }
}

/// Splits a selector prelude at top-level commas, keeping brackets, strings and
/// pseudo-class arguments intact.
fn split_selector_groups(selector: &str) -> Vec<&str> {
    let bytes = selector.as_bytes();
    let mut groups = Vec::new();
    let mut start = 0usize;
    let mut index = 0usize;
    while index < bytes.len() {
        match bytes[index] {
            b'\\' => index += 2,
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() {
                    if bytes[index] == b'\\' {
                        index += 2;
                        continue;
                    }
                    if bytes[index] == quote {
                        index += 1;
                        break;
                    }
                    index += 1;
                }
            }
            b'[' => index = skip_bracket(bytes, index),
            b'(' => index = skip_balanced(bytes, index),
            b',' => {
                let group = selector[start..index].trim();
                if !group.is_empty() {
                    groups.push(group);
                }
                start = index + 1;
                index += 1;
            }
            _ => index += 1,
        }
    }
    let group = selector[start..].trim();
    if !group.is_empty() {
        groups.push(group);
    }
    groups
}

/// State pseudos of one selector group split by position: the returned
/// `(subject, ancestor)` lists keep source order. The subject is the last
/// compound; anything before the last top-level combinator is an ancestor.
fn split_state_pseudos(group: &str) -> (Vec<String>, Vec<String>) {
    let bytes = group.as_bytes();
    let mut subject = Vec::new();
    let mut ancestor = Vec::new();
    let mut found: Vec<(usize, String)> = Vec::new();
    let mut subject_start = 0usize;
    let mut index = 0usize;
    while index < bytes.len() {
        match bytes[index] {
            b'\\' => index += 2,
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() {
                    if bytes[index] == b'\\' {
                        index += 2;
                        continue;
                    }
                    if bytes[index] == quote {
                        index += 1;
                        break;
                    }
                    index += 1;
                }
            }
            b'[' => index = skip_bracket(bytes, index),
            b'>' => {
                subject_start = index + 1;
                index += 1;
            }
            byte if byte.is_ascii_whitespace() => {
                // Only whitespace followed by another compound is a descendant
                // combinator; trailing whitespace must not move the subject.
                let mut look = index + 1;
                while look < bytes.len() && bytes[look].is_ascii_whitespace() {
                    look += 1;
                }
                if look < bytes.len() {
                    subject_start = look;
                }
                index += 1;
            }
            b':' => {
                let doubled = index + 1 < bytes.len() && bytes[index + 1] == b':';
                let pseudo_start = index;
                index += if doubled { 2 } else { 1 };
                let name_start = index;
                while index < bytes.len() && is_ident_byte(bytes[index]) {
                    index += 1;
                }
                let name = group[name_start..index].to_ascii_lowercase();
                if !doubled && hook_state_bit(&name).is_some() {
                    found.push((pseudo_start, name));
                }
                if index < bytes.len() && bytes[index] == b'(' {
                    index = skip_balanced(bytes, index);
                }
            }
            _ => index += 1,
        }
    }
    for (position, name) in found {
        if position >= subject_start {
            subject.push(name);
        } else {
            ancestor.push(name);
        }
    }
    (subject, ancestor)
}

/// True when every selector in the list applies its declarations only while a
/// state pseudo is active (so declarations are scoped to a state style).
fn selector_is_state_scoped(selector: &str) -> bool {
    let groups = split_selector_groups(selector);
    !groups.is_empty()
        && groups
            .iter()
            .all(|group| !split_state_pseudos(group).0.is_empty())
}

fn skip_bracket(bytes: &[u8], mut index: usize) -> usize {
    index += 1;
    while index < bytes.len() {
        match bytes[index] {
            b'\\' => index += 2,
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() {
                    if bytes[index] == b'\\' {
                        index += 2;
                        continue;
                    }
                    if bytes[index] == quote {
                        index += 1;
                        break;
                    }
                    index += 1;
                }
            }
            b']' => return index + 1,
            _ => index += 1,
        }
    }
    index
}

fn check_selector(
    selector: &str,
    path: &Path,
    line: u32,
    diagnostics: &mut Vec<Diagnostic>,
) -> bool {
    let bytes = selector.as_bytes();
    let mut unsupported = false;
    // Blob v6 only carries state overrides on the subject compound. A state
    // pseudo on an ancestor compound is very common CSS (`.card:hover .label`)
    // but this runtime cannot model it: it would compile nothing or the wrong
    // node's state. Reject it explicitly instead of accepting silently.
    for group in split_selector_groups(selector) {
        let (_, ancestor_states) = split_state_pseudos(group);
        for name in ancestor_states {
            diagnostics.push(Diagnostic::new(
                path,
                line,
                Some(format!("selector '{selector}'")),
                format!(
                    "pseudo-clase de estado ':{name}' en un compuesto ancestro no soportada en \
                     esta lane: el estado solo se aplica al compuesto sujeto (p. ej. \
                     '.card:hover .label' no se compila)"
                ),
            ));
            unsupported = true;
        }
    }
    let mut index = 0usize;
    let mut state_bits = 0u8;
    while index < bytes.len() {
        match bytes[index] {
            b',' => {
                // One state per rule is the supported contract; `:hover:active`
                // is the press combination and is therefore allowed.
                if report_state_combination(selector, path, line, state_bits, diagnostics) {
                    unsupported = true;
                }
                state_bits = 0;
                index += 1;
            }
            b'\\' => index += 2,
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() {
                    if bytes[index] == b'\\' {
                        index += 2;
                        continue;
                    }
                    if bytes[index] == quote {
                        index += 1;
                        break;
                    }
                    index += 1;
                }
            }
            b'[' => {
                index += 1;
                while index < bytes.len() && bytes[index] != b']' {
                    if bytes[index] == b'"' || bytes[index] == b'\'' {
                        let quote = bytes[index];
                        index += 1;
                        while index < bytes.len() && bytes[index] != quote {
                            if bytes[index] == b'\\' {
                                index += 1;
                            }
                            index += 1;
                        }
                    }
                    index += 1;
                }
                index += 1;
            }
            b':' => {
                let doubled = index + 1 < bytes.len() && bytes[index + 1] == b':';
                index += if doubled { 2 } else { 1 };
                let name_start = index;
                while index < bytes.len() && is_ident_byte(bytes[index]) {
                    index += 1;
                }
                let name = selector[name_start..index].to_ascii_lowercase();
                let mut argument: Option<&str> = None;
                if index < bytes.len() && bytes[index] == b'(' {
                    let open = index;
                    let after = skip_balanced(bytes, index);
                    if after > open + 1 && after <= bytes.len() && bytes[after - 1] == b')' {
                        argument = Some(&selector[open + 1..after - 1]);
                    }
                    index = after;
                }
                if doubled {
                    diagnostics.push(Diagnostic::new(
                        path,
                        line,
                        Some(format!("selector '{selector}'")),
                        format!(
                            "pseudo-elemento '::{name}' no soportado por subset v1 (fuera de v1)"
                        ),
                    ));
                    unsupported = true;
                } else if name == "checked" {
                    diagnostics.push(Diagnostic::new(
                        path,
                        line,
                        Some(format!("selector '{selector}'")),
                        "pseudo-clase ':checked' diferida: no hay widget checkbox/radio en esta \
                         lane (no se simula)"
                            .to_string(),
                    ));
                    unsupported = true;
                } else if !ALLOWED_PSEUDO_CLASSES.contains(&name.as_str()) {
                    diagnostics.push(Diagnostic::new(
                        path,
                        line,
                        Some(format!("selector '{selector}'")),
                        format!("pseudo-clase ':{name}' no soportada por subset v1"),
                    ));
                    unsupported = true;
                } else if name == "nth-child" || name == NTH_OF_TYPE {
                    let supported = argument.is_some_and(nth_argument_supported);
                    if !supported {
                        diagnostics.push(Diagnostic::new(
                            path,
                            line,
                            Some(format!("selector '{selector}'")),
                            format!(
                                "forma no soportada en ':{name}()': usa an+b, odd, even o un \
                                 entero (las variantes con 'of', 'nth-last-*' y otros argumentos \
                                 no se compilan)"
                            ),
                        ));
                        unsupported = true;
                    }
                } else if let Some(bit) = hook_state_bit(&name) {
                    state_bits |= bit;
                }
            }
            b'+' | b'~' => {
                diagnostics.push(Diagnostic::new(
                    path,
                    line,
                    Some(format!("selector '{selector}'")),
                    format!(
                        "combinador '{}' no soportado por subset v1",
                        bytes[index] as char
                    ),
                ));
                unsupported = true;
                index += 1;
            }
            _ => index += 1,
        }
    }
    if report_state_combination(selector, path, line, state_bits, diagnostics) {
        unsupported = true;
    }
    unsupported
}

fn report_state_combination(
    selector: &str,
    path: &Path,
    line: u32,
    state_bits: u8,
    diagnostics: &mut Vec<Diagnostic>,
) -> bool {
    if state_bits.count_ones() <= 1 || state_bits == (HOOK_STATE_HOVER | HOOK_STATE_ACTIVE) {
        return false;
    }
    diagnostics.push(Diagnostic::new(
        path,
        line,
        Some(format!("selector '{selector}'")),
        "combinacion de pseudo-clases de estado no soportada en esta lane \
         (solo una por regla; :hover:active forma el estilo press)"
            .to_string(),
    ));
    true
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_' || byte >= 0x80
}

/// The subset accepts exactly the An+B forms the selector parser compiles.
fn nth_argument_supported(argument: &str) -> bool {
    parse_an_plus_b(argument).is_some()
}

fn skip_balanced(bytes: &[u8], mut index: usize) -> usize {
    let mut depth = 0i32;
    while index < bytes.len() {
        match bytes[index] {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return index + 1;
                }
            }
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() && bytes[index] != quote {
                    if bytes[index] == b'\\' {
                        index += 1;
                    }
                    index += 1;
                }
            }
            _ => {}
        }
        index += 1;
    }
    index
}

fn push_at_rule(prelude: &str, path: &Path, line: u32, diagnostics: &mut Vec<Diagnostic>) {
    let name = at_rule_name(prelude);
    let message = if name == "font-face" {
        format!("at-rule '@{name}' no soportada por subset v1 (sin web fonts en v1)")
    } else {
        format!("at-rule '@{name}' no soportada por subset v1")
    };
    diagnostics.push(Diagnostic::new(
        path,
        line,
        Some(format!("at-rule '@{name}'")),
        message,
    ));
}

fn at_rule_name(prelude: &str) -> String {
    let rest = prelude.trim().trim_start_matches('@');
    let name: String = rest
        .chars()
        .take_while(|character| {
            !character.is_whitespace() && *character != '(' && *character != ';'
        })
        .collect();
    if name.is_empty() {
        "unknown".to_string()
    } else {
        name.to_ascii_lowercase()
    }
}

// --------------------------------------------------------------- properties

/// The only properties a blob v6 state style carries: background, text color,
/// the uniform border color (including the per-side color longhands) and the
/// additive `font-weight`.
fn state_color_property(name: &str) -> bool {
    matches!(
        name,
        "background"
            | "background-color"
            | "color"
            | "border-color"
            | "border-top-color"
            | "border-right-color"
            | "border-bottom-color"
            | "border-left-color"
            | "font-weight"
    )
}

/// `border` and `border-<side>` also set width/style, which a state rule cannot
/// represent; they are diagnosed as a partial (color-only) declaration.
fn is_border_shorthand(name: &str) -> bool {
    matches!(
        name,
        "border" | "border-top" | "border-right" | "border-bottom" | "border-left"
    )
}

/// A state color with alpha 0 normalizes to "no override" in the cascade, so
/// the declaration would silently do nothing. Uses the same `parse_color` path
/// the cascade uses (alpha is dropped; only alpha <= 0 becomes `None`).
fn state_color_has_zero_alpha(value: &str) -> bool {
    let components = split_components(value);
    if components.len() != 1 {
        return false;
    }
    let component = components[0];
    is_declared_color(component)
        && !component.eq_ignore_ascii_case("currentcolor")
        && parse_color(Some(component)).is_none()
}

#[allow(clippy::too_many_arguments)]
fn check_property(
    name: &str,
    value: &str,
    path: &Path,
    line: u32,
    where_context: &str,
    state_rule: bool,
    pilot: bool,
    diagnostics: &mut Vec<Diagnostic>,
) {
    let context = Some(format!("{where_context}, propiedad '{name}'"));
    let mut report = |message: String| {
        diagnostics.push(Diagnostic::new(path, line, context.clone(), message));
    };

    let trimmed = value.trim();
    if trimmed.is_empty() {
        report(format!("valor vacio en '{name}'"));
        return;
    }
    // A state rule is not a full declaration block: blob v6 only carries the
    // three color groups for hover/press/focus/disabled. Diagnose anything the
    // runtime would drop instead of leaving a rule that visually does nothing.
    if state_rule {
        if !state_color_property(name) {
            if is_border_shorthand(name) {
                report(format!(
                    "'{name}' en una regla de estado solo aplica el color: ancho y estilo de \
                     borde no se soportan (usa border-color o border-*-color)"
                ));
            } else {
                report(format!(
                    "propiedad '{name}' no soportada en una regla de estado: solo aplican \
                     background/background-color, color, border-color (color de borde uniforme) \
                     y font-weight"
                ));
            }
            return;
        }
        if state_color_has_zero_alpha(trimmed) {
            report(format!(
                "color de estado con alfa 0 no soportado en '{name}': '{trimmed}' no tendria \
                 efecto (usa un color opaco)"
            ));
        }
    }
    let lowered = trimmed.to_ascii_lowercase();
    for (needle, message) in [
        (
            "calc(",
            "calc() no soportado (subset v1: px/%/valores simples)",
        ),
        ("var(", "var()/custom properties no soportadas"),
        ("env(", "env() no soportado"),
        ("gradient(", "gradientes no soportados (fuera de v1)"),
        (
            "url(",
            "url()/recursos externos no soportados (fuera de v1)",
        ),
    ] {
        if lowered.contains(needle) {
            report(format!("{message} en '{name}'"));
            return;
        }
    }

    // v2 pilot extensions: warning by default, silent with `--pilot`.
    let pilot_extension = |feature: &str, report: &mut dyn FnMut(String)| {
        if !pilot {
            report(format!(
                "{feature} es una extension del piloto v2 (R2, fuera de subset v1); usa --pilot para permitirla"
            ));
        }
    };
    // v3 pilot extensions (the CSS layer for borders/radii/shadows/units).
    let pilot_v3_extension = |feature: &str, report: &mut dyn FnMut(String)| {
        if !pilot {
            report(format!(
                "{feature} es una extension del piloto v3 (estilos CSS, fuera de subset v1); usa --pilot para permitirla"
            ));
        }
    };

    match name {
        "display" => {
            if lowered == "grid" {
                pilot_extension("'display: grid'", &mut report);
            } else {
                check_keyword(
                    name,
                    &lowered,
                    &["block", "flex", "none"],
                    "block | flex | none",
                    &mut report,
                );
            }
        }
        "position" => {
            if matches!(lowered.as_str(), "fixed" | "sticky") {
                report(format!(
                    "position: {lowered} no soportado en esta lane (subset: static | relative | absolute; fixed/sticky diferidos)"
                ));
            } else {
                check_keyword(
                    name,
                    &lowered,
                    &["static", "relative", "absolute"],
                    "static | relative | absolute",
                    &mut report,
                );
            }
        }
        "top" | "right" | "bottom" | "left" => {
            check_lengths(
                name,
                trimmed,
                1,
                true,
                true,
                true,
                false,
                pilot,
                "px | % | auto",
                &mut report,
            );
        }
        "inset" => {
            report(
                "shorthand 'inset' no soportado en esta lane; usa top/right/bottom/left"
                    .to_string(),
            );
        }
        "z-index" => {
            // Blob v5: `auto | <integer>`. The runtime only applies it to
            // positioned nodes (relative/absolute), which is CSS-correct for
            // the subset (no flex/grid item stacking contexts yet).
            if !z_index_value_is_valid(trimmed) {
                report(format!(
                    "valor '{trimmed}' no soportado en 'z-index' (v5: auto | entero de 32 bits; -2147483648 reservado para auto)"
                ));
            }
        }
        "flex-direction" => check_keyword(
            name,
            &lowered,
            &["row", "column"],
            "row | column",
            &mut report,
        ),
        "justify-content" => check_keyword(
            name,
            &lowered,
            &["flex-start", "center", "flex-end", "space-between"],
            "flex-start | center | flex-end | space-between",
            &mut report,
        ),
        "align-items" => check_keyword(
            name,
            &lowered,
            &["stretch", "flex-start", "center", "flex-end"],
            "stretch | flex-start | center | flex-end",
            &mut report,
        ),
        "flex-wrap" => check_keyword(
            name,
            &lowered,
            &["nowrap", "wrap"],
            "nowrap | wrap",
            &mut report,
        ),
        "box-sizing" => check_keyword(
            name,
            &lowered,
            &["border-box", "content-box"],
            "border-box",
            &mut report,
        ),
        /* Flex growth: numbers, or a px/auto/0 basis. Values are validated by
         * the cascade; the subset only rejects empty/unknown keywords. */
        "flex-grow" | "flex-shrink" | "flex-basis" | "flex" => {
            if trimmed.is_empty() {
                report(format!("propiedad CSS '{name}' sin valor"));
            }
        }
        "border-style" => check_keyword(
            name,
            &lowered,
            &["solid", "none"],
            "solid | none",
            &mut report,
        ),
        "overflow" => {
            if matches!(lowered.as_str(), "hidden" | "auto" | "scroll") {
                if lowered != "hidden" {
                    pilot_extension(&format!("'overflow: {lowered}' (scroll R2)"), &mut report);
                }
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en 'overflow' (v1: hidden; piloto v2: auto | scroll)"
                ));
            }
        }
        "text-overflow" => {
            if lowered != "ellipsis" && lowered != "clip" {
                report(format!(
                    "valor '{trimmed}' no soportado en 'text-overflow' (solo ellipsis | clip)"
                ));
            }
        }
        "grid-template-columns" => {
            if parse_grid_template_columns(trimmed).is_some() {
                pilot_extension(
                    "'grid-template-columns: repeat(auto-fill, <px>)'",
                    &mut report,
                );
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en 'grid-template-columns' (v2: solo repeat(auto-fill, <px>))"
                ));
            }
        }
        "grid-auto-rows" => {
            if parse_grid_auto_rows(trimmed).is_some() {
                pilot_extension("'grid-auto-rows: <px> | auto'", &mut report);
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en 'grid-auto-rows' (v2: <px> | auto)"
                ));
            }
        }
        "object-fit" => {
            if matches!(lowered.as_str(), "fill" | "cover" | "contain") {
                pilot_extension("'object-fit' (imagenes R2)", &mut report);
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en 'object-fit' (v2: fill | cover | contain)"
                ));
            }
        }
        "font-weight" => {
            // CSS keys and integers share the cascade parser, so the subset
            // and the emitted style cannot disagree about what is valid.
            if parse_font_weight(trimmed).is_none() {
                report(format!(
                    "valor '{trimmed}' no soportado en 'font-weight' (subset v1: normal | bold | 100-900)"
                ));
            }
        }
        "text-align" => check_keyword(
            name,
            &lowered,
            &["left", "center", "right"],
            "left | center | right",
            &mut report,
        ),
        "width" | "height" => {
            check_lengths(
                name,
                trimmed,
                1,
                true,
                false,
                true,
                true,
                pilot,
                "px | % | auto | em | rem | vh | vw",
                &mut report,
            );
        }
        "font-size" => {
            check_lengths(
                name,
                trimmed,
                1,
                false,
                false,
                false,
                false,
                pilot,
                "px",
                &mut report,
            );
        }
        "line-height" => {
            if lowered != "normal" {
                check_lengths(
                    name,
                    trimmed,
                    1,
                    false,
                    false,
                    false,
                    false,
                    pilot,
                    "px | normal",
                    &mut report,
                );
            }
        }
        "gap" | "row-gap" | "column-gap" => {
            check_lengths(
                name,
                trimmed,
                1,
                false,
                false,
                false,
                true,
                pilot,
                "px | em | rem | vh | vw",
                &mut report,
            );
        }
        "margin" => {
            check_lengths(
                name,
                trimmed,
                4,
                false,
                true,
                true,
                true,
                pilot,
                "1-4 valores px | em | rem | vh | vw | auto",
                &mut report,
            );
        }
        "padding" => {
            check_lengths(
                name,
                trimmed,
                4,
                false,
                false,
                false,
                true,
                pilot,
                "1-4 valores px | em | rem | vh | vw",
                &mut report,
            );
        }
        "margin-top" | "margin-right" | "margin-bottom" | "margin-left" => {
            check_lengths(
                name,
                trimmed,
                1,
                false,
                true,
                true,
                true,
                pilot,
                "px | em | rem | vh | vw | auto",
                &mut report,
            );
        }
        "padding-top" | "padding-right" | "padding-bottom" | "padding-left" => {
            check_lengths(
                name,
                trimmed,
                1,
                false,
                false,
                false,
                true,
                pilot,
                "px | em | rem | vh | vw",
                &mut report,
            );
        }
        "border-width" => {
            check_lengths(
                name,
                trimmed,
                4,
                false,
                false,
                false,
                true,
                pilot,
                "1-4 valores px | em | rem | vh | vw",
                &mut report,
            );
        }
        "border-radius" => {
            check_lengths(
                name,
                trimmed,
                4,
                false,
                false,
                false,
                true,
                pilot,
                "1-4 valores px | em | rem | vh | vw",
                &mut report,
            );
        }
        "border" => {
            let components = split_components(trimmed);
            if components.is_empty()
                || components.len() > 3
                || !components
                    .iter()
                    .all(|component| border_component_is_valid(component))
            {
                report(format!(
                    "valor '{trimmed}' no soportado en 'border' (subset v1: ancho px + solid|none + color)"
                ));
            } else if components
                .iter()
                .any(|component| is_relative_length(component))
                && !pilot
            {
                report(
                    "unidad relativa (em/rem/vh/vw) en 'border' es una extension del piloto v3 (estilos CSS, fuera de subset v1); usa --pilot para permitirla"
                        .to_string(),
                );
            }
        }
        "border-color" => {
            let components = split_components(trimmed);
            if components.is_empty()
                || components.len() > 4
                || !components
                    .iter()
                    .all(|component| is_supported_color(component))
            {
                report(format!(
                    "valor '{trimmed}' no soportado en 'border-color' (subset v1: 1-4 colores)"
                ));
            }
        }
        "min-width" | "max-width" | "min-height" | "max-height" => {
            // New v3 pilot property: px/%/auto/relative unit.
            let valid = check_lengths(
                name,
                trimmed,
                1,
                true,
                false,
                true,
                true,
                true,
                "px | % | auto | em | rem | vh | vw",
                &mut report,
            );
            if valid {
                pilot_v3_extension(&format!("'{name}'"), &mut report);
            }
        }
        "border-top-width" | "border-right-width" | "border-bottom-width" | "border-left-width" => {
            let valid = check_lengths(
                name,
                trimmed,
                1,
                false,
                false,
                false,
                true,
                true,
                "px | em | rem | vh | vw",
                &mut report,
            );
            if valid {
                pilot_v3_extension("propiedades de borde por lado", &mut report);
            }
        }
        "border-top-style" | "border-right-style" | "border-bottom-style" | "border-left-style" => {
            if matches!(lowered.as_str(), "solid" | "none") {
                pilot_v3_extension("propiedades de borde por lado", &mut report);
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en '{name}' (v3: solid | none)"
                ));
            }
        }
        "border-top-color" | "border-right-color" | "border-bottom-color" | "border-left-color" => {
            let components = split_components(trimmed);
            if components.len() == 1 && is_supported_color(components[0]) {
                pilot_v3_extension("propiedades de borde por lado", &mut report);
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en '{name}' (v3: un color)"
                ));
            }
        }
        "border-top" | "border-right" | "border-bottom" | "border-left" => {
            let components = split_components(trimmed);
            if components.is_empty()
                || components.len() > 3
                || !components
                    .iter()
                    .all(|component| border_component_is_valid(component))
            {
                report(format!(
                    "valor '{trimmed}' no soportado en '{name}' (v3: ancho + solid|none + color)"
                ));
            } else {
                pilot_v3_extension("propiedades de borde por lado", &mut report);
            }
        }
        "box-shadow" => {
            if box_shadow_is_valid(trimmed) {
                pilot_v3_extension("'box-shadow' (sombras R2)", &mut report);
            } else {
                report(format!(
                    "valor '{trimmed}' no soportado en 'box-shadow' (v3: <x> <y> [blur] [spread] [color], lista con comas)"
                ));
            }
        }
        "color" | "background" | "background-color" => {
            let components = split_components(trimmed);
            if components.len() != 1 || !is_supported_color(components[0]) {
                report(format!(
                    "valor '{trimmed}' no soportado en '{name}' (v1: hex 3/4/6/8, rgb[a](), hsl[a]() o nombre CSS3)"
                ));
            }
        }
        "opacity" => match parse_css_number(trimmed) {
            Some(value) if (0.0..=1.0).contains(&value) => {}
            _ => report(format!(
                "valor '{trimmed}' no soportado en 'opacity' (v1: 0-1)"
            )),
        },
        "font-family" => {}
        _ => report(format!("propiedad CSS '{name}' no soportada por subset v1")),
    }
}

fn check_keyword(
    name: &str,
    value: &str,
    allowed: &[&str],
    description: &str,
    report: &mut impl FnMut(String),
) {
    if !allowed.contains(&value) {
        report(format!(
            "valor '{value}' no soportado en '{name}' (subset v1: {description})"
        ));
    }
}

/// Valid component of `border`/`border-<side>`: width (px or relative),
/// style (`solid`/`none`) or color.
fn border_component_is_valid(component: &str) -> bool {
    component.eq_ignore_ascii_case("solid")
        || component.eq_ignore_ascii_case("none")
        || is_relative_length(component)
        || is_length(component, false, false)
        || is_supported_color(component)
}

#[allow(clippy::too_many_arguments)]
fn check_lengths(
    name: &str,
    value: &str,
    max_count: usize,
    allow_percent: bool,
    allow_negative: bool,
    allow_auto: bool,
    allow_relative: bool,
    pilot: bool,
    description: &str,
    report: &mut impl FnMut(String),
) -> bool {
    if allow_auto && value.eq_ignore_ascii_case("auto") {
        return true;
    }
    let components = split_components(value);
    if components.is_empty() || components.len() > max_count {
        report(format!(
            "valor '{value}' no soportado en '{name}' (subset v1: {description})"
        ));
        return false;
    }
    let mut relative_reported = false;
    for component in components {
        if allow_auto && component.eq_ignore_ascii_case("auto") {
            continue;
        }
        if allow_relative && is_relative_length(component) {
            if !pilot && !relative_reported {
                report(format!(
                    "unidad relativa '{component}' (em/rem/vh/vw) es una extension del piloto v3 (estilos CSS, fuera de subset v1); usa --pilot para permitirla"
                ));
                relative_reported = true;
            }
            continue;
        }
        if !is_length(component, allow_percent, allow_negative) {
            report(format!(
                "unidad o valor '{component}' no soportado en '{name}' (subset v1: {description})"
            ));
            return false;
        }
    }
    true
}

fn split_components(value: &str) -> Vec<&str> {
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
            b'(' => {
                depth += 1;
                if start.is_none() {
                    start = Some(index);
                }
            }
            b')' => {
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
                    out.push(&value[begin..index]);
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
        out.push(&value[begin..]);
    }
    out
}

fn is_length(token: &str, allow_percent: bool, allow_negative: bool) -> bool {
    let trimmed = token.trim();
    if trimmed.is_empty() {
        return false;
    }
    if trimmed == "0" {
        return true;
    }
    if let Some(number) = trimmed.strip_suffix("px") {
        if let Some(value) = parse_css_number(number) {
            return allow_negative || value >= 0.0;
        }
        return false;
    }
    if allow_percent {
        if let Some(number) = trimmed.strip_suffix('%') {
            if let Some(value) = parse_css_number(number) {
                return allow_negative || value >= 0.0;
            }
        }
    }
    false
}

/// `auto` or an ASCII integer inside int32. `INT32_MIN` is the blob's `auto`
/// sentinel, so it is not a representable explicit value.
fn z_index_value_is_valid(value: &str) -> bool {
    let text = value.trim();
    if text.eq_ignore_ascii_case("auto") {
        return true;
    }
    let digits = text.strip_prefix(['+', '-']).unwrap_or(text);
    if digits.is_empty() || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return false;
    }
    match text.parse::<i64>() {
        Ok(number) => number != i64::from(i32::MIN) && i32::try_from(number).is_ok(),
        Err(_) => false,
    }
}

fn parse_css_number(text: &str) -> Option<f64> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return None;
    }
    if !trimmed
        .chars()
        .all(|character| character.is_ascii_digit() || matches!(character, '.' | '-' | '+'))
    {
        return None;
    }
    if matches!(trimmed, "-" | "+" | ".") {
        return None;
    }
    trimmed.parse::<f64>().ok()
}

/// Exactly the declared-color grammar used by `background`, `color`,
/// `border*` and `box-shadow`. Keeping this as a delegation (and
/// not a second list) is what guarantees a value the cascade drops (`lab()`,
/// `color-mix()`, …) is always diagnosed by `--strict-subset`, while every
/// accepted form stays silent.
fn is_supported_color(token: &str) -> bool {
    is_declared_color(token)
}

// ------------------------------------------------------------------ utils

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

fn first_non_ws(text: &str, start: usize, end: usize) -> usize {
    let slice = &text[start..end];
    start + slice.len() - slice.trim_start().len()
}

/// First index that is neither whitespace nor a `/* ... */` comment.
fn first_content_index(text: &str, start: usize, end: usize) -> usize {
    let bytes = text.as_bytes();
    let mut index = start;
    loop {
        while index < end && bytes[index].is_ascii_whitespace() {
            index += 1;
        }
        if index + 1 < end && bytes[index] == b'/' && bytes[index + 1] == b'*' {
            index += 2;
            while index + 1 < end && !(bytes[index] == b'*' && bytes[index + 1] == b'/') {
                index += 1;
            }
            if index + 1 < end {
                index += 2;
            } else {
                index = end;
            }
            continue;
        }
        return index;
    }
}

/// Replaces comments with spaces while preserving bytes and line breaks, so
/// `cssparser` positions and `line_at` stay valid.
fn mask_comments(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = bytes.to_vec();
    let mut index = 0usize;
    let mut quote: Option<u8> = None;
    while index < bytes.len() {
        if let Some(active) = quote {
            if bytes[index] == b'\\' {
                index += 2;
                continue;
            }
            if bytes[index] == active {
                quote = None;
            }
            index += 1;
            continue;
        }
        match bytes[index] {
            b'"' | b'\'' => {
                quote = Some(bytes[index]);
                index += 1;
            }
            b'/' if index + 1 < bytes.len() && bytes[index + 1] == b'*' => {
                out[index] = b' ';
                out[index + 1] = b' ';
                index += 2;
                while index < bytes.len() {
                    if index + 1 < bytes.len() && bytes[index] == b'*' && bytes[index + 1] == b'/' {
                        out[index] = b' ';
                        out[index + 1] = b' ';
                        index += 2;
                        break;
                    }
                    if bytes[index] != b'\n' {
                        out[index] = b' ';
                    }
                    index += 1;
                }
            }
            _ => index += 1,
        }
    }
    String::from_utf8(out).unwrap_or_else(|_| text.to_string())
}

/// Strips comments from a prelude while keeping the remaining text.
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

fn line_at(text: &str, base_line: u32, byte_index: usize) -> u32 {
    let end = byte_index.min(text.len());
    base_line
        + text.as_bytes()[..end]
            .iter()
            .filter(|byte| **byte == b'\n')
            .count() as u32
}

/// Resolves a linked stylesheet only if it is local, same as `dom::stylesheet_texts`.
fn read_linked_stylesheet(href: &str, base_dir: &Path) -> Option<(String, std::path::PathBuf)> {
    if href.is_empty()
        || href.starts_with("http:")
        || href.starts_with("https:")
        || href.starts_with("//")
    {
        return None;
    }
    let path = base_dir.join(href);
    let text = fs::read_to_string(&path).ok()?;
    Some((text, path))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn css(text: &str) -> Vec<Diagnostic> {
        let mut diagnostics = Vec::new();
        check_stylesheet(text, Path::new("test.css"), 1, &mut diagnostics);
        diagnostics
    }

    #[test]
    fn supported_html_is_clean() {
        let html = r##"<!doctype html>
<html>
  <head>
    <style>.a { color: #fff; display: flex; gap: 8px; }</style>
  </head>
  <body>
    <div class="a" data-hover="#fff" style="margin: 0">x</div>
  </body>
</html>"##;
        let diagnostics = check_document(html, Path::new("page.html"), Path::new("."));
        assert!(diagnostics.is_empty(), "{diagnostics:#?}");
    }

    #[test]
    fn flags_tags_attributes_and_lines() {
        let html =
            "<html>\n<body>\n<script>x</script>\n<div onclick=\"x\">y</div>\n</body>\n</html>";
        let diagnostics = check_document(html, Path::new("page.html"), Path::new("."));
        assert_eq!(diagnostics.len(), 2, "{diagnostics:#?}");
        assert!(diagnostics[0].message.contains("<script>"));
        assert_eq!(diagnostics[0].line, Some(3));
        assert!(diagnostics[1].message.contains("onclick"));
        assert_eq!(diagnostics[1].line, Some(4));
    }

    #[test]
    fn flags_values_properties_and_at_rules() {
        let diagnostics = css(
            ".x {\n  display: grid;\n  box-shadow: 0 0 1px #000;\n  color: lab(0% 0 0);\n}\n@media (min-width: 10px) { .y { color: red; } }\n",
        );
        assert_eq!(diagnostics.len(), 4, "{diagnostics:#?}");
        let lines: Vec<Option<u32>> = diagnostics.iter().map(|item| item.line).collect();
        assert_eq!(lines, vec![Some(2), Some(3), Some(4), Some(6)]);
    }

    #[test]
    fn comments_do_not_trigger_diagnostics() {
        let diagnostics = css("/* + ~ ::before */\n.a /* + */ {\n  color: red /* comment */;\n}\n");
        assert!(diagnostics.is_empty(), "{diagnostics:#?}");
    }

    #[test]
    fn style_block_uses_html_line_numbers() {
        let html =
            "<html>\n<head>\n<style>\n.a {\n  transition: none;\n}\n</style>\n</head>\n</html>";
        let diagnostics = check_document(html, Path::new("page.html"), Path::new("."));
        assert_eq!(diagnostics.len(), 1, "{diagnostics:#?}");
        assert_eq!(diagnostics[0].file, "page.html");
        assert_eq!(diagnostics[0].line, Some(5));
    }

    #[test]
    fn pilot_extensions_warn_by_default_and_are_allowed_with_pilot() {
        let text = ".g {\n  display: grid;\n  grid-template-columns: repeat(auto-fill, 180px);\n  grid-auto-rows: 240px;\n  overflow: auto;\n  object-fit: cover;\n}\n";
        let mut diagnostics = Vec::new();
        check_stylesheet_pilot(text, Path::new("test.css"), 1, false, &mut diagnostics);
        assert_eq!(diagnostics.len(), 5, "{diagnostics:#?}");
        assert!(
            diagnostics
                .iter()
                .all(|item| item.message.contains("piloto v2")),
            "{diagnostics:#?}"
        );
        let lines: Vec<Option<u32>> = diagnostics.iter().map(|item| item.line).collect();
        assert_eq!(lines, vec![Some(2), Some(3), Some(4), Some(5), Some(6)]);

        let mut allowed = Vec::new();
        check_stylesheet_pilot(text, Path::new("test.css"), 1, true, &mut allowed);
        assert!(allowed.is_empty(), "{allowed:#?}");
    }

    #[test]
    fn pilot_keeps_invalid_grid_values_rejected() {
        let text = ".g {\n  display: grid;\n  grid-template-columns: 1fr;\n  grid-auto-rows: minmax(10px, 1fr);\n  object-fit: zoom;\n}\n";
        let mut diagnostics = Vec::new();
        check_stylesheet_pilot(text, Path::new("test.css"), 1, true, &mut diagnostics);
        assert_eq!(diagnostics.len(), 3, "{diagnostics:#?}");
        assert!(diagnostics
            .iter()
            .all(|item| !item.message.contains("piloto v2")));
    }

    #[test]
    fn inline_pilot_extension_follows_the_same_gate() {
        let html = "<html><body><div style=\"display: grid; overflow: auto\">x</div></body></html>";
        let strict = check_document_pilot(html, Path::new("page.html"), Path::new("."), false);
        assert_eq!(strict.len(), 2, "{strict:#?}");
        let allowed = check_document_pilot(html, Path::new("page.html"), Path::new("."), true);
        assert!(allowed.is_empty(), "{allowed:#?}");
    }

    /// The declared-color superset is accepted without diagnostics (the seven
    /// reviewer rows plus the border shorthand that used to drop the whole
    /// declaration).
    #[test]
    fn declared_color_superset_is_clean() {
        let text = ".a {\n  color: rebeccapurple;\n  background: rgb(255 0 0);\n  border: 2px solid rebeccapurple;\n  border-color: rgba(0,0,0,50%);\n  box-shadow: 0 0 4px rebeccapurple;\n  background-color: rgb(0,128,0,0.5);\n}\n";
        let mut diagnostics = Vec::new();
        check_stylesheet_pilot(text, Path::new("test.css"), 1, true, &mut diagnostics);
        assert!(diagnostics.is_empty(), "{diagnostics:#?}");

        // Without `--pilot` only the v3 box-shadow extension is reported; the
        // colors themselves stay clean.
        let mut default = Vec::new();
        check_stylesheet_pilot(text, Path::new("test.css"), 1, false, &mut default);
        assert_eq!(default.len(), 1, "{default:#?}");
        assert!(default[0].message.contains("piloto v3"), "{default:#?}");
    }

    /// Modern color syntax must be diagnosed instead of silently dropped.
    #[test]
    fn modern_color_syntax_is_diagnosed() {
        let text = ".a {\n  color: lab(50% 40 59);\n  background: color-mix(in srgb, red, blue);\n  border: 2px solid lab(50% 40 59);\n}\n";
        let mut diagnostics = Vec::new();
        check_stylesheet_pilot(text, Path::new("test.css"), 1, true, &mut diagnostics);
        assert_eq!(diagnostics.len(), 3, "{diagnostics:#?}");
        let lines: Vec<Option<u32>> = diagnostics.iter().map(|item| item.line).collect();
        assert_eq!(lines, vec![Some(2), Some(3), Some(4)]);
        for item in &diagnostics {
            assert!(
                item.message.contains("no soportado"),
                "unexpected message: {}",
                item.message
            );
        }
    }

    /// State pseudos are only semantic on the subject compound. The ancestor
    /// form is common CSS this lane cannot render, so it is diagnosed rather
    /// than compiled silently; the subject form stays clean.
    #[test]
    fn ancestor_state_pseudo_is_diagnosed() {
        let diagnostics = css(
            ".card:hover .label { color: red; }\n.card .label:hover { color: blue; }\n",
        );
        assert_eq!(diagnostics.len(), 1, "{diagnostics:#?}");
        assert!(diagnostics[0].message.contains("ancestro"), "{diagnostics:#?}");
        assert!(
            diagnostics[0]
                .context
                .as_deref()
                .is_some_and(|context| context.contains(".card:hover .label")),
            "{diagnostics:#?}"
        );
    }

    /// A state rule only carries colors in blob v6; the rest would be dropped,
    /// so it is diagnosed (including a zero-alpha color that becomes "no
    /// override" in the cascade).
    #[test]
    fn state_rules_only_accept_color_declarations() {
        let diagnostics = css(
            ".b:hover {\n  opacity: 0.5;\n  border-color: #ffffff;\n  background: transparent;\n}\n",
        );
        assert_eq!(diagnostics.len(), 2, "{diagnostics:#?}");
        let lines: Vec<Option<u32>> = diagnostics.iter().map(|item| item.line).collect();
        assert_eq!(lines, vec![Some(2), Some(4)]);
        assert!(
            diagnostics.iter().any(|item| item.message.contains("'opacity'")),
            "{diagnostics:#?}"
        );
        assert!(
            diagnostics.iter().any(|item| item.message.contains("alfa 0")),
            "{diagnostics:#?}"
        );
    }
}
