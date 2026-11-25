//! CLI de paridad: HTML+CSS -> display-list IR JSON (rect + text_run).
//!
//! Uso:
//!   frontend-rust.exe --html <path> --width <W> --height <H> --out <ir.json>
//!                     [--timings <timings.json>] [--atlas <atlas.bin>]
//!                     [--strict-subset] [--pilot]
//!
//! Mismas etapas y semantica que `src/frontend/python` (incluido texto).
//! `--strict-subset` valida el contrato `docs/subset-v1.md` y devuelve exit
//! != 0 si encuentra cualquier feature fuera del subset. `--pilot` permite las
//! extensiones v2 del piloto de biblioteca (grid, scroll, object-fit).

mod cascade;
mod color;
mod css;
mod dom;
mod emit;
mod grid;
mod layout;
mod r1;
mod rss;
mod sha256;
mod style_ext;
mod subset;
mod text;
mod ui_tree;

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Instant;

struct Args {
    html: PathBuf,
    width: u32,
    height: u32,
    out: Option<PathBuf>,
    timings: Option<PathBuf>,
    atlas: Option<PathBuf>,
    css: Option<PathBuf>,
    /// `--emit-ui [path]`: escribe el arbol de estilos pre-layout para R2.
    emit_ui: Option<PathBuf>,
    /// `--strict-subset`: diagnosticos de subset v1 como errores (exit != 0).
    strict_subset: bool,
    /// `--pilot`: permite las extensiones del piloto v2 (grid, scroll, imagenes).
    pilot: bool,
}

fn usage() -> &'static str {
    "uso: frontend-rust --html <path> --width <W> --height <H> --out <ir.json> [--timings <timings.json>] [--atlas <atlas.bin>] [--strict-subset] [--pilot]\n     frontend-rust --html <path> --width <W> --height <H> --emit-ui [ui.json] [--css <path>] [--timings <timings.json>] [--strict-subset] [--pilot]"
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
    let mut out: Option<PathBuf> = None;
    let mut timings: Option<PathBuf> = None;
    let mut atlas: Option<PathBuf> = None;
    let mut css: Option<PathBuf> = None;
    let mut emit_ui: Option<PathBuf> = None;
    let mut emit_ui_requested = false;
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
            "--out" => out = Some(PathBuf::from(take_value(&argv, &mut index, &inline_value, "--out")?)),
            "--timings" => {
                timings = Some(PathBuf::from(take_value(&argv, &mut index, &inline_value, "--timings")?))
            }
            "--atlas" => {
                atlas = Some(PathBuf::from(take_value(&argv, &mut index, &inline_value, "--atlas")?))
            }
            "--emit-ui" => {
                emit_ui_requested = true;
                if let Some(value) = inline_value.clone() {
                    emit_ui = Some(PathBuf::from(value));
                } else if let Some(next) = argv.get(index + 1) {
                    if !next.starts_with("--") {
                        emit_ui = Some(PathBuf::from(next.clone()));
                        index += 1;
                    }
                }
            }
            "--strict-subset" => strict_subset = true,
            "--pilot" => pilot = true,
            "--help" | "-h" => return Err(usage().to_string()),
            other => return Err(format!("argumento desconocido: {other}\n{}", usage())),
        }
        index += 1;
    }

    if emit_ui_requested && emit_ui.is_none() {
        emit_ui = Some(out.clone().unwrap_or_else(|| PathBuf::from("ui.json")));
    }
    if emit_ui.is_none() && out.is_none() {
        return Err(format!("falta --out\n{}", usage()));
    }

    Ok(Args {
        html: html.ok_or_else(|| format!("falta --html\n{}", usage()))?,
        width: width.ok_or_else(|| format!("falta --width\n{}", usage()))?,
        height: height.ok_or_else(|| format!("falta --height\n{}", usage()))?,
        out,
        timings,
        atlas,
        css,
        emit_ui,
        strict_subset,
        pilot,
    })
}

fn ms(since: Instant) -> f64 {
    since.elapsed().as_secs_f64() * 1000.0
}

fn ensure_parent(path: &Path) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|err| format!("{}: {err}", parent.display()))?;
        }
    }
    Ok(())
}

fn write_file(path: &Path, contents: &str) -> Result<(), String> {
    ensure_parent(path)?;
    // `write_text` de Python en Windows traduce "\n" a "\r\n"; replicarlo deja
    // los artefactos byte a byte iguales a los del frontend de referencia.
    let payload = if cfg!(windows) {
        contents.replace('\n', "\r\n")
    } else {
        contents.to_string()
    };
    fs::write(path, payload).map_err(|err| format!("{}: {err}", path.display()))
}

