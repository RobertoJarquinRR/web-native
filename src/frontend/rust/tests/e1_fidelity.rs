//! E1 fidelity tests: `font-weight` real variants, `text-align`,
//! `line-height` and `margin: auto` in the pre-layout tree.
//!
//! They run the real `frontend-rust --emit-ui` binary on temporary fixtures and
//! read the emitted `ui.json`; the C runtime invariants live in
//! `src/runtime/c-r2/tests/fidelity`.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

fn exe() -> &'static str {
    env!("CARGO_BIN_EXE_frontend-rust")
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn run(dir: &Path, html: &str, extra: &[&str]) -> Output {
    let mut command = Command::new(exe());
    command
        .arg("--html")
        .arg(dir.join(html))
        .arg("--width")
        .arg("480")
        .arg("--height")
        .arg("320");
    for arg in extra {
        command.arg(arg);
    }
    command.current_dir(dir).output().unwrap()
}

fn stderr(output: &Output) -> String {
    String::from_utf8_lossy(&output.stderr).to_string()
}

fn emit_ui(dir: &Path, html: &str, out: &str, extra: &[&str]) -> String {
    let mut args = vec!["--emit-ui", out];
    args.extend_from_slice(extra);
    let output = run(dir, html, &args);
    assert!(
        output.status.success(),
        "emit-ui failed: exit {:?}\n{}",
        output.status.code(),
        stderr(&output)
    );
    fs::read_to_string(dir.join(out)).unwrap()
}

/// Numeric CSS field of the node whose `"name"` matches exactly.
fn field_number(json: &str, node_name: &str, key: &str) -> f64 {
    let marker = format!("\"name\": \"{node_name}\"");
    let start = json
        .find(&marker)
        .unwrap_or_else(|| panic!("node not found: {node_name}"));
    let rest = &json[start..];
    let key_marker = format!("\"{key}\": ");
    let key_at = rest
        .find(&key_marker)
        .unwrap_or_else(|| panic!("{key} not found for {node_name}"));
    let value = &rest[key_at + key_marker.len()..];
    let end = value
        .find(|c: char| c == ',' || c == '\n' || c == '}')
        .unwrap_or(value.len());
    value[..end].trim().parse::<f64>().unwrap()
}

fn field_string<'a>(json: &'a str, node_name: &str, key: &str) -> &'a str {
    let marker = format!("\"name\": \"{node_name}\"");
    let start = json
        .find(&marker)
        .unwrap_or_else(|| panic!("node not found: {node_name}"));
    let rest = &json[start..];
    let key_marker = format!("\"{key}\": \"");
    let key_at = rest
        .find(&key_marker)
        .unwrap_or_else(|| panic!("{key} not found for {node_name}"));
    let value = &rest[key_at + key_marker.len()..];
    &value[..value.find('"').unwrap()]
}

fn field_raw<'a>(json: &'a str, node_name: &str, key: &str) -> &'a str {
    let marker = format!("\"name\": \"{node_name}\"");
    let start = json
        .find(&marker)
        .unwrap_or_else(|| panic!("node not found: {node_name}"));
    let rest = &json[start..];
    let key_marker = format!("\"{key}\": ");
    let key_at = rest
        .find(&key_marker)
        .unwrap_or_else(|| panic!("{key} not found for {node_name}"));
    let slice = &rest[key_at + key_marker.len()..];
    let end = slice.find(']').map(|index| index + 1).unwrap_or(slice.len());
    &slice[..end]
}

const BOLD_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .row { display: block; font-size: 18px; }
  .bold { font-weight: 700; }
</style></head>
<body>
  <div id="regular" class="row">Media Bold</div>
  <div id="bold" class="row bold">Media Bold</div>
</body></html>
"##;

#[test]
fn font_weight_700_selects_a_real_bold_variant() {
    let dir = temp_dir("e1-bold");
    fs::write(dir.join("page.html"), BOLD_PAGE).unwrap();
    let json = emit_ui(&dir, "page.html", "ui.json", &[]);

    // La pagina no usa features v2: sigue siendo /1 (sin campos extra).
    assert!(json.contains("\"schema\": \"webnative.ui-tree/1\""), "{json}");
    let regular = field_number(&json, "body.root/div#regular.row/text", "width_px");
    let bold = field_number(&json, "body.root/div#bold.row/text[1]", "width_px");
    assert!(
        bold >= regular + 2.0,
        "bold must use a wider real variant: regular={regular}, bold={bold}"
    );
    let regular_h = field_number(&json, "body.root/div#regular.row/text", "height_px");
    let bold_h = field_number(&json, "body.root/div#bold.row/text[1]", "height_px");
    assert!(
        (regular_h - bold_h).abs() < 0.01,
        "bold must keep the regular line box: {regular_h} vs {bold_h}"
    );
    assert_eq!(
        field_string(&json, "body.root/div#bold.row/text[1]", "font_weight"),
        "700"
    );
}

