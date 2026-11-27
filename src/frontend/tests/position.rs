//! `position: relative|absolute|fixed|sticky` + `top/right/bottom/left` in the
//! pre-layout tree and in `subset v1` diagnostics.
//!
//! The layout semantics live in the C runtime suites; here the contract is the
//! computed style that travels in the `.wnui` blob: the position keyword, the
//! four inset values and the explicit mask (declared side + percentage unit).

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

const DEFAULTS_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .row { display: block; width: 120px; height: 20px; }
</style></head>
<body><div id="a" class="row"></div></body></html>
"##;

#[test]
fn pages_without_position_keep_static_and_all_auto() {
    let dir = temp_dir("position-defaults");
    let tree = build(&dir, DEFAULTS_PAGE);
    let plain = node(&tree, "body.root/div#a.row");

    assert_eq!(plain.style.position, "static");
    assert_eq!(plain.style.inset, [0.0; 4]);
    assert_eq!(plain.style.inset_mask, 0);
}

const VALUES_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #ffffff; }
  .box { display: block; width: 80px; height: 30px; }
  .rel {
    position: relative;
    top: 12px;
    right: 5%;
    bottom: -3px;
    left: auto;
  }
  .abs {
    position: absolute;
    left: 25%;
    top: 7px;
  }
  .both { position: relative; left: 4px; top: 6px; }
</style></head>
<body>
  <div id="rel" class="box rel"></div>
  <div id="abs" class="box abs"></div>
  <div id="both" class="box both"></div>
</body></html>
"##;

#[test]
fn position_and_insets_are_recorded_with_explicit_mask() {
    let dir = temp_dir("position-values");
    let tree = build(&dir, VALUES_PAGE);

    // top bit0, right bit1 (+percentage bit5), bottom bit2; left stays auto.
    let relative = node(&tree, "body.root/div#rel.box");
    assert_eq!(relative.style.position, "relative");
    assert_eq!(relative.style.inset, [12.0, 5.0, -3.0, 0.0]);
    assert_eq!(relative.style.inset_mask, 0b0010_0111);

    // left bit3 (+percentage bit7), top bit0.
    let absolute = node(&tree, "body.root/div#abs.box");
    assert_eq!(absolute.style.position, "absolute");
    assert_eq!(absolute.style.inset, [7.0, 0.0, 0.0, 25.0]);
    assert_eq!(absolute.style.inset_mask, 0b1000_1001);

    // Left/top win at layout time; both sides are still recorded.
    let both = node(&tree, "body.root/div#both.box");
    assert_eq!(both.style.position, "relative");
    assert_eq!(both.style.inset_mask & 0x0F, 0b1001);
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

#[test]
fn supported_position_and_insets_produce_no_diagnostics() {
    let report = diagnostics(VALUES_PAGE, "position-valid");
    assert!(report.is_empty(), "unexpected diagnostics: {report:#?}");
}

const FIXED_STICKY_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .fixed { position: fixed; top: 0; left: 0; width: 20px; height: 10px; }
  .sticky { position: sticky; top: 10px; }
</style></head><body>
  <div id="fixed" class="fixed"></div><div id="sticky" class="sticky"></div>
</body></html>"##;

#[test]
fn fixed_and_sticky_are_recorded_with_their_insets() {
    let dir = temp_dir("position-fixed-sticky");
    let tree = build(&dir, FIXED_STICKY_PAGE);
    let fixed = node(&tree, "body.root/div#fixed.fixed");
    assert_eq!(fixed.style.position, "fixed");
    assert_eq!(fixed.style.inset, [0.0, 0.0, 0.0, 0.0]);
    assert_eq!(fixed.style.inset_mask, 0b1001);
    let sticky = node(&tree, "body.root/div#sticky.sticky");
    assert_eq!(sticky.style.position, "sticky");
    assert_eq!(sticky.style.inset_mask, 0b0001);

    let report = diagnostics(FIXED_STICKY_PAGE, "position-fixed-sticky-valid");
    assert!(report.is_empty(), "unexpected diagnostics: {report:#?}");
}

#[test]
fn unsupported_values_and_deferred_features_are_diagnosed() {
    let html = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .bad-value { position: center; }
  .bad-unit { position: relative; top: 10em; }
  .shorthand { position: absolute; inset: 0; }
</style></head><body>
  <div class="bad-value"></div><div class="bad-unit"></div>
  <div class="shorthand"></div>
</body></html>"##;
    let report = diagnostics(html, "position-unsupported");
    assert!(
        report
            .iter()
            .any(|line| line.contains("position") && line.contains("center")),
        "missing bad position value diagnostic: {report:#?}"
    );
    assert!(
        report
            .iter()
            .any(|line| line.contains("top") && line.contains("10em")),
        "missing inset unit diagnostic: {report:#?}"
    );
    // `inset` is now supported by the frontend (expanded to the four
    // longhands), so it must stay clean here.
    assert!(
        !report.iter().any(|line| line.contains("'inset'")),
        "the inset shorthand is supported now: {report:#?}"
    );
}
