//! CLI: HTML+CSS -> canonical `.wnui` blob for the runtime.
//!
//! Usage:
//!   frontend-rust.exe --html <path> --width <W> --height <H> --emit-blob <app.wnui>
//!                     [--css <path>] [--strict-subset] [--pilot]
//!
//! `--strict-subset` validates the `docs/subset-v1.md` contract and exits
//! non-zero on any feature outside the subset. `--pilot` allows the v2/v3
//! extensions (grid, scroll, images, object-fit, input). `--css` is accepted
//! for compatibility; stylesheets are discovered from the document itself.
//!
//! The pipeline lives in the library crate (`frontend_rust::build_tree`), so
//! tests can inspect the pre-layout tree without serializing it.

use std::path::PathBuf;
use std::process::ExitCode;

use frontend_rust::{build_tree_from_file, check_document_pilot, print_diagnostics};

struct Args {
    html: PathBuf,
    width: u32,
    height: u32,
    /// Canonical `.wnui` v2 document output.
    emit_blob: PathBuf,
    /// Report subset-v1 diagnostics as errors (exit != 0).
    strict_subset: bool,
    /// Allow the v2 pilot extensions (grid, scroll, images).
    pilot: bool,
}

fn usage() -> &'static str {
    "uso: frontend-rust --html <path> --width <W> --height <H> --emit-blob <app.wnui> [--css <path>] [--strict-subset] [--pilot]"
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
            "--css" => {
                // Accepted for compatibility; the document's `<link>`/`<style>`
                // is the stylesheet source.
                take_value(&argv, &mut index, &inline_value, "--css")?;
            }
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
        emit_blob: emit_blob.ok_or_else(|| format!("falta --emit-blob <app.wnui>\n{}", usage()))?,
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

    let html = std::fs::read_to_string(&args.html)
        .map_err(|err| format!("{}: {err}", args.html.display()))?;
    let base_dir = args
        .html
        .parent()
        .map(std::path::Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));

    // `subset v1`: inspects the input HTML/CSS only. It does not affect the
    // cascade or emission; without `--strict-subset` it only prints warnings.
    let diagnostics = check_document_pilot(&html, &args.html, &base_dir, args.pilot);
    print_diagnostics(&diagnostics, args.strict_subset);
    let subset_ok = !args.strict_subset || diagnostics.is_empty();

    let tree = build_tree_from_file(&args.html, (args.width, args.height))?;
    tree.write_blob(&args.emit_blob)?;
    println!(
        "emit-blob: {} nodes, {} runs -> {}",
        tree.nodes.len(),
        tree.text_run_count,
        args.emit_blob.display(),
    );
    Ok(subset_ok)
}
