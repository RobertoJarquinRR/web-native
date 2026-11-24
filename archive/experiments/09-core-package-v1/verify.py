"""Verificación de aceptación end-to-end de exp09-v1-core.

Ejecuta exactamente los comandos del contrato, mide tiempos, comprueba geometría,
IR, píxeles y paridad CPU/wgpu, y escribe `verify/report.json`.

Uso:  .venv\\Scripts\\python.exe verify.py
"""
import hashlib
import io
import json
import subprocess
import sys
import time
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402

from webnative.cli import build_all  # noqa: E402
from webnative.fonts import resolve_font_file  # noqa: E402
from webnative.ir import DisplayList, RectOp, TextOp  # noqa: E402
from webnative.renderer import create_renderer  # noqa: E402
from webnative.text import TextEngine  # noqa: E402

VIEWPORT = (400, 240)
OLD_VIEWPORT = (400, 300)
OUT = ROOT / "verify"
TITLE_BAND = (0, 10, 400, 70)
CHIPS = [(20, "#e74c3c"), (88, "#2ecc71"), (156, "#3498db")]

CHECKS: list[dict] = []


def check(name: str, ok: bool, detail: str = "") -> bool:
    CHECKS.append({"check": name, "pass": bool(ok), "detail": detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" -> {detail}" if detail else ""))
    return bool(ok)


def rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def luminance(pixel) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def region(image: Image.Image, box) -> list:
    x0, y0, x1, y1 = box
    return [image.getpixel((x, y)) for y in range(y0, y1) for x in range(x0, x1)]


def lit(pixels, threshold: float = 100.0) -> int:
    return sum(1 for pixel in pixels if luminance(pixel) > threshold)


def run_cli(*args: str) -> tuple[subprocess.CompletedProcess, float]:
    started = time.perf_counter()
    result = subprocess.run([sys.executable, "-m", "webnative", *args], cwd=ROOT,
                            capture_output=True, text=True, check=False)
    elapsed = round((time.perf_counter() - started) * 1000, 2)
    print(f"$ python -m webnative {' '.join(args)}")
    for line in result.stdout.strip().splitlines():
        print(f"  {line}")
    if result.stderr.strip():
        for line in result.stderr.strip().splitlines():
            print(f"  stderr: {line}")
    return result, elapsed


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pixel_sha(image: Image.Image) -> str:
    return hashlib.sha256(image.tobytes()).hexdigest()


def find_box(box, *, node_id=None, css_class=None):
    for candidate in box.walk():
        if node_id is not None and candidate.node.attrs.get("id") == node_id:
            return candidate
        if css_class is not None and css_class in candidate.node.attrs.get("class", "").split():
            return candidate
    raise KeyError(f"box no encontrada id={node_id!r} class={css_class!r}")


def package_version(name: str) -> str:
    try:
        return version(name)
    except PackageNotFoundError:
        return "?"


