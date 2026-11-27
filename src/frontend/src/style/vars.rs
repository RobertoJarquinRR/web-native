//! CSS custom properties (`--token`) and `var()` substitution.
//!
//! This is the simple, build-time case: custom properties cascade and inherit
//! like any other declaration, `var(--name)` / `var(--name, fallback)` are
//! substituted before the value is interpreted, and cycles make the custom
//! property invalid (its `var()` call then uses the fallback, if any).
//!
//! Nothing here reaches the blob: substitution finishes in the cascade, so the
//! emitted style keeps carrying concrete numbers and colors.
//!
//! Scope (documented in `docs/subset-v1.md`):
//! - custom property names are case-sensitive (`--Ink` and `--ink` differ);
//! - the fallback is used when the token is undefined **or** invalid (cycle),
//!   matching CSS Variables 1;
//! - a `var()` without fallback that cannot be resolved makes the declaration
//!   invalid at computed-value time: the property falls back to its inherited
//!   or initial value instead of writing a wrong number.

use std::collections::{HashMap, HashSet};

/// `--name` with at least one name char after the two dashes.
pub(crate) fn is_custom_property(name: &str) -> bool {
    let Some(rest) = name.strip_prefix("--") else {
        return false;
    };
    !rest.is_empty() && rest.chars().all(is_custom_name_char)
}

fn is_custom_name_char(character: char) -> bool {
    character.is_ascii_alphanumeric()
        || character == '-'
        || character == '_'
        || (character as u32) >= 0x80
}

/// True when `value` contains a textual `var(` function token. A malformed
/// call still counts so the callers validate/reject it instead of falling
/// through to the property grammar.
pub(crate) fn has_var_function(value: &str) -> bool {
    find_var(value, 0).is_some()
}

/// One `var()` call. `end` is the index just after the closing `)`.
struct VarCall<'a> {
    start: usize,
    end: usize,
    name: &'a str,
    fallback: Option<&'a str>,
}

enum VarScan<'a> {
    Call(VarCall<'a>),
    /// `var(` was found but the argument is not `--name[, fallback]`.
    Invalid {
        end: usize,
    },
    /// `var(` without a closing `)`.
    Unterminated,
}

/// Finds the next `var()` call at or after `from`, skipping strings.
fn find_var(value: &str, from: usize) -> Option<VarScan<'_>> {
    let bytes = value.as_bytes();
    let mut index = from;
    while index < bytes.len() {
        match bytes[index] {
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
            byte if is_var_start(bytes, index) => {
                let Some(close) = matching_paren(bytes, index + 3) else {
                    return Some(VarScan::Unterminated);
                };
                let inside = &value[index + 4..close - 1];
                let argument_end = close;
                return Some(match parse_argument(inside) {
                    Some((name, fallback)) => VarScan::Call(VarCall {
                        start: index,
                        end: argument_end,
                        name,
                        fallback,
                    }),
                    None => VarScan::Invalid { end: argument_end },
                });
            }
            _ => index += 1,
        }
    }
    None
}

/// `var(` (case-insensitive) at `index`, but not the tail of a longer
/// identifier (`myvar(` is a different function token).
fn is_var_start(bytes: &[u8], index: usize) -> bool {
    if index + 3 >= bytes.len() {
        return false;
    }
    if !bytes[index].eq_ignore_ascii_case(&b'v')
        || !bytes[index + 1].eq_ignore_ascii_case(&b'a')
        || !bytes[index + 2].eq_ignore_ascii_case(&b'r')
        || bytes[index + 3] != b'('
    {
        return false;
    }
    index == 0 || !is_ident_byte(bytes[index - 1])
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_' || byte >= 0x80
}

/// Index just after the `)` matching the `(` at `open`.
fn matching_paren(bytes: &[u8], open: usize) -> Option<usize> {
    let mut depth = 0i32;
    let mut index = open;
    while index < bytes.len() {
        match bytes[index] {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(index + 1);
                }
            }
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() {
                    if bytes[index] == b'\\' {
                        index += 2;
                        continue;
                    }
                    if bytes[index] == quote {
                        break;
                    }
                    index += 1;
                }
            }
            _ => {}
        }
        index += 1;
    }
    None
}

/// Splits `--name[ , fallback]`; the name must be a custom property.
fn parse_argument(inside: &str) -> Option<(&str, Option<&str>)> {
    match split_first_top_level_comma(inside) {
        Some((name, fallback)) => {
            let name = name.trim();
            if is_custom_property(name) {
                Some((name, Some(fallback)))
            } else {
                None
            }
        }
        None => {
            let name = inside.trim();
            if is_custom_property(name) {
                Some((name, None))
            } else {
                None
            }
        }
    }
}

