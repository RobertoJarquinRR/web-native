//! CSS `font-weight` 100–900 in the computed style and in `subset v1`.
//!
//! The cascade normalizes `normal`/`bold` and every valid integer to a numeric
//! string so `NodeOut` and the `.wnui` blob carry the real weight. Invalid
//! values are dropped like CSS (the node keeps the inherited computed weight)
//! and reported by the subset.

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

fn node<'a>(tree: &'a Tree, element_id: &str) -> &'a NodeOut {
    tree.nodes
        .iter()
        .find(|node| node.element_id.as_deref() == Some(element_id))
        .unwrap_or_else(|| panic!("no node #{element_id}"))
}

fn text_run<'a>(tree: &'a Tree, element_id: &str) -> &'a NodeOut {
    let parent = tree
        .nodes
        .iter()
        .position(|node| node.element_id.as_deref() == Some(element_id))
        .unwrap_or_else(|| panic!("no element #{element_id}"));
    tree.nodes
        .iter()
        .find(|node| node.kind == frontend_rust::KIND_TEXT && node.parent == parent as i64)
        .unwrap_or_else(|| panic!("no text run under #{element_id}"))
}

fn diagnostics(name: &str, html: &str) -> Vec<String> {
    let dir = temp_dir(name);
    let path = dir.join("page.html");
    check_document_pilot(html, &path, &dir, false)
        .into_iter()
        .map(|diagnostic| diagnostic.render(false))
        .collect()
}

const WEIGHTS_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  #w300 { font-weight: 300; }
  #w500 { font-weight: 500; }
  #w600 { font-weight: 600; }
  #w900 { font-weight: 900; }
  #normal { font-weight: normal; }
  #bold { font-weight: bold; }
</style></head>
<body>
  <div id="w300">Light</div>
  <div id="w500">Medium</div>
  <div id="w600">Semibold</div>
  <div id="w900">Black</div>
  <div id="normal">Normal</div>
  <div id="bold">Bold</div>
</body></html>
"##;

#[test]
fn weights_100_to_900_are_emitted_as_numbers() {
    let dir = temp_dir("font-weight-values");
    let tree = build(&dir, WEIGHTS_PAGE);

    assert_eq!(node(&tree, "w300").style.font_weight, "300");
    assert_eq!(node(&tree, "w500").style.font_weight, "500");
    assert_eq!(node(&tree, "w600").style.font_weight, "600");
    assert_eq!(node(&tree, "w900").style.font_weight, "900");
    assert_eq!(node(&tree, "normal").style.font_weight, "400");
    assert_eq!(node(&tree, "bold").style.font_weight, "700");

    // The text run inherits the element's weight; nothing is baked.
    let run = text_run(&tree, "w500");
    assert_eq!(run.style.font_weight, "500");
    assert_eq!(run.style.width_px, 0.0);
}

const INHERIT_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; font-weight: 500; }
  #heavy { font-weight: 900; }
</style></head>
<body>
  <div id="plain">inherits 500</div>
  <div id="heavy"><div id="heavy-child">inherits 900</div></div>
</body></html>
"##;

#[test]
fn font_weight_is_inherited_like_css() {
    let dir = temp_dir("font-weight-inherit");
    let tree = build(&dir, INHERIT_PAGE);

    assert_eq!(node(&tree, "plain").style.font_weight, "500");
    assert_eq!(node(&tree, "heavy").style.font_weight, "900");
    assert_eq!(node(&tree, "heavy-child").style.font_weight, "900");

    // The text run sees the same computed value as its element.
    let run = text_run(&tree, "heavy-child");
    assert_eq!(run.style.font_weight, "900");
}

const INVALID_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; font-weight: 600; }
  #bolder { font-weight: bolder; }
  #lighter { font-weight: lighter; }
  #too-big { font-weight: 1000; }
  #too-small { font-weight: 50; }
  #decimal { font-weight: 700.5; }
  #unit { font-weight: 300px; }
  #multi { font-weight: bold 700; }
</style></head>
<body>
  <div id="bolder">bolder</div>
  <div id="lighter">lighter</div>
  <div id="too-big">1000</div>
  <div id="too-small">50</div>
  <div id="decimal">700.5</div>
  <div id="unit">300px</div>
  <div id="multi">bold 700</div>
</body></html>
"##;

#[test]
fn invalid_weights_are_diagnosed_and_keep_the_inherited_value() {
    let dir = temp_dir("font-weight-invalid");
    let tree = build(&dir, INVALID_PAGE);

    // CSS drops the invalid declaration: body's 600 survives on every child.
    for id in [
        "bolder", "lighter", "too-big", "too-small", "decimal", "unit", "multi",
    ] {
        assert_eq!(
            node(&tree, id).style.font_weight,
            "600",
            "{id} must keep the inherited weight"
        );
    }

    let rendered = diagnostics("font-weight-invalid-subset", INVALID_PAGE);
    let weight_diagnostics: Vec<&String> = rendered
        .iter()
        .filter(|line| line.contains("'font-weight'"))
        .collect();
    assert_eq!(weight_diagnostics.len(), 7, "{rendered:#?}");
    for value in ["bolder", "lighter", "1000", "50", "700.5", "300px", "bold 700"] {
        assert!(
            rendered.iter().any(|line| line.contains(value)
                && line.contains("'font-weight'")),
            "missing diagnostic for {value}: {rendered:#?}"
        );
    }
    // `normal`, `bold` and 100–900 stay clean.
    let clean = r##"<html><head><style>
      .a { font-weight: normal; }
      .b { font-weight: bold; }
      .c { font-weight: 300; }
      .d { font-weight: 900; }
    </style></head><body><div class="a b c d">x</div></body></html>"##;
    assert!(diagnostics("font-weight-clean", clean).is_empty());
}

#[test]
fn invalid_root_weight_falls_back_to_400() {
    let dir = temp_dir("font-weight-root");
    let html = r##"<!doctype html><html><head><style>
      html { font-weight: 1000; }
      body { display: block; }
    </style></head><body><div id="a">x</div></body></html>"##;
    let tree = build(&dir, html);
    assert_eq!(node(&tree, "a").style.font_weight, "400");
}
