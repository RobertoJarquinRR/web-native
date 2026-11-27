//! Narrow static `:has()`: `:has(.x)` (any descendant) and `:has(> .x)`
//! (direct child) over a single compound.
//!
//! The rule matches on the element carrying `:has`; the inner compound is not
//! styled directly. Everything outside the narrow grammar (selector lists,
//! combinators inside the argument, state pseudo-classes, nested `:has`) is
//! diagnosed and the rule is dropped instead of matching nothing silently.

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

const HAS_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .card { display: block; width: 10px; height: 10px; background: #000000; }
  .card:has(.badge) { background: #111111; }
  .direct:has(> .badge) { background: #222222; }
  .typed:has(span.badge) { background: #333333; }
  .by-id:has(#target) { background: #444444; }
  .by-attr:has([title="note"]) { background: #555555; }
</style></head><body>
  <div id="descendant" class="card">
    <div><span class="badge"></span></div>
  </div>
  <div id="direct" class="direct">
    <span class="badge"></span>
  </div>
  <div id="nested-only" class="direct">
    <div><span class="badge"></span></div>
  </div>
  <div id="typed" class="typed"><span class="badge"></span></div>
  <div id="by-id" class="by-id"><p id="target"></p></div>
  <div id="by-attr" class="by-attr"><p title="note"></p></div>
  <div id="no-match" class="card"></div>
</body></html>"##;

#[test]
fn has_matches_descendants_and_direct_children() {
    let dir = temp_dir("has-matching");
    let tree = build(&dir, HAS_PAGE);

    assert_eq!(
        node(&tree, "body.root/div#descendant.card")
            .style
            .background
            .as_deref(),
        Some("#111111"),
        "`:has(.badge)` matches a nested descendant"
    );
    assert_eq!(
        node(&tree, "body.root/div#direct.direct")
            .style
            .background
            .as_deref(),
        Some("#222222"),
        "`:has(> .badge)` matches the direct child"
    );
    assert_eq!(
        node(&tree, "body.root/div#nested-only.direct")
            .style
            .background
            .as_deref(),
        None,
        "`:has(> .badge)` must not match a grandchild"
    );
    assert_eq!(
        node(&tree, "body.root/div#typed.typed")
            .style
            .background
            .as_deref(),
        Some("#333333"),
        "the inner compound may combine type and class"
    );
    assert_eq!(
        node(&tree, "body.root/div#by-id.by-id")
            .style
            .background
            .as_deref(),
        Some("#444444")
    );
    assert_eq!(
        node(&tree, "body.root/div#by-attr.by-attr")
            .style
            .background
            .as_deref(),
        Some("#555555")
    );
    assert_eq!(
        node(&tree, "body.root/div#no-match.card")
            .style
            .background
            .as_deref(),
        Some("#000000"),
        "without a matching descendant the rule does not apply"
    );

    let report = diagnostics(HAS_PAGE, "has-matching-valid");
    assert!(report.is_empty(), "unexpected diagnostics: {report:#?}");
}

const SPECIFICITY_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .card { display: block; width: 10px; height: 10px; background: #000000; }
  .card { background: #111111; }
  .card:has(.badge) { background: #222222; }
</style></head><body>
  <div id="card" class="card"><span class="badge"></span></div>
</body></html>"##;

#[test]
fn has_contributes_the_inner_compound_specificity() {
    let dir = temp_dir("has-specificity");
    let tree = build(&dir, SPECIFICITY_PAGE);
    // `.card:has(.badge)` is (0,2,0) and wins over the later `.card` (0,1,0).
    assert_eq!(
        node(&tree, "body.root/div#card.card")
            .style
            .background
            .as_deref(),
        Some("#222222")
    );
}

const HAS_STATE_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .card { display: block; width: 10px; height: 10px; background: #000000; color: #000000; }
  .card:has(.badge):hover { background: #abcdef; color: #fedcba; }
</style></head><body>
  <div id="card" class="card"><span class="badge"></span></div>
</body></html>"##;

#[test]
fn has_combines_with_a_subject_state_pseudo() {
    let dir = temp_dir("has-state");
    let tree = build(&dir, HAS_STATE_PAGE);
    let card = node(&tree, "body.root/div#card.card");
    let hover = card.states.hover.as_ref().expect("hover style");
    assert_eq!(hover.background.as_deref(), Some("#abcdef"));
    assert_eq!(hover.text.as_deref(), Some("#fedcba"));
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
fn has_forms_outside_the_narrow_subset_are_diagnosed_and_dropped() {
    let html = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .card { display: block; width: 10px; height: 10px; background: #000000; }
  .descendant-chain:has(.a .b) { background: #ff0000; }
  .sibling:has(+ .b) { background: #ff0000; }
  .state:has(:hover) { background: #ff0000; }
  .empty:has() { background: #ff0000; }
  .two-children:has(> .a > .b) { background: #ff0000; }
  .nested:has(:has(.b)) { background: #ff0000; }
  .list:has(.a, .b) { background: #ff0000; }
</style></head><body>
  <div id="descendant-chain" class="descendant-chain card"><div class="a"><span class="b"></span></div></div>
  <div id="sibling" class="sibling card"><span class="b"></span></div>
  <div id="state" class="state card"><span class="b"></span></div>
  <div id="empty" class="empty card"><span class="b"></span></div>
  <div id="two-children" class="two-children card"><div class="a"><span class="b"></span></div></div>
  <div id="nested" class="nested card"><span class="b"></span></div>
  <div id="list" class="list card"><span class="a"></span></div>
</body></html>"##;
    let report = diagnostics(html, "has-invalid");
    assert_eq!(report.len(), 7, "{report:#?}");
    assert!(
        report.iter().all(|line| line.contains("':has()'")),
        "every diagnostic names :has(): {report:#?}"
    );

    let dir = temp_dir("has-invalid-build");
    let tree = build(&dir, html);
    for name in [
        "body.root/div#descendant-chain.descendant-chain",
        "body.root/div#sibling.sibling",
        "body.root/div#state.state",
        "body.root/div#empty.empty",
        "body.root/div#two-children.two-children",
        "body.root/div#nested.nested",
        "body.root/div#list.list",
    ] {
        assert_eq!(
            node(&tree, name).style.background.as_deref(),
            Some("#000000"),
            "a rejected :has form must not style {name}"
        );
    }
}
