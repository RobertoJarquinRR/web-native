//! Color parity for `data-hover` / `data-press` / `data-selected`.
//!
//! The fixture exercises the three alpha-bearing forms the runtime blends:
//!
//! * `rgba(255,0,0,0.5)`  -> `0x80FF0000`
//! * `#ff000080`          -> `0x80FF0000`
//! * `hsl(0,100%,50%)`    -> `0xFFFF0000`
//!
//! The golden `tests/golden/color_hover.h` is the output of
//! `emit_ui.py --ui-json` (regenerate with `tests/make_color_hover_golden.py`),
//! so comparing the whole header proves `frontend-rust --emit-header` is
//! byte-identical to the Python emitter without a Python dependency in CI.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

fn exe() -> &'static str {
    env!("CARGO_BIN_EXE_frontend-rust")
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

fn assert_same_bytes(actual: &[u8], expected: &[u8], label: &str) {
    if actual == expected {
        return;
    }
    let end = actual.len().min(expected.len());
    let index = (0..end)
        .find(|index| actual[*index] != expected[*index])
        .unwrap_or(end);
    let line = 1 + expected[..index].iter().filter(|byte| **byte == b'\n').count();
    panic!(
        "{label}: first difference at byte {index} (expected line {line}); \
         actual {} bytes, golden {} bytes",
        actual.len(),
        expected.len(),
    );
}

#[test]
fn data_hover_alpha_matches_the_python_emitter() {
    let dir = temp_dir("color_header_hover");
    let fixture = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/color_hover");
    fs::copy(fixture.join("index.html"), dir.join("index.html")).unwrap();
    fs::copy(fixture.join("style.css"), dir.join("style.css")).unwrap();

    let ui = dir.join("ui.json");
    let header = dir.join("ui_data.h");
    let Output {
        status,
        stdout,
        stderr,
    } = Command::new(exe())
        .arg("--html")
        .arg(dir.join("index.html"))
        .arg("--css")
        .arg(dir.join("style.css"))
        .arg("--width")
        .arg("320")
        .arg("--height")
        .arg("200")
        .arg("--emit-ui")
        .arg(&ui)
        .arg("--emit-header")
        .arg(&header)
        .current_dir(&dir)
        .output()
        .unwrap();
    assert!(
        status.success(),
        "frontend-rust failed\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&stdout),
        String::from_utf8_lossy(&stderr),
    );

    // The tree keeps the raw attribute and the alpha-bearing ARGB: that is
    // exactly what `emit_ui.py --ui-json` reads back.
    let tree = fs::read_to_string(&ui).unwrap();
    for (raw, argb) in [
        ("rgba(255,0,0,0.5)", 0x80FF_0000u32),
        ("#ff000080", 0x80FF_0000),
        ("hsl(0,100%,50%)", 0xFFFF_0000),
    ] {
        assert!(
            tree.contains(&format!("\"hover\": \"{raw}\"")),
            "ui.json lost data-hover={raw}"
        );
        assert!(
            tree.contains(&format!("\"hover\": {argb}")),
            "ui.json hover {raw} is not {argb:#010x}"
        );
    }
    // `background`/`color` keep the v1 opaque normalization.
    assert!(tree.contains("\"background\": \"#101010\""));
    assert!(tree.contains("\"background\": 4279242768")); // 0xFF101010

    let bytes = fs::read(&header).unwrap();
    assert_same_bytes(
        &bytes,
        include_bytes!("golden/color_hover.h"),
        "color_hover header",
    );
}
