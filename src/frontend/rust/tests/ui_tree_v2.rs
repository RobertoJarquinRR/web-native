//! Tests de integracion del contrato `webnative.ui-tree/2`.
//!
//! Ejecutan el binario real (`--emit-ui` / `--out`) sobre fixtures temporales.
//! No usan el runtime C ni comparan contra el oraculo Python: solo verifican
//! que el frontend Rust parsea y emite los campos v2 acordados en
//! `docs/ui-tree-v2.md` y que el camino R1 avisa en vez de inventar rects.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

// --------------------------------------------------------------- infra

fn exe() -> &'static str {
    env!("CARGO_BIN_EXE_frontend-rust")
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn write(path: &Path, text: &str) {
    fs::write(path, text).unwrap();
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

fn emit_ui(dir: &Path, html: &str, out: &str, extra: &[&str]) -> Output {
    let mut args = vec!["--emit-ui", out];
    args.extend_from_slice(extra);
    run(dir, html, &args)
}

fn read_json(path: &Path) -> Json {
    let text = fs::read_to_string(path).unwrap_or_else(|err| panic!("{}: {err}", path.display()));
    let mut parser = JsonParser {
        bytes: text.as_bytes(),
        pos: 0,
    };
    let value = parser.value();
    parser.ws();
    assert_eq!(parser.pos, parser.bytes.len(), "JSON con bytes sobrantes");
    value
}

// --------------------------------------------------------------- JSON

#[derive(Debug, Clone, PartialEq)]
enum Json {
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(items) => items
                .iter()
                .find(|(name, _)| name == key)
                .map(|(_, value)| value),
            _ => None,
        }
    }

    fn as_str(&self) -> Option<&str> {
        match self {
            Json::String(value) => Some(value),
            _ => None,
        }
    }

    fn as_f64(&self) -> Option<f64> {
        match self {
            Json::Number(value) => Some(*value),
            _ => None,
        }
    }

    fn as_array(&self) -> Option<&[Json]> {
        match self {
            Json::Array(items) => Some(items),
            _ => None,
        }
    }
}

struct JsonParser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> JsonParser<'a> {
    fn ws(&mut self) {
        while self.pos < self.bytes.len()
            && matches!(self.bytes[self.pos], b' ' | b'\n' | b'\r' | b'\t')
        {
            self.pos += 1;
        }
    }

    fn expect(&mut self, byte: u8) {
        assert_eq!(self.bytes[self.pos], byte, "byte inesperado");
        self.pos += 1;
    }

    fn literal(&mut self, text: &str) {
        assert!(
            self.bytes[self.pos..].starts_with(text.as_bytes()),
            "literal {text}"
        );
        self.pos += text.len();
    }

    fn value(&mut self) -> Json {
        self.ws();
        match self.bytes[self.pos] {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => Json::String(self.string()),
            b't' => {
                self.literal("true");
                Json::Bool(true)
            }
            b'f' => {
                self.literal("false");
                Json::Bool(false)
            }
            b'n' => {
                self.literal("null");
                Json::Null
            }
            _ => self.number(),
        }
    }

    fn object(&mut self) -> Json {
        self.expect(b'{');
        let mut items = Vec::new();
        self.ws();
        if self.bytes[self.pos] == b'}' {
            self.pos += 1;
            return Json::Object(items);
        }
        loop {
            self.ws();
            let key = self.string();
            self.ws();
            self.expect(b':');
            let value = self.value();
            items.push((key, value));
            self.ws();
            match self.bytes[self.pos] {
                b',' => self.pos += 1,
                b'}' => {
                    self.pos += 1;
                    break;
                }
                other => panic!("objeto: byte {other}"),
            }
        }
        Json::Object(items)
    }

    fn array(&mut self) -> Json {
        self.expect(b'[');
        let mut items = Vec::new();
        self.ws();
        if self.bytes[self.pos] == b']' {
            self.pos += 1;
            return Json::Array(items);
        }
        loop {
            items.push(self.value());
            self.ws();
            match self.bytes[self.pos] {
                b',' => self.pos += 1,
                b']' => {
                    self.pos += 1;
                    break;
                }
                other => panic!("array: byte {other}"),
            }
        }
        Json::Array(items)
    }

    fn string(&mut self) -> String {
        self.expect(b'"');
        let mut out = Vec::new();
        loop {
            let byte = self.bytes[self.pos];
            self.pos += 1;
            match byte {
                b'"' => break,
                b'\\' => {
                    let escape = self.bytes[self.pos];
                    self.pos += 1;
                    match escape {
                        b'"' => out.push(b'"'),
                        b'\\' => out.push(b'\\'),
                        b'/' => out.push(b'/'),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0c),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'u' => {
                            let hex =
                                std::str::from_utf8(&self.bytes[self.pos..self.pos + 4]).unwrap();
                            self.pos += 4;
                            let code = u32::from_str_radix(hex, 16).unwrap();
                            let ch = char::from_u32(code).unwrap();
                            let mut buffer = [0u8; 4];
                            out.extend_from_slice(ch.encode_utf8(&mut buffer).as_bytes());
                        }
                        other => panic!("escape desconocido: {other}"),
                    }
                }
                other => out.push(other),
            }
        }
        String::from_utf8(out).unwrap()
    }

    fn number(&mut self) -> Json {
        let start = self.pos;
        while self.pos < self.bytes.len()
            && matches!(
                self.bytes[self.pos],
                b'0'..=b'9' | b'+' | b'-' | b'.' | b'e' | b'E'
            )
        {
            self.pos += 1;
        }
        let text = std::str::from_utf8(&self.bytes[start..self.pos]).unwrap();
        Json::Number(text.parse().unwrap())
    }
}

