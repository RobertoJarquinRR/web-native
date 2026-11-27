//! CSS custom properties (`--token`) and `var()` / `var(--token, fallback)`.
//!
//! Resolution happens in the cascade, before the value is interpreted: the blob
//! still carries concrete numbers/colors. Custom properties inherit, names are
//! case-sensitive, cycles are invalid and an unresolved `var()` without a
//! fallback invalidates the declaration (inherited/initial value wins).

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

const VARS_PAGE: &str = r##"<!doctype html>
<html><head><style>
  :root { --ink: #112233; --pad: 6px; --weight: 700; }
  body { display: block; color: #010203; --gap: 4px; }
  .card { color: var(--ink); padding: var(--pad); margin-top: var(--gap); }
  .fallback { width: var(--missing, 42px); }
  .unresolved { color: var(--missing); width: var(--missing); }
  .cycle { color: var(--loop, #00ff00); }
  .weight { font-weight: var(--weight); }
  .inset { position: relative; inset: var(--pad); }
  .case { color: var(--Ink, #aabbcc); }
  .case-fallback { color: var(--ink, #445566); }
  .inline-var { padding-top: var(--fromattr); }
</style></head><body>
  <div id="card" class="card"></div>
  <div id="fallback" class="fallback"></div>
  <div id="unresolved" class="unresolved"></div>
  <div id="cycle" class="cycle"></div>
  <div id="weight" class="weight"></div>
  <div id="inset" class="inset"></div>
  <div id="case" class="case"></div>
  <div id="case-fallback" class="case-fallback"></div>
  <div id="inline-var" class="inline-var" style="--fromattr: 12px"></div>
  <div id="inline" class="inline" style="--local: 9px; padding-left: var(--local)"></div>
</body></html>"##;

const CYCLE_PAGE: &str = r##"<!doctype html>
<html><head><style>
  :root { --a: var(--b); --b: var(--a); }
  body { display: block; }
  .box { width: var(--a, 33px); height: var(--a); }
</style></head><body><div id="box" class="box"></div></body></html>"##;

#[test]
fn custom_properties_resolve_inherit_and_fallback() {
    let dir = temp_dir("vars-values");
    let tree = build(&dir, VARS_PAGE);

    let card = node(&tree, "body.root/div#card.card");
    assert_eq!(card.style.color, "#112233", "root token wins for color");
    assert_eq!(card.style.padding, [6.0, 6.0, 6.0, 6.0]);
    assert_eq!(card.style.margin[0], 4.0, "body token inherits to the card");

    let fallback = node(&tree, "body.root/div#fallback.fallback");
    assert_eq!(fallback.style.width_px, 42.0);

    let unresolved = node(&tree, "body.root/div#unresolved.unresolved");
    assert_eq!(
        unresolved.style.color, "#010203",
        "an unresolved var() drops the declaration and keeps the inherited color"
    );
    assert_eq!(
        unresolved.style.width_px, -1.0,
        "an unresolved var() without fallback leaves width at its initial value"
    );

    let cycle = node(&tree, "body.root/div#cycle.cycle");
    assert_eq!(
        cycle.style.color, "#00ff00",
        "a cycle makes the token invalid and the fallback applies"
    );

    let weight = node(&tree, "body.root/div#weight.weight");
    assert_eq!(weight.style.font_weight, "700");

    let inset = node(&tree, "body.root/div#inset.inset");
    assert_eq!(inset.style.inset, [6.0, 6.0, 6.0, 6.0]);
    assert_eq!(inset.style.inset_mask, 0b0000_1111);

    let case = node(&tree, "body.root/div#case.case");
    assert_eq!(
        case.style.color, "#aabbcc",
        "custom property names are case-sensitive"
    );
    let case_fallback = node(&tree, "body.root/div#case-fallback.case-fallback");
    assert_eq!(case_fallback.style.color, "#112233");

    let inline_var = node(&tree, "body.root/div#inline-var.inline-var");
    assert_eq!(
        inline_var.style.padding[0], 12.0,
        "a token declared in an inline style is visible to the rule"
    );

    let inline = node(&tree, "body.root/div#inline.inline");
    assert_eq!(inline.style.padding[3], 9.0);

    let report = diagnostics(VARS_PAGE, "vars-values-valid");
    // Only the deliberate undefined-token case is reported; every valid form
    // (root token, inherited token, fallback, inline attribute token) is clean.
    assert_eq!(report.len(), 2, "unexpected diagnostics: {report:#?}");
    assert!(
        report.iter().all(|line| line.contains("'--missing'")),
        "only the unresolved token is reported: {report:#?}"
    );
}

#[test]
fn cycles_are_invalid_and_never_produce_a_wrong_value() {
    let dir = temp_dir("vars-cycle");
    let tree = build(&dir, CYCLE_PAGE);
    let card = node(&tree, "body.root/div#box.box");
    assert_eq!(card.style.width_px, 33.0, "fallback after a cycle");
    assert_eq!(
        card.style.height_px, -1.0,
        "without fallback the cyclic token invalidates the declaration"
    );
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
fn undefined_malformed_and_unrepresentable_uses_are_diagnosed() {
    let html = r##"<!doctype html>
<html><head><style>
  :root { --ink: #112233; }
  body { display: block; }
  .ok { color: var(--ink); padding: var(--pad, 2px); }
  .undefined { color: var(--missing); }
  .fallback { width: var(--missing, 5px); }
  .border-var { border: 1px solid var(--ink); }
  .state-prop:hover { --state: red; color: red; }
  .malformed { color: var(--ink; }
</style></head><body>
  <div class="ok"></div><div class="undefined"></div><div class="fallback"></div>
  <div class="malformed"></div><div class="border-var"></div><div class="state-prop"></div>
</body></html>"##;
    let report = diagnostics(html, "vars-invalid");
    assert_eq!(report.len(), 4, "{report:#?}");
    assert!(
        report
            .iter()
            .any(|line| line.contains("'--missing'") && line.contains("sin fallback")),
        "missing undefined-token diagnostic: {report:#?}"
    );
    assert!(
        report.iter().any(|line| line.contains("mal formada")),
        "missing malformed-var diagnostic: {report:#?}"
    );
    assert!(
        report.iter().any(|line| line.contains("var() en 'border'")),
        "missing border-shorthand diagnostic: {report:#?}"
    );
    assert!(
        report
            .iter()
            .any(|line| line.contains("regla de estado") && line.contains("'--state'")),
        "missing custom-property-in-state diagnostic: {report:#?}"
    );
    assert!(
        !report
            .iter()
            .any(|line| line.contains("'--ink'") || line.contains("'--pad'")),
        "defined tokens and fallbacks must stay clean: {report:#?}"
    );
}
