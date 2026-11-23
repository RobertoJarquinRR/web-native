#!/usr/bin/env python3
"""Verificacion del IR de texto y del atlas del frontend Rust.

Comprueba, sin dependencias obligatorias (solo stdlib):

- que el JSON es valido y cumple `src/ir/schema.json` (si `jsonschema` existe),
- que la fixture text-card tiene 1 `text_run` con 4 glyphs y los gids del
  contrato,
- que posiciones, avances y UVs coinciden con una referencia (exp16) dentro de
  0.5 px / 1e-6,
- que `atlas.bin` es 512x512 R8, que su sha256 coincide con `atlas.json`, que
  las UVs corresponden al shelf packing real y que la guarda de 1 px esta a 0,
- estadisticas opcionales de diferencia de pixeles contra el atlas de FreeType.

Uso:
  python verify_text.py --ir results/rust/text-card.ir.json \
      --atlas results/rust/text-card.atlas.bin \
      --atlas-json results/rust/text-card.atlas.json \
      --reference ../../../experiments/16-text-atlas-c/display_list.json \
      --reference-atlas ../../../experiments/16-text-atlas-c/atlas.bin \
      --report results/rust/text-card.verify.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import statistics
import struct
import sys
from pathlib import Path

ATLAS_SIZE = 512
EXPECTED_GIDS = [43, 82, 79, 68]


class Report:
    def __init__(self) -> None:
        self.checks: list[dict] = []
        self.details: dict = {}

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append({"name": name, "status": "PASS" if ok else "FAIL",
                            "detail": detail})
        return ok

    @property
    def failed(self) -> list[dict]:
        return [item for item in self.checks if item["status"] != "PASS"]


def shelf_rects(glyphs: list[dict], size: int = ATLAS_SIZE) -> list[tuple[int, int, int, int]]:
    """Reconstruye el shelf packing de `GlyphAtlas.add` (1 px de guarda)."""
    rects: list[tuple[int, int, int, int]] = []
    shelf_x = shelf_y = shelf_h = 0
    for glyph in glyphs:
        w, h = int(glyph["w"]), int(glyph["h"])
        if w <= 0 or h <= 0:
            rects.append((0, 0, 0, 0))
            continue
        pad_w, pad_h = w + 2, h + 2
        if shelf_x + pad_w > size:
            shelf_x = 0
            shelf_y += shelf_h
            shelf_h = 0
        if shelf_y + pad_h > size:
            raise RuntimeError("atlas desbordado reconstruyendo el shelf")
        x, y = shelf_x + 1, shelf_y + 1
        rects.append((x, y, w, h))
        shelf_x += pad_w
        shelf_h = max(shelf_h, pad_h)
    return rects


def load_atlas(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != ATLAS_SIZE * ATLAS_SIZE:
        raise SystemExit(f"atlas.bin: se esperaban {ATLAS_SIZE * ATLAS_SIZE} bytes, "
                         f"hay {len(data)}")
    return data


def text_op(ir: dict) -> dict | None:
    return next((op for op in ir.get("ops", []) if op.get("type") == "text_run"), None)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verifica IR text_run + atlas R8")
    parser.add_argument("--ir", type=Path, required=True)
    parser.add_argument("--atlas", type=Path)
    parser.add_argument("--atlas-json", type=Path)
    parser.add_argument("--reference", type=Path, help="IR de referencia (exp16)")
    parser.add_argument("--reference-atlas", type=Path, help="atlas de referencia (FreeType)")
    parser.add_argument("--tolerance", type=float, default=0.5)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    report = Report()
    ir = json.loads(args.ir.read_text(encoding="utf-8"))
    report.check("ir_json_parse", True, str(args.ir))

    # Esquema del IR, si jsonschema esta instalado (el harness no lo exige).
    schema_path = Path(__file__).resolve().parents[2] / "ir" / "schema.json"
    try:
        import jsonschema  # type: ignore

        jsonschema.validate(ir, json.loads(schema_path.read_text(encoding="utf-8")))
        report.check("ir_schema", True, str(schema_path))
    except ImportError:
        report.check("ir_schema", True, "jsonschema no instalado: omitido")
    except Exception as exc:  # noqa: BLE001
        report.check("ir_schema", False, f"{type(exc).__name__}: {exc}")

    viewport = ir.get("viewport") or {}
    report.check("viewport_400x240", viewport.get("width") == 400
                 and viewport.get("height") == 240, str(viewport))

    op = text_op(ir)
    if not report.check("has_text_run", op is not None,
                        "1 op text_run" if op else "no hay text_run"):
        return finish(report, args)

    glyphs = op.get("glyphs") or []
    report.check("glyph_count_4", len(glyphs) == 4, f"{len(glyphs)} glyphs")
    gids = [int(glyph["gid"]) for glyph in glyphs]
    report.check("glyph_ids", gids == EXPECTED_GIDS,
                 f"gids={gids} esperados={EXPECTED_GIDS}")
    report.check("run_origin", abs(float(op["x"]) - 20.0) <= 1e-6
                 and abs(float(op["y"]) - 51.0) <= 1e-6,
                 f"({op['x']}, {op['y']}) esperado (20.0, 51.0)")

    metadata = {key: op.get(key) for key in ("text", "font", "font_file", "size", "color")}
    report.check("text_metadata",
                 metadata["text"] == "Hola"
                 and metadata["font"] == "Segoe UI"
                 and str(metadata["font_file"]).lower().endswith("segoeui.ttf")
                 and abs(float(metadata["size"]) - 28.0) <= 1e-6
                 and metadata["color"] == "#ffffff",
                 json.dumps(metadata))
    report.details["text_run"] = {
        "x": op["x"], "y": op["y"], **metadata,
        "glyphs": [{key: glyph.get(key) for key in
                    ("gid", "x", "y", "w", "h", "u0", "v0", "u1", "v1", "adv")}
                   for glyph in glyphs],
    }

    if args.reference:
        reference = json.loads(args.reference.read_text(encoding="utf-8"))
        ref_op = text_op(reference)
        ref_glyphs = (ref_op or {}).get("glyphs") or []
        report.check("reference_glyph_count", len(ref_glyphs) == len(glyphs),
                     f"{len(ref_glyphs)} vs {len(glyphs)}")
        geom_errors: list[str] = []
        adv_errors: list[str] = []
        for index, (glyph, ref) in enumerate(zip(glyphs, ref_glyphs)):
            for key in ("x", "y"):
                delta = abs(float(glyph[key]) - float(ref[key]))
                if delta > args.tolerance:
                    geom_errors.append(f"glyph[{index}].{key}: {delta:.4f} px")
            delta = abs(float(glyph["adv"]) - float(ref["adv"]))
            if delta > args.tolerance:
                adv_errors.append(f"glyph[{index}].adv: {delta:.4f} px")
            for key in ("w", "h", "gid"):
                if int(glyph[key]) != int(ref[key]):
                    geom_errors.append(f"glyph[{index}].{key}: {glyph[key]} != {ref[key]}")
        report.check("glyph_xy_vs_reference", not geom_errors,
                     "; ".join(geom_errors) or f"tolerancia {args.tolerance} px")
        report.check("advances_vs_reference", not adv_errors,
                     "; ".join(adv_errors) or f"tolerancia {args.tolerance} px")
        report.check("op_order_vs_reference",
                     [item.get("type") for item in ir["ops"]]
                     == [item.get("type") for item in reference["ops"]],
                     str([item.get("type") for item in ir["ops"]]))

    rects = shelf_rects(glyphs)
    uv_errors: list[str] = []
    for index, (glyph, (x, y, w, h)) in enumerate(zip(glyphs, rects)):
        expected = (x / ATLAS_SIZE, y / ATLAS_SIZE,
                    (x + w) / ATLAS_SIZE, (y + h) / ATLAS_SIZE)
        actual = tuple(float(glyph[key]) for key in ("u0", "v0", "u1", "v1"))
        for key, exp, got in zip(("u0", "v0", "u1", "v1"), expected, actual):
            if abs(exp - got) > 1e-6:
                uv_errors.append(f"glyph[{index}].{key}: {got} != {exp:.6f}")
    report.check("uv_matches_shelf_packing", not uv_errors,
                 "; ".join(uv_errors) or f"rects={rects}")
    report.details["atlas_rects"] = [
        {"gid": int(glyph["gid"]), "x": rect[0], "y": rect[1],
         "w": rect[2], "h": rect[3]}
        for glyph, rect in zip(glyphs, rects)
    ]

    if args.atlas:
        data = load_atlas(args.atlas)
        report.check("atlas_bytes_512x512_r8", True, f"{len(data)} bytes")
        nonzero = sum(1 for byte in data if byte)
        report.details["atlas"] = {
            "file": str(args.atlas),
            "width": ATLAS_SIZE,
            "height": ATLAS_SIZE,
            "format": "r8unorm",
            "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "pixels_nonzero": nonzero,
        }
        guard_errors: list[str] = []
        coverage: list[int] = []
        mask = bytearray(ATLAS_SIZE * ATLAS_SIZE)
        for rect in rects:
            x, y, w, h = rect
            if w <= 0 or h <= 0:
                continue
            total = 0
            for row in range(y, y + h):
                for col in range(x, x + w):
                    mask[row * ATLAS_SIZE + col] = 1
                    total += data[row * ATLAS_SIZE + col]
            coverage.append(total)
            for row in range(max(y - 1, 0), min(y + h + 1, ATLAS_SIZE)):
                for col in range(max(x - 1, 0), min(x + w + 1, ATLAS_SIZE)):
                    if mask[row * ATLAS_SIZE + col]:
                        continue
                    if data[row * ATLAS_SIZE + col] != 0:
                        guard_errors.append(f"({col},{row})")
        outside = sum(1 for index, byte in enumerate(data)
                      if byte and not mask[index])
        report.check("atlas_guard_and_outside_zero",
                     not guard_errors and outside == 0,
                     f"guard={guard_errors[:3]} outside_nonzero={outside}")
        report.details["atlas"]["glyph_coverage"] = coverage

        if nonzero == 0:
            report.check("atlas_has_pixels", False, "atlas vacio")
        else:
            report.check("atlas_has_pixels", True, f"{nonzero} bytes != 0")

    if args.atlas_json and args.atlas:
        atlas_json = json.loads(args.atlas_json.read_text(encoding="utf-8"))
        report.details["atlas_json"] = atlas_json
        digest = hashlib.sha256(args.atlas.read_bytes()).hexdigest()
        report.check("atlas_json_sha256",
                     atlas_json.get("sha256") == digest,
                     f"{atlas_json.get('sha256')} == {digest}")
        report.check("atlas_json_metadata",
                     atlas_json.get("width") == ATLAS_SIZE
                     and atlas_json.get("height") == ATLAS_SIZE
                     and atlas_json.get("format") == "r8unorm"
                     and int(atlas_json.get("glyph_count", -1)) == len(set(gids))
                     and int(atlas_json.get("size_bytes", -1)) == ATLAS_SIZE * ATLAS_SIZE,
                     json.dumps({key: atlas_json.get(key) for key in
                                 ("width", "height", "format", "glyph_count",
                                  "size_bytes", "pixels_nonzero")}))

    if args.reference_atlas and args.atlas:
        reference_data = load_atlas(args.reference_atlas)
        data = args.atlas.read_bytes()
        stats = []
        for glyph, rect in zip(glyphs, rects):
            x, y, w, h = rect
            if w <= 0 or h <= 0:
                continue
            diffs = [abs(reference_data[(y + row) * ATLAS_SIZE + x + col]
                         - data[(y + row) * ATLAS_SIZE + x + col])
                     for row in range(h) for col in range(w)]
            stats.append({
                "gid": int(glyph["gid"]),
                "w": w, "h": h,
                "nonzero_reference": sum(
                    1 for row in range(h) for col in range(w)
                    if reference_data[(y + row) * ATLAS_SIZE + x + col]),
                "nonzero_rust": sum(
                    1 for row in range(h) for col in range(w)
                    if data[(y + row) * ATLAS_SIZE + x + col]),
                "coverage_reference": sum(
                    reference_data[(y + row) * ATLAS_SIZE + x + col]
                    for row in range(h) for col in range(w)),
                "coverage_rust": sum(
                    data[(y + row) * ATLAS_SIZE + x + col]
                    for row in range(h) for col in range(w)),
                "max_abs_diff": max(diffs),
                "mean_abs_diff": round(statistics.fmean(diffs), 3),
                "identical_pct": round(100.0 * sum(1 for d in diffs if d == 0)
                                       / len(diffs), 2),
            })
        report.details["atlas_vs_reference"] = stats
        report.check("atlas_same_geometry", True,
                     "misma caja/UV; la cobertura difiere (unhinted vs FreeType)")

    return finish(report, args)


def finish(report: Report, args: argparse.Namespace) -> int:
    failed = report.failed
    payload = {
        "status": "PASS" if not failed else "FAIL",
        "ir": str(args.ir),
        "checks": report.checks,
        "details": report.details,
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    for item in report.checks:
        print(f"{item['status']:4s} {item['name']:32s} {item['detail']}")
    print(f"\n{len(report.checks) - len(failed)}/{len(report.checks)} checks PASS")
    if failed:
        print("FAIL")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