const ALIGN_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .row { display: block; width: 300px; font-size: 16px; }
  .center { text-align: center; }
  .right { text-align: right; }
</style></head>
<body>
  <div id="left" class="row">Aligned</div>
  <div id="center" class="row center">Aligned</div>
  <div id="right" class="row right">Aligned</div>
</body></html>
"##;

#[test]
fn text_align_is_cascaded_to_the_text_run() {
    let dir = temp_dir("e1-align");
    fs::write(dir.join("page.html"), ALIGN_PAGE).unwrap();
    let json = emit_ui(&dir, "page.html", "ui.json", &[]);

    assert_eq!(
        field_string(&json, "body.root/div#left.row", "text_align"),
        "left"
    );
    assert_eq!(
        field_string(&json, "body.root/div#center.row", "text_align"),
        "center"
    );
    assert_eq!(
        field_string(&json, "body.root/div#right.row/text[2]", "text_align"),
        "right"
    );
    // El nodo de texto hereda la alineacion del elemento.
    assert_eq!(
        field_string(&json, "body.root/div#center.row/text[1]", "text_align"),
        "center"
    );
}

#[test]
fn text_align_does_not_add_fields_to_unrelated_pages() {
    let dir = temp_dir("e1-align-off");
    let html = r##"<!doctype html><html><head><style>
      body { display: block; }
      .row { display: block; font-size: 16px; }
    </style></head><body><div id="a" class="row">Plain</div></body></html>"##;
    fs::write(dir.join("page.html"), html).unwrap();
    let json = emit_ui(&dir, "page.html", "ui.json", &[]);
    assert!(!json.contains("\"text_align\""), "no text_align fields");
    assert!(!json.contains("\"line_height\""), "no line_height fields");
    assert!(!json.contains("\"margin_auto\""), "no margin_auto fields");
}

const LINE_HEIGHT_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .row { display: block; width: 300px; font-size: 16px; }
  .tall { line-height: 40px; }
</style></head>
<body>
  <div id="normal" class="row">Line</div>
  <div id="tall" class="row tall">Line</div>
</body></html>
"##;

#[test]
fn line_height_sets_the_text_box_and_the_baseline() {
    let dir = temp_dir("e1-line-height");
    fs::write(dir.join("page.html"), LINE_HEIGHT_PAGE).unwrap();
    let json = emit_ui(&dir, "page.html", "ui.json", &[]);

    let normal = field_number(&json, "body.root/div#normal.row/text", "height_px");
    let tall = field_number(&json, "body.root/div#tall.row/text[1]", "height_px");
    assert!(normal < 30.0, "natural line box: {normal}");
    assert!((tall - 40.0).abs() < 0.01, "line-height text box: {tall}");
    assert_eq!(
        field_number(&json, "body.root/div#tall.row/text[1]", "line_height"),
        40.0
    );
    // `baseline` vive en el objeto text{}: half-leading por encima del ascender.
    let baseline = field_number(&json, "body.root/div#tall.row/text[1]", "baseline");
    assert!(baseline > 0.0 && baseline < 40.0, "baseline: {baseline}");
    assert!(
        baseline > 20.0,
        "half-leading pushes the baseline down: {baseline}"
    );
}

const AUTO_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .pin { display: flex; flex-direction: column; width: 200px; height: 120px; }
  .head { height: 20px; }
  .foot { height: 20px; margin-top: auto; }
  .box { display: block; width: 300px; }
  .mid { width: 100px; height: 20px; margin: 0 auto; }
</style></head>
<body>
  <div id="pin" class="pin"><div id="head" class="head"></div><div id="foot" class="foot"></div></div>
  <div id="box" class="box"><div id="mid" class="mid"></div></div>
</body></html>
"##;

#[test]
fn margin_auto_is_parsed_and_emitted_per_side() {
    let dir = temp_dir("e1-margin-auto");
    fs::write(dir.join("page.html"), AUTO_PAGE).unwrap();
    let json = emit_ui(&dir, "page.html", "ui.json", &[]);

    assert_eq!(
        field_raw(&json, "body.root/div#pin.pin/div#foot.foot", "margin_auto"),
        "[1, 0, 0, 0]".to_string()
    );
    assert_eq!(
        field_raw(&json, "body.root/div#box.box/div#mid.mid", "margin_auto"),
        "[0, 1, 0, 1]".to_string()
    );
    // `margin: 0 auto` y `margin-top: auto` ya no son errores de subset v1:
    // con --strict-subset la pagina sigue emitiendo.
    let strict = run(&dir, "page.html", &["--emit-ui", "ui.json", "--strict-subset"]);
    assert!(
        strict.status.success(),
        "--strict-subset rejected margin:auto:\n{}",
        stderr(&strict)
    );
}
