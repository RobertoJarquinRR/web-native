//! Structural guards for `--emit-header` (`ui_data.h`).
//!
//! These run the real binary on small fixtures; byte parity with
//! `emit_ui.py --ui-json` is covered by the acceptance script, while these
//! tests make sure the conditional parts of the header (node count, E1 extras,
//! input include) cannot silently disappear.

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

fn emit(dir: &Path, html: &str, css: &str, extra: &[&str]) -> String {
    fs::write(dir.join("index.html"), html).unwrap();
    fs::write(dir.join("style.css"), css).unwrap();
    let header = dir.join("ui_data.h");
    let mut command = Command::new(exe());
    command
        .arg("--html")
        .arg(dir.join("index.html"))
        .arg("--css")
        .arg(dir.join("style.css"))
        .arg("--width")
        .arg("320")
        .arg("--height")
        .arg("200")
        .arg("--emit-ui")
        .arg(dir.join("ui.json"))
        .arg("--emit-header")
        .arg(&header);
    for arg in extra {
        command.arg(arg);
    }
    let Output {
        status,
        stderr,
        stdout,
    } = command.current_dir(dir).output().unwrap();
    assert!(
        status.success(),
        "frontend-rust failed\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&stdout),
        String::from_utf8_lossy(&stderr),
    );
    fs::read_to_string(&header).unwrap()
}

/// Small page that uses none of the E1 fields and has no `<input>`: no extras
/// block, no document include and no `kUiInputs` table.
#[test]
fn header_without_extras_or_inputs() {
    let dir = temp_dir("emit_header_plain");
    let header = emit(
        &dir,
        "<!doctype html><html><head><link rel=\"stylesheet\" href=\"style.css\"></head><body><div class=\"a\">Hola</div></body></html>",
        ".a { width: 100px; height: 20px; }",
        &[],
    );
    // body.root + div.a + text.
    assert!(header.contains("#define UI_NODE_COUNT 3"), "{header}");
    assert!(header.contains("#define UI_VIEWPORT_WIDTH 320"));
    assert!(header.contains("#define UI_VIEWPORT_HEIGHT 200"));
    assert!(!header.contains("UI_STYLE_EXTRAS"), "{header}");
    assert!(!header.contains("int text_align;"));
    assert!(!header.contains("wn_document_format.h"));
    assert!(!header.contains("kUiInputs"));
    assert!(!header.contains("R2_NODE_INPUT"));
    assert!(header.ends_with("#endif /* UI_DATA_H */\n"), "{header}");
}

/// `* { box-sizing: border-box }` makes every node use an E1 field, so the
/// struct gains the seven fields and every node line the seven values.
#[test]
fn header_with_extras() {
    let dir = temp_dir("emit_header_extras");
    let header = emit(
        &dir,
        "<!doctype html><html><head><link rel=\"stylesheet\" href=\"style.css\"></head><body><div class=\"a\">Hola</div></body></html>",
        "* { box-sizing: border-box; } .a { width: 100px; }",
        &[],
    );
    assert!(header.contains("#define UI_STYLE_EXTRAS 1"), "{header}");
    assert!(header.contains("int box_sizing;"));
    assert!(header.contains("float flex_basis;"));
    // Node line tail: box_sizing, flex_grow, flex_shrink, flex_basis.
    assert!(header.contains(", 0, 0.0000f, 0, 1, 0.0000f, 1.0000f, -1.0000f, "));
}

/// `<input type="text">` under `--pilot` adds `R2_NODE_INPUT`, the document
/// include, `UI_INPUT_COUNT` and the `kUiInputs` table.
#[test]
fn header_with_input() {
    let dir = temp_dir("emit_header_input");
    let header = emit(
        &dir,
        "<!doctype html><html><head><link rel=\"stylesheet\" href=\"style.css\"></head><body><input type=\"text\" maxlength=\"8\" placeholder=\"Buscar\"></body></html>",
        "input { width: 200px; height: 24px; }",
        &["--pilot"],
    );
    assert!(header.contains("R2_NODE_INPUT"), "{header}");
    assert!(header.contains("#include \"document/wn_document_format.h\""));
    assert!(header.contains("#define UI_INPUT_COUNT 1"));
    assert!(header.contains("kUiInputs[UI_INPUT_COUNT > 0 ? UI_INPUT_COUNT : 1]"));
    // body.root + input.
    assert!(header.contains("#define UI_NODE_COUNT 2"), "{header}");
    assert!(header.contains("    { 1, \"\", \"Buscar\", 8, 0 },"), "{header}");
}
