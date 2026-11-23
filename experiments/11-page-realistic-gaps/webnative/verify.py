"""Verificación: geometría del layout, IR y píxeles del PNG.

Cada check es un dict serializable. Todas las comprobaciones son obligatorias:
si una falla, el experimento no puede declararse SUCCESS.
"""
from PIL import Image

from .layout import Box, boxes_with_class, find_boxes


def rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def luminance(pixel) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def dist2(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b))


def close(pixel, color, tol: int = 8) -> bool:
    return max(abs(a - b) for a, b in zip(pixel[:3], rgb(color))) <= tol


def _rect(box: Box) -> tuple[int, int, int, int]:
    return (int(round(box.x)), int(round(box.y)),
            int(round(box.w)), int(round(box.h)))


def _region(img: Image.Image, box: Box) -> list[tuple[int, int, int]]:
    x, y, w, h = _rect(box)
    x0, y0 = max(x, 0), max(y, 0)
    x1, y1 = min(x + w, img.width), min(y + h, img.height)
    return [img.getpixel((px, py))[:3]
            for py in range(y0, y1) for px in range(x0, x1)]


def _sample(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


class Checks:
    def __init__(self):
        self.items: list[dict] = []

    def add(self, kind: str, name: str, passed: bool, expected=None, got=None,
            detail: str = "") -> bool:
        self.items.append({"kind": kind, "name": name, "pass": bool(passed),
                           "expected": expected, "got": got, "detail": detail})
        return bool(passed)

    def near(self, kind: str, name: str, expected, got, tol: float = 0.01) -> bool:
        try:
            ok = abs(float(expected) - float(got)) <= tol
        except (TypeError, ValueError):
            ok = False
        return self.add(kind, name, ok, expected, got, f"tol={tol}")

    def pixel(self, img, name: str, x: float, y: float, color: str,
              tol: int = 8) -> bool:
        got = _sample(img, x, y)
        expected = rgb(color)
        ok = max(abs(a - b) for a, b in zip(got, expected)) <= tol
        return self.add("pixel", name, ok, color, "#%02x%02x%02x" % got,
                        f"({int(round(x))},{int(round(y))}) tol={tol}")


def _text_runs(display) -> list[dict]:
    return [op for op in display if op.get("type") == "text_run"]


def verify(layout_root: Box, display: dict, atlas, img: Image.Image,
           viewport: tuple[int, int]) -> tuple[Checks, dict]:
    checks = Checks()
    img = img.convert("RGB")
    ops = display.get("ops", [])
    text_ops = _text_runs(ops)
    stats = {
        "ops": len(ops),
        "rects": sum(1 for op in ops if op.get("type") == "rect"),
        "text_runs": len(text_ops),
        "glyph_quads": sum(len(op.get("glyphs", [])) for op in text_ops),
        "nonzero_quads": sum(1 for op in text_ops for g in op.get("glyphs", [])
                             if g.get("w", 0) > 0 and g.get("h", 0) > 0),
        "atlas_unique": atlas.unique(),
        "atlas_overflow": atlas.overflow,
        "atlas_bytes": len(atlas.data),
    }

    # ------------------------------------------------------------- tamaño / IR
    checks.add("image", "png size", img.size == tuple(viewport),
               list(viewport), list(img.size))
    viewport_data = display.get("viewport", {})
    checks.add("ir", "viewport en display_list", viewport_data == {
        "width": viewport[0], "height": viewport[1]}, viewport, viewport_data)
    checks.add("ir", "hay ops rect y text_run",
               stats["rects"] >= 8 and stats["text_runs"] >= 14,
               ">=8 rects, >=14 runs", f"{stats['rects']} rects, {stats['text_runs']} runs")
    checks.add("ir", "todos los text_run tienen glifos",
               all(op.get("glyphs") for op in text_ops),
               "glyphs>0 en cada run",
               [len(op.get("glyphs", [])) for op in text_ops])
    checks.add("ir", "glifos dibujables suficientes", stats["nonzero_quads"] >= 150,
               ">=150 quads con w,h>0", stats["nonzero_quads"])
    checks.add("atlas", "bitmaps únicos", stats["atlas_unique"] >= 40,
               ">=40", stats["atlas_unique"])
    checks.add("atlas", "sin overflow", stats["atlas_overflow"] == 0,
               "0", stats["atlas_overflow"])

    # --------------------------------------------------------------- geometría
    nav = boxes_with_class(layout_root, "nav")
    cards = sorted(boxes_with_class(layout_root, "card"), key=lambda box: box.x)
    footer = boxes_with_class(layout_root, "footer")
    hero = boxes_with_class(layout_root, "hero")
    h1 = find_boxes(layout_root, lambda box: box.tag == "h1")
    paragraph = find_boxes(layout_root, lambda box: box.tag == "p")
    titles = boxes_with_class(layout_root, "card-title")
    bodies = boxes_with_class(layout_root, "card-body")
    badges = boxes_with_class(layout_root, "badge")
    logo = boxes_with_class(layout_root, "logo")
    links = boxes_with_class(layout_root, "links")

    checks.add("geometry", "existe nav/hero/footer", bool(nav and hero and footer),
               "1 de cada", f"{len(nav)}/{len(hero)}/{len(footer)}")
    checks.add("geometry", "existen 3 cards", len(cards) == 3, 3, len(cards))
    if not (nav and hero and footer and len(cards) == 3):
        return checks, stats

    checks.near("geometry", "nav x", 0, nav[0].x)
    checks.near("geometry", "nav y", 0, nav[0].y)
    checks.near("geometry", "nav width", viewport[0], nav[0].w)
    checks.near("geometry", "nav height", 56, nav[0].h)
    checks.near("geometry", "hero x", 0, hero[0].x)
    checks.near("geometry", "hero width", viewport[0], hero[0].w)

    for index, (card, expected_x) in enumerate(zip(cards, (16, 212, 408))):
        checks.near("geometry", f"card {index} x", expected_x, card.x)
        checks.near("geometry", f"card {index} y", cards[0].y, card.y)
        checks.near("geometry", f"card {index} width", 180, card.w)
        checks.near("geometry", f"card {index} height", 120, card.h)
    checks.near("geometry", "gap card0-card1", 16,
                cards[1].x - (cards[0].x + cards[0].w))
    checks.near("geometry", "gap card1-card2", 16,
                cards[2].x - (cards[1].x + cards[1].w))
    checks.near("geometry", "footer height", 32, footer[0].h)
    checks.near("geometry", "footer bottom", viewport[1], footer[0].bottom)
    checks.near("geometry", "footer y", 448, footer[0].y)
    checks.near("geometry", "footer width", viewport[0], footer[0].w)

    checks.near("geometry", "hero padding-left (h1 x)", 32, h1[0].x if h1 else -1)
    checks.near("geometry", "hero padding-left (p x)", 32,
                paragraph[0].x if paragraph else -1)
    checks.near("geometry", "nav padding-left (logo x)", 16, logo[0].x if logo else -1)
    if links:
        checks.near("geometry", "nav padding-right (links right)", viewport[0] - 16,
                    links[0].right)
    checks.add("geometry", "nav space-between", bool(
        nav[0].flex_info and nav[0].flex_info.get("justify") == "space-between"),
        "space-between", nav[0].flex_info.get("justify") if nav[0].flex_info else None)

    checks.add("geometry", "card flex column", bool(cards and cards[0].flex_info
               and cards[0].flex_info.get("direction") == "column"),
               "column", cards[0].flex_info if cards else None)
    checks.add("geometry", "3 títulos y 3 cuerpos de card",
               len(titles) == 3 and len(bodies) == 3, "3/3",
               f"{len(titles)}/{len(bodies)}")
    checks.add("geometry", "3 badges", len(badges) == 3, 3, len(badges))
    for index, body in enumerate(bodies):
        checks.add("geometry", f"card {index} body hace wrap",
                   len(body.lines) >= 2, ">=2 líneas", len(body.lines),
                   " | ".join(line.text for line in body.lines))
    checks.add("geometry", "hero h1 en una línea", bool(h1 and len(h1[0].lines) == 1),
               "1 línea", len(h1[0].lines) if h1 else None)
    checks.add("geometry", "hero p en una línea a 640px",
               bool(paragraph and len(paragraph[0].lines) == 1),
               "1 línea (cabe)", len(paragraph[0].lines) if paragraph else None)

    # ------------------------------------------------------------- texto / IR
    expected_texts = ["webnative", "Docs", "Demo", "About", "Compila la web",
                      "HTML y CSS a nativo, sin navegador.", "IR neutral",
                      "Texto real", "Cero WebView", "sin navegador"]
    all_text = " ".join(op.get("text", "") for op in text_ops)
    for expected in expected_texts:
        checks.add("text", f"run contiene {expected!r}", expected in all_text,
                   expected, "presente" if expected in all_text else "ausente")
    for line in [line for box in bodies for line in box.lines]:
        checks.add("text", f"línea de card: {line.text[:18]!r}",
                   bool(line.text.strip()), "texto", line.text)

    # ------------------------------------------------------------------ píxeles
    page_bg = "#0f0f14"
    nav_bg = "#12121a"
    card_bg = "#1a1a22"
    footer_bg = "#0b0b0e"
    muted = "#9aa0aa"

    checks.pixel(img, "nav bg (centro)", viewport[0] / 2, 8, nav_bg)
    checks.pixel(img, "nav bg (banda inferior)", viewport[0] / 2, 50, nav_bg)
    checks.pixel(img, "page bg entre main y footer", viewport[0] / 2, 400, page_bg)
    checks.pixel(img, "footer bg (derecha)", viewport[0] - 10, 465, footer_bg)
    checks.pixel(img, "gap entre cards", cards[0].right + 8,
                 cards[0].y + 60, page_bg)

    badge_colors = ["#e74c3c", "#2ecc71", "#3498db"]
    for index, (card, badge, color) in enumerate(zip(cards, badges, badge_colors)):
        checks.pixel(img, f"card {index} bg", card.x + 170, card.y + 100, card_bg)
        css_color = badge.style.background if badge.style else None
        checks.add("geometry", f"badge {index} color CSS", css_color == color,
                   color, css_color)
        checks.pixel(img, f"badge {index} color", badge.x + 40, badge.y + 2, color)

    # presencia de texto por regiones derivadas del layout
    if logo:
        pixels = _region(img, logo[0])
        lit = sum(1 for p in pixels if luminance(p) > 120)
        checks.add("pixel", "texto logo (px claros)", lit >= 30, ">=30", lit)
    if links:
        pixels = _region(img, links[0])
        count = sum(1 for p in pixels if dist2(p, rgb(muted)) < dist2(p, rgb(nav_bg)))
        checks.add("pixel", "texto links (px muted)", count >= 60, ">=60", count)
    if h1:
        pixels = _region(img, h1[0])
        lit = sum(1 for p in pixels if luminance(p) > 120)
        checks.add("pixel", "texto h1 (px claros)", lit >= 150, ">=150", lit)
    if paragraph:
        pixels = _region(img, paragraph[0])
        count = sum(1 for p in pixels if dist2(p, rgb(muted)) < dist2(p, rgb(page_bg)))
        checks.add("pixel", "texto hero p (px muted)", count >= 80, ">=80", count)
    for index, title in enumerate(titles):
        pixels = _region(img, title)
        lit = sum(1 for p in pixels if luminance(p) > 120)
        checks.add("pixel", f"texto título card {index}", lit >= 20, ">=20", lit)
    for index, body in enumerate(bodies):
        pixels = _region(img, body)
        count = sum(1 for p in pixels if dist2(p, rgb(muted)) < dist2(p, rgb(card_bg)))
        checks.add("pixel", f"texto cuerpo card {index}", count >= 50, ">=50", count)
    footer_text = find_boxes(layout_root, lambda box: box.tag == "span")
    if footer_text:
        pixels = _region(img, footer_text[0])
        lit = sum(1 for p in pixels if luminance(p) > 40)
        checks.add("pixel", "texto footer (px legibles)", lit >= 60, ">=60", lit)

    return checks, stats
