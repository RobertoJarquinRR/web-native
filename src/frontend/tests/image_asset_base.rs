//! Asset-base regression: intrinsic image sizes are baked from the document's
//! directory, not from wherever the `.wnui` blob is written.
//!
//! Pre-consolidation the image `src` resolved against the `ui.json` directory;
//! after the blob-only refactor the writer briefly used the blob's parent, so
//! writing a fixture blob next to its `builtin_blob.h` (a different directory
//! than the page) zeroed the intrinsic size. These tests pin the values that
//! the dynamic fixture (104x60) and the offscreen cache fixture (512x512)
//! carried before the regression.

use std::fs;
use std::path::{Path, PathBuf};

use frontend_rust::build_tree_from_file;

const NODE_RECORD_SIZE: usize = 220;
const IMAGE_WIDTH_OFFSET: usize = 132;
const IMAGE_HEIGHT_OFFSET: usize = 136;

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("wn-asset-base-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

/// Minimal PNG header: `image_size` only reads the signature and IHDR.
fn write_png(path: &Path, width: u32, height: u32) {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"\x89PNG\r\n\x1a\n");
    bytes.extend_from_slice(&13u32.to_be_bytes());
    bytes.extend_from_slice(b"IHDR");
    bytes.extend_from_slice(&width.to_be_bytes());
    bytes.extend_from_slice(&height.to_be_bytes());
    bytes.extend_from_slice(&[8, 2, 0, 0, 0]);
    bytes.extend_from_slice(&[0, 0, 0, 0]);
    fs::write(path, bytes).unwrap();
}

/// Intrinsic `(width, height)` pairs stored in a blob's node records.
fn blob_image_sizes(blob: &[u8]) -> Vec<(f32, f32)> {
    let section_count = u32::from_le_bytes(blob[8..12].try_into().unwrap()) as usize;
    let node_count = u32::from_le_bytes(blob[16..20].try_into().unwrap()) as usize;
    let mut nodes_offset = None;
    for index in 0..section_count {
        let entry = 32 + index * 16;
        let id = u32::from_le_bytes(blob[entry..entry + 4].try_into().unwrap());
        if id == 1 {
            nodes_offset =
                Some(u32::from_le_bytes(blob[entry + 4..entry + 8].try_into().unwrap()) as usize);
        }
    }
    let nodes_offset = nodes_offset.expect("blob has no node section");
    let mut sizes = Vec::new();
    for index in 0..node_count {
        let base = nodes_offset + index * NODE_RECORD_SIZE;
        let width = f32::from_le_bytes(
            blob[base + IMAGE_WIDTH_OFFSET..base + IMAGE_WIDTH_OFFSET + 4]
                .try_into()
                .unwrap(),
        );
        let height = f32::from_le_bytes(
            blob[base + IMAGE_HEIGHT_OFFSET..base + IMAGE_HEIGHT_OFFSET + 4]
                .try_into()
                .unwrap(),
        );
        if width != 0.0 || height != 0.0 {
            sizes.push((width, height));
        }
    }
    sizes
}

fn assert_intrinsic_survives_relocation(name: &str, image: (u32, u32)) {
    let root = temp_dir(name);
    let page = root.join("page");
    let images = page.join("images");
    fs::create_dir_all(&images).unwrap();
    write_png(&images.join("pic.png"), image.0, image.1);

    // Only a width: the height must come from the image's intrinsic ratio.
    let html = r#"<!doctype html>
<html><head><style>.pic { width: 100px; }</style></head>
<body><img class="pic" src="images/pic.png" alt="pic"></body></html>
"#;
    let html_path = page.join("index.html");
    fs::write(&html_path, html).unwrap();

    let tree = build_tree_from_file(&html_path, (480, 320)).unwrap();

    // Write the blob somewhere unrelated to the HTML directory.
    let elsewhere = root.join("fixture");
    fs::create_dir_all(&elsewhere).unwrap();
    let relocated = elsewhere.join("app.wnui");
    tree.write_blob(&relocated).unwrap();

    let expected = (image.0 as f32, image.1 as f32);
    assert_eq!(
        blob_image_sizes(&fs::read(&relocated).unwrap()),
        vec![expected],
        "{name}: intrinsic image size changed when the blob moved"
    );

    // Writing next to the page and writing elsewhere must be byte-identical.
    let beside_page = page.join("app.wnui");
    tree.write_blob(&beside_page).unwrap();
    assert_eq!(
        fs::read(&beside_page).unwrap(),
        fs::read(&relocated).unwrap(),
        "{name}: blob bytes depend on the output directory"
    );
}

#[test]
fn dynamic_fixture_keeps_104x60_when_written_elsewhere() {
    assert_intrinsic_survives_relocation("dynamic", (104, 60));
}

#[test]
fn cache_fixture_keeps_512x512_when_written_elsewhere() {
    assert_intrinsic_survives_relocation("cache", (512, 512));
}
