//! Byte-level contract for the v1 `overflow` field.
//!
//! `overflow: hidden` is a v1 style field, not a v2/v3 feature: it has to reach
//! the blob even when the page stays on the historical schema
//! (`Tree::detailed == false`). The writer used to zero it there, and every
//! other fixture happened to need the v2 fields, so the regression only showed
//! on a plain page with an `overflow: hidden` box.

use std::fs;
use std::path::{Path, PathBuf};

use frontend_rust::build_tree;

// Blob v5 `WnBlobNode`: 252 bytes; `overflow` is the i32 right after the four
// grid/gap fields, at byte 128 of the record.
const NODE_RECORD_SIZE: usize = 252;
const OVERFLOW_OFFSET: usize = 128;
const OVERFLOW_HIDDEN: i32 = 1;

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

/// Offset of the node section: the header is 32 bytes and every section entry
/// is 16 bytes (`id`, `offset`, `size`, `flags`).
fn nodes_offset(blob: &[u8]) -> usize {
    let section_count = u32::from_le_bytes(blob[8..12].try_into().unwrap()) as usize;
    for index in 0..section_count {
        let entry = 32 + index * 16;
        let id = u32::from_le_bytes(blob[entry..entry + 4].try_into().unwrap());
        if id == 1 {
            return u32::from_le_bytes(blob[entry + 4..entry + 8].try_into().unwrap()) as usize;
        }
    }
    panic!("blob has no node section");
}

fn overflow_of_node(blob: &[u8], index: usize) -> i32 {
    let base = nodes_offset(blob) + index * NODE_RECORD_SIZE;
    i32::from_le_bytes(blob[base + OVERFLOW_OFFSET..base + OVERFLOW_OFFSET + 4].try_into().unwrap())
}

const PAGE_WITH_HIDDEN: &str = r##"<!doctype html>
<html><head><style>
  body { margin: 0; width: 200px; height: 100px; overflow: hidden; background: #ffffff; }
  .box { width: 50px; height: 50px; background: #3366cc; }
</style></head>
<body><div id="box" class="box"></div></body></html>
"##;

const PAGE_WITHOUT_HIDDEN: &str = r##"<!doctype html>
<html><head><style>
  body { margin: 0; width: 200px; height: 100px; background: #ffffff; }
  .box { width: 50px; height: 50px; background: #3366cc; }
</style></head>
<body><div id="box" class="box"></div></body></html>
"##;

#[test]
fn overflow_hidden_reaches_a_page_without_v2_or_v3_fields() {
    let dir = temp_dir("overflow-hidden-flat");
    let tree = build_tree(PAGE_WITH_HIDDEN, &dir, (200, 100))
        .unwrap_or_else(|err| panic!("pipeline failed: {err}"));
    assert!(
        !tree.detailed,
        "overflow: hidden alone must not force the v2 sections"
    );

    let blob = dir.join("overflow-hidden.wnui");
    tree.write_blob(&blob).unwrap();
    let bytes = fs::read(&blob).unwrap();
    assert_eq!(&bytes[0..4], b"WNUI");
    assert_eq!(u32::from_le_bytes(bytes[4..8].try_into().unwrap()), 7);

    // Node 0 is the body root; the field is not inherited by the box.
    assert_eq!(overflow_of_node(&bytes, 0), OVERFLOW_HIDDEN);
    assert_eq!(overflow_of_node(&bytes, 1), 0, "overflow is not inherited");
}

#[test]
fn a_flat_page_without_overflow_still_writes_zero() {
    let dir = temp_dir("overflow-default-flat");
    let tree = build_tree(PAGE_WITHOUT_HIDDEN, &dir, (200, 100))
        .unwrap_or_else(|err| panic!("pipeline failed: {err}"));
    assert!(!tree.detailed);

    let blob = dir.join("overflow-default.wnui");
    tree.write_blob(&blob).unwrap();
    let bytes = fs::read(&blob).unwrap();
    assert_eq!(overflow_of_node(&bytes, 0), 0);
}
