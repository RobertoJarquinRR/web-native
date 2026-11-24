#!/usr/bin/env python3
"""Probe: is the full WeasyPrint engine usable on this Windows machine?

WeasyPrint >= 53 no longer bundles its native dependencies: it needs Pango,
GObject, HarfBuzz and cairo/fontconfig DLLs on PATH (typically from a GTK
installation). This script only checks that; it does not feed the pipeline.
"""
import sys
import traceback
from pathlib import Path

BASE = Path(__file__).resolve().parent
print(f"python {sys.version.split()[0]}")

try:
    import weasyprint
except Exception as error:
    print("import weasyprint: FAILED")
    print(f"  {type(error).__name__}: {error}")
    try:
        import weasyprint.layout  # noqa: F401  (also fails via package __init__)
    except Exception as layout_error:
        print("import weasyprint.layout: FAILED")
        print(f"  {type(layout_error).__name__}: {layout_error}")
    sys.exit(1)

print(f"weasyprint {weasyprint.__version__}: import OK")
try:
    from weasyprint import HTML
    document = HTML(filename=str(BASE / "input.html")).render()
    print(f"render OK: {document.width}x{document.height}")

    def walk(box, depth=0):
        text = getattr(box, "text", "")
        print("  " * depth + f"{type(box).__name__} <{box.element_tag}> "
              f"{box.position_x},{box.position_y} {box.width}x{box.height} {text!r}")
        for child in getattr(box, "children", ()):
            walk(child, depth + 1)

    walk(document.pages[0]._page_box)
except Exception:
    print("layout/render: FAILED")
    traceback.print_exc()
    sys.exit(2)
sys.exit(0)
