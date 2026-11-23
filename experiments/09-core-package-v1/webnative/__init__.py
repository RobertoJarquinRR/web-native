"""webnative v1: HTML+CSS -> display-list IR (rect + text_run) -> render nativo.

Sin navegador, WebView, Skia ni Electron. Las capas puras (tree, html_parse,
css_parse, cascade, layout, ir, fonts) no dependen de PIL, wgpu, FreeType ni
HarfBuzz; el texto vive en `webnative.text` y los renderers son plugins bajo
`webnative.renderer`.
"""
from .cascade import ComputedStyle, compute_styles
from .css_parse import Rule, parse_stylesheet, parse_stylesheets
from .fonts import resolve_font_file
from .html_parse import parse_html, parse_html_file, stylesheet_texts
from .ir import DisplayList, Glyph, Op, RectOp, TextOp, TextRunOp, register_op
from .layout import LayoutBox, TextRun, layout_tree, paint, to_display_list
from .tree import Node

__version__ = "1.0.0"
__all__ = [
    "Node", "parse_html", "parse_html_file", "stylesheet_texts",
    "Rule", "parse_stylesheet", "parse_stylesheets",
    "ComputedStyle", "compute_styles", "LayoutBox", "TextRun", "layout_tree",
    "paint", "to_display_list", "DisplayList", "Op", "RectOp", "Glyph",
    "TextOp", "TextRunOp", "register_op", "resolve_font_file",
]
