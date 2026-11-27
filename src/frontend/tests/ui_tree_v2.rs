//! Integration tests for the v2/v3 pre-layout tree.
//!
//! They run the library pipeline (`frontend_rust::build_tree`) and inspect the
//! in-memory [`Tree`]. No process is spawned and no JSON is produced: every
//! assertion here checks the values the blob writer consumes.
//!
//! The historical `ui-tree/1|2|3` schema strings were a JSON transport detail.
//! What still matters is the *content* decision: [`Tree::detailed`] is `true`
//! when the page needs the v2/v3 blob fields (grid, scroll, images, CSS v3 or
//! input). Those checks replaced the old schema-string assertions.

use std::fs;
use std::path::{Path, PathBuf};

use frontend_rust::{
    build_tree, check_document_pilot, Diagnostic, GridAutoRows, NodeOut, Tree, KIND_BLOCK,
    KIND_GRID, KIND_IMAGE, KIND_INPUT, Unit,
};

// --------------------------------------------------------------- infra

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn build(dir: &Path, html: &str) -> Tree {
    build_tree(html, dir, (480, 320)).unwrap_or_else(|err| panic!("pipeline failed: {err}"))
}

fn diag_text(diagnostics: &[Diagnostic], strict: bool) -> String {
    diagnostics
        .iter()
        .map(|diagnostic| diagnostic.render(strict))
        .collect::<Vec<_>>()
        .join("\n")
}

fn node_with_id<'a>(tree: &'a Tree, element_id: &str) -> &'a NodeOut {
    tree.nodes
        .iter()
        .find(|node| node.element_id.as_deref() == Some(element_id))
        .unwrap_or_else(|| panic!("no hay nodo #{element_id}"))
}

