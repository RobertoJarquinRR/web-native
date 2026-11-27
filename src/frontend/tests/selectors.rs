//! Static structural selectors: `:nth-child()` / `:nth-of-type()`.
//!
//! Only An+B forms are compiled; `of` clauses, `nth-last-*` and malformed
//! arguments are diagnosed and dropped instead of silently matching nothing.

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
    build_tree(html, dir, (480, 320)).unwrap_or_else(|err| panic!("pipeline failed: {err}"))
}

fn node_with_id<'a>(tree: &'a Tree, element_id: &str) -> &'a NodeOut {
    tree.nodes
        .iter()
        .find(|node| node.element_id.as_deref() == Some(element_id))
        .unwrap_or_else(|| panic!("no node #{element_id}"))
}

fn diagnostics(html: &str, name: &str) -> Vec<String> {
    let dir = temp_dir(name);
    let path = dir.join("page.html");
    check_document_pilot(html, &path, &dir, true)
        .into_iter()
        .map(|diagnostic| diagnostic.render(false))
        .collect()
}

const NTH_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .list div { display: block; width: 10px; height: 10px; background: #000000; }
  .list div:nth-child(2) { background: #111111; }
  .list div:nth-child(odd) { background: #222222; }
  .list div:nth-child(3n+1) { background: #333333; }
  .list div:nth-of-type(2) { background: #444444; }
  .list div:nth-child(-n+2) { background: #555555; }
</style></head><body>
  <div id="list" class="list">
    <div id="a"></div>
    <span id="skip"></span>
    <div id="b"></div>
    <div id="c"></div>
    <div id="d"></div>
  </div>
</body></html>
"##;

#[test]
fn nth_child_and_nth_of_type_match_static_positions() {
    let dir = temp_dir("nth-static");
    let tree = build(&dir, NTH_PAGE);
    // Element children are a(1), span(2), b(3), c(4), d(5).
    // `odd` matches a/b/d at specification order; the later 3n+1 rule wins
    // over it for a/d because both are normal declarations with equal
    // specificity and it appears later.
    assert_eq!(
        node_with_id(&tree, "a").style.background.as_deref(),
        Some("#555555"),
        "a is 1: odd, 3n+1 and -n+2 all match; the last rule wins"
    );
    assert_eq!(
        node_with_id(&tree, "b").style.background.as_deref(),
        Some("#444444"),
        "b is the second div: nth-of-type(2) is the last rule that matches"
    );
    assert_eq!(
        node_with_id(&tree, "c").style.background.as_deref(),
        Some("#333333"),
        "c is 4: only 3n+1 matches"
    );
    assert_eq!(
        node_with_id(&tree, "d").style.background.as_deref(),
        Some("#222222"),
        "d is 5: only odd matches"
    );
}

const NTH_CHILD_COMBINATOR_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .list div { display: block; width: 10px; height: 10px; background: #000000; }
  .list > div:nth-child(3) { background: #abcdef; }
</style></head><body>
  <div class="list">
    <div id="a"></div>
    <span id="span"></span>
    <div id="b"></div>
    <div id="c"></div>
  </div>
</body></html>
"##;

#[test]
fn nth_child_works_with_the_child_combinator() {
    let dir = temp_dir("nth-child-combinator");
    let tree = build(&dir, NTH_CHILD_COMBINATOR_PAGE);
    // Element children are a(1), span(2), b(3), c(4): nth-child counts the
    // span even though the subject is restricted to div.
    assert_eq!(
        node_with_id(&tree, "b").style.background.as_deref(),
        Some("#abcdef"),
        "the third element child matches through the child combinator"
    );
    assert_ne!(
        node_with_id(&tree, "a").style.background.as_deref(),
        Some("#abcdef"),
        "a is the first child"
    );
}

const NTH_UNSUPPORTED_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .list div { display: block; width: 10px; height: 10px; background: #000000; }
  .list div:nth-child(2) + div { background: #ff0000; }
  .list div:nth-child(2)::before { background: #ff0000; }
  .list div:nth-child(2n+) { background: #ff0000; }
  .list div:nth-child(-1n-9223372036854775808) { background: #ff0000; }
  .list div:nth-child(9223372036854775807) { background: #ff0000; }
</style></head><body>
  <div class="list"><div id="x"></div><div id="y"></div><div id="z"></div></div>
</body></html>
"##;

#[test]
fn nth_invalid_combinator_and_pseudo_element_are_locked() {
    let dir = temp_dir("nth-unsupported");
    let diagnostics = diagnostics(NTH_UNSUPPORTED_PAGE, "nth-unsupported");
    // One per rule: sibling combinator, pseudo-element, malformed argument and
    // the two out-of-range integers (signed near i64::MIN and a bare i64::MAX).
    assert_eq!(diagnostics.len(), 5, "{diagnostics:#?}");
    assert!(
        diagnostics.iter().any(|line| line.contains("combinador '+'")),
        "missing sibling-combinator diagnostic: {diagnostics:#?}"
    );
    assert!(
        diagnostics
            .iter()
            .any(|line| line.contains("pseudo-elemento '::before'")),
        "missing pseudo-element diagnostic: {diagnostics:#?}"
    );
    assert_eq!(
        diagnostics
            .iter()
            .filter(|line| line.contains("forma no soportada"))
            .count(),
        3,
        "the malformed and out-of-range An+B forms are rejected: {diagnostics:#?}"
    );

    // The same stylesheet must still compile without matching or panicking:
    // rejected forms become `Simple::Never`, never an overflowing An+B.
    let tree = build(&dir, NTH_UNSUPPORTED_PAGE);
    for id in ["x", "y", "z"] {
        assert_ne!(
            node_with_id(&tree, id).style.background.as_deref(),
            Some("#ff0000"),
            "an unsupported nth form must not style #{id}"
        );
    }
}

const NTH_OF_TYPE_PAGE: &str = r##"<!doctype html>
<html><head><style>
  body { display: block; }
  .list div { display: block; width: 10px; height: 10px; background: #000000; }
  .list div:nth-of-type(2) { background: #abcdef; }
</style></head><body>
  <div class="list"><span></span><div id="first"></div><div id="second"></div></div>
</body></html>
"##;

#[test]
fn nth_of_type_counts_only_same_tag_siblings() {
    let dir = temp_dir("nth-of-type");
    let tree = build(&dir, NTH_OF_TYPE_PAGE);
    assert_eq!(
        node_with_id(&tree, "first").style.background.as_deref(),
        Some("#000000"),
        "first div is the first of its type"
    );
    assert_eq!(
        node_with_id(&tree, "second").style.background.as_deref(),
        Some("#abcdef"),
        "second div is the second of its type, despite the span"
    );
}

#[test]
fn unsupported_nth_forms_are_diagnosed() {
    let report = diagnostics(
        "<!doctype html><html><head><style>\n\
         body { display: block; }\n\
         p:nth-child(2n of .x) { color: red; }\n\
         p:nth-child(foo) { color: red; }\n\
         p:nth-last-child(2) { color: red; }\n\
         p:nth-child(2) { color: red; }\n\
         p:nth-of-type(even) { color: red; }\n\
         </style></head><body><p id=\"p\">x</p></body></html>",
        "nth-diagnostics",
    );
    assert_eq!(report.len(), 3, "{report:#?}");
    for needle in ["'of'", "foo", "nth-last-child"] {
        assert!(
            report.iter().any(|line| line.contains(needle)),
            "missing diagnostic for {needle}: {report:#?}"
        );
    }
    assert!(
        !report.iter().any(|line| line.contains(":nth-child(2)")),
        "a supported nth-child must stay clean: {report:#?}"
    );
    assert!(
        !report.iter().any(|line| line.contains("nth-of-type(even)")),
        "even is a supported nth-of-type: {report:#?}"
    );
}
