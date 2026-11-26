//! CLI: HTML+CSS -> pre-layout style tree (`ui.json`) for the runtime.
//!
//! Usage:
//!   frontend-rust.exe --html <path> --width <W> --height <H> --emit-ui <ui.json>
//!                     [--emit-header <ui_data.h>] [--emit-blob <app.wnui>]
//!                     [--css <path>] [--strict-subset] [--pilot]
//!
//! `--strict-subset` validates the `docs/subset-v1.md` contract and exits
//! non-zero on any feature outside the subset. `--pilot` allows the v2/v3
//! extensions (grid, scroll, images, object-fit, input).

mod cli;
mod emit;
mod parse;
mod style;

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use crate::cli::subset;
use crate::emit::ui_tree;
use crate::parse::{css, dom};
use crate::style::cascade;

struct Args {
    html: PathBuf,
    width: u32,
    height: u32,
    css: Option<PathBuf>,
    /// Pre-layout style tree consumed by the runtime.
    emit_ui: PathBuf,
    /// Optional `ui_data.h`, byte-identical to the Python emitter.
    emit_header: Option<PathBuf>,
    /// Optional `.wnui` v2 document output.
    emit_blob: Option<PathBuf>,
    /// Report subset-v1 diagnostics as errors (exit != 0).
    strict_subset: bool,
    /// Allow the v2 pilot extensions (grid, scroll, images).
    pilot: bool,
}

fn usage() -> &'static str {
    "uso: frontend-rust --html <path> --width <W> --height <H> --emit-ui <ui.json> [--emit-header <ui_data.h>] [--emit-blob <app.wnui>] [--css <path>] [--strict-subset] [--pilot]"
}

fn take_value(
    argv: &[String],
    index: &mut usize,
    inline: &Option<String>,
    name: &str,
) -> Result<String, String> {
    if let Some(value) = inline {
        return Ok(value.clone());
    }
    *index += 1;
    argv.get(*index)
        .cloned()
        .ok_or_else(|| format!("falta el valor de {name}"))
}

fn parse_args() -> Result<Args, String> {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut html: Option<PathBuf> = None;
    let mut width: Option<u32> = None;
    let mut height: Option<u32> = None;
    let mut css: Option<PathBuf> = None;
    let mut emit_ui: Option<PathBuf> = None;
    let mut emit_header: Option<PathBuf> = None;
    let mut emit_blob: Option<PathBuf> = None;
    let mut strict_subset = false;
    let mut pilot = false;

    let mut index = 0usize;
    while index < argv.len() {
        let arg = &argv[index];
        let (flag, inline_value) = match arg.split_once('=') {
            Some((flag, value)) => (flag.to_string(), Some(value.to_string())),
            None => (arg.clone(), None),
        };
        match flag.as_str() {
            "--html" => html = Some(PathBuf::from(take_value(&argv, &mut index, &inline_value, "--html")?)),
            "--css" => css = Some(PathBuf::from(take_value(&argv, &mut index, &inline_value, "--css")?)),
            "--width" => {
                width = Some(
                    take_value(&argv, &mut index, &inline_value, "--width")?
                        .parse::<u32>()
                        .map_err(|_| "--width debe ser un entero".to_string())?,
                )
            }
            "--height" => {
                height = Some(
                    take_value(&argv, &mut index, &inline_value, "--height")?
                        .parse::<u32>()
                        .map_err(|_| "--height debe ser un entero".to_string())?,
                )
            }
            "--emit-ui" => {
                emit_ui = Some(PathBuf::from(take_value(
                    &argv,
                    &mut index,
                    &inline_value,
                    "--emit-ui",
                )?))
            }
            "--emit-header" => {
                emit_header = Some(PathBuf::from(take_value(
                    &argv,
                    &mut index,
                    &inline_value,
                    "--emit-header",
                )?))
            }
            "--emit-blob" => {
                emit_blob = Some(PathBuf::from(take_value(
                    &argv,
                    &mut index,
                    &inline_value,
                    "--emit-blob",
                )?))
            }
            "--strict-subset" => strict_subset = true,
            "--pilot" => pilot = true,
            "--help" | "-h" => return Err(usage().to_string()),
            other => return Err(format!("argumento desconocido: {other}\n{}", usage())),
        }
        index += 1;
    }

    Ok(Args {
        html: html.ok_or_else(|| format!("falta --html\n{}", usage()))?,
        width: width.ok_or_else(|| format!("falta --width\n{}", usage()))?,
        height: height.ok_or_else(|| format!("falta --height\n{}", usage()))?,
        css,
        emit_ui: emit_ui.ok_or_else(|| format!("falta --emit-ui <ui.json>\n{}", usage()))?,
        emit_header,
        emit_blob,
        strict_subset,
        pilot,
    })
}

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(message) => {
            eprintln!("ERROR: {message}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<bool, String> {
    let args = parse_args()?;

    let html = fs::read_to_string(&args.html)
        .map_err(|err| format!("{}: {err}", args.html.display()))?;
    let document = dom::parse_html(&html);
    let base_dir = args
        .html
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));
    let stylesheets = dom::stylesheet_texts(&document, &base_dir)?;
    let rules = css::parse_stylesheets(&stylesheets);

    // `subset v1`: inspects the input HTML/CSS only. It does not affect the
    // cascade or emission; without `--strict-subset` it only prints warnings.
    let diagnostics = subset::check_document_pilot(&html, &args.html, &base_dir, args.pilot);
    subset::print_diagnostics(&diagnostics, args.strict_subset);
    let subset_ok = !args.strict_subset || diagnostics.is_empty();

    let styles = cascade::compute_styles(
        &document,
        &rules,
        (f64::from(args.width), f64::from(args.height)),
    );

    let source_html = args.html.to_string_lossy().to_string();
    let source_css = args.css.as_ref().map(|path| path.to_string_lossy().to_string());
    let summary = ui_tree::emit_ui_tree(
        &document,
        &styles,
        (args.width, args.height),
        &source_html,
        source_css.as_deref(),
        Path::new(&args.emit_ui),
        args.emit_header.as_deref(),
        args.emit_blob.as_deref(),
        rules.len(),
    )?;
    println!(
        "emit-ui: {} nodes, {} runs -> {}",
        summary.nodes,
        summary.runs,
        args.emit_ui.display(),
    );
    if let Some(header) = &args.emit_header {
        println!("emit-header: -> {}", header.display());
    }
    if let Some(blob) = &args.emit_blob {
        println!("emit-blob: -> {}", blob.display());
    }
    Ok(subset_ok)
}