// ------------------------------------------------------------ helpers

fn nodes(doc: &Json) -> &[Json] {
    doc.get("nodes").and_then(Json::as_array).expect("nodes[]")
}

fn node_with_id<'a>(doc: &'a Json, element_id: &str) -> &'a Json {
    nodes(doc)
        .iter()
        .find(|node| {
            node.get("element_id")
                .and_then(Json::as_str)
                .is_some_and(|value| value == element_id)
        })
        .unwrap_or_else(|| panic!("no hay nodo #{element_id}"))
}

fn images(doc: &Json) -> Vec<&Json> {
    nodes(doc)
        .iter()
        .filter(|node| node.get("kind").and_then(Json::as_str) == Some("image"))
        .collect()
}

fn style_f64(node: &Json, key: &str) -> f64 {
    node.get("style")
        .and_then(|style| style.get(key))
        .and_then(Json::as_f64)
        .unwrap_or_else(|| panic!("style.{key}"))
}

fn style_str<'a>(node: &'a Json, key: &str) -> Option<&'a str> {
    node.get("style")
        .and_then(|style| style.get(key))
        .and_then(Json::as_str)
}

/// Array de 4 (o N) numeros del style; los `null` se conservan como `None`.
fn style_array_opt(node: &Json, key: &str) -> Vec<Option<f64>> {
    node.get("style")
        .and_then(|style| style.get(key))
        .and_then(Json::as_array)
        .unwrap_or_else(|| panic!("style.{key}"))
        .iter()
        .map(Json::as_f64)
        .collect()
}

fn style_value<'a>(node: &'a Json, key: &str) -> Option<&'a Json> {
    node.get("style").and_then(|style| style.get(key))
}

fn node_str<'a>(node: &'a Json, key: &str) -> Option<&'a str> {
    node.get(key).and_then(Json::as_str)
}

// --------------------------------------------------------------- tests

const GRID_PAGE: &str = r##"<!doctype html>
<html>
<head>
<style>
.gallery {
  display: grid;
  grid-template-columns: repeat(auto-fill, 180px);
  grid-auto-rows: 240px;
  gap: 8px;
  row-gap: 12px;
  column-gap: 6px;
  overflow: auto;
  width: 400px;
  height: 320px;
}
.poster { width: 180px; height: 240px; }
.auto-rows {
  display: grid;
  grid-template-columns: repeat(auto-fill, 100px);
  grid-auto-rows: auto;
  overflow: scroll;
}
.auto-size { width: auto; height: auto; }
</style>
</head>
<body>
  <div id="gallery" class="gallery">
    <img src="poster.png" alt="Demo" width="180" height="240" style="object-fit: cover">
    <img src="poster2.png" class="poster">
  </div>
  <div id="auto" class="auto-rows">
    <img src="poster3.png" width="100">
    <img src="poster4.png" class="auto-size" width="100" height="120">
  </div>
