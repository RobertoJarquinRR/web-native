#!/usr/bin/env python3
"""Visual verifier for the media-library pilot (A0 fixture).

Runs in two layers:

1. Fixture layer (always): catalog <-> posters <-> page/index.html <->
   expected.json, plus an independent recomputation of the grid math from
   page/style.css.
2. Runtime layer (when a pilot R2 binary is available): offscreen renders at
   1280x800 and 800x600, layout-report geometry, poster pixels, clipping and
   wheel/scroll offset checks.

The runtime needs the v2 pilot features (grid, local images, clipped vertical
scroll) and an offscreen scroll injection. On a checkout without a pilot
runtime (or one built from a different page), this script exits 2 with
`ENGINE FEATURE MISSING` and the exact blockers; the error text also lists the
runtime contract needed for PASS.

Usage:
    .venv\\Scripts\\python verify.py
    .venv\\Scripts\\python verify.py --runtime path\\to\\runtime_r2_pilot.exe
    .venv\\Scripts\\python verify.py --fixtures-only

Exit codes:
    0 PASS (or fixtures-only PASS)
    1 FAIL (fixture drift, wrong layout/pixels)
    2 ENGINE FEATURE MISSING / runtime or Pillow not available
"""

from __future__ import annotations

import argparse
import hashlib
import html as html_mod
import json
import math
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_OUT = HERE / "build" / "verify"
VIEWPORTS = ("1280x800", "800x600")
EXPECTED_SAMPLE_KEYS = ("bg", "rule", "footer")
TOL = 4  # per-channel tolerance for rendered colour samples

try:
    from PIL import Image, ImageChops
except ImportError:  # handled in main with a helpful message
    Image = None
    ImageChops = None


# --------------------------------------------------------------------- state

CHECKS: list[dict] = []
BLOCKERS: list[dict] = []
NOTES: list[str] = []
RUNTIME_CWD: Path | None = None  # page/ by default: image src is HTML-relative


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def fail_check(group: str, name: str, detail: str) -> bool:
    return check(group, name, False, detail)


def blocker(feature: str, detail: str) -> None:
    BLOCKERS.append({"feature": feature, "detail": detail})
    print(f"[MISSING] {feature}: {detail}")


def note(text: str) -> None:
    NOTES.append(text)
    print(f"[note] {text}")


def close(a, b, eps: float = 0.02) -> bool:
    return abs(float(a) - float(b)) <= eps


def rect_close(a: dict, b: dict, eps: float = 0.51) -> bool:
    return all(close(a.get(k, -9999), b.get(k, -9999), eps) for k in ("x", "y", "w", "h"))


def rgb_close(a, b, tol: int = TOL) -> bool:
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def hex_rgb(text: str) -> tuple[int, int, int]:
    text = text.strip().lstrip("#")
    return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)


def px(img, x: float, y: float):
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --------------------------------------------------------------- fixture load

def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def validate_catalog(catalog: dict) -> list[dict] | None:
    items = catalog.get("items")
    ok = isinstance(items, list) and 24 <= len(items) <= 36
    if not check("fixtures/catalog", "item count 24..36", ok,
                 f"{len(items) if isinstance(items, list) else 'n/a'} items"):
        return None

    ids = set()
    problems: list[str] = []
    required = ("id", "title", "path", "duration_ms", "width", "height", "tags", "poster", "favorite")
    for index, item in enumerate(items):
        if not isinstance(item, dict):
            problems.append(f"[{index}] not an object")
            continue
        missing = [key for key in required if key not in item]
        if missing:
            problems.append(f"[{index}] missing {missing}")
            continue
        if not re.fullmatch(r"vid-\d{3}", str(item["id"])):
            problems.append(f"[{index}] bad id {item['id']!r}")
        if item["id"] in ids:
            problems.append(f"[{index}] duplicate id {item['id']}")
        ids.add(item["id"])
        if not isinstance(item["title"], str) or not item["title"].strip():
            problems.append(f"[{index}] empty title")
        if not isinstance(item["path"], str) or not item["path"]:
            problems.append(f"[{index}] empty path")
        if not isinstance(item["duration_ms"], int) or item["duration_ms"] <= 0:
            problems.append(f"[{index}] bad duration_ms")
        if not isinstance(item["width"], int) or item["width"] <= 0:
            problems.append(f"[{index}] bad width")
        if not isinstance(item["height"], int) or item["height"] <= 0:
            problems.append(f"[{index}] bad height")
        if not isinstance(item["tags"], list) or not (1 <= len(item["tags"]) <= 4) or \
                not all(isinstance(tag, str) and tag for tag in item["tags"]):
            problems.append(f"[{index}] bad tags")
        if not isinstance(item["poster"], str) or not item["poster"].startswith("posters/") \
                or not item["poster"].endswith(".png"):
            problems.append(f"[{index}] bad poster path")
        if not isinstance(item["favorite"], bool):
            problems.append(f"[{index}] favorite must be bool")

    check("fixtures/catalog", "schema/fields/ids", not problems,
          "all required fields valid" if not problems else "; ".join(problems[:6]))
    return items if not problems else None


