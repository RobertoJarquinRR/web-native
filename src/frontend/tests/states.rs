//! CSS state styles (blob v6): `:hover`, `:active`, `:focus`, `:disabled`.
//!
//! The pipeline builds four effective states per node. Press includes hover
//! (both bits are active in the cascade), focus only includes focus and
//! disabled only applies to nodes that can be disabled. Only `background`,
//! text `color` and the uniform border color travel in the state styles.

use std::fs;
use std::path::{Path, PathBuf};

use frontend_rust::{build_tree, build_tree_from_file, check_document_pilot, NodeOut, Tree};

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn build(dir: &Path, html: &str) -> Tree {
    build_tree(html, dir, (480, 320)).unwrap_or_else(|err| panic!("pipeline failed: {err}"))
}

fn node_with_id<'a>(tree: &'a Tree, element_id: &str) -> &'a NodeOut {
    tree.nodes
        .iter()
        .find(|node| node.element_id.as_deref() == Some(element_id))
        .unwrap_or_else(|| panic!("no node #{element_id}"))
}

fn node_with_class<'a>(tree: &'a Tree, class: &str) -> &'a NodeOut {
    tree.nodes
        .iter()
        .find(|node| node.classes.iter().any(|item| item == class))
        .unwrap_or_else(|| panic!("no node .{class}"))
}

fn diagnostics(html: &str, name: &str) -> Vec<String> {
    let dir = temp_dir(name);
    let path = dir.join("page.html");
    check_document_pilot(html, &path, &dir, true)
        .into_iter()
        .map(|diagnostic| diagnostic.render(false))
        .collect()
}

