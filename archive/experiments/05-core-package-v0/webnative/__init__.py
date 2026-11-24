"""webnative v0: HTML+CSS -> display-list IR -> render nativo.

Sin navegador, WebView, Skia ni Electron. Las capas puras no dependen de PIL ni
wgpu; los renderers son plugins bajo `webnative.renderer`.
"""
from .cascade import ComputedStyle, compute_styles
from .css_parse import Rule, parse_stylesheet, parse_stylesheets
from .html_parse import parse_html, parse_html_file, stylesheet_texts
from .ir import DisplayList, Op, RectOp, register_op
from .layout import LayoutBox, layout_tree, paint, to_display_list
from .tree import Node

__version__ = "0.1.0"
__all__ = [
    "Node", "parse_html", "parse_html_file", "stylesheet_texts",
    "Rule", "parse_stylesheet", "parse_stylesheets",
    "ComputedStyle", "compute_styles", "LayoutBox", "layout_tree", "paint",
    "to_display_list", "DisplayList", "Op", "RectOp", "register_op",
]