def validate_posters(items: list[dict], expected: dict, manifest: dict | None) -> dict | None:
    if Image is None:
        return None
    sample_points = expected.get("poster", {}).get("sample_points", {})
    if set(sample_points) != set(EXPECTED_SAMPLE_KEYS):
        fail_check("fixtures/posters", "sample points", f"expected {list(EXPECTED_SAMPLE_KEYS)}, got {list(sample_points)}")
        return None
    if manifest is not None:
        check("fixtures/posters", "manifest count", manifest.get("count") == len(items),
              f"{manifest.get('count')} in manifest, {len(items)} catalog items")
        check("fixtures/posters", "manifest sample points",
              manifest.get("sample_points") == sample_points,
              f"{manifest.get('sample_points')} == {sample_points}")

    manifest_by_id = {}
    if manifest:
        manifest_by_id = {entry.get("id"): entry for entry in manifest.get("posters", [])}
        manifest_ids = set(manifest_by_id)
        catalog_ids = {item["id"] for item in items}
        check("fixtures/posters", "manifest ids match catalog", manifest_ids == catalog_ids,
              f"{len(manifest_ids)} ids" if manifest_ids == catalog_ids
              else f"only manifest={sorted(manifest_ids - catalog_ids)[:4]} only catalog={sorted(catalog_ids - manifest_ids)[:4]}")

    poster_data: dict[str, dict] = {}
    problems: list[str] = []
    accents: dict[tuple[int, int, int], str] = {}
    for item in items:
        path = (HERE / item["poster"]).resolve()
        try:
            path.relative_to((HERE / "posters").resolve())
        except ValueError:
            problems.append(f"{item['id']}: poster escapes posters/")
            continue
        if not path.is_file():
            problems.append(f"{item['id']}: missing {path.name}")
            continue
        try:
            with Image.open(path) as raw:
                raw.load()
                img = raw.convert("RGB")
        except Exception as exc:  # noqa: BLE001 - report as fixture problem
            problems.append(f"{item['id']}: cannot open PNG ({exc})")
            continue
        if img.size != (expected["poster"]["width"], expected["poster"]["height"]):
            problems.append(f"{item['id']}: size {img.size}")
            continue
        samples = {key: px(img, point[0], point[1]) for key, point in sample_points.items()}
        accent = samples["footer"]
        if accent in accents:
            problems.append(f"{item['id']}: accent collides with {accents[accent]}")
        accents[accent] = item["id"]
        if rgb_close(samples["bg"], accent, 20):
            problems.append(f"{item['id']}: bg/footer too similar")
        if rgb_close(accent, hex_rgb(expected["colors"]["page"]), 20) or \
                rgb_close(accent, hex_rgb(expected["colors"]["card"]), 20):
            problems.append(f"{item['id']}: accent too close to UI colour")

        entry = manifest_by_id.get(item["id"])
        if entry:
            if entry.get("file") != item["poster"]:
                problems.append(f"{item['id']}: manifest path {entry.get('file')!r}")
            if entry.get("sha256") != sha256(path):
                problems.append(f"{item['id']}: manifest sha256 drift")
            manifest_colours = {
                "bg": entry.get("bg"),
                "rule": entry.get("accent"),
                "footer": entry.get("accent"),
            }
            for key, value in samples.items():
                if manifest_colours[key] != "#{:02x}{:02x}{:02x}".format(*value):
                    problems.append(f"{item['id']}: manifest {key} colour drift")

        poster_data[item["id"]] = {
            "path": path,
            "samples": samples,
            "accent": accent,
        }

    check("fixtures/posters", "files/dimensions/samples/unique accents", not problems,
          f"{len(poster_data)}/{len(items)} valid" if not problems else "; ".join(problems[:6]))
    return poster_data if not problems else None


def validate_page(items: list[dict], expected: dict) -> bool:
    page_path = HERE / "page" / "index.html"
    css_path = HERE / "page" / "style.css"
    if not check("fixtures/page", "page files", page_path.is_file() and css_path.is_file(),
                 f"{page_path.name} + {css_path.name}"):
        return False
    page = page_path.read_text(encoding="utf-8")
    css = css_path.read_text(encoding="utf-8")
    problems: list[str] = []

    if page.count('class="poster-card"') != len(items):
        problems.append(f"poster-card count {page.count('class=\"poster-card\"')} != {len(items)}")
    if page.count('class="poster"') != len(items):
        problems.append(f"poster img count {page.count('class=\"poster\"')} != {len(items)}")
    if page.count('class="poster-grid"') != 1:
        problems.append("poster-grid missing/duplicated")
    for required in ('class="sidebar"', 'class="search-field"', 'class="detail-panel"'):
        if required not in page:
            problems.append(f"{required} missing")
    for label in ("Todos", "Favoritos", "Recientes", "Carpetas"):
        if label not in page:
            problems.append(f"nav label {label!r} missing")
    for item in items:
        src = "../" + item["poster"]
        if page.count(f'src="{src}"') != 1:
            problems.append(f"{item['id']}: src {src} count != 1")
        title = html_mod.escape(item["title"])
        if page.count(f'class="poster-title">{title}<') != 1:
            problems.append(f"{item['id']}: title {item['title']!r} missing")
    for tag in ("<script", "<input", "<video", "<audio", "<canvas", "<svg", "<iframe"):
        if tag in page.lower():
            problems.append(f"forbidden tag {tag} in v1 fixture")
    if 'href="style.css"' not in page:
        problems.append("stylesheet link missing")

    check("fixtures/page", "HTML shape and catalog consistency", not problems,
          f"{len(items)} cards linked to catalog" if not problems else "; ".join(problems[:8]))
    return not problems


def css_block(css: str, selector: str) -> str | None:
    pattern = r"(?m)^" + re.escape(selector) + r"\s*\{([^}]*)\}"
    found = re.search(pattern, css)
    return found.group(1) if found else None


def declarations(css: str, selector: str) -> dict[str, str] | None:
    block = css_block(css, selector)
    if block is None:
        return None
    result: dict[str, str] = {}
    for statement in block.split(";"):
        if ":" in statement:
            prop, value = statement.split(":", 1)
            result[prop.strip()] = value.strip()
    return result


def parse_pct(value: str | None):
    if not value:
        return None
    found = re.search(r"(-?[\d.]+)%", value)
    return float(found.group(1)) if found else None


def parse_px(value: str | None):
    if not value:
        return None
    found = re.search(r"(-?[\d.]+)px", value)
    return float(found.group(1)) if found else None