fn split_first_top_level_comma(text: &str) -> Option<(&str, &str)> {
    let bytes = text.as_bytes();
    let mut depth = 0i32;
    let mut index = 0usize;
    while index < bytes.len() {
        match bytes[index] {
            b'(' | b'[' => depth += 1,
            b')' | b']' => depth -= 1,
            b'"' | b'\'' => {
                let quote = bytes[index];
                index += 1;
                while index < bytes.len() {
                    if bytes[index] == b'\\' {
                        index += 2;
                        continue;
                    }
                    if bytes[index] == quote {
                        break;
                    }
                    index += 1;
                }
            }
            b',' if depth == 0 => {
                return Some((&text[..index], &text[index + 1..]));
            }
            _ => {}
        }
        index += 1;
    }
    None
}

/// Replaces every `var()` using `props` (already resolved values).
///
/// Returns `None` when a reference is malformed or cannot be resolved and has
/// no fallback: the caller must treat that declaration as invalid at
/// computed-value time.
pub(crate) fn substitute_vars(value: &str, props: &HashMap<String, String>) -> Option<String> {
    substitute_with(value, &mut |name| props.get(name).cloned())
}

fn substitute_with<F>(value: &str, lookup: &mut F) -> Option<String>
where
    F: FnMut(&str) -> Option<String>,
{
    let mut out = String::with_capacity(value.len());
    let mut rest = 0usize;
    while let Some(scan) = find_var(value, rest) {
        match scan {
            VarScan::Unterminated | VarScan::Invalid { .. } => return None,
            VarScan::Call(call) => {
                out.push_str(&value[rest..call.start]);
                let resolved = lookup(call.name).or_else(|| {
                    call.fallback
                        .and_then(|fallback| substitute_with(fallback.trim(), lookup))
                });
                out.push_str(&resolved?);
                rest = call.end;
            }
        }
    }
    out.push_str(&value[rest..]);
    Some(out)
}

/// Resolves every custom property, following references and rejecting cycles.
///
/// `raw` maps names to declared values (own declarations over inherited,
/// already-computed values). A cycle leaves all its members out of the result:
/// CSS treats them as guaranteed-invalid, which makes a `var()` with fallback
/// use that fallback.
pub(crate) fn resolve_custom_properties(raw: &HashMap<String, String>) -> HashMap<String, String> {
    let mut resolved = HashMap::new();
    let mut visiting: Vec<String> = Vec::new();
    for name in raw.keys() {
        resolve_one(name, raw, &mut resolved, &mut visiting);
    }
    resolved
}

fn resolve_one(
    name: &str,
    raw: &HashMap<String, String>,
    resolved: &mut HashMap<String, String>,
    visiting: &mut Vec<String>,
) -> Option<String> {
    if let Some(done) = resolved.get(name) {
        return Some(done.clone());
    }
    if visiting.iter().any(|item| item == name) {
        return None;
    }
    let value = raw.get(name)?.clone();
    visiting.push(name.to_string());
    let result = substitute_with(&value, &mut |other| {
        resolve_one(other, raw, resolved, visiting)
    });
    visiting.pop();
    if let Some(text) = &result {
        resolved.insert(name.to_string(), text.clone());
    }
    result
}

/// True when a `var()` call has no fallback; used by the diagnostic layer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum VarIssue {
    /// `var(` without `)`, or an argument that is not `--name[, fallback]`.
    Malformed,
    /// `--name` is not defined anywhere in the document and there is no
    /// fallback.
    Undefined(String),
}

/// Static validation used by `subset v1`.
///
/// `defined` is the document-wide set of declared custom property names, so a
/// token from another stylesheet or from an inline `style=""` is not reported.
pub(crate) fn collect_var_issues(value: &str, defined: &HashSet<String>) -> Vec<VarIssue> {
    let mut issues = Vec::new();
    collect_var_issues_into(value, defined, &mut issues);
    issues
}

fn collect_var_issues_into(value: &str, defined: &HashSet<String>, issues: &mut Vec<VarIssue>) {
    let mut rest = 0usize;
    while let Some(scan) = find_var(value, rest) {
        match scan {
            VarScan::Unterminated => {
                issues.push(VarIssue::Malformed);
                return;
            }
            VarScan::Invalid { end } => {
                issues.push(VarIssue::Malformed);
                rest = end;
            }
            VarScan::Call(call) => {
                if call.fallback.is_none() && !defined.contains(call.name) {
                    issues.push(VarIssue::Undefined(call.name.to_string()));
                }
                if let Some(fallback) = call.fallback {
                    collect_var_issues_into(fallback, defined, issues);
                }
                rest = call.end;
            }
        }
    }
}

