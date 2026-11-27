//! `inset` shorthand (1-4 values) mapped onto the v4 `top/right/bottom/left`
//! fields.
//!
//! The blob does not change: the parser expands `inset` before the cascade, so
//! the emitted style is exactly the same as writing the four longhands.

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

const FOUR_FORMS_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .box { position: absolute; width: 10px; height: 10px; }
  .one { inset: 1px; }
  .two { inset: 1px 2px; }
  .three { inset: 1px 2px 3px; }
  .four { inset: 1px 2px 3px 4px; }
  .auto { inset: auto 10% auto -3px; }
</style></head><body>
  <div id="one" class="box one"></div>
  <div id="two" class="box two"></div>
  <div id="three" class="box three"></div>
  <div id="four" class="box four"></div>
  <div id="auto" class="box auto"></div>
</body></html>"##;

#[test]
fn inset_expands_like_the_css_four_value_shorthand() {
    let dir = temp_dir("inset-forms");
    let tree = build(&dir, FOUR_FORMS_PAGE);

    let one = node(&tree, "body.root/div#one.box");
    assert_eq!(one.style.inset, [1.0, 1.0, 1.0, 1.0]);
    assert_eq!(one.style.inset_mask, 0b0000_1111);

    let two = node(&tree, "body.root/div#two.box");
    assert_eq!(two.style.inset, [1.0, 2.0, 1.0, 2.0]);

    let three = node(&tree, "body.root/div#three.box");
    assert_eq!(three.style.inset, [1.0, 2.0, 3.0, 2.0]);

    let four = node(&tree, "body.root/div#four.box");
    assert_eq!(four.style.inset, [1.0, 2.0, 3.0, 4.0]);

    // auto stays undeclared; `10%` sets the percentage bit (bit 5) for `right`
    // and `-3px` is a valid negative inset.
    let auto = node(&tree, "body.root/div#auto.box");
    assert_eq!(auto.style.inset, [0.0, 10.0, 0.0, -3.0]);
    assert_eq!(auto.style.inset_mask, 0b0010_1010);

    let report = diagnostics(FOUR_FORMS_PAGE, "inset-forms-valid");
    assert!(report.is_empty(), "unexpected diagnostics: {report:#?}");
}

const ORDER_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .box { position: relative; width: 10px; height: 10px; }
  .longhand-last { inset: 1px; top: 5px; }
  .shorthand-last { top: 5px; inset: 1px; }
  .inline { inset: 2px 3px; }
</style></head><body>
  <div id="longhand-last" class="box longhand-last"></div>
  <div id="shorthand-last" class="box shorthand-last"></div>
  <div id="inline" class="box" style="inset: 2px 3px"></div>
</body></html>"##;

#[test]
fn cascade_order_and_inline_style_apply_to_the_expanded_longhands() {
    let dir = temp_dir("inset-order");
    let tree = build(&dir, ORDER_PAGE);

    let longhand_last = node(&tree, "body.root/div#longhand-last.box");
    assert_eq!(longhand_last.style.inset[0], 5.0);

    let shorthand_last = node(&tree, "body.root/div#shorthand-last.box");
    assert_eq!(shorthand_last.style.inset[0], 1.0);

    let inline = node(&tree, "body.root/div#inline.box");
    assert_eq!(inline.style.inset, [2.0, 3.0, 2.0, 3.0]);
}

fn diagnostics(html: &str, name: &str) -> Vec<String> {
    let dir = temp_dir(name);
    let path = dir.join("page.html");
    check_document_pilot(html, &path, &dir, false)
        .into_iter()
        .map(|diagnostic| diagnostic.render(false))
        .collect()
}

#[test]
fn malformed_inset_forms_are_diagnosed_and_dropped() {
    let html = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .too-many { position: absolute; inset: 1px 2px 3px 4px 5px; }
  .bad-unit { position: absolute; inset: 10em; }
  .bad-value { position: absolute; inset: 1px nope; }
</style></head><body>
  <div id="too-many" class="too-many"></div>
  <div id="bad-unit" class="bad-unit"></div>
  <div id="bad-value" class="bad-value"></div>
</body></html>"##;
    let report = diagnostics(html, "inset-invalid");
    assert_eq!(report.len(), 3, "{report:#?}");
    assert!(
        report.iter().all(|line| line.contains("'inset'")),
        "every diagnostic names the property: {report:#?}"
    );
    assert!(
        report.iter().any(|line| line.contains("10em")),
        "missing unsupported-unit diagnostic: {report:#?}"
    );

    let dir = temp_dir("inset-invalid-build");
    let tree = build(&dir, html);
    for name in [
        "body.root/div#too-many.too-many",
        "body.root/div#bad-unit.bad-unit",
        "body.root/div#bad-value.bad-value",
    ] {
        assert_eq!(
            node(&tree, name).style.inset_mask,
            0,
            "an invalid inset must not declare any side in {name}"
        );
    }
}
