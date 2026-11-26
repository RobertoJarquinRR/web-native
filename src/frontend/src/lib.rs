//! Testable library surface for the HTML+CSS frontend.
//!
//! [`build_tree`] runs the whole pipeline (HTML -> DOM -> CSS cascade ->
//! pre-layout [`Tree`]) **in memory**. [`build_tree_from_file`] is the same
//! with the HTML file on disk, resolving relative stylesheet links against its
//! directory. `main.rs` only parses arguments, calls the library and writes the
//! canonical `.wnui` blob; integration tests inspect the tree without
//! spawning a process or serializing anything.

mod cli;
mod emit;
mod parse;
mod style;

use std::fs;
use std::path::{Path, PathBuf};

use parse::dom::Dom;

pub use cli::subset::{check_document_pilot, print_diagnostics, Diagnostic};
pub use emit::ui_tree::{
    ArgbOut, DataOut, ImageOut, InputOut, NodeOut, StyleOut, TextOut, Tree, KIND_BLOCK, KIND_FLEX,
    KIND_GRID, KIND_IMAGE, KIND_INPUT, KIND_TEXT,
};
pub use style::grid::{GridAutoRows, GridTemplateColumns};
pub use style::style_ext::{BorderOut, BorderSide, BoxShadow, RelLength, StyleV3, Unit};

/// Parses `html` and its inline/`<link>` stylesheets, runs the cascade and
/// returns the pre-layout tree. `base_dir` resolves relative stylesheet links.
pub fn build_tree(
    html: &str,
    base_dir: &Path,
    viewport: (u32, u32),
) -> Result<Tree, String> {
    let document = parse::dom::parse_html(html);
    build_tree_from_dom(&document, base_dir, viewport)
}

/// Same as [`build_tree`], reading the HTML from `html_path`. Relative
/// stylesheet links resolve against the file's directory.
pub fn build_tree_from_file(html_path: &Path, viewport: (u32, u32)) -> Result<Tree, String> {
    let html =
        fs::read_to_string(html_path).map_err(|err| format!("{}: {err}", html_path.display()))?;
    let base_dir = html_path
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));
    build_tree(&html, &base_dir, viewport)
}

fn build_tree_from_dom(
    document: &Dom,
    base_dir: &Path,
    viewport: (u32, u32),
) -> Result<Tree, String> {
    let stylesheets = parse::dom::stylesheet_texts(document, base_dir)?;
    let rules = parse::css::parse_stylesheets(&stylesheets);
    let styles = style::cascade::compute_styles(
        document,
        &rules,
        (f64::from(viewport.0), f64::from(viewport.1)),
    );
    emit::ui_tree::emit_tree(document, &styles, viewport, rules.len())
}