/// Best-effort document scan: every `--name:` that looks like a declaration.
///
/// It is intentionally lexical (no selector matching): over-approximating the
/// defined set only avoids false diagnostics. Comments/strings may add names,
/// which is harmless for that purpose.
pub(crate) fn collect_custom_property_definitions(text: &str, out: &mut HashSet<String>) {
    let bytes = text.as_bytes();
    let mut index = 0usize;
    while index + 1 < bytes.len() {
        if bytes[index] != b'-' || bytes[index + 1] != b'-' {
            index += 1;
            continue;
        }
        let previous_is_ident = index > 0 && is_ident_byte(bytes[index - 1]);
        if previous_is_ident {
            index += 1;
            continue;
        }
        let start = index;
        let mut end = index + 2;
        while end < bytes.len() && is_ident_byte(bytes[end]) {
            end += 1;
        }
        if end > start + 2 {
            let mut colon = end;
            while colon < bytes.len() && bytes[colon].is_ascii_whitespace() {
                colon += 1;
            }
            if colon < bytes.len() && bytes[colon] == b':' {
                out.insert(text[start..end].to_string());
            }
        }
        index = end;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn props(entries: &[(&str, &str)]) -> HashMap<String, String> {
        entries
            .iter()
            .map(|(name, value)| (name.to_string(), value.to_string()))
            .collect()
    }

    #[test]
    fn custom_property_names_are_case_sensitive_and_require_a_name() {
        assert!(is_custom_property("--ink"));
        assert!(is_custom_property("--Ink"));
        assert!(!is_custom_property("--"));
        assert!(!is_custom_property("--bad name"));
        assert!(!is_custom_property("color"));
    }

    #[test]
    fn var_uses_the_token_then_the_fallback() {
        let values = props(&[("--ink", "#123456")]);
        assert_eq!(
            substitute_vars("var(--ink)", &values).as_deref(),
            Some("#123456")
        );
        assert_eq!(
            substitute_vars("var(--missing, 4px)", &values).as_deref(),
            Some("4px")
        );
        assert_eq!(substitute_vars("var(--missing)", &values), None);
    }

    #[test]
    fn var_ignores_the_tail_of_a_longer_function_name() {
        let values = props(&[]);
        assert_eq!(
            substitute_vars("myvar(--x)", &values).as_deref(),
            Some("myvar(--x)")
        );
        assert!(!has_var_function("myvar(--x)"));
    }

    #[test]
    fn nested_fallback_and_cycles() {
        let raw = props(&[
            ("--a", "var(--b)"),
            ("--b", "var(--c)"),
            ("--c", "2px"),
            ("--loop-a", "var(--loop-b)"),
            ("--loop-b", "var(--loop-a)"),
        ]);
        let resolved = resolve_custom_properties(&raw);
        assert_eq!(resolved.get("--a").map(String::as_str), Some("2px"));
        assert!(!resolved.contains_key("--loop-a"));
        assert!(!resolved.contains_key("--loop-b"));

        assert_eq!(
            substitute_vars("var(--loop-a, 9px)", &resolved).as_deref(),
            Some("9px")
        );
    }

    #[test]
    fn malformed_var_is_never_substituted() {
        let values = props(&[("--ink", "red")]);
        for value in ["var()", "var(ink)", "var(--ink", "var(--ink, red"] {
            assert_eq!(substitute_vars(value, &values), None, "{value}");
        }
        assert!(has_var_function("var(ink)"));
    }

    #[test]
    fn undefined_token_without_fallback_is_reported() {
        let mut defined = HashSet::new();
        defined.insert("--ink".to_string());
        assert!(collect_var_issues("var(--ink)", &defined).is_empty());
        assert!(collect_var_issues("var(--missing, red)", &defined).is_empty());
        assert_eq!(
            collect_var_issues("var(--missing)", &defined),
            vec![VarIssue::Undefined("--missing".to_string())]
        );
        assert_eq!(
            collect_var_issues("var(bad)", &defined),
            vec![VarIssue::Malformed]
        );
    }

    #[test]
    fn definitions_are_collected_lexically() {
        let mut defined = HashSet::new();
        collect_custom_property_definitions(
            ":root { --ink: #000; --pad : 4px; } .x { color: --not-a-def; }",
            &mut defined,
        );
        assert!(defined.contains("--ink"));
        assert!(defined.contains("--pad"));
        assert!(!defined.contains("--not-a-def"));
    }
}
