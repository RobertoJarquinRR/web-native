//! CLI de paridad: HTML+CSS -> display-list IR JSON (rect + text_run).
//!
//! Uso:
//!   frontend-rust.exe --html <path> --width <W> --height <H> --out <ir.json>
//!                     [--timings <timings.json>] [--atlas <atlas.bin>]
//!
//! Mismas etapas y semantica que `src/frontend/python` (incluido texto).

mod cascade;
mod color;
mod css;
mod dom;
mod emit;
mod layout;
mod rss;
mod sha256;
mod text;

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Instant;

struct Args {
    html: PathBuf,
    width: u32,
    height: u32,
    out: PathBuf,
    timings: Option<PathBuf>,
    atlas: Option<PathBuf>,
}

fn usage() -> &'static str {
    "uso: frontend-rust --html <path> --width <W> --height <H> --out <ir.json> [--timings <timings.json>] [--atlas <atlas.bin>]"
}

fn parse_args() -> Result<Args, String> {
    let mut html: Option<PathBuf> = None;
    let mut width: Option<u32> = None;
    let mut height: Option<u32> = None;
    let mut out: Option<PathBuf> = None;
    let mut timings: Option<PathBuf> = None;
    let mut atlas: Option<PathBuf> = None;

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let (flag, inline_value) = match arg.split_once('=') {
            Some((flag, value)) => (flag.to_string(), Some(value.to_string())),
            None => (arg.clone(), None),
        };
        let mut take = |name: &str| -> Result<String, String> {
            match &inline_value {
                Some(value) => Ok(value.clone()),
                None => args
                    .next()
                    .ok_or_else(|| format!("falta el valor de {name}")),
            }
        };
        match flag.as_str() {
            "--html" => html = Some(PathBuf::from(take("--html")?)),
            "--width" => {
                width = Some(
                    take("--width")?
                        .parse::<u32>()
                        .map_err(|_| "--width debe ser un entero".to_string())?,
                )
            }
            "--height" => {
                height = Some(
                    take("--height")?
                        .parse::<u32>()
                        .map_err(|_| "--height debe ser un entero".to_string())?,
                )
            }
            "--out" => out = Some(PathBuf::from(take("--out")?)),
            "--timings" => timings = Some(PathBuf::from(take("--timings")?)),
            "--atlas" => atlas = Some(PathBuf::from(take("--atlas")?)),
            "--help" | "-h" => return Err(usage().to_string()),
            other => return Err(format!("argumento desconocido: {other}\n{}", usage())),
        }
    }

    Ok(Args {
        html: html.ok_or_else(|| format!("falta --html\n{}", usage()))?,
        width: width.ok_or_else(|| format!("falta --width\n{}", usage()))?,
        height: height.ok_or_else(|| format!("falta --height\n{}", usage()))?,
        out: out.ok_or_else(|| format!("falta --out\n{}", usage()))?,
        timings,
        atlas,
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
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("ERROR: {message}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
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

    let since = Instant::now();
    let styles = cascade::compute_styles(&document, &rules);
    let cascade_ms = ms(since);

    let since = Instant::now();
    let engine = text::TextEngine::new();
    let metrics = |family: &str, size: f64| engine.metrics(family, size);
    let fonts_ms = ms(since);

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
    write_file(&args.out, &ir)?;
    if let Some(atlas) = &atlas {
        let atlas_path = args.atlas.clone().unwrap_or_else(|| {
            args.out
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
    Ok(())
}

fn round3(value: f64) -> f64 {
    (value * 1000.0).round() / 1000.0
}