</body>
</html>
"##;

#[test]
fn grid_overflow_and_images_emit_schema_2() {
    let dir = temp_dir("grid-image-v2");
    write(&dir.join("page.html"), GRID_PAGE);

    let output = emit_ui(&dir, "page.html", "ui.json", &["--pilot"]);
    assert!(
        output.status.success(),
        "exit {:?}\n{}",
        output.status.code(),
        stderr(&output)
    );
    let doc = read_json(&dir.join("ui.json"));

    assert_eq!(
        doc.get("schema").and_then(Json::as_str),
        Some("webnative.ui-tree/2")
    );
    assert_eq!(nodes(&doc).len(), 7, "{doc:#?}");

    let gallery = node_with_id(&doc, "gallery");
    assert_eq!(node_str(gallery, "kind"), Some("grid"));
    assert_eq!(style_str(gallery, "display"), Some("grid"));
    assert_eq!(style_f64(gallery, "gap"), 8.0);
    assert_eq!(style_f64(gallery, "row_gap"), 12.0);
    assert_eq!(style_f64(gallery, "column_gap"), 6.0);
    assert_eq!(style_str(gallery, "overflow"), Some("auto"));
    let columns = gallery
        .get("style")
        .and_then(|style| style.get("grid_template_columns"))
        .expect("grid_template_columns");
    assert_eq!(
        columns.get("mode").and_then(Json::as_str),
        Some("auto_fill")
    );
    assert_eq!(columns.get("size_px").and_then(Json::as_f64), Some(180.0));
    let rows = gallery
        .get("style")
        .and_then(|style| style.get("grid_auto_rows"))
        .expect("grid_auto_rows");
    assert_eq!(rows.get("px").and_then(Json::as_f64), Some(240.0));

    let auto = node_with_id(&doc, "auto");
    assert_eq!(node_str(auto, "kind"), Some("grid"));
    assert_eq!(style_str(auto, "overflow"), Some("scroll"));
    assert_eq!(
        auto.get("style")
            .and_then(|style| style.get("grid_auto_rows")),
        Some(&Json::String("auto".to_string()))
    );

    // El body no usa features v2, pero el schema /2 incluye los campos.
    let body = &nodes(&doc)[0];
    assert_eq!(node_str(body, "kind"), Some("block"));
    assert_eq!(
        body.get("style")
            .and_then(|style| style.get("grid_template_columns")),
        Some(&Json::Null)
    );
    assert_eq!(
        body.get("style").and_then(|style| style.get("overflow")),
        Some(&Json::Null)
    );

    let images = images(&doc);
    assert_eq!(images.len(), 4, "{doc:#?}");

    let poster = images[0];
    assert_eq!(node_str(poster, "tag"), Some("img"));
    assert_eq!(style_f64(poster, "width_px"), 180.0);
    assert_eq!(style_f64(poster, "height_px"), 240.0);
    let image = poster.get("image").expect("image");
    assert_eq!(image.get("src").and_then(Json::as_str), Some("poster.png"));
    assert_eq!(image.get("alt").and_then(Json::as_str), Some("Demo"));
    assert_eq!(
        image.get("object_fit").and_then(Json::as_str),
        Some("cover")
    );

    // CSS gana a los atributos; sin object-fit el default es fill.
    let css_sized = images[1];
    assert_eq!(style_f64(css_sized, "width_px"), 180.0);
    assert_eq!(style_f64(css_sized, "height_px"), 240.0);
    assert_eq!(
        css_sized
            .get("image")
            .and_then(|image| image.get("object_fit"))
            .and_then(Json::as_str),
        Some("fill")
    );

    // Solo `width`; la altura queda auto (-1) para el tamano intrinseco.
    let width_only = images[2];
    assert_eq!(style_f64(width_only, "width_px"), 100.0);
    assert_eq!(style_f64(width_only, "height_px"), -1.0);
    assert_eq!(
        width_only.get("image").and_then(|image| image.get("alt")),
        Some(&Json::Null)
    );

    // `width: auto`/`height: auto` en CSS ganan a los atributos: el runtime
    // mide el tamano intrinseco.
    let css_auto = images[3];
    assert_eq!(style_f64(css_auto, "width_px"), -1.0);
    assert_eq!(style_f64(css_auto, "height_px"), -1.0);

    // `image` solo existe en nodos imagen.
    for node in nodes(&doc) {
        let is_image = node.get("kind").and_then(Json::as_str) == Some("image");
        assert_eq!(node.get("image").is_some(), is_image, "{node:#?}");
    }
}