fn images(tree: &Tree) -> Vec<&NodeOut> {
    tree.nodes
        .iter()
        .filter(|node| node.kind == KIND_IMAGE)
        .collect()
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
fn grid_overflow_and_images_are_carried_in_the_tree() {
    let dir = temp_dir("grid-image-v2");
    let tree = build(&dir, GRID_PAGE);

    assert!(tree.detailed, "grid/images need the v2 blob fields");
    assert_eq!(tree.nodes.len(), 7);

    let gallery = node_with_id(&tree, "gallery");
    assert_eq!(gallery.kind, KIND_GRID);
    assert_eq!(gallery.style.display.as_deref(), Some("grid"));
    assert_eq!(gallery.style.gap, 8.0);
    assert_eq!(gallery.style.row_gap, 12.0);
    assert_eq!(gallery.style.column_gap, 6.0);
    assert_eq!(gallery.style.overflow.as_deref(), Some("auto"));
    let columns = gallery
        .style
        .grid_template_columns
        .as_ref()
        .expect("grid_template_columns");
    assert_eq!(columns.mode, "auto_fill");
    assert_eq!(columns.size_px, 180.0);
    assert_eq!(
        gallery.style.grid_auto_rows,
        Some(GridAutoRows::Px(240.0))
    );

    let auto = node_with_id(&tree, "auto");
    assert_eq!(auto.kind, KIND_GRID);
    assert_eq!(auto.style.overflow.as_deref(), Some("scroll"));
    assert_eq!(auto.style.grid_auto_rows, Some(GridAutoRows::Auto));

    // The body uses no v2 feature; the fields stay absent in the tree.
    let body = &tree.nodes[0];
    assert_eq!(body.kind, KIND_BLOCK);
    assert!(body.style.grid_template_columns.is_none());
    assert!(body.style.overflow.is_none());

    let images = images(&tree);
    assert_eq!(images.len(), 4);

    let poster = images[0];
    assert_eq!(poster.tag, "img");
    assert_eq!(poster.style.width_px, 180.0);
    assert_eq!(poster.style.height_px, 240.0);
    let image = poster.image.as_ref().expect("image");
    assert_eq!(image.src.as_deref(), Some("poster.png"));
    assert_eq!(image.alt.as_deref(), Some("Demo"));
    assert_eq!(image.object_fit, "cover");

    // CSS wins over attributes; without object-fit the default is fill.
    let css_sized = images[1];
    assert_eq!(css_sized.style.width_px, 180.0);
    assert_eq!(css_sized.style.height_px, 240.0);
    assert_eq!(
        css_sized.image.as_ref().map(|image| image.object_fit.as_str()),
        Some("fill")
    );

    // Only `width`; height stays auto (-1) for the intrinsic size.
    let width_only = images[2];
    assert_eq!(width_only.style.width_px, 100.0);
    assert_eq!(width_only.style.height_px, -1.0);
    assert_eq!(width_only.image.as_ref().and_then(|image| image.alt.as_deref()), None);

    // `width: auto`/`height: auto` in CSS win over the attributes: the runtime
    // measures the intrinsic size.
    let css_auto = images[3];
    assert_eq!(css_auto.style.width_px, -1.0);
    assert_eq!(css_auto.style.height_px, -1.0);

    // `image` only exists on image nodes.
    for node in &tree.nodes {
        assert_eq!(node.image.is_some(), node.kind == KIND_IMAGE, "{}", node.name);
    }
}

#[test]
fn v1_page_stays_compact_and_blob_output_is_deterministic() {
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
    let tree = build(&dir, html);
    assert!(!tree.detailed, "a v1 page must not claim v2 fields");

    let row = &tree.nodes[1];
    assert_eq!(row.kind, frontend_rust::KIND_FLEX);
    assert_eq!(row.style.gap, 4.0);
    let boxed = &tree.nodes[2];
    assert_eq!(boxed.style.width_px, 10.0);
    assert_eq!(boxed.style.height_px, 10.0);
    assert_eq!(boxed.style.background.as_deref(), Some("#111111"));
    // `overflow: hidden` existed in v1: it does not force the v2 fields.
    assert_eq!(boxed.style.overflow.as_deref(), Some("hidden"));
    assert!(boxed.style.grid_template_columns.is_none());
    assert!(boxed.style.grid_auto_rows.is_none());
    assert!(boxed.image.is_none());

    let first = dir.join("a.wnui");
    let second = dir.join("b.wnui");
    tree.write_blob(&first).unwrap();
    tree.write_blob(&second).unwrap();
    assert_eq!(
        fs::read(&first).unwrap(),
        fs::read(&second).unwrap(),
        "two emissions must be byte-identical"
    );
}

#[test]
fn subset_rejects_pilot_features_without_pilot_and_allows_them_with_it() {
    let dir = temp_dir("subset-pilot");
    let path = dir.join("page.html");
    fs::write(&path, GRID_PAGE).unwrap();

    let strict = diag_text(&check_document_pilot(GRID_PAGE, &path, &dir, false), true);
    assert!(strict.contains("error[subset]"), "{strict}");
    assert!(strict.contains("piloto v2"), "{strict}");

    let allowed = check_document_pilot(GRID_PAGE, &path, &dir, true);
    assert!(allowed.is_empty(), "{}", diag_text(&allowed, true));

    // Without `--strict-subset` the same findings are warnings.
    let default = diag_text(&check_document_pilot(GRID_PAGE, &path, &dir, false), false);
    assert!(default.contains("warning[subset]"), "{default}");
    assert!(default.contains("piloto v2"), "{default}");
}

#[test]
fn object_fit_defaults_to_fill_without_css() {
    let dir = temp_dir("object-fit-default");
    let html = r##"<!doctype html>
<html><head><style>.g { display: grid; }</style></head>
<body><div class="g"><img src="a.png"></div></body></html>
"##;
    let tree = build(&dir, html);
    assert!(tree.detailed);
    let images = images(&tree);
    assert_eq!(images.len(), 1);
    assert_eq!(
        images[0].image.as_ref().map(|image| image.object_fit.as_str()),
        Some("fill")
    );
    // No size: the runtime uses the intrinsic one (WIC).
    assert_eq!(images[0].style.width_px, -1.0);
    assert_eq!(images[0].style.height_px, -1.0);
}

// ------------------------------------------------------- CSS v3 (borders/radii/shadows/units)

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
fn css_v3_carries_border_radius_shadow_and_relative_units() {
    let dir = temp_dir("css-v3-card");
    let tree = build(&dir, CSS_V3_PAGE);
    assert!(tree.detailed);

    let card = node_with_id(&tree, "card");
    let (border_width, _) = card.style.css3.border.uniform_width();
    assert_eq!(border_width, 2.0);
    assert_eq!(card.style.css3.border.visible_color(), Some("#334155"));
    assert_eq!(card.style.css3.border.style_name(), "solid");
    assert_eq!(card.style.css3.border_radius, [12.0, 12.0, 12.0, 12.0]);

    let shadows = &card.style.css3.box_shadow;
    assert_eq!(shadows.len(), 2);
    assert_eq!(shadows[0].offset_x_px, 0.0);
    assert_eq!(shadows[0].offset_y_px, 4.0);
    assert_eq!(shadows[0].blur_px, 12.0);
    assert_eq!(shadows[0].color, "#00000066");
    assert!(!shadows[0].inset);
    assert!(shadows[1].inset);
    assert_eq!(shadows[1].offset_x_px, 1.0);
    assert_eq!(shadows[1].color, "#ff0000");

    // em uses the node's font-size (20px); rem uses html's (16px).
    assert_eq!(card.style.width_px, 400.0);
    assert_eq!(card.style.css3.width_rel.unwrap().in_unit(Unit::Em), Some(20.0));
    assert_eq!(card.style.height_px, 240.0);
    assert_eq!(
        card.style.css3.height_rel.unwrap().in_unit(Unit::Rem),
        Some(15.0)
    );

    // min/max px/%/relative.
    assert_eq!(card.style.css3.min_width, Some(240.0));
    assert_eq!(card.style.css3.max_width, Some(288.0));
    assert_eq!(
        card.style.css3.max_width_rel.unwrap().in_unit(Unit::Vw),
        Some(60.0)
    );
    assert_eq!(card.style.css3.min_height, Some(64.0));
    assert_eq!(card.style.css3.max_height, Some(288.0));
    assert_eq!(
        card.style.css3.max_height_rel.unwrap().in_unit(Unit::Vh),
        Some(90.0)
    );

    // padding/margin/gap keep the resolved px and the declared unit.
    assert_eq!(card.style.padding, [20.0, 32.0, 20.0, 32.0]);
    assert_eq!(card.style.css3.padding_rel[0].unwrap().in_unit(Unit::Em), Some(1.0));
    assert_eq!(card.style.css3.padding_rel[1].unwrap().in_unit(Unit::Rem), Some(2.0));
    assert_eq!(card.style.css3.padding_rel[2].unwrap().in_unit(Unit::Em), Some(1.0));
    assert_eq!(card.style.css3.padding_rel[3].unwrap().in_unit(Unit::Rem), Some(2.0));
    for side in card.style.css3.margin_rel {
        assert_eq!(side.unwrap().in_unit(Unit::Em), Some(0.5));
    }
    assert_eq!(card.style.gap, 3.2);
    assert_eq!(card.style.css3.gap_rel.unwrap().in_unit(Unit::Vh), Some(1.0));
    assert_eq!(card.style.css3.row_gap_rel.unwrap().in_unit(Unit::Vh), Some(1.0));
    assert_eq!(card.style.css3.column_gap_rel.unwrap().in_unit(Unit::Vh), Some(1.0));

    // Child: `%` is kept as width_pct and `2vw` resolves to 9.6 px.
    let inner = node_with_id(&tree, "inner");
    assert_eq!(inner.style.width_px, -1.0);
    assert_eq!(inner.style.width_pct, 50.0);
    assert_eq!(inner.style.height_px, 9.6);
    assert_eq!(
        inner.style.css3.height_rel.unwrap().in_unit(Unit::Vw),
        Some(2.0)
    );

    // Per-side longhands: a longhand after the shorthand wins.
    let edge = node_with_id(&tree, "edge");
    let (edge_width, _) = edge.style.css3.border.uniform_width();
    assert_eq!(edge_width, 16.0);
    assert_eq!(edge.style.css3.border.style_name(), "solid");
    let top = &edge.style.css3.border.top;
    assert_eq!(top.width_px, 3.0);
    assert_eq!(top.color.as_deref(), Some("#ff0000"));
    assert_eq!(top.style, "solid");
    let right = &edge.style.css3.border.right;
    assert_eq!(right.width_px, 16.0);
    assert_eq!(right.color.as_deref(), Some("#0000ff"));
    assert_eq!(edge.style.css3.border.bottom.style, "none");

    // `border-style: none` turns the border off even if width/color stay declared.
    let none = node_with_id(&tree, "none");
    let (none_width, _) = none.style.css3.border.uniform_width();
    assert_eq!(none_width, 0.0);
    assert_eq!(none.style.css3.border.style_name(), "none");
    assert_eq!(none.style.css3.border.visible_color(), None);
    assert_eq!(none.style.css3.border.top.width_px, 5.0);

    // Uniform border with a relative width: resolved px + declared unit.
    let relative = node_with_id(&tree, "rel");
    let (relative_width, relative_rel) = relative.style.css3.border.uniform_width();
    assert_eq!(relative_width, 16.0);
    assert_eq!(relative_rel.unwrap().in_unit(Unit::Em), Some(1.0));
    assert_eq!(relative.style.css3.border.visible_color(), Some("#123456"));
    assert_eq!(relative.style.css3.border_radius, [8.0; 4]);
    assert_eq!(
        relative.style.css3.border_radius_rel.unwrap().in_unit(Unit::Rem),
        Some(0.5)
    );
}

#[test]
fn css_v3_features_are_pilot_extensions() {
    let dir = temp_dir("css-v3-subset");
    let path = dir.join("page.html");
    fs::write(&path, CSS_V3_PAGE).unwrap();

    let default = diag_text(&check_document_pilot(CSS_V3_PAGE, &path, &dir, false), false);
    assert!(default.contains("warning[subset]"), "{default}");
    assert!(default.contains("piloto v3"), "{default}");

    let strict = diag_text(&check_document_pilot(CSS_V3_PAGE, &path, &dir, false), true);
    assert!(strict.contains("error[subset]"), "{strict}");
    assert!(strict.contains("piloto v3"), "{strict}");

    let allowed = check_document_pilot(CSS_V3_PAGE, &path, &dir, true);
    assert!(allowed.is_empty(), "{}", diag_text(&allowed, true));
}

// --------------------------------------------- declared colors (superset)

/// The declared-color grammar must keep every form the pre-lane frontend
/// rendered. In particular `border: 2px solid rebeccapurple` must not drop the
/// whole shorthand (width/style/color) when the color is not a CSS3 name.
#[test]
fn declared_color_superset_keeps_borders_and_shadows() {
    let dir = temp_dir("declared-colors");
    let page = r##"<!doctype html>
<html>
<head>
<style>
body { margin: 0; }
#fb1 { background: rebeccapurple; }
#fb2 { background: rgb(255 0 0); }
#fb3 { background: rgb(0,128,0,0.5); }
#fb4 { background: rgba(0,0,0,50%); }
#fc1 { color: rebeccapurple; }
#fc2 { color: rgb(255 0 0); }
#border5 { border: 2px solid rebeccapurple; }
#bc6 { border-style: solid; border-width: 2px; border-color: rebeccapurple; }
#bc4 { border: 1px solid rgba(0,0,0,50%); }
#shadow7 { box-shadow: 0 0 4px rebeccapurple; }
</style>
</head>
<body>
  <div id="fb1">a</div><div id="fb2">b</div><div id="fb3">c</div>
  <div id="fb4">d</div><div id="fc1">e</div><div id="fc2">f</div>
  <div id="border5">g</div><div id="bc6">h</div><div id="bc4">i</div>
  <div id="shadow7">j</div>
</body>
</html>
"##;
    let tree = build(&dir, page);

    // `--pilot --strict-subset`: box-shadow is an allowed v3 extension and the
    // declared colors must not produce diagnostics.
    let path = dir.join("page.html");
    let diagnostics = check_document_pilot(page, &path, &dir, true);
    assert!(
        diagnostics.is_empty(),
        "strict subset rejected a declared color\n{}",
        diag_text(&diagnostics, true)
    );

    for (id, background) in [
        ("fb1", "#663399"),
        ("fb2", "#ff0000"),
        ("fb3", "#008000"),
        ("fb4", "#000000"),
    ] {
        let node = node_with_id(&tree, id);
        assert_eq!(
            node.style.background.as_deref(),
            Some(background),
            "{id} background"
        );
    }
    for (id, color) in [("fc1", "#663399"), ("fc2", "#ff0000")] {
        let node = node_with_id(&tree, id);
        assert_eq!(node.style.color, color, "{id} color");
    }

    // The shorthand must stay expanded: width + style + color.
    for id in ["border5", "bc6"] {
        let node = node_with_id(&tree, id);
        let (width, _) = node.style.css3.border.uniform_width();
        assert_eq!(width, 2.0, "{id} width");
        assert_eq!(node.style.css3.border.style_name(), "solid", "{id} style");
        assert_eq!(
            node.style.css3.border.visible_color(),
            Some("#663399"),
            "{id} color"
        );
        let top = &node.style.css3.border.top;
        assert_eq!(top.width_px, 2.0);
        assert_eq!(top.color.as_deref(), Some("#663399"));
        assert_eq!(top.style, "solid");
    }

    // A 50% alpha keeps the alpha channel in the v3 color string.
    let alpha_border = node_with_id(&tree, "bc4");
    assert_eq!(alpha_border.style.css3.border.top.width_px, 1.0);
    assert_eq!(
        alpha_border.style.css3.border.visible_color(),
        Some("#00000080")
    );

    let shadow = node_with_id(&tree, "shadow7");
    assert_eq!(shadow.style.css3.box_shadow.len(), 1);
    assert_eq!(shadow.style.css3.box_shadow[0].color, "#663399");
}

// ------------------------------------------------------- input nodes

const INPUT_PAGE: &str = r##"<!doctype html>
<html>
<head>
<style>
body { margin: 0; }
.bar { display: flex; flex-direction: column; gap: 10px; }
.search { width: 320px; height: 34px; padding: 7px 9px; font-size: 20px; color: #f2f5f8; background: #1c2430; }
.empty { width: 220px; height: 28px; font-size: 16px; color: #f2f5f8; }
</style>
</head>
<body>
  <div class="bar">
    <input id="search" class="search" type="text" placeholder="Buscar..." value="abc" maxlength="5">
    <input id="empty" class="empty" placeholder="Otro" value="abcdef" maxlength="5">
  </div>
</body>
</html>
"##;

#[test]
fn input_nodes_carry_the_edit_spec() {
    let dir = temp_dir("input-v3");
    let tree = build(&dir, INPUT_PAGE);
    assert!(tree.detailed);
    assert_eq!(tree.nodes.len(), 4);

    let search = node_with_id(&tree, "search");
    assert_eq!(search.kind, KIND_INPUT);
    assert_eq!(search.tag, "input");
    let input = search.input.as_ref().expect("input spec");
    assert_eq!(input.value, "abc");
    assert_eq!(input.placeholder, "Buscar...");
    assert_eq!(input.max_length, 5);
    assert!(!input.password);
    // The runtime shapes the text: the spec has no glyphs or line metrics.
    assert_eq!(search.style.width_px, 320.0);
    assert_eq!(search.style.height_px, 34.0);
    assert_eq!(search.style.gap, 0.0);

    // maxlength truncates the initial value; without `type` it is assumed text.
    let empty = node_with_id(&tree, "empty");
    let empty_input = empty.input.as_ref().expect("input spec");
    assert_eq!(empty_input.value, "abcde");
    assert_eq!(empty_input.max_length, 5);
    assert_eq!(empty_input.placeholder, "Otro");

    // Only input nodes carry `input`.
    for node in &tree.nodes {
        assert_eq!(node.input.is_some(), node.kind == KIND_INPUT, "{}", node.name);
    }
}

#[test]
fn input_is_a_pilot_extension() {
    let dir = temp_dir("input-subset");
    let path = dir.join("page.html");
    fs::write(&path, INPUT_PAGE).unwrap();

    let strict = diag_text(&check_document_pilot(INPUT_PAGE, &path, &dir, false), true);
    assert!(strict.contains("error[subset]"), "{strict}");
    assert!(strict.contains("piloto v2"), "{strict}");
    assert!(strict.contains("<input>"), "{strict}");

    let allowed = check_document_pilot(INPUT_PAGE, &path, &dir, true);
    assert!(allowed.is_empty(), "{}", diag_text(&allowed, true));
}

#[test]
fn input_does_not_leak_into_v1_or_v2_pages() {
    let dir = temp_dir("input-no-leak");
    let v1 = r##"<!doctype html>
<html><head><style>body { background: #ffffff; }</style></head>
<body><div style="width: 10px; height: 10px; background: #111111"></div></body></html>
"##;
    let first = build(&dir, v1);
    assert!(!first.detailed);
    assert!(first.nodes.iter().all(|node| node.kind != KIND_INPUT));
    assert!(first.nodes.iter().all(|node| node.input.is_none()));

    let v2 = r##"<!doctype html>
<html><head><style>.g { display: grid; grid-template-columns: repeat(auto-fill, 80px); }</style></head>
<body><div class="g"><div style="width: 80px; height: 40px"></div></div></body></html>
"##;
    let second = build(&dir, v2);
    assert!(second.detailed);
    assert!(second.nodes.iter().all(|node| node.kind != KIND_INPUT));
    assert!(second.nodes.iter().all(|node| node.input.is_none()));
    assert!(second
        .nodes
        .iter()
        .any(|node| node.style.grid_template_columns.is_some()));
}

// ------------------------------------------------------------ opacity (blob v3)

const OPACITY_PAGE: &str = r##"<!doctype html>
<html>
<head><style>
body { margin: 0; }
.group { width: 100px; height: 50px; background: #ff0000; opacity: 0.5; }
.child { width: 20px; height: 20px; background: #0000ff; }
.default { width: 10px; height: 10px; }
.zero { width: 10px; height: 10px; opacity: 0; }
.half { width: 10px; height: 10px; opacity: 0.5; }
.one { width: 10px; height: 10px; opacity: 1; }
.over { width: 10px; height: 10px; opacity: 1.5; }
.negative { width: 10px; height: 10px; opacity: -0.25; }
.percent { width: 10px; height: 10px; opacity: 50%; }
</style></head>
<body>
  <div id="group" class="group"><div id="child" class="child"></div></div>
  <div id="default" class="default"></div>
  <div id="zero" class="zero"></div>
  <div id="half" class="half"></div>
  <div id="one" class="one"></div>
  <div id="over" class="over"></div>
  <div id="negative" class="negative"></div>
  <div id="percent" class="percent"></div>
</body>
</html>
"##;

#[test]
fn opacity_is_parsed_clamped_and_not_inherited() {
    let dir = temp_dir("opacity-v3");
    let tree = build(&dir, OPACITY_PAGE);

    // Blob v3 is a plain style field: a page that only uses `opacity` does
    // not need the v2 sections and still carries the value.
    assert!(!tree.detailed, "opacity alone must not force the v2 sections");

    assert_eq!(tree.nodes[0].style.opacity, 1.0, "default is opaque");
    assert_eq!(node_with_id(&tree, "group").style.opacity, 0.5);
    assert_eq!(node_with_id(&tree, "zero").style.opacity, 0.0);
    assert_eq!(node_with_id(&tree, "half").style.opacity, 0.5);
    assert_eq!(node_with_id(&tree, "one").style.opacity, 1.0);
    // Out of range clamps like the browser; percentages/unknown are ignored.
    assert_eq!(node_with_id(&tree, "over").style.opacity, 1.0);
    assert_eq!(node_with_id(&tree, "negative").style.opacity, 0.0);
    assert_eq!(node_with_id(&tree, "percent").style.opacity, 1.0);

    // `opacity` is not inherited. The child of a 0.5 group keeps 1.0: the
    // runtime fades the subtree with a group layer instead.
    assert_eq!(node_with_id(&tree, "child").style.opacity, 1.0);
    // The generated text run is not faded either; the owning element's layer
    // covers it. (This page has no text, so check the invariant on the group.)
    for node in &tree.nodes {
        if node.tag == "#text" {
            assert_eq!(node.style.opacity, 1.0, "{}", node.name);
        }
    }

    // The writer emits the current blob version (v7) and records the field
    // byte-for-byte.
    let blob = dir.join("opacity.wnui");
    tree.write_blob(&blob).unwrap();
    let bytes = fs::read(&blob).unwrap();
    assert_eq!(&bytes[0..4], b"WNUI");
    assert_eq!(u32::from_le_bytes(bytes[4..8].try_into().unwrap()), 7);
}

#[test]
fn opacity_subset_accepts_zero_to_one_and_diagnoses_the_rest() {
    let dir = temp_dir("opacity-subset");
    let path = dir.join("page.html");
    fs::write(&path, OPACITY_PAGE).unwrap();

    let accepted = check_document_pilot(OPACITY_PAGE, &path, &dir, true);
    let text = diag_text(&accepted, true);
    // 50% is ignored by the cascade and diagnosed by the strict subset; it is
    // not a pilot feature.
    assert!(text.contains("error[subset]"), "{text}");
    assert!(text.contains("'50%' no soportado en 'opacity'"), "{text}");
    assert!(!text.contains("piloto"), "{text}");
}