def main() -> int:
    OUT.mkdir(exist_ok=True)
    started = time.perf_counter()
    timings: dict[str, float] = {}

    # ---------------------------------------------------------- 1. build + layout
    since = time.perf_counter()
    result = build_all(ROOT / "demo" / "input.html", VIEWPORT)
    timings.update({f"build_{key}": value for key, value in result.timings.items()})
    timings["build_total"] = round((time.perf_counter() - since) * 1000, 2)
    display, box = result.display, result.box

    root_box = find_box(box, node_id="root")
    title = find_box(box, css_class="title")
    row = find_box(box, css_class="row")
    chip_boxes = [find_box(box, css_class=name) for name in ("a", "b", "c")]
    ascender, descender = TextEngine().metrics("Segoe UI", 28)

    geometry = {
        "root": [root_box.x, root_box.y, root_box.width, root_box.height],
        "title": [title.x, title.y, title.width, title.height],
        "title_baseline": title.text_runs[0].baseline,
        "title_ascender_28": ascender,
        "title_descender_28": descender,
        "title_bottom": title.bottom,
        "row": [row.x, row.y, row.width, row.height],
        "chips": [[chip.x, chip.y, chip.width, chip.height] for chip in chip_boxes],
    }
    check("geometría root = (0,0,400,240)",
          geometry["root"] == [0, 0, 400, 240], str(geometry["root"]))
    check("baseline = 20 + ascender(28px)",
          abs(geometry["title_baseline"] - (20 + ascender)) < 1e-6,
          f"{geometry['title_baseline']} = 20 + {ascender}")
    check("fila = bottom(título) + gap 12",
          abs(row.y - (title.bottom + 12)) < 1e-6,
          f"{row.y} = {title.bottom} + 12")
    check("chips x = 20, 88, 156; y = fila; 60x60",
          [[c.x, c.y, c.width, c.height] for c in chip_boxes] ==
          [[20, row.y, 60, 60], [88, row.y, 60, 60], [156, row.y, 60, 60]],
          str([[c.x, c.y, c.width, c.height] for c in chip_boxes]))

    # ---------------------------------------------------------------- 2. IR
    ops = [op.to_dict() for op in display.ops]
    text_ops = [op for op in display.ops if isinstance(op, TextOp)]
    rect_ops = [op for op in display.ops if isinstance(op, RectOp)]
    text = text_ops[0] if text_ops else None
    ir_summary = {
        "viewport": display.viewport, "total_ops": len(ops),
        "rect_ops": len(rect_ops), "text_run_ops": len(text_ops),
        "text_run": text.to_dict() if text else None,
    }
    check("PNG/viewport del IR = 400x240",
          (display.width, display.height) == VIEWPORT, str(display.viewport))
    check("exactamente un text_run", len(text_ops) == 1, f"{len(text_ops)}")
    check("text_run: 'Hola', 28px, blanco, 4 glyphs",
          bool(text) and (text.text, text.size, text.color, len(text.glyphs)) ==
          ("Hola", 28, "#ffffff", 4),
          repr((text.text, text.size, text.color, len(text.glyphs))) if text else "sin text_run")
    check("root rect (0,0,400,240,#101014) presente",
          {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 240,
           "fill": "#101014"} in ops)
    row_y = round(row.y)
    expected_chips = [{"type": "rect", "x": x, "y": row_y, "w": 60, "h": 60,
                       "fill": fill} for x, fill in CHIPS]
    check("3 chip rects en el IR",
          all(expected in ops for expected in expected_chips),
          str(expected_chips))

    roundtrip = DisplayList.from_json(display.to_json())
    check("IR roundtrip exacto (ops ==)",
          roundtrip.ops == display.ops and roundtrip.to_dict() == display.to_dict())

    # ------------------------------------------------------- 3. CLI GPU (contrato)
    gpu_cli, timings["cli_wgpu_wall"] = run_cli(
        "demo/input.html", "--out", "render.png", "--ir", "display_list.json",
        "--backend", "wgpu")
    check("CLI wgpu termina con código 0", gpu_cli.returncode == 0,
          f"rc={gpu_cli.returncode} {gpu_cli.stderr.strip()}")
    check("CLI wgpu imprime backend elegido", "backend = wgpu" in gpu_cli.stdout,
          next((line for line in gpu_cli.stdout.splitlines()
                if line.startswith("backend =")), "?"))
    gpu_image = Image.open(ROOT / "render.png").convert("RGB")
    display_gpu = DisplayList.from_json((ROOT / "display_list.json").read_text(encoding="utf-8"))
    check("render.png 400x240", gpu_image.size == VIEWPORT, str(gpu_image.size))
    check("display_list.json: 1 text_run + 5 rects",
          sum(isinstance(op, TextOp) for op in display_gpu.ops) == 1 and
          sum(isinstance(op, RectOp) for op in display_gpu.ops) == 5,
          str([op.type for op in display_gpu.ops]))

    # ------------------------------------------------------------- 4. CPU + auto
    cpu_cli, timings["cli_cpu_wall"] = run_cli(
        "demo/input.html", "--out", "render-cpu.png", "--ir", "display_list-cpu.json",
        "--backend", "cpu")
    check("CLI cpu termina con código 0", cpu_cli.returncode == 0,
          f"rc={cpu_cli.returncode} {cpu_cli.stderr.strip()}")
    check("CLI cpu elige cpu", "backend = cpu" in cpu_cli.stdout)
    cpu_image = Image.open(ROOT / "render-cpu.png").convert("RGB")
    check("render-cpu.png 400x240", cpu_image.size == VIEWPORT, str(cpu_image.size))
    check("IR cpu == IR wgpu (byte-idéntico)",
          (ROOT / "display_list.json").read_bytes() ==
          (ROOT / "display_list-cpu.json").read_bytes())

    auto_cli, timings["cli_auto_wall"] = run_cli(
        "demo/input.html", "--out", "render-auto.png", "--ir", "display_list-auto.json",
        "--backend", "auto")
    chosen = next((line.split("=", 1)[1].strip() for line in auto_cli.stdout.splitlines()
                   if line.startswith("backend =")), "?")
    check("CLI auto imprime el backend elegido", auto_cli.returncode == 0 and chosen != "?",
          chosen)
    check("CLI auto elige wgpu en esta máquina", chosen.startswith("wgpu"), chosen)
    check("render-auto.png 400x240",
          Image.open(ROOT / "render-auto.png").size == VIEWPORT)

    # ------------------------------------------------------------- 5. píxeles
    pixel_results = {}
    for name, image in (("wgpu", gpu_image), ("cpu", cpu_image)):
        samples = {f"chip_{index}": image.getpixel((x + 30, row_y + 30))
                   for index, (x, _fill) in enumerate(CHIPS)}
        expected = {f"chip_{index}": rgb(fill) for index, (_x, fill) in enumerate(CHIPS)}
        title_pixels = region(image, TITLE_BAND)
        title_lit = lit(title_pixels)
        channel = sum(1 for pixel in title_pixels if max(pixel) > 100)
        pixel_results[name] = {
            "chip_samples": {key: list(value) for key, value in samples.items()},
            "chips_exact": samples == expected,
            "background": list(image.getpixel((350, 200))),
            "title_lit_luminance_gt100": title_lit,
            "title_channel_gt100": channel,
            "pixel_sha256": pixel_sha(image),
        }
        check(f"{name}: centros de chip exactos", samples == expected, str(samples))
        check(f"{name}: fondo #101014 exacto", image.getpixel((350, 200)) == (16, 16, 20))
        check(f"{name}: título con >100 píxeles brillantes", title_lit > 100,
              f"luminance>100 = {title_lit}, channel>100 = {channel}")

    # ------------------------------------------------- 6. paridad CPU vs wgpu
    x0, y0, x1, y1 = TITLE_BAND
    outside = [(x, y) for y in range(VIEWPORT[1]) for x in range(VIEWPORT[0])
               if not (x0 <= x < x1 and y0 <= y < y1)]
    outside_equal = all(gpu_image.getpixel(point) == cpu_image.getpixel(point)
                        for point in outside)
    gpu_pixels, cpu_pixels = region(gpu_image, TITLE_BAND), region(cpu_image, TITLE_BAND)
    diffs = [max(abs(a - b) for a, b in zip(p, q)) for p, q in zip(gpu_pixels, cpu_pixels)]
    mean_abs = sum(sum(abs(a - b) for a, b in zip(p, q))
                   for p, q in zip(gpu_pixels, cpu_pixels)) / (len(gpu_pixels) * 3)
    gpu_lit = [luminance(p) > 100 for p in gpu_pixels]
    cpu_lit = [luminance(p) > 100 for p in cpu_pixels]
    iou = sum(a and b for a, b in zip(gpu_lit, cpu_lit)) / sum(a or b for a, b in zip(gpu_lit, cpu_lit))
    parity = {
        "outside_title_band_pixel_identical": outside_equal,
        "text_band": {
            "mean_abs_channel_diff": round(mean_abs, 4),
            "max_channel_diff": max(diffs),
            "differing_pixels": sum(1 for diff in diffs if diff),
            "band_pixels": len(diffs),
            "differing_fraction": round(sum(1 for diff in diffs if diff) / len(diffs), 5),
            "lit_mask_iou": round(iou, 4),
        },
    }
    check("fuera de la banda de texto: CPU == wgpu píxel a píxel", outside_equal)
    check("texto CPU aprox. vs wgpu: IoU de máscara > 0.90", iou > 0.90,
          f"IoU={iou:.3f}")
    check("texto CPU vs wgpu: diferencia media < 1 nivel", mean_abs < 1.0,
          f"mean_abs={mean_abs:.3f}, max={max(diffs)}, "
          f"px distintos={sum(1 for diff in diffs if diff)}/{len(diffs)}")

    # ---------------------------------------- 7. regresión contrato v0 (2 divs)
    old = build_all(ROOT / "input.html", OLD_VIEWPORT)
    old_ops = [op.to_dict() for op in old.display.ops]
    old_expected = [
        {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#0f0f12"},
        {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#101014"},
        {"type": "rect", "x": 20, "y": 20, "w": 120, "h": 80, "fill": "#e74c3c"},
        {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80, "fill": "#3498db"},
    ]
    check("contrato v0: IR de 4 rects sin texto", old_ops == old_expected,
          f"{len(old_ops)} ops")

    wgpu_renderer = create_renderer("wgpu")
    cpu_renderer = create_renderer("cpu")
    old_gpu = wgpu_renderer.render(old.display)
    old_cpu = cpu_renderer.render(old.display)
    old_gpu_path, old_cpu_path = OUT / "old-wgpu.png", OUT / "old-cpu.png"
    old_gpu.save(old_gpu_path)
    old_cpu.save(old_cpu_path)
    check("contrato v0: CPU vs wgpu píxeles byte-idénticos",
          old_gpu.tobytes() == old_cpu.tobytes())
    check("contrato v0: PNG CPU vs wgpu byte-idéntico",
          old_gpu_path.read_bytes() == old_cpu_path.read_bytes(),
          f"sha256={sha256(old_gpu_path)[:16]}…")
    check("contrato v0: tamaño 400x300", old_gpu.size == OLD_VIEWPORT == old_cpu.size)

    # ------------------------------------------------------- 8. roundtrip render
    rebound = wgpu_renderer.render(DisplayList.from_json(display.to_json()))
    check("render desde JSON == render directo (byte-idéntico)",
          rebound.tobytes() == gpu_image.tobytes())

    # ------------------------------------------------------------- 9. timings
    clean_gpu = create_renderer("wgpu")
    clean_cpu = create_renderer("cpu")
    since = time.perf_counter()
    clean_gpu.render(display)
    timings["wgpu_frame_1"] = round((time.perf_counter() - since) * 1000, 2)
    since = time.perf_counter()
    clean_gpu.render(display)
    timings["wgpu_frame_2_cached"] = round((time.perf_counter() - since) * 1000, 2)
    since = time.perf_counter()
    clean_cpu.render(display)
    timings["cpu_frame"] = round((time.perf_counter() - since) * 1000, 2)
    since = time.perf_counter()
    gpu_image.save(io.BytesIO(), format="PNG")
    timings["png_encode"] = round((time.perf_counter() - since) * 1000, 2)
    timings["verify_total"] = round((time.perf_counter() - started) * 1000, 2)

    versions = {
        "python": sys.version.split()[0],
        "html5lib": package_version("html5lib"),
        "tinycss2": package_version("tinycss2"),
        "cssselect2": package_version("cssselect2"),
        "wgpu": package_version("wgpu"),
        "pillow": package_version("Pillow"),
        "uharfbuzz": package_version("uharfbuzz"),
        "freetype-py": package_version("freetype-py"),
        "pytest": package_version("pytest"),
    }
    font = resolve_font_file("Segoe UI")

    report = {
        "status": "SUCCESS" if all(item["pass"] for item in CHECKS) else "FAIL",
        "experiment": "exp09-v1-core",
        "viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]},
        "backend": wgpu_renderer.backend_info,
        "font_file": str(font),
        "versions": versions,
        "checks": CHECKS,
        "geometry": geometry,
        "ir": {key: value for key, value in ir_summary.items() if key != "text_run"},
        "text_run": {
            "x": text.x if text else None, "y": text.y if text else None,
            "text": text.text if text else None, "size": text.size if text else None,
            "color": text.color if text else None,
            "glyphs": [glyph.to_dict() for glyph in text.glyphs] if text else [],
        },
        "pixels": pixel_results,
        "parity": parity,
        "old_contract": {
            "png_sha256_wgpu": sha256(old_gpu_path),
            "png_sha256_cpu": sha256(old_cpu_path),
            "pixel_sha256_wgpu": pixel_sha(old_gpu),
            "pixel_sha256_cpu": pixel_sha(old_cpu),
        },
        "timings_ms": timings,
        "artifacts": ["render.png", "render-cpu.png", "render-auto.png",
                      "display_list.json", "display_list-cpu.json",
                      "display_list-auto.json", "verify/old-wgpu.png",
                      "verify/old-cpu.png"],
    }
    (OUT / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False),
                                     encoding="utf-8")

    print("\ntimings (ms):")
    for name, value in timings.items():
        print(f"  {name:<24} {value:9.2f}")
    print(f"\nSTATUS: {report['status']} "
          f"({sum(item['pass'] for item in CHECKS)}/{len(CHECKS)} checks)")
    print(f"report: {OUT / 'report.json'}")
    return 0 if report["status"] == "SUCCESS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
