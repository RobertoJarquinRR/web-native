"""Sondas de gaps: ejecuta los casos que el motor NO cubre y guarda números.

No forma parte del render; es evidencia reproducible para GAPS.md.
Salida: gaps_evidence.json
"""
import json
from pathlib import Path

from webnative.cascade import compute_styles
from webnative.css_parse import parse_stylesheet
from webnative.html_parse import parse_html, parse_html_file
from webnative.layout import layout_document
from webnative.text import FontBook

HERE = Path(__file__).resolve().parent
PAGE = HERE / "page"


def build(html: str, css: str, viewport=(400, 200), fonts=None):
    root = parse_html(f"<body>{html}</body>")
    styles = compute_styles(root, parse_stylesheet(css))
    box, engine = layout_document(root, styles, fonts, viewport)
    return root, styles, box, engine


def all_lines(box):
    out = []
    for current in box.walk():
        out.extend(line.text for line in current.lines)
    return out


def find_class(box, name):
    return [b for b in box.walk() if name in b.class_set()]


def main():
    fonts = FontBook()
    evidence = {}

    # 1. viewport más bajo que el height fijo del fixture
    root = parse_html_file(PAGE / "input.html")
    rules = parse_stylesheet((PAGE / "input.css").read_text(encoding="utf-8"))
    styles = compute_styles(root, rules)
    box, _ = layout_document(root, styles, fonts, (640, 400))
    footer = find_class(box, "footer")[0]
    evidence["fixed_viewport_height"] = {
        "viewport": [640, 400], "body_height": box.h,
        "footer_y": footer.y, "footer_bottom": footer.bottom,
        "conclusion": "body fijo en 480px; el footer cae en 448..480, fuera de un viewport de 400px",
    }

    # 2. sin hoja de estilos de usuario: h1/p no reciben tamaño, peso ni márgenes
    root, styles, box, _ = build(
        "<h1>Compila la web</h1><p>HTML y CSS a nativo.</p>",
        "body { background-color: #000000; color: #ffffff; }", (640, 200), fonts)
    h1 = next(n for n in root.walk() if n.tag == "h1")
    p = next(n for n in root.walk() if n.tag == "p")
    evidence["no_ua_stylesheet"] = {
        "h1_font_size": styles[h1].font_size, "h1_font_weight": styles[h1].font_weight,
        "h1_margin_top": styles[h1].margin[0],
        "p_margin_top": styles[p].margin[0],
        "browser_reference": "h1: 32px/700, margin 21.44px; p: 16px, margin 16px",
        "conclusion": "sin reset, el motor no aplica la hoja UA; el fixture la compensa con *{margin:0} y font-size/weight explícitos",
    }

    # 3. font-family de CSS no se resuelve: siempre se carga Segoe UI
    root, styles, box, _ = build(
        "<div class='t'>webnative</div>",
        "body { margin: 0; color: #ffffff; }"
        " .t { font-family: Arial, sans-serif; font-size: 18px; }",
        (200, 80), fonts)
    node = next(n for n in root.walk() if n.tag == "div")
    family_style = styles[node]
    face_key = fonts.face_key(family_style)
    evidence["font_family_ignored"] = {
        "css_family": "Arial, sans-serif",
        "computed_font_family": family_style.font_family,
        "resolved_face_key": face_key,
        "loaded_regular": str(fonts.faces[face_key].path),
        "loaded_bold": str(fonts.faces["bold"].path),
        "conclusion": "FontBook no lee font-family: CSS 'Arial' sigue renderizando con segoeui.ttf",
    }

    # 4. box-sizing: content-box se ignora; el motor siempre es border-box
    _, _, box, _ = build(
        "<div class='c'>x</div>",
        "body { margin: 0; } .c { width: 180px; height: 120px; padding: 12px;"
        " box-sizing: content-box; background-color: #1a1a22; }", (400, 200), fonts)
    card = find_class(box, "c")[0]
    evidence["box_sizing_forced"] = {
        "declared": "width:180px; padding:12px; box-sizing:content-box",
        "engine_border_width": card.w, "engine_border_height": card.h,
        "browser_border_box": [204, 144],
        "conclusion": "box-sizing se parsea pero no se usa; el motor fuerza border-box",
    }

    # 5. color: inherit no se resuelve; parse_color('inherit') -> None -> negro
    root, styles, box, _ = build(
        "<div class='p'><span class='s'>texto</span></div>",
        "body { margin: 0; background-color: #000000; }"
        ".p { color: #ff0000; } .s { color: inherit; }", (200, 80), fonts)
    span = next(n for n in root.walk() if n.tag == "span")
    evidence["css_wide_keywords"] = {
        "parent_color": "#ff0000", "span_computed_color": styles[span].color,
        "conclusion": "inherit/initial/unset no se interpretan; color:inherit termina en negro",
    }

    # 6. contenido inline mezclado con bloque: el texto suelto se descarta
    _, _, box, _ = build(
        "<div class='d'>Hola <p>mundo</p> tail</div>",
        "body { margin: 0; color: #ffffff; } .d { background-color: #111111; }",
        (300, 120), fonts)
    evidence["mixed_inline_block"] = {
        "html": "<div>Hola <p>mundo</p> tail</div>",
        "rendered_lines": all_lines(box),
        "conclusion": "solo se pinta la caja de texto del <p>; 'Hola' y 'tail' se pierden",
    }

    # 7. texto directo dentro de un flex container: no hay ítem anónimo
    _, _, box, _ = build(
        "<div class='f'><span>a</span>texto suelto</div>",
        "body { margin: 0; color: #ffffff; } .f { display: flex; }",
        (300, 80), fonts)
    evidence["anonymous_flex_item"] = {
        "html": "<div style='display:flex'><span>a</span>texto suelto</div>",
        "rendered_lines": all_lines(box),
        "conclusion": "el texto sin elemento se ignora; el fixture envuelve el footer en <span>",
    }

    # 8. flex-grow se parsea pero no se aplica
    _, _, box, _ = build(
        "<div class='f'><div class='a'>1</div><div class='b'>2</div></div>",
        "body { margin: 0; color: #ffffff; } .f { display: flex; width: 400px;"
        " height: 50px; background-color: #000000; } .a, .b { width: 100px;"
        " flex-grow: 1; background-color: #222222; }", (400, 100), fonts)
    kids = find_class(box, "a") + find_class(box, "b")
    evidence["flex_grow_ignored"] = {
        "container_width": 400, "child_widths": [child.w for child in kids],
        "flex_info": find_class(box, "f")[0].flex_info,
        "conclusion": "free=200px no se reparte; no hay grow/shrink/basis",
    }

    # 9. height:100% no se resuelve
    _, _, box, _ = build(
        "<div class='p'><div class='c'>x</div></div>",
        "body { margin: 0; color: #ffffff; } .p { height: 200px; background-color: #000000; }"
        " .c { height: 50%; background-color: #333333; }", (300, 250), fonts)
    child = find_class(box, "c")[0]
    evidence["percent_sizes"] = {
        "parent_height": 200, "child_css_height": "50%", "engine_child_height": child.h,
        "browser_expected": 100,
        "conclusion": "%/em/rem/vh/min/max se tratan como auto (None)",
    }

    # 10. colapso de márgenes: 20 + 30 se suman
    _, _, box, _ = build(
        "<div class='a'>a</div><div class='b'>b</div>",
        "body { margin: 0; color: #ffffff; } .a { margin-bottom: 20px; }"
        " .b { margin-top: 30px; }", (300, 200), fonts)
    a = find_class(box, "a")[0]
    b = find_class(box, "b")[0]
    evidence["margin_collapse"] = {
        "margin_bottom_a": 20, "margin_top_b": 30,
        "gap": b.y - a.bottom, "browser_gap": 30,
        "conclusion": "no hay colapso de márgenes; los márgenes verticales se suman",
    }

    # 11. palabra larga: se parte por caracteres (un browser desbordaría)
    _, _, box, _ = build(
        "<div class='w'>Supercalifragilisticoespialidoso</div>",
        "body { margin: 0; color: #ffffff; } .w { width: 80px; font-size: 16px; }",
        (120, 200), fonts)
    word = find_class(box, "w")[0]
    evidence["long_word_break"] = {
        "width": 80, "font_size": 16, "lines": [line.text for line in word.lines],
        "conclusion": "el wrapping parte palabras largas por carácter; no hay overflow-wrap/word-break",
    }

    # 12. pesos: 600 se convierte en la fuente bold (700)
    root, styles, box, _ = build(
        "<div class='t'>webnative</div>",
        "body { margin: 0; color: #ffffff; } .t { font-size: 18px; font-weight: 600; }",
        (200, 80), fonts)
    node = next(n for n in root.walk() if n.tag == "div")
    evidence["font_weight"] = {
        "requested": 600, "computed": styles[node].font_weight,
        "face": fonts.face_key(styles[node]),
        "face_file": str(fonts.faces[fonts.face_key(styles[node])].path),
        "conclusion": "solo hay 400/700; 600 usa el archivo bold. Sin italic ni pesos variables",
    }

    # 13. métricas de línea usadas vs line-height normal de CSS
    metrics = {}
    for face, size in (("regular", 16), ("regular", 13), ("bold", 28), ("bold", 18)):
        asc, desc, gap = fonts.metrics(face, size)
        metrics[f"{face}_{size}"] = {"asc": round(asc, 2), "desc": round(desc, 2),
                                     "gap": round(gap, 2),
                                     "line_height": round(asc + desc + gap, 2)}
    evidence["line_metrics"] = {
        "metrics": metrics,
        "conclusion": "line-height:normal usa las extents de HarfBuzz (asc+desc+gap); no se leen OS/2 typo metrics ni overrides CSS",
    }

    # 14. borde: se declara pero no se pinta ni ocupa espacio
    _, _, box, _ = build(
        "<div class='b'>x</div>",
        "body { margin: 0; color: #ffffff; } .b { width: 100px; height: 40px;"
        " border: 4px solid #ff0000; background-color: #111111; }", (200, 100), fonts)
    bordered = find_class(box, "b")[0]
    evidence["border_ignored"] = {
        "declared_border": "4px solid #ff0000", "engine_width": bordered.w,
        "engine_height": bordered.h,
        "conclusion": "border no se parsea como shorthand ni se pinta; no hay border-radius",
    }

    # 15. dirección/script fijos: no hay bidi
    evidence["bidi"] = {
        "shaping_direction": "ltr", "script": "Latn",
        "conclusion": "Face.shape fija ltr/Latn; árabe, hebreo, devanagari y RTL no están soportados",
    }

    out = HERE / "gaps_evidence.json"
    out.write_text(json.dumps(evidence, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(evidence, indent=2, ensure_ascii=False))
    print(f"\n[probes] escrito {out}")


if __name__ == "__main__":
    main()