def validate_css_and_math(expected: dict, items: list[dict]) -> dict | None:
    css = (HERE / "page" / "style.css").read_text(encoding="utf-8")
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    selectors = (".root", ".sidebar", ".main", ".poster-grid", ".poster-card", ".poster")
    decls = {}
    missing = []
    for selector in selectors:
        block = declarations(css, selector)
        if block is None:
            missing.append(selector)
        else:
            decls[selector] = block
    if not check("expected/css", "selectors present", not missing,
                 "all present" if not missing else f"missing {missing}"):
        return None

    sidebar_pct = parse_pct(decls[".sidebar"].get("width"))
    main_pct = parse_pct(decls[".main"].get("width"))
    padding = parse_px(decls[".main"].get("padding"))
    card_w = parse_px(decls[".poster-card"].get("width"))
    card_h = parse_px(decls[".poster-card"].get("height"))
    poster_w = parse_px(decls[".poster"].get("width"))
    poster_h = parse_px(decls[".poster"].get("height"))
    gap = parse_px(decls[".poster-grid"].get("gap"))
    row_h = parse_px(decls[".poster-grid"].get("grid-auto-rows"))
    grid_display = decls[".poster-grid"].get("display", "")
    grid_overflow = decls[".poster-grid"].get("overflow", "")
    grid_height = decls[".poster-grid"].get("height", "")
    grid_width = decls[".poster-grid"].get("width", "")
    object_fit = decls[".poster"].get("object-fit", "")
    columns_value = decls[".poster-grid"].get("grid-template-columns", "")
    cols_re = re.search(r"repeat\(\s*auto-fill\s*,\s*([\d.]+)px\s*\)", columns_value)

    problems: list[str] = []
    if sidebar_pct is None or main_pct is None or not close(sidebar_pct + main_pct, 100.0):
        problems.append("sidebar% + main% must be 100")
    for name, value in (("sidebar%", sidebar_pct), ("main%", main_pct), ("padding", padding),
                        ("card_w", card_w), ("card_h", card_h), ("poster_w", poster_w),
                        ("poster_h", poster_h), ("gap", gap), ("row_h", row_h)):
        if value is None:
            problems.append(f"{name} missing")
    if grid_display.strip() != "grid":
        problems.append("poster-grid display != grid")
    if grid_overflow.strip() not in ("auto", "scroll"):
        problems.append("poster-grid overflow must be auto|scroll")
    if grid_width.strip() != "100%":
        problems.append("poster-grid must be width 100%")
    # The grid may fill its parent either with height:100% (v2 contract) or with
    # flex:1 inside a column flex main (resize-proof, E8 flex-grow).
    grid_flex = decls[".poster-grid"].get("flex", "").strip()
    if grid_height.strip() != "100%" and not grid_flex.startswith("1"):
        problems.append("poster-grid must be height 100% or flex: 1")
    if object_fit.strip() != "cover":
        problems.append("poster object-fit != cover")
    if cols_re is None:
        problems.append("grid-template-columns must be repeat(auto-fill, Npx)")
    elif not close(float(cols_re.group(1)), card_w):
        problems.append("auto-fill size != card width")
    if problems:
        check("expected/css", "v2 contract declarations", False, "; ".join(problems))
        return None

    css_contract = expected.get("css_contract", {})
    contract_problems: list[str] = []
    for key, got in (
        ("sidebar_width_pct", sidebar_pct), ("main_width_pct", main_pct),
        ("main_padding_px", padding), ("card_width_px", card_w),
        ("card_height_px", card_h), ("poster_width_px", poster_w),
        ("poster_height_px", poster_h), ("grid_gap_px", gap),
        ("grid_row_height_px", row_h),
    ):
        if key not in css_contract or not close(css_contract[key], got):
            contract_problems.append(f"{key}: expected {css_contract.get(key)} got {got}")
    check("expected/css", "CSS constants match expected.json", not contract_problems,
          "all constants match" if not contract_problems else "; ".join(contract_problems))

    if not close(card_w, poster_w):
        contract_problems.append("poster width must equal card width")
    check("expected/css", "poster fills card width", close(card_w, poster_w),
          f"{poster_w} == {card_w}")

    if contract_problems:
        return None

    n = len(items)
    math_problems: list[str] = []
    computed_sizes: dict[str, dict] = {}
    for key in expected.get("viewports", []):
        vw, vh = (float(part) for part in key.split("x"))
        sidebar_w = vw * sidebar_pct / 100.0
        main_w = vw * main_pct / 100.0
        grid_x = sidebar_w + padding
        grid_y = padding
        content_w = main_w - 2 * padding
        grid_h = vh - 2 * padding
        columns = max(1, int((content_w + gap) // (card_w + gap)))
        rows = math.ceil(n / columns)
        column_pitch = card_w + gap
        row_pitch = row_h + gap
        content_h = rows * row_h + (rows - 1) * gap
        scroll_max = max(0.0, content_h - grid_h)

        # The auto-fill fit must be exact: columns fit, columns+1 does not.
        used = columns * card_w + (columns - 1) * gap
        next_used = (columns + 1) * card_w + columns * gap
        if used > content_w + 0.01:
            math_problems.append(f"{key}: {columns} columns overflow ({used} > {content_w})")
        if next_used <= content_w + 0.01:
            math_problems.append(f"{key}: {columns + 1} columns would also fit ({next_used} <= {content_w})")

        computed = {
            "sidebar": {"x": 0.0, "y": 0.0, "w": sidebar_w, "h": vh},
            "main": {"x": sidebar_w, "y": 0.0, "w": main_w, "h": vh},
            "grid": {"x": grid_x, "y": grid_y, "w": content_w, "h": grid_h},
            "content_width": content_w,
            "content_height": content_h,
            "viewport_height": grid_h,
            "columns": columns,
            "rows": rows,
            "column_pitch": column_pitch,
            "row_pitch": row_pitch,
            "scroll_max": scroll_max,
        }
        computed_sizes[key] = computed

        exp = expected.get("sizes", {}).get(key)
        if not exp:
            math_problems.append(f"{key}: missing from expected.json")
            continue
        for field in ("content_width", "content_height", "viewport_height",
                      "columns", "rows", "column_pitch", "row_pitch", "scroll_max"):
            if not close(exp.get(field, -9999), computed[field]):
                math_problems.append(f"{key}.{field}: expected {exp.get(field)} got {computed[field]}")
        for part in ("sidebar", "main", "grid"):
            if not rect_close(exp.get(part, {}), computed[part]):
                math_problems.append(f"{key}.{part}: expected {exp.get(part)} got {computed[part]}")

        cards = exp.get("cards", [])
        if len(cards) != n:
            math_problems.append(f"{key}: {len(cards)} cards, expected {n}")
        for index, item in enumerate(items):
            if index >= len(cards):
                break
            card = cards[index]
            if card.get("id") != item["id"]:
                math_problems.append(f"{key}[{index}]: id {card.get('id')} != {item['id']}")
            col, row = index % columns, index // columns
            x = grid_x + col * column_pitch
            y = grid_y + row * row_pitch
            for field, value in (("x", x), ("y", y), ("w", card_w), ("h", card_h)):
                if not close(card.get(field, -9999), value):
                    math_problems.append(f"{key}[{index}].{field}: {card.get(field)} != {value}")
            image = card.get("image", {})
            if not rect_close(image, {"x": x, "y": y, "w": poster_w, "h": poster_h}, 0.02):
                math_problems.append(f"{key}[{index}].image: {image}")
            meta = card.get("meta", {})
            if not rect_close(meta, {"x": x, "y": y + poster_h, "w": card_w, "h": card_h - poster_h}, 0.02):
                math_problems.append(f"{key}[{index}].meta: {meta}")

    math_summary = ", ".join(
        f"{key}: {value['columns']} cols x {value['rows']} rows"
        for key, value in computed_sizes.items())
    check("expected/math", "grid columns and card positions recomputed from CSS",
          not math_problems,
          math_summary if not math_problems else "; ".join(math_problems[:8]))

    scroll_problems: list[str] = []
    scroll_tests = expected.get("scroll_contract", {}).get("tests", {})
    for key, computed in computed_sizes.items():
        tests = scroll_tests.get(key)
        if not tests:
            scroll_problems.append(f"{key}: missing scroll tests")
            continue
        exp = expected["sizes"][key]

        def expected_runs(top: float):
            return [[round(top + 196, 2), round(top + 204, 2)],
                    [round(top + 224, 2), round(top + 240, 2)]]

        shift = tests.get("shift", {})
        card = exp["cards"][shift.get("card_index", -1)]
        if shift.get("card_id") != card.get("id"):
            scroll_problems.append(f"{key}.shift: card id mismatch")
        after_top = card["y"] - shift["delta"]
        if not close(shift.get("after_top", -9999), after_top):
            scroll_problems.append(f"{key}.shift.after_top {shift.get('after_top')} != {after_top}")
        if shift.get("before_runs") != expected_runs(card["y"]):
            scroll_problems.append(f"{key}.shift.before_runs mismatch")
        if shift.get("after_runs") != expected_runs(after_top):
            scroll_problems.append(f"{key}.shift.after_runs mismatch")
        if shift.get("column_x") != card["x"] + 5:
            scroll_problems.append(f"{key}.shift.column_x mismatch")

        offset = tests.get("offset", {})
        card = exp["cards"][offset.get("card_index", -1)]
        if offset.get("card_id") != card.get("id"):
            scroll_problems.append(f"{key}.offset: card id mismatch")
        after_top = card["y"] - offset["delta"]
        if not close(offset.get("after_top", -9999), after_top):
            scroll_problems.append(f"{key}.offset.after_top {offset.get('after_top')} != {after_top}")
        if offset.get("after_runs") != expected_runs(after_top):
            scroll_problems.append(f"{key}.offset.after_runs mismatch")
        if offset.get("column_x") != card["x"] + 5:
            scroll_problems.append(f"{key}.offset.column_x mismatch")

        clamp = tests.get("clamp", {})
        card = exp["cards"][clamp.get("card_index", -1)]
        if clamp.get("card_id") != card.get("id"):
            scroll_problems.append(f"{key}.clamp: card id mismatch")
        if clamp.get("effective_scroll") != computed["scroll_max"]:
            scroll_problems.append(f"{key}.clamp.effective {clamp.get('effective_scroll')} != {computed['scroll_max']}")
        after_top = card["y"] - computed["scroll_max"]
        if not close(clamp.get("after_top", -9999), after_top):
            scroll_problems.append(f"{key}.clamp.after_top {clamp.get('after_top')} != {after_top}")
        if clamp.get("after_runs") != expected_runs(after_top):
            scroll_problems.append(f"{key}.clamp.after_runs mismatch")
        if clamp.get("column_x") != card["x"] + 5:
            scroll_problems.append(f"{key}.clamp.column_x mismatch")
        if after_top + card_h > computed["grid"]["y"] + computed["grid"]["h"] + 0.5:
            scroll_problems.append(f"{key}.clamp: last card extends past grid bottom")

    check("expected/math", "scroll probes and clamp recomputed", not scroll_problems,
          "shift=100px, offset=400px, clamp=content_h-grid_h"
          if not scroll_problems else "; ".join(scroll_problems[:8]))

    return computed_sizes if not math_problems and not scroll_problems else None


# ------------------------------------------------------------------- runtime

def run_process(exe: Path, args: list[str], timeout: float) -> dict:
    env = dict(os.environ)
    env["PATH"] = f"{os.environ.get('WINDIR', r'C:\\Windows')}\\System32;{exe.parent}"
    cwd = RUNTIME_CWD if RUNTIME_CWD is not None else exe.parent
    try:
        proc = subprocess.run(
            [str(exe), *args],
            cwd=str(cwd),
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return {"code": proc.returncode, "stdout": proc.stdout, "stderr": proc.stderr, "error": None}
    except subprocess.TimeoutExpired:
        return {"code": None, "stdout": "", "stderr": "", "error": f"timeout after {timeout}s"}
    except OSError as exc:
        return {"code": None, "stdout": "", "stderr": "", "error": str(exc)}


def parse_dump(stdout: str) -> list[dict]:
    nodes = []
    for line in stdout.splitlines():
        found = re.match(r"^\[\s*(\d+)\]\s+parent=\s*(-?\d+)\s+(\S+)\s+kind=(\w+)", line)
        if found:
            nodes.append({
                "id": int(found.group(1)),
                "parent": int(found.group(2)),
                "name": found.group(3),
                "kind": found.group(4),
            })
    return nodes


def resolve_runtime(arg: str | None) -> tuple[Path | None, str]:
    if arg:
        path = Path(arg).expanduser().resolve()
        if path.is_file():
            return path, "explicit --runtime"
        return None, f"--runtime path does not exist: {path}"
    candidates = (
        (HERE / "build" / "runtime_r2_pilot.exe", "pilot build"),
        (HERE / "build" / "runtime_r2.exe", "pilot build"),
        (REPO / "src" / "runtime" / "c-r2" / "build" / "runtime_r2.exe", "shared R2 build"),
    )
    for path, label in candidates:
        if path.is_file():
            return path, label
    return None, "no runtime found (looked for build/runtime_r2_pilot.exe and src/runtime/c-r2/build/runtime_r2.exe)"


def nodes_for_size(report: dict | None, key: str, dump_nodes: list[dict]) -> list[dict]:
    if report:
        for size in report.get("sizes", []):
            if f"{size.get('width')}x{size.get('height')}" == key:
                nodes = size.get("nodes")
                if isinstance(nodes, list) and nodes:
                    return nodes
    return dump_nodes


def grid_node(nodes: list[dict], suffix: str) -> dict | None:
    for node in nodes:
        if str(node.get("name", "")).endswith(suffix):
            return node
    for node in nodes:
        if "poster-grid" in str(node.get("name", "")):
            return node
    for node in nodes:
        if node.get("kind") == "grid":
            return node
    return None


CARD_RE = re.compile(r"div\.poster-card(?:\[(\d+)\])?$")
IMAGE_RE = re.compile(r"div\.poster-card(?:\[(\d+)\])?/img\.poster(?:\[(\d+)\])?$")


def card_index(node: dict) -> int | None:
    found = CARD_RE.search(str(node.get("name", "")))
    if not found:
        return None
    return int(found.group(1)) if found.group(1) is not None else 0


def image_card_index(node: dict) -> int | None:
    found = IMAGE_RE.search(str(node.get("name", "")))
    if found:
        return int(found.group(1)) if found.group(1) is not None else 0
    return None


def report_geometry(report: dict | None, expected: dict) -> bool:
    if not report:
        fail_check("runtime/report", "layout report available", False,
                   "runtime did not produce --report; card geometry can only be checked from pixels")
        return False
    all_ok = True
    for key in VIEWPORTS:
        exp = expected["sizes"][key]
        size = next((s for s in report.get("sizes", [])
                     if f"{s.get('width')}x{s.get('height')}" == key), None)
        if size is None:
            all_ok = fail_check("runtime/report", f"{key} present", False, "missing size in report")
            continue
        nodes = size.get("nodes", [])
        expected_grid = grid_node(nodes, expected["runtime_contract"]["grid_node_suffix"])
        if expected_grid is None:
            all_ok = fail_check("runtime/report", f"{key} grid node", False,
                                "no /div.poster-grid node in report")
            continue
        if not rect_close(expected_grid, exp["grid"]):
            all_ok = fail_check("runtime/report", f"{key} grid rect", False,
                                f"{expected_grid} != {exp['grid']}")
        else:
            check("runtime/report", f"{key} grid rect", True,
                  f"x={expected_grid['x']} y={expected_grid['y']} w={expected_grid['w']} h={expected_grid['h']}")

        cards = [node for node in nodes if card_index(node) is not None]
        if len(cards) != len(exp["cards"]):
            all_ok = fail_check("runtime/report", f"{key} card count", False,
                                f"{len(cards)} card nodes, expected {len(exp['cards'])}")
            continue
        indexed = {card_index(node): node for node in cards}
        if len(indexed) != len(cards):
            all_ok = fail_check("runtime/report", f"{key} card node names", False,
                                "card node indices are not unique")
            continue
        problems = []
        for expected_card in exp["cards"]:
            node = indexed.get(expected_card["index"])
            if node is None:
                problems.append(f"missing card {expected_card['index']}")
                continue
            if not rect_close(node, expected_card):
                problems.append(
                    f"card {expected_card['index']}: ({node.get('x')},{node.get('y')},"
                    f"{node.get('w')},{node.get('h')}) != ({expected_card['x']},"
                    f"{expected_card['y']},{expected_card['w']},{expected_card['h']})")
        all_ok = check("runtime/report", f"{key} card positions", not problems,
                       "all 30 cards match" if not problems else "; ".join(problems[:5])) and all_ok

        row0 = [node for node in cards if close(node.get("y", -9999), exp["grid"]["y"], 0.51)]
        row0_columns = len({round(float(node.get("x", -9999)), 2) for node in row0})
        all_ok = check("runtime/report", f"{key} grid columns", row0_columns == exp["columns"],
                       f"{row0_columns} first-row columns, expected {exp['columns']}") and all_ok

        images = [node for node in nodes if image_card_index(node) is not None]
        if len(images) != len(exp["cards"]):
            all_ok = fail_check("runtime/report", f"{key} image count", False,
                                f"{len(images)} image nodes, expected {len(exp['cards'])}")
        else:
            image_by_card = {image_card_index(node): node for node in images}
            problems = []
            for expected_card in exp["cards"]:
                node = image_by_card.get(expected_card["index"])
                if node is None:
                    problems.append(f"missing image for card {expected_card['index']}")
                elif not rect_close(node, expected_card["image"]):
                    problems.append(f"image {expected_card['index']} rect mismatch")
            all_ok = check("runtime/report", f"{key} image rects", not problems,
                           "all poster rects match" if not problems else "; ".join(problems[:5])) and all_ok
    return all_ok


# ---------------------------------------------------------------- raster ops

def load_png(path: Path):
    with Image.open(path) as raw:
        raw.load()
        return raw.convert("RGB")


def find_runs(img, x: int, rgb, tol: int = TOL) -> list[list[int]]:
    runs: list[list[int]] = []
    start = None
    for y in range(img.height):
        match = rgb_close(px(img, x, y), rgb, tol)
        if match and start is None:
            start = y
        elif not match and start is not None:
            runs.append([start, y])
            start = None
    if start is not None:
        runs.append([start, img.height])
    return runs


def has_run(runs: list[list[int]], expected: list[float], tol: int = 1) -> bool:
    return any(abs(run[0] - expected[0]) <= tol and abs(run[1] - expected[1]) <= tol for run in runs)


def count_colors_outside(img, boxes: list[tuple[int, int, int, int]],
                         colors: list[tuple[int, int, int]], tol: int = TOL) -> tuple[int, str | None]:
    """Count near-accent pixels in flat regions outside the grid.

    Regions outside the grid are flat (page background), so Pillow's
    `getcolors()` usually returns a handful of unique colours; that keeps this
    fast without a per-pixel Python loop over 1M pixels.
    """
    total = 0
    first = None
    for box in boxes:
        left, top, right, bottom = box
        left = max(0, min(img.width, int(left)))
        top = max(0, min(img.height, int(top)))
        right = max(0, min(img.width, int(right)))
        bottom = max(0, min(img.height, int(bottom)))
        if right <= left or bottom <= top:
            continue
        crop = img.crop((left, top, right, bottom))
        histogram = crop.getcolors(maxcolors=crop.width * crop.height)
        if histogram is None:  # pathological fallback
            histogram = []
            for y in range(crop.height):
                for x in range(crop.width):
                    histogram.append((1, crop.getpixel((x, y))[:3]))
        for count, value in histogram:
            if any(rgb_close(value, color, tol) for color in colors):
                total += count
                if first is None:
                    first = f"region ({left},{top})-({right},{bottom})"
    return total, first


def outside_grid_boxes(exp: dict) -> list[tuple[int, int, int, int]]:
    grid = exp["grid"]
    return [
        (grid["x"], 0, grid["x"] + grid["w"], grid["y"]),
        (0, grid["y"] + grid["h"], exp["viewport"]["width"], exp["viewport"]["height"]),
        (exp["main"]["x"], 0, grid["x"], exp["viewport"]["height"]),
        (grid["x"] + grid["w"], 0, exp["viewport"]["width"], exp["viewport"]["height"]),
    ]


def render(exe: Path, width: int, height: int, path: Path, extra: list[str] | None,
           timeout: float) -> tuple[bool, dict, str]:
    path.unlink(missing_ok=True)
    args = ["--render", str(width), str(height), str(path)]
    if extra:
        args.extend(extra)
    result = run_process(exe, args, timeout)
    if result["code"] != 0 or not path.is_file():
        return False, result, f"exit={result['code']} error={result['error']} stderr={result['stderr'].strip()[:300]}"
    try:
        img = load_png(path)
    except Exception as exc:  # noqa: BLE001
        return False, result, f"cannot read PNG: {exc}"
    return True, result, ""


def check_baseline(key: str, exp: dict, expected: dict, img, poster_data: dict) -> bool:
    vw, vh = exp["viewport"]["width"], exp["viewport"]["height"]
    grid = exp["grid"]
    ok = check(f"raster/{key}", "PNG size", img.size == (vw, vh), f"{img.size} == ({vw}, {vh})")
    if img.size != (vw, vh):
        return False

    first_row = [card for card in exp["cards"] if card["row"] == 0]
    first_row_ok = len(first_row) == exp["columns"] and all(
        rgb_close(px(img, card["x"] + 4, card["y"] + 4),
                  poster_data[card["id"]]["samples"]["bg"])
        for card in first_row)
    ok = check(f"raster/{key}", "grid columns (first-row poster colours)", first_row_ok,
               f"{len(first_row)} columns visible, expected {exp['columns']}") and ok

    mismatches = []
    checked = 0
    for card in exp["cards"]:
        samples = poster_data[card["id"]]["samples"]
        for sample_key, (dx, dy) in expected["poster"]["sample_points"].items():
            x = card["x"] + dx
            y = card["y"] + dy
            if not (grid["x"] + 1 <= x < grid["x"] + grid["w"] - 1 and
                    grid["y"] + 1 <= y < grid["y"] + grid["h"] - 1):
                continue
            expected_rgb = samples[sample_key]
            actual = px(img, x, y)
            checked += 1
            if not rgb_close(actual, expected_rgb):
                if len(mismatches) < 6:
                    mismatches.append(
                        f"card {card['index']} {sample_key} at ({x},{y}): "
                        f"#{actual[0]:02x}{actual[1]:02x}{actual[2]:02x} != "
                        f"#{expected_rgb[0]:02x}{expected_rgb[1]:02x}{expected_rgb[2]:02x}")
    ok = check(f"raster/{key}", "poster source colours at sample points", not mismatches,
               f"{checked} visible samples match" if not mismatches else "; ".join(mismatches)) and ok

    card0 = exp["cards"][0]
    gap_x = card0["x"] + card0["w"] + expected_gap(exp) / 2.0
    gap_y = card0["y"] + card0["h"] + expected_gap(exp) / 2.0
    page_rgb = hex_rgb(expected["colors"]["page"])
    gap_ok = rgb_close(px(img, gap_x, card0["y"] + 100), page_rgb) and \
        rgb_close(px(img, card0["x"] + 90, gap_y), page_rgb)
    ok = check(f"raster/{key}", "grid gap = background", gap_ok,
               f"column gap x={gap_x:.0f}, row gap y={gap_y:.0f}") and ok

    accents = [data["accent"] for data in poster_data.values()]
    total, first = count_colors_outside(img, outside_grid_boxes(exp), accents)
    ok = check(f"raster/{key}", "no poster pixels outside grid clip", total == 0,
               "0 stray pixels" if total == 0 else f"{total} accent pixels, first at {first}") and ok

    # A card whose rule+footer bands are fully below the clip must not paint
    # those colours anywhere.
    clip_bottom = grid["y"] + grid["h"]
    for card in exp["cards"]:
        if card["y"] + 196 > clip_bottom + 0.5:
            accent = poster_data[card["id"]]["accent"]
            runs = find_runs(img, int(card["x"] + 5), accent)
            ok = check(f"raster/{key}", f"clipped card {card['index']} bands absent", not runs,
                       "0 accent runs" if not runs else f"unexpected accent runs {runs[:4]}") and ok
            break

    return ok


def expected_gap(exp: dict) -> float:
    return float(exp["cards"][1]["x"] - exp["cards"][0]["x"] - exp["cards"][0]["w"])


def scroll_render(exe: Path, method: str, grid_id: int | None, exp: dict, delta: int,
                  path: Path, timeout: float) -> tuple[bool, str]:
    vw, vh = exp["viewport"]["width"], exp["viewport"]["height"]
    grid = exp["grid"]
    if method == "scroll":
        if grid_id is None:
            return False, "no grid node id"
        extra = ["--scroll", str(grid_id), str(delta)]
    elif method == "wheel3":
        # Current R2 CLI: --wheel <x> <y> <delta_y>.
        extra = ["--wheel", str(int(grid["x"] + grid["w"] / 2)),
                 str(int(grid["y"] + grid["h"] / 2)), str(delta)]
    else:
        # Documented ABI shape: --wheel <x> <y> <delta_x> <delta_y>.
        extra = ["--wheel", str(int(grid["x"] + grid["w"] / 2)),
                 str(int(grid["y"] + grid["h"] / 2)), "0", str(delta)]
    ok, result, error = render(exe, vw, vh, path, extra, timeout)
    if not ok:
        return False, error
    return True, " ".join(extra)


def detect_scroll_method(exe: Path, grid_id: int | None, exp: dict, baseline,
                         out: Path, timeout: float) -> tuple[str | None, str]:
    candidates = []
    if grid_id is not None:
        candidates.append("scroll")
    candidates.extend(("wheel3", "wheel4"))
    evidence = []
    for method in candidates:
        path = out / f"probe_scroll_{method}.png"
        ok, detail = scroll_render(exe, method, grid_id, exp, 100, path, timeout)
        if not ok:
            evidence.append(f"{method}: {detail}")
            continue
        scrolled = load_png(path)
        diff = ImageChops.difference(baseline, scrolled).getbbox()
        if diff is not None:
            return method, f"{method} accepted and changed pixels ({detail})"
        evidence.append(f"{method}: image identical to scroll_y=0 ({detail})")
    return None, "; ".join(evidence)


def check_scroll(key: str, exp: dict, expected: dict, poster_data: dict, method: str,
                 grid_id: int | None, exe: Path, baseline, out: Path, timeout: float,
                 injection_detail: str) -> bool:
    tests = expected["scroll_contract"]["tests"][key]
    ok = True
    for test_name in ("shift", "offset", "clamp"):
        test = tests[test_name]
        delta = int(test["delta"])
        path = out / f"{key}_scroll_{test_name}.png"
        rendered, detail = scroll_render(exe, method, grid_id, exp, delta, path, timeout)
        if not rendered:
            ok = fail_check(f"raster/{key}", f"scroll {test_name}", False,
                            f"render failed: {detail}") and False
            continue
        img = load_png(path)
        if img.size != baseline.size:
            ok = fail_check(f"raster/{key}", f"scroll {test_name} size", False,
                            f"{img.size} != {baseline.size}") and False
            continue
        accent = poster_data[test["card_id"]]["accent"]
        runs = find_runs(img, int(test["column_x"]), accent)
        after_ok = all(has_run(runs, run) for run in test["after_runs"])
        if not after_ok:
            detail_text = (f"accent runs {runs[:6]} do not contain {test['after_runs']} "
                           f"at x={test['column_x']}")
        else:
            detail_text = f"runs {test['after_runs']} present at x={test['column_x']}"

        if test_name == "shift":
            before_runs = find_runs(baseline, int(test["column_x"]), accent)
            before_ok = all(has_run(before_runs, run) for run in test["before_runs"])
            if not before_ok:
                detail_text += f"; baseline runs {before_runs[:6]} missing {test['before_runs']}"
                after_ok = False
            else:
                footer_before = test["before_runs"][-1][0]
                footer_after = test["after_runs"][-1][0]
                actual_shift = footer_before - footer_after
                if actual_shift != test["delta"]:
                    detail_text += f"; measured shift {actual_shift} != {test['delta']}"
                    after_ok = False
                else:
                    detail_text += f"; measured shift {actual_shift}px"
        ok = check(f"raster/{key}", f"scroll {test_name}", after_ok, detail_text) and ok

        # Clipping must hold for scrolled frames too.
        accents = [data["accent"] for data in poster_data.values()]
        total, first = count_colors_outside(img, outside_grid_boxes(exp), accents)
        clip_ok = total == 0
        ok = check(f"raster/{key}", f"scroll {test_name} clip", clip_ok,
                   "0 stray pixels" if clip_ok else f"{total} accent pixels, first at {first}") and ok

        if test_name == "shift" and after_ok:
            # At the top edge of the grid, nothing may paint above the clip.
            top_ok = rgb_close(px(img, int(test["column_x"]), exp["grid"]["y"] - 1),
                               hex_rgb(expected["colors"]["page"]))
            ok = check(f"raster/{key}", "scroll top clip", top_ok,
                       f"y={exp['grid']['y'] - 1} is page background") and ok

    note(f"{key}: scroll injection via {injection_detail}")
    return ok


# ---------------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser(description="media-library pilot visual verifier")
    parser.add_argument("--runtime", help="path to a pilot runtime_r2.exe")
    parser.add_argument("--fixtures-only", action="store_true",
                        help="validate catalog/posters/page/expected math without a runtime")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="directory for rendered PNGs and verify_report.json")
    parser.add_argument("--timeout", type=float, default=120.0, help="per runtime invocation (s)")
    parser.add_argument("--grid-node-id", type=int, default=None,
                        help="override the grid node id for --scroll (normally from --report)")
    parser.add_argument("--runtime-cwd", type=Path, default=None,
                        help="process cwd for the runtime (default: page/, so ../posters resolves)")
    args = parser.parse_args()

    global RUNTIME_CWD
    RUNTIME_CWD = (args.runtime_cwd or (HERE / "page")).resolve()

    print(f"verify: pilot at {HERE}")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    if Image is None:
        print("ERROR: Pillow is required for PNG checks.", file=sys.stderr)
        print("  python -m venv .venv", file=sys.stderr)
        print("  .venv\\Scripts\\python -m pip install Pillow", file=sys.stderr)
        return 2

    # ------------------------------------------------------------- fixtures
    catalog = load_json(HERE / "catalog.json")
    expected = load_json(HERE / "expected.json")
    manifest = None
    manifest_path = HERE / "posters" / "manifest.json"
    if manifest_path.is_file():
        manifest = load_json(manifest_path)
    items = validate_catalog(catalog)
    if not items:
        return finish("FAIL", out, None)
    poster_data = validate_posters(items, expected, manifest)
    if not poster_data:
        return finish("FAIL", out, None)
    if not validate_page(items, expected):
        return finish("FAIL", out, None)
    computed_sizes = validate_css_and_math(expected, items)
    if not computed_sizes:
        return finish("FAIL", out, None)

    check("fixtures", "catalog/posters/page/expected internally consistent", True,
          f"{len(items)} items, {len(poster_data)} posters, 2 viewports")
    if args.fixtures_only:
        return finish("PASS", out, None)

    # --------------------------------------------------------------- runtime
    runtime, runtime_source = resolve_runtime(args.runtime)
    if runtime is None:
        blocker("runtime", runtime_source)
        return finish("ENGINE FEATURE MISSING", out, None)
    note(f"runtime: {runtime} ({runtime_source})")

    help_result = run_process(runtime, ["--help"], min(args.timeout, 30))
    help_text = f"{help_result['stdout']}\n{help_result['stderr']}"
    dump_result = run_process(runtime, ["--dump"], min(args.timeout, 30))
    dump_nodes = parse_dump(dump_result["stdout"])
    if not dump_nodes:
        note(f"--dump produced no parseable nodes (exit={dump_result['code']})")

    report = None
    report_path = out / "report.json"
    report_path.unlink(missing_ok=True)
    report_result = run_process(
        runtime, ["--report", str(report_path), "--sizes", ",".join(VIEWPORTS)],
        args.timeout)
    if report_result["code"] == 0 and report_path.is_file():
        try:
            report = load_json(report_path)
        except json.JSONDecodeError as exc:
            note(f"--report JSON is invalid: {exc}")
    else:
        note(f"--report failed: exit={report_result['code']} "
             f"stderr={report_result['stderr'].strip()[:200]}")

    nodes = nodes_for_size(report, "1280x800", dump_nodes)
    grid = grid_node(nodes, expected["runtime_contract"]["grid_node_suffix"])
    image_nodes = [node for node in nodes if image_card_index(node) is not None]
    image_kind_nodes = [node for node in nodes if node.get("kind") == expected["runtime_contract"]["image_kind"]]

    if grid is None:
        installed = [str(node.get("name", "")) for node in nodes]
        looks_like_28 = (report and report.get("node_count") == 28) or \
            any("div.cards" in name for name in installed)
        detail = (
            f"report/dump has {len(nodes)} nodes but no node ending "
            f"{expected['runtime_contract']['grid_node_suffix']!r}"
        )
        if looks_like_28:
            detail += "; this binary is the 28-node R2 fixture (div.cards), not the pilot page"
        blocker("grid", detail)
    elif grid.get("kind") != expected["runtime_contract"]["grid_kind"]:
        blocker("grid", f"node {grid.get('name')!r} has kind={grid.get('kind')!r}, "
                        f"expected kind={expected['runtime_contract']['grid_kind']!r}")

    if not image_kind_nodes:
        if image_nodes:
            blocker("images", "nodes named img.poster exist but no kind=image node")
        else:
            blocker("images", "no kind=image / img.poster nodes in the layout report/dump")

    if grid is None or grid.get("kind") != expected["runtime_contract"]["grid_kind"] or not image_kind_nodes:
        if "--scroll" not in help_text and "--wheel" not in help_text:
            blocker("scroll", "--help does not advertise --scroll or --wheel, and no pilot grid "
                              "node is available for offscreen scroll injection")
        else:
            blocker("scroll", "grid/image layout is not available, so scroll cannot be verified")
        return finish("ENGINE FEATURE MISSING", out, {"runtime": str(runtime), "source": runtime_source})

    geometry_ok = report_geometry(report, expected)

    # ------------------------------------------------------ offscreen renders
    # Re-resolve grid node per size because ids are stable but geometry differs.
    grid_ids: dict[str, int | None] = {}
    for key in VIEWPORTS:
        size_nodes = nodes_for_size(report, key, dump_nodes)
        size_grid = grid_node(size_nodes, expected["runtime_contract"]["grid_node_suffix"])
        grid_ids[key] = size_grid.get("id") if size_grid else None
    if args.grid_node_id is not None:
        for key in VIEWPORTS:
            grid_ids[key] = args.grid_node_id

    baselines = {}
    render_ok = True
    for key in VIEWPORTS:
        exp = expected["sizes"][key]
        path = out / f"{key}_scroll0.png"
        ok, result, error = render(
            runtime, exp["viewport"]["width"], exp["viewport"]["height"], path,
            None, args.timeout)
        if not ok:
            blocker("offscreen render", f"{key}: {error}")
            render_ok = False
            continue
        baselines[key] = load_png(path)
    if not render_ok:
        return finish("ENGINE FEATURE MISSING", out, {"runtime": str(runtime), "source": runtime_source})

    any_baseline_fail = False
    for key in VIEWPORTS:
        if not check_baseline(key, expected["sizes"][key], expected, baselines[key],
                              poster_data):
            any_baseline_fail = True

    # ---------------------------------------------------------------- scroll
    probe_grid_id = grid_ids.get("1280x800")
    method, method_detail = detect_scroll_method(
        runtime, probe_grid_id, expected["sizes"]["1280x800"],
        baselines["1280x800"], out, args.timeout)
    if method is None:
        blocker("scroll", f"offscreen scroll injection did not work: {method_detail}")
        return finish("ENGINE FEATURE MISSING", out, {"runtime": str(runtime), "source": runtime_source})
    note(f"scroll injection supported: {method_detail}")

    scroll_ok = True
    for key in VIEWPORTS:
        ok = check_scroll(key, expected["sizes"][key], expected, poster_data, method,
                          grid_ids.get(key), runtime, baselines[key], out, args.timeout,
                          method_detail)
        scroll_ok = scroll_ok and ok

    result = "PASS" if not any_baseline_fail and geometry_ok and scroll_ok else "FAIL"
    return finish(result, out, {"runtime": str(runtime), "source": runtime_source})


def finish(result: str, out: Path, runtime_info: dict | None) -> int:
    failures = [item for item in CHECKS if not item["ok"]]
    if result == "PASS" and (failures or BLOCKERS):
        result = "FAIL" if failures else "ENGINE FEATURE MISSING"
    if result == "ENGINE FEATURE MISSING" and not BLOCKERS:
        result = "FAIL" if failures else "PASS"

    print()
    if BLOCKERS and result == "ENGINE FEATURE MISSING":
        print(f"ENGINE FEATURE MISSING - {len(BLOCKERS)} blocker(s):")
        for item in BLOCKERS:
            print(f"  - {item['feature']}: {item['detail']}")
        print()
        print("Runtime must expose (docs/ui-tree-v2.md, pilot contract):")
        print("  * ui-tree/2 nodes kind=grid and kind=image from frontend + emit_ui")
        print("  * grid layout: auto_fill 180px, grid_auto_rows 320px, gap 16px,")
        print("    overflow:auto clipped to the grid content box")
        print("  * local PNG decode/cache (WIC -> RGBA8) + object-fit:cover")
        print("  * offscreen --render W H out.png and --report FILE --sizes WxH,...")
        print("  * offscreen --scroll <grid_node_id> <delta_y> (preferred) or")
        print("    --wheel <x> <y> <delta_y> (R2) / --wheel <x> <y> <dx> <dy> (ABI)")
        print("  * a pilot runtime built with page/index.html (not the 28-node fixture)")
    print()
    if result == "PASS":
        print(f"RESULT: PASS ({len(CHECKS)} checks, 0 failures)")
    elif result == "FAIL":
        print(f"RESULT: FAIL ({len(CHECKS)} checks, {len(failures)} failure(s))")
        for item in failures[:10]:
            print(f"  - {item['group']}: {item['name']} - {item['detail']}")
    else:
        print(f"RESULT: ENGINE FEATURE MISSING ({len(BLOCKERS)} blocker(s), "
              f"{len(failures)} failure(s))")

    report = {
        "schema": "webnative.pilot-media-library.verify/1",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "result": result,
        "runtime": runtime_info,
        "blockers": BLOCKERS,
        "checks": CHECKS,
        "notes": NOTES,
    }
    try:
        out.mkdir(parents=True, exist_ok=True)
        (out / "verify_report.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {out / 'verify_report.json'}")
    except OSError as exc:
        print(f"warning: could not write verify_report.json: {exc}", file=sys.stderr)

    if result == "PASS":
        return 0
    if result == "FAIL":
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