fn write_bytes(path: &Path, contents: &[u8]) -> Result<(), String> {
    ensure_parent(path)?;
    fs::write(path, contents).map_err(|err| format!("{}: {err}", path.display()))
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
    let started = Instant::now();

    let since = Instant::now();
    let html = fs::read_to_string(&args.html)
        .map_err(|err| format!("{}: {err}", args.html.display()))?;
    let document = dom::parse_html(&html);
    let base_dir = args
        .html
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));
    let stylesheets = dom::stylesheet_texts(&document, &base_dir)?;
    let html_ms = ms(since);

    let since = Instant::now();
    let rules = css::parse_stylesheets(&stylesheets);
    let css_ms = ms(since);

    // `subset v1`: inspeccion del HTML/CSS de entrada. No toca la cascada, el
    // layout ni la emision; sin `--strict-subset` solo imprime warnings.
    // `--pilot` permite las extensiones v2 (grid, scroll, object-fit).
    let diagnostics = subset::check_document_pilot(&html, &args.html, &base_dir, args.pilot);
    subset::print_diagnostics(&diagnostics, args.strict_subset);
    let subset_ok = !args.strict_subset || diagnostics.is_empty();

    let since = Instant::now();
    let styles = cascade::compute_styles(
        &document,
        &rules,
        (f64::from(args.width), f64::from(args.height)),
    );
    let cascade_ms = ms(since);

    // R1 (IR post-layout): grid/scroll/imagenes no se implementan. Avisar y
    // seguir sin tocarlas: el layout actual las ignora, no inventa rects.
    if args.emit_ui.is_none() {
        let warnings = r1::collect(&document, &styles);
        r1::print(&warnings);
    }

    let since = Instant::now();
    let engine = text::TextEngine::new();
    let metrics = |family: &str, size: f64| engine.metrics(family, size);
    let fonts_ms = ms(since);

    if let Some(ui_path) = &args.emit_ui {
        let since = Instant::now();
        let source_html = args.html.to_string_lossy().to_string();
        let source_css = args.css.as_ref().map(|path| path.to_string_lossy().to_string());
        let summary = ui_tree::emit_ui_tree(
            &document,
            &styles,
            (args.width, args.height),
            &source_html,
            source_css.as_deref(),
            Path::new(ui_path),
            rules.len(),
        )?;
        let text_ms = ms(since);
        let atlas = summary
            .atlas
            .map(|(width, height)| format!(", atlas {width}x{height}"))
            .unwrap_or_default();
        println!(
            "emit-ui: {} nodes, {} glyphs, {} runs{} -> {}{}",
            summary.nodes,
            summary.glyphs,
            summary.runs,
            atlas,
            ui_path.display(),
            summary
                .atlas_path
                .as_ref()
                .map(|path| format!(", {}", path.display()))
                .unwrap_or_default(),
        );
        if let Some(path) = &args.timings {
            let timings = emit::Timings {
                html: round3(html_ms),
                css: round3(css_ms),
                cascade: round3(cascade_ms),
                fonts: round3(fonts_ms),
                layout: 0.0,
                text: round3(text_ms),
                emit: 0.0,
                pipeline: round3(ms(started)),
                ops: summary.nodes,
                peak_rss_kb: rss::peak_rss_kb(),
            };
            write_file(path, &emit::timings_json(&timings))?;
        }
        return Ok(subset_ok);
    }

    let since = Instant::now();
    let layout_root = layout::layout_tree(&document, &styles, f64::from(args.width), &metrics)?;
    let mut paint_ops = Vec::new();
    if let Some(root) = &layout_root {
        layout::paint(root, &mut paint_ops);
    }
    let layout_ms = ms(since);

    let since = Instant::now();
    let runs: Vec<layout::TextRun> = paint_ops
        .iter()
        .filter_map(|op| match op {
            layout::PaintOp::Run(run) => Some(run.clone()),
            layout::PaintOp::Rect(_) => None,
        })
        .collect();
    let (text_ops, atlas) = text::build_text_ops(&runs, &engine)?;
    let mut text_ops = text_ops.into_iter();
    let mut ops: Vec<emit::IrOp> = Vec::with_capacity(paint_ops.len());
    for op in paint_ops {
        match op {
            layout::PaintOp::Rect(rect) => ops.push(emit::IrOp::Rect(rect)),
            layout::PaintOp::Run(_) => match text_ops.next() {
                Some(text_op) => ops.push(emit::IrOp::Text(text_op)),
                None => return Err("run de texto sin op shapeada".to_string()),
            },
        }
    }
    let text_ms = ms(since);

    let since = Instant::now();
    let ir = emit::ir_json(args.width, args.height, &ops);
    let out_path = args
        .out
        .as_ref()
        .ok_or_else(|| "falta --out".to_string())?
        .clone();
    write_file(&out_path, &ir)?;
    if let Some(atlas) = &atlas {
        let atlas_path = args.atlas.clone().unwrap_or_else(|| {
            out_path
                .parent()
                .map(|parent| parent.join("atlas.bin"))
                .unwrap_or_else(|| PathBuf::from("atlas.bin"))
        });
        write_bytes(&atlas_path, &atlas.data)?;
        let atlas_json_path = atlas_path.with_extension("json");
        write_file(&atlas_json_path, &emit::atlas_json(atlas))?;
    }
    let emit_ms = ms(since);

    let timings = emit::Timings {
        html: round3(html_ms),
        css: round3(css_ms),
        cascade: round3(cascade_ms),
        fonts: round3(fonts_ms),
        layout: round3(layout_ms),
        text: round3(text_ms),
        emit: round3(emit_ms),
        pipeline: round3(ms(started)),
        ops: ops.len(),
        peak_rss_kb: rss::peak_rss_kb(),
    };

    if let Some(path) = &args.timings {
        write_file(path, &emit::timings_json(&timings))?;
    }
    println!("{}", emit::timings_inline(&timings));
    Ok(subset_ok)
}

fn round3(value: f64) -> f64 {
    (value * 1000.0).round() / 1000.0
}