const STATES_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; background: #000000; }
  .btn {
    display: block;
    width: 120px;
    height: 30px;
    background: #101010;
    color: #ffffff;
    border: 2px solid #202020;
  }
  .btn:hover {
    background: #2a3140;
    color: #ffff00;
    border-color: #ff0000;
  }
  .btn:active { background: #3a4a5c; }
  .btn:focus { background: #003366; }
  .btn:disabled { background: #444444; color: #999999; }
</style></head><body>
  <button id="btn" class="btn">Go</button>
  <button id="off" class="btn" disabled>Off</button>
  <div id="plain" class="plain" data-hover="#abcdef" data-action="select"></div>
</body></html>
"##;

#[test]
fn hover_press_focus_and_disabled_are_effective_states() {
    let dir = temp_dir("states-four");
    let tree = build(&dir, STATES_PAGE);
    let button = node_with_id(&tree, "btn");
    let hover = button.states.hover.as_ref().expect("hover style");
    assert_eq!(hover.background.as_deref(), Some("#2a3140"));
    assert_eq!(hover.text.as_deref(), Some("#ffff00"));
    assert_eq!(hover.border.as_deref(), Some("#ff0000"));

    // Press includes hover through the same cascade; :active overrides the
    // background after it in source order.
    let press = button.states.press.as_ref().expect("press style");
    assert_eq!(press.background.as_deref(), Some("#3a4a5c"));
    assert_eq!(press.text.as_deref(), Some("#ffff00"));
    assert_eq!(press.border.as_deref(), Some("#ff0000"));

    // Focus only includes its own state: the background changes, the hover
    // text color does not leak in (the node has its own base color).
    let focus = button.states.focus.as_ref().expect("focus style");
    assert_eq!(focus.background.as_deref(), Some("#003366"));
    assert_eq!(focus.text, None);
    assert_eq!(focus.border, None);

    let disabled = button.states.disabled.as_ref().expect("disabled style");
    assert_eq!(disabled.background.as_deref(), Some("#444444"));
    assert_eq!(disabled.text.as_deref(), Some("#999999"));
    assert_eq!(disabled.border, None);

    // `disabled` on a button seeds the runtime bit in the states record.
    assert_eq!(button.initial_state, 0);
    assert_eq!(node_with_id(&tree, "off").initial_state, 0x10);

    // A div is not a node that can be disabled: no disabled style is stored.
    let plain = node_with_id(&tree, "plain");
    assert!(plain.states.disabled.is_none());
    assert!(plain.states.hover.is_none());
    assert_eq!(plain.data.hover.as_deref(), Some("#abcdef"));
}

#[test]
fn state_text_color_inherits_into_generated_text_runs() {
    let dir = temp_dir("states-text-inherit");
    let tree = build(&dir, STATES_PAGE);
    let button = node_with_id(&tree, "btn");
    let prefix = format!("{}/", button.name);
    let text = tree
        .nodes
        .iter()
        .find(|node| node.tag == "#text" && node.name.starts_with(&prefix))
        .expect("text run of the button");
    assert_eq!(text.states.hover.as_ref().unwrap().text.as_deref(), Some("#ffff00"));
    assert_eq!(text.states.press.as_ref().unwrap().text.as_deref(), Some("#ffff00"));
    assert!(text.states.focus.is_none());
}

const SPECIFICITY_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .btn { display: block; background: #101010; }
  .btn:hover { background: #111111; }
  #btn:hover { background: #222222 !important; }
  .locked { background: #abcdef !important; }
  .locked:hover { background: #333333; }
</style></head><body>
  <div id="btn" class="btn"></div>
  <div id="locked" class="btn locked"></div>
</body></html>
"##;

#[test]
fn state_cascade_keeps_specificity_and_important() {
    let dir = temp_dir("states-specificity");
    let tree = build(&dir, SPECIFICITY_PAGE);
    assert_eq!(
        node_with_id(&tree, "btn")
            .states
            .hover
            .as_ref()
            .unwrap()
            .background
            .as_deref(),
        Some("#222222")
    );
    // A base `!important` wins over a normal state declaration, so there is
    // no state override for the background.
    assert!(node_with_id(&tree, "locked").states.hover.is_none());
}

const COMBO_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .a:hover:active { background: #ff0000; }
  .b:hover:focus { background: #00ff00; }
  .c:checked { background: #0000ff; }
</style></head><body>
  <div class="a"></div><div class="b"></div><div class="c"></div>
</body></html>
"##;

#[test]
fn state_combinations_and_unsupported_pseudos_are_diagnosed() {
    let report = diagnostics(COMBO_PAGE, "states-combos");
    assert!(
        !report.iter().any(|line| line.contains("'.a'")),
        "hover+active is the supported press combination: {report:#?}"
    );
    assert!(
        report.iter().any(|line| line.contains("hover:focus")),
        "missing hover+focus combination diagnostic: {report:#?}"
    );
    assert!(
        report.iter().any(|line| line.contains(":checked")),
        "missing :checked diagnostic: {report:#?}"
    );
}

#[test]
fn migrated_runtime_page_uses_css_states() {
    let page = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/runtime/page/index.html");
    let tree = build_tree_from_file(&page, (960, 640)).expect("runtime page compiles");
    let nav = node_with_class(&tree, "nav");
    assert_eq!(nav.data.hover, None, "data-hover was migrated");
    assert_eq!(nav.data.press, None, "data-press was migrated");
    assert_eq!(
        nav.states.hover.as_ref().unwrap().background.as_deref(),
        Some("#2a3140")
    );
    assert_eq!(
        nav.states.press.as_ref().unwrap().background.as_deref(),
        Some("#3a4a5c")
    );
}

#[test]
fn migrated_dynamic_page_uses_css_states() {
    let page =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../src/runtime/tests/dynamic/page/index.html");
    let tree = build_tree_from_file(&page, (400, 300)).expect("dynamic page compiles");
    let card = node_with_class(&tree, "card");
    assert_eq!(card.data.hover, None, "data-hover was migrated");
    assert_eq!(card.data.press, None, "data-press was migrated");
    assert_eq!(
        card.states.hover.as_ref().unwrap().background.as_deref(),
        Some("#2a3140")
    );
    assert_eq!(
        card.states.press.as_ref().unwrap().background.as_deref(),
        Some("#3a4a5c")
    );
}

#[test]
fn migrated_pilot_page_uses_css_states() {
    let page = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../benchmarks/pilot-media-library/page/index.html");
    let tree = build_tree_from_file(&page, (1280, 800)).expect("pilot page compiles");
    let nav = node_with_class(&tree, "nav-item");
    assert_eq!(nav.data.hover, None, "data-hover was migrated");
    assert_eq!(nav.data.press, None, "data-press was migrated");
    assert_eq!(
        nav.states.hover.as_ref().unwrap().background.as_deref(),
        Some("#263248")
    );
    assert_eq!(
        nav.states.press.as_ref().unwrap().background.as_deref(),
        Some("#2e3c55")
    );
    let card = node_with_class(&tree, "poster-card");
    assert_eq!(
        card.states.hover.as_ref().unwrap().background.as_deref(),
        Some("#1f2735")
    );
    assert_eq!(
        card.states.press.as_ref().unwrap().background.as_deref(),
        Some("#263043")
    );
}
