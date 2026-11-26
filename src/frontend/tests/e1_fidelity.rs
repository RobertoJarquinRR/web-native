//! E1 fidelity tests: `font-weight` real variants, `text-align`,
//! `line-height`, `margin: auto` and flex growth in the pre-layout tree.
//!
//! These tests use the library pipeline (`frontend_rust::build_tree`) and
//! inspect the in-memory [`Tree`]: no process spawn, no JSON. The C runtime
//! invariants live in the runtime test suites.

use std::fs;
use std::path::{Path, PathBuf};

use frontend_rust::{build_tree, check_document_pilot, NodeOut, Tree};

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn build(dir: &Path, html: &str) -> Tree {
    fs::write(dir.join("page.html"), html).unwrap();
    build_tree(html, dir, (480, 320)).unwrap_or_else(|err| panic!("pipeline failed: {err}"))
}

fn node<'a>(tree: &'a Tree, name: &str) -> &'a NodeOut {
    tree.nodes
        .iter()
        .find(|node| node.name == name)
        .unwrap_or_else(|| panic!("node not found: {name}"))
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
fn font_weight_700_is_recorded_and_text_is_not_baked() {
    let dir = temp_dir("e1-bold");
    let tree = build(&dir, BOLD_PAGE);

    let regular = node(&tree, "body.root/div#regular.row/text");
    let bold = node(&tree, "body.root/div#bold.row/text[1]");
    assert_eq!(regular.style.font_weight, "400");
    assert_eq!(bold.style.font_weight, "700");
    // The runtime measures and shapes: the document carries no box or glyphs.
    assert_eq!(bold.style.width_px, 0.0);
    assert_eq!(bold.style.font_size, 18.0);
    assert_eq!(bold.text.as_ref().map(|text| text.content.as_str()), Some("Media Bold"));
    // A page without v2 features keeps the compact blob section.
    assert!(!tree.detailed);
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
    let tree = build(&dir, ALIGN_PAGE);

    assert_eq!(node(&tree, "body.root/div#left.row").style.text_align, "left");
    assert_eq!(
        node(&tree, "body.root/div#center.row").style.text_align,
        "center"
    );
    assert_eq!(
        node(&tree, "body.root/div#right.row/text[2]").style.text_align,
        "right"
    );
    // The text node inherits the element's alignment.
    assert_eq!(
        node(&tree, "body.root/div#center.row/text[1]").style.text_align,
        "center"
    );
}

#[test]
fn pages_without_e1_features_keep_neutral_defaults() {
    let dir = temp_dir("e1-defaults");
    let html = r##"<!doctype html><html><head><style>
      body { display: block; }
      .row { display: block; font-size: 16px; }
    </style></head><body><div id="a" class="row">Plain</div></body></html>"##;
    let tree = build(&dir, html);

    let plain = node(&tree, "body.root/div#a.row");
    assert_eq!(plain.style.text_align, "left");
    assert_eq!(plain.style.line_height, 0.0);
    assert_eq!(plain.style.margin_auto, [false; 4]);
    assert!(!plain.style.box_sizing_border);
    assert_eq!(plain.style.flex_grow, 0.0);
    assert_eq!(plain.style.flex_shrink, 1.0);
    assert_eq!(plain.style.flex_basis, -1.0);
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
fn line_height_sets_the_text_box() {
    let dir = temp_dir("e1-line-height");
    let tree = build(&dir, LINE_HEIGHT_PAGE);

    let normal = node(&tree, "body.root/div#normal.row/text");
    let tall = node(&tree, "body.root/div#tall.row/text[1]");
    assert_eq!(normal.style.height_px, 0.0, "auto box is measured at runtime");
    assert_eq!(
        tall.style.height_px, 40.0,
        "declared line-height travels in the document"
    );
    assert_eq!(tall.style.line_height, 40.0);
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
fn margin_auto_is_parsed_per_side() {
    let dir = temp_dir("e1-margin-auto");
    let tree = build(&dir, AUTO_PAGE);

    assert_eq!(
        node(&tree, "body.root/div#pin.pin/div#foot.foot").style.margin_auto,
        [true, false, false, false]
    );
    assert_eq!(
        node(&tree, "body.root/div#box.box/div#mid.mid").style.margin_auto,
        [false, true, false, true]
    );
    // The flex container keeps its declared direction.
    assert_eq!(node(&tree, "body.root/div#pin.pin").style.direction, 1);
    // `margin: 0 auto` and `margin-top: auto` are no longer subset-v1 errors.
    let html_path = dir.join("page.html");
    let diagnostics = check_document_pilot(AUTO_PAGE, &html_path, &dir, false);
    assert!(diagnostics.is_empty(), "{diagnostics:?}");
}
