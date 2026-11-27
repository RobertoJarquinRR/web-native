//! Blob v5 `z-index: auto | <integer>` in the computed style and in
//! `subset v1` diagnostics.
//!
//! The stacking order itself lives in the C runtime suite; here the contract is
//! the value that travels in the `.wnui` blob: `None` for `auto`, an explicit
//! int32 otherwise (negative and zero included).

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

fn diagnostics(html: &str, name: &str) -> Vec<String> {
    let dir = temp_dir(name);
    let path = dir.join("page.html");
    let base = Path::new(&dir);
    check_document_pilot(html, &path, base, false)
        .into_iter()
        .map(|diagnostic| diagnostic.render(false))
        .collect()
}

const DEFAULTS_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .row { display: block; width: 120px; height: 20px; }
</style></head>
<body><div id="a" class="row"></div></body></html>
"##;

#[test]
fn pages_without_z_index_keep_auto() {
    let dir = temp_dir("zindex-defaults");
    let tree = build(&dir, DEFAULTS_PAGE);
    let plain = node(&tree, "body.root/div#a.row");
    assert_eq!(plain.style.z_index, None);
}

const VALUES_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .box { display: block; position: relative; width: 80px; height: 30px; }
  .auto { z-index: auto; }
  .negative { z-index: -3; }
  .zero { z-index: 0; }
  .positive { z-index: 12; }
</style></head>
<body>
  <div id="auto" class="box auto"></div>
  <div id="negative" class="box negative"></div>
  <div id="zero" class="box zero"></div>
  <div id="positive" class="box positive"></div>
</body></html>
"##;

#[test]
fn z_index_auto_and_integers_are_recorded() {
    let dir = temp_dir("zindex-values");
    let tree = build(&dir, VALUES_PAGE);

    assert_eq!(node(&tree, "body.root/div#auto.box").style.z_index, None);
    assert_eq!(
        node(&tree, "body.root/div#negative.box").style.z_index,
        Some(-3)
    );
    assert_eq!(node(&tree, "body.root/div#zero.box").style.z_index, Some(0));
    assert_eq!(
        node(&tree, "body.root/div#positive.box").style.z_index,
        Some(12)
    );
}

#[test]
fn supported_z_index_produces_no_diagnostics() {
    let report = diagnostics(VALUES_PAGE, "zindex-valid");
    assert!(report.is_empty(), "unexpected diagnostics: {report:#?}");
}

#[test]
fn z_index_on_static_flex_and_grid_items_is_recorded() {
    let html = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .flex { display: flex; width: 200px; height: 40px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, 50px); width: 200px; }
  .item { width: 50px; height: 20px; }
  .z1 { z-index: 1; }
  .z2 { z-index: 2; }
</style></head><body>
  <div id="flex" class="flex">
    <div id="f1" class="item z1"></div>
    <div id="f2" class="item z2"></div>
  </div>
  <div id="grid" class="grid">
    <div id="g1" class="item z1"></div>
    <div id="g2" class="item z2"></div>
  </div>
</body></html>"##;
    let dir = temp_dir("zindex-static-items");
    let tree = build(&dir, html);
    for name in [
        "body.root/div#flex.flex/div#f1.item",
        "body.root/div#flex.flex/div#f2.item",
        "body.root/div#grid.grid/div#g1.item",
        "body.root/div#grid.grid/div#g2.item",
    ] {
        let item = node(&tree, name);
        assert_eq!(item.style.position, "static", "{name}");
    }
    assert_eq!(
        node(&tree, "body.root/div#flex.flex/div#f1.item")
            .style
            .z_index,
        Some(1)
    );
    assert_eq!(
        node(&tree, "body.root/div#flex.flex/div#f2.item")
            .style
            .z_index,
        Some(2)
    );
    assert_eq!(
        node(&tree, "body.root/div#grid.grid/div#g2.item")
            .style
            .z_index,
        Some(2)
    );
}

#[test]
fn malformed_and_out_of_range_z_index_are_diagnosed() {
    let html = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .fraction { position: relative; z-index: 1.5; }
  .unit { position: relative; z-index: 2px; }
  .overflow { position: relative; z-index: 2147483648; }
  .sentinel { position: relative; z-index: -2147483648; }
</style></head><body>
  <div class="fraction"></div><div class="unit"></div>
  <div class="overflow"></div><div class="sentinel"></div>
</body></html>"##;
    let report = diagnostics(html, "zindex-invalid");
    for needle in [
        "z-index",
        "1.5",
        "2px",
        "2147483648",
        "-2147483648",
    ] {
        assert!(
            report.iter().any(|line| line.contains(needle)),
            "missing diagnostic for {needle}: {report:#?}"
        );
    }
}