#[test]
fn v1_page_stays_schema_1_without_v2_fields() {
    let dir = temp_dir("v1-unchanged");
    let html = r##"<!doctype html>
<html>
<head>
<style>
body { background: #ffffff; }
.row { display: flex; gap: 4px; }
.box { width: 10px; height: 10px; background: #111111; overflow: hidden; }
</style>
</head>
<body><div class="row"><div class="box"></div></div></body>
</html>
"##;
    write(&dir.join("page.html"), html);

    let first = emit_ui(&dir, "page.html", "ui-a.json", &[]);
    assert!(first.status.success(), "{}", stderr(&first));
    let second = emit_ui(&dir, "page.html", "ui-b.json", &[]);
    assert!(second.status.success(), "{}", stderr(&second));

    let first_bytes = fs::read(dir.join("ui-a.json")).unwrap();
    let second_bytes = fs::read(dir.join("ui-b.json")).unwrap();
    assert_eq!(
        first_bytes, second_bytes,
        "dos ejecuciones deben ser iguales"
    );

    let text = String::from_utf8(first_bytes).unwrap();
    assert!(
        text.contains("\"schema\": \"webnative.ui-tree/1\""),
        "{text}"
    );
    for absent in [
        "grid_template_columns",
        "grid_auto_rows",
        "row_gap",
        "column_gap",
        "\"image\"",
        "\"overflow\"",
        "border_width_px",
        "border_radius_px",
        "border_sides",
        "box_shadow",
        "width_em",
        "min_width_px",
        "padding_em",
        "\"opacity\"",
    ] {
        assert!(!text.contains(absent), "/1 no debe contener {absent}");
    }
}

#[test]
fn subset_rejects_pilot_features_without_pilot_and_allows_them_with_it() {
    let dir = temp_dir("subset-pilot");
    write(&dir.join("page.html"), GRID_PAGE);

    let strict = emit_ui(&dir, "page.html", "strict.json", &["--strict-subset"]);
    assert!(!strict.status.success(), "sin --pilot debe fallar");
    let strict_err = stderr(&strict);
    assert!(strict_err.contains("error[subset]"), "{strict_err}");
    assert!(strict_err.contains("piloto v2"), "{strict_err}");

    let allowed = emit_ui(
        &dir,
        "page.html",
        "allowed.json",
        &["--strict-subset", "--pilot"],
    );
    assert!(
        allowed.status.success(),
        "con --pilot debe pasar: {}",
        stderr(&allowed)
    );
    assert!(!stderr(&allowed).contains("error[subset]"));

    let default = emit_ui(&dir, "page.html", "default.json", &[]);
    assert!(default.status.success(), "{}", stderr(&default));
    let default_err = stderr(&default);
    assert!(default_err.contains("warning[subset]"), "{default_err}");
    assert!(default_err.contains("piloto v2"), "{default_err}");
}

#[test]
fn r1_warns_about_grid_scroll_and_images_and_emits_no_rects() {
    let dir = temp_dir("r1-warning");
    let html = r##"<!doctype html>
<html>
<head>
<style>
.gallery { display: grid; grid-template-columns: repeat(auto-fill, 180px); overflow: auto; height: 320px; }
</style>
</head>
<body>
  <div class="gallery"><img src="poster.png" width="180" height="240"></div>
</body>
</html>
"##;
    write(&dir.join("page.html"), html);

    let output = run(&dir, "page.html", &["--out", "page.ir.json"]);
    assert!(output.status.success(), "{}", stderr(&output));
    let err = stderr(&output);
    assert!(err.contains("warning[r1]"), "{err}");
    assert!(err.contains("layout grid"), "{err}");
    assert!(err.contains("scroll"), "{err}");
    assert!(err.contains("kind:image"), "{err}");

    let ir = read_json(&dir.join("page.ir.json"));
    let ops = ir.get("ops").and_then(Json::as_array).expect("ops");
    assert!(ops.is_empty(), "R1 no debe inventar rects: {ir:#?}");
}

#[test]
fn object_fit_defaults_to_fill_without_css() {
    let dir = temp_dir("object-fit-default");
    let html = r##"<!doctype html>
<html><head><style>.g { display: grid; }</style></head>
<body><div class="g"><img src="a.png"></div></body></html>
"##;
    write(&dir.join("page.html"), html);

    let output = emit_ui(&dir, "page.html", "ui.json", &["--pilot"]);
    assert!(output.status.success(), "{}", stderr(&output));
    let doc = read_json(&dir.join("ui.json"));
    assert_eq!(
        doc.get("schema").and_then(Json::as_str),
        Some("webnative.ui-tree/2")
    );
    let images = images(&doc);
    assert_eq!(images.len(), 1);
    assert_eq!(
        images[0]
            .get("image")
            .and_then(|image| image.get("object_fit"))
            .and_then(Json::as_str),
        Some("fill")
    );
    // Sin tamano: el runtime usa el intrinseco (WIC).
    assert_eq!(style_f64(images[0], "width_px"), -1.0);
    assert_eq!(style_f64(images[0], "height_px"), -1.0);
}

// ------------------------------------------------------- CSS v3 (bordes/radios/sombras/unidades)

const CSS_V3_PAGE: &str = r##"<!doctype html>
<html>
<head>
<style>
html { font-size: 16px; }
body { margin: 0; }
.card {
  width: 20em;
  height: 15rem;
  min-width: 240px;
  max-width: 60vw;
  min-height: 4rem;
  max-height: 90vh;
  padding: 1em 2rem;
  margin: 0.5em;
  gap: 1vh;
  border: 2px solid #334155;
  border-radius: 12px;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4), inset 1px 2px 0 #ff0000;
  background: #ffffff;
  font-size: 20px;
}
.inner { width: 50%; height: 2vw; }
.edge { border-top: 3px solid red; border-right: 1em solid blue; }
.none { border: 5px solid red; border-style: none; }
.rel { border: 1em solid #123456; border-radius: 0.5rem; }
</style>
</head>
<body>
  <div id="card" class="card"><div id="inner" class="inner"></div></div>
  <div id="edge" class="edge"></div>
  <div id="none" class="none"></div>
  <div id="rel" class="rel"></div>
</body>
</html>
"##;

#[test]
fn css_v3_emits_border_radius_shadow_and_relative_units() {
    let dir = temp_dir("css-v3-card");
    write(&dir.join("page.html"), CSS_V3_PAGE);

    let output = emit_ui(&dir, "page.html", "ui.json", &["--pilot"]);
    assert!(
        output.status.success(),
        "exit {:?}\n{}",
        output.status.code(),
        stderr(&output)
    );
    let doc = read_json(&dir.join("ui.json"));
    assert_eq!(
        doc.get("schema").and_then(Json::as_str),
        Some("webnative.ui-tree/2")
    );

    let card = node_with_id(&doc, "card");
    assert_eq!(style_f64(card, "border_width_px"), 2.0);
    assert_eq!(style_str(card, "border_color"), Some("#334155"));
    assert_eq!(style_str(card, "border_style"), Some("solid"));
    assert_eq!(style_f64(card, "border_radius_px"), 12.0);
    assert_eq!(
        style_array_opt(card, "border_radii_px"),
        vec![Some(12.0), Some(12.0), Some(12.0), Some(12.0)]
    );

    let shadows = style_value(card, "box_shadow")
        .and_then(Json::as_array)
        .expect("box_shadow[]");
    assert_eq!(shadows.len(), 2, "{card:#?}");
    assert_eq!(
        shadows[0].get("offset_x_px").and_then(Json::as_f64),
        Some(0.0)
    );
    assert_eq!(
        shadows[0].get("offset_y_px").and_then(Json::as_f64),
        Some(4.0)
    );
    assert_eq!(shadows[0].get("blur_px").and_then(Json::as_f64), Some(12.0));
    assert_eq!(
        shadows[0].get("color").and_then(Json::as_str),
        Some("#00000066")
    );
    assert_eq!(shadows[0].get("inset"), Some(&Json::Bool(false)));
    assert_eq!(shadows[1].get("inset"), Some(&Json::Bool(true)));
    assert_eq!(
        shadows[1].get("offset_x_px").and_then(Json::as_f64),
        Some(1.0)
    );
    assert_eq!(
        shadows[1].get("color").and_then(Json::as_str),
        Some("#ff0000")
    );

    // em usa el font-size del nodo (20px); rem el de html (16px).
    assert_eq!(style_f64(card, "width_px"), 400.0);
    assert_eq!(style_f64(card, "width_em"), 20.0);
    assert_eq!(style_value(card, "width_rem"), Some(&Json::Null));
    assert_eq!(style_f64(card, "height_px"), 240.0);
    assert_eq!(style_f64(card, "height_rem"), 15.0);
    assert_eq!(style_value(card, "height_em"), Some(&Json::Null));

    // min/max px/%/relativos.
    assert_eq!(style_f64(card, "min_width_px"), 240.0);
    assert_eq!(style_f64(card, "max_width_px"), 288.0);
    assert_eq!(style_f64(card, "max_width_vw"), 60.0);
    assert_eq!(style_f64(card, "min_height_px"), 64.0);
    assert_eq!(style_f64(card, "max_height_px"), 288.0);
    assert_eq!(style_f64(card, "max_height_vh"), 90.0);

    // padding/margin/gap conservan px resueltos y la unidad declarada.
    assert_eq!(
        style_array_opt(card, "padding"),
        vec![Some(20.0), Some(32.0), Some(20.0), Some(32.0)]
    );
    assert_eq!(
        style_array_opt(card, "padding_em"),
        vec![Some(1.0), None, Some(1.0), None]
    );
    assert_eq!(
        style_array_opt(card, "padding_rem"),
        vec![None, Some(2.0), None, Some(2.0)]
    );
    assert_eq!(
        style_array_opt(card, "margin_em"),
        vec![Some(0.5), Some(0.5), Some(0.5), Some(0.5)]
    );
    assert_eq!(style_f64(card, "gap"), 3.2);
    assert_eq!(style_f64(card, "gap_vh"), 1.0);
    assert_eq!(style_f64(card, "row_gap_vh"), 1.0);
    assert_eq!(style_f64(card, "column_gap_vh"), 1.0);

    // Hijo: % se conserva como width_pct y `2vw` resuelve a 9.6 px.
    let inner = node_with_id(&doc, "inner");
    assert_eq!(style_f64(inner, "width_px"), -1.0);
    assert_eq!(style_f64(inner, "width_pct"), 50.0);
    assert_eq!(style_f64(inner, "height_px"), 9.6);
    assert_eq!(style_f64(inner, "height_vw"), 2.0);

    // Longhands por lado: gana el longhand posterior al shorthand.
    let edge = node_with_id(&doc, "edge");
    assert_eq!(style_f64(edge, "border_width_px"), 16.0);
    assert_eq!(style_str(edge, "border_style"), Some("solid"));
    let sides = style_value(edge, "border_sides").expect("border_sides");
    let top = sides.get("top").expect("border_sides.top");
    assert_eq!(top.get("width_px").and_then(Json::as_f64), Some(3.0));
    assert_eq!(top.get("color").and_then(Json::as_str), Some("#ff0000"));
    assert_eq!(top.get("style").and_then(Json::as_str), Some("solid"));
    let right = sides.get("right").expect("border_sides.right");
    assert_eq!(right.get("width_px").and_then(Json::as_f64), Some(16.0));
    assert_eq!(right.get("color").and_then(Json::as_str), Some("#0000ff"));
    let bottom = sides.get("bottom").expect("border_sides.bottom");
    assert_eq!(bottom.get("style").and_then(Json::as_str), Some("none"));

    // `border-style: none` apaga el borde aunque ancho/color sigan declarados.
    let none = node_with_id(&doc, "none");
    assert_eq!(style_f64(none, "border_width_px"), 0.0);
    assert_eq!(style_str(none, "border_style"), Some("none"));
    assert_eq!(style_value(none, "border_color"), Some(&Json::Null));
    let none_top = style_value(none, "border_sides")
        .and_then(|sides| sides.get("top"))
        .expect("border_sides.top");
    assert_eq!(none_top.get("width_px").and_then(Json::as_f64), Some(5.0));

    // Borde uniforme con ancho relativo: px resuelto + unidad declarada.
    let relative = node_with_id(&doc, "rel");
    assert_eq!(style_f64(relative, "border_width_px"), 16.0);
    assert_eq!(style_f64(relative, "border_width_em"), 1.0);
    assert_eq!(style_str(relative, "border_color"), Some("#123456"));
    assert_eq!(style_f64(relative, "border_radius_px"), 8.0);
    assert_eq!(style_f64(relative, "border_radius_rem"), 0.5);
}

#[test]
fn css_v3_features_are_pilot_extensions() {
    let dir = temp_dir("css-v3-subset");
    write(&dir.join("page.html"), CSS_V3_PAGE);

    let default = emit_ui(&dir, "page.html", "default.json", &[]);
    assert!(default.status.success(), "{}", stderr(&default));
    let default_err = stderr(&default);
    assert!(default_err.contains("warning[subset]"), "{default_err}");
    assert!(default_err.contains("piloto v3"), "{default_err}");

    let strict = emit_ui(&dir, "page.html", "strict.json", &["--strict-subset"]);
    assert!(!strict.status.success(), "sin --pilot debe fallar");
    let strict_err = stderr(&strict);
    assert!(strict_err.contains("error[subset]"), "{strict_err}");
    assert!(strict_err.contains("piloto v3"), "{strict_err}");

    let allowed = emit_ui(
        &dir,
        "page.html",
        "allowed.json",
        &["--strict-subset", "--pilot"],
    );
    assert!(
        allowed.status.success(),
        "con --pilot debe pasar: {}",
        stderr(&allowed)
    );
    assert!(!stderr(&allowed).contains("[subset]"));
}

#[test]
fn r1_warns_about_border_radius_and_shadow_without_invalid_rects() {
    let dir = temp_dir("css-v3-r1");
    write(&dir.join("page.html"), CSS_V3_PAGE);

    let output = run(&dir, "page.html", &["--out", "page.ir.json"]);
    assert!(output.status.success(), "{}", stderr(&output));
    let err = stderr(&output);
    assert!(err.contains("warning[r1]"), "{err}");
    assert!(err.contains("'border' solo se renderiza en R2"), "{err}");
    assert!(err.contains("'border-radius'"), "{err}");
    assert!(err.contains("'box-shadow'"), "{err}");

    let ir = read_json(&dir.join("page.ir.json"));
    let ops = ir.get("ops").and_then(Json::as_array).expect("ops");
    // Solo la caja del card (fondo). Ni bordes, ni radios, ni sombras.
    assert_eq!(ops.len(), 1, "{ir:#?}");
    for op in ops {
        assert_eq!(op.get("type").and_then(Json::as_str), Some("rect"));
        assert_eq!(op.get("w").and_then(Json::as_f64), Some(400.0));
        assert_eq!(op.get("h").and_then(Json::as_f64), Some(240.0));
    }
}
