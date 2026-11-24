"""CSS -> reglas con selector compilado por cssselect2.

Se expanden margin/padding a longhands; el resto queda como declaración cruda
para que cascade/layout la interprete (o la ignore si aún no se soporta).
"""
from dataclasses import dataclass

import cssselect2
import tinycss2
from cssselect2.compiler import CompiledSelector

SIDES = ("top", "right", "bottom", "left")


@dataclass(frozen=True)
class Rule:
    selector: CompiledSelector
    declarations: tuple[tuple[str, str, bool], ...]


def _expand(name: str, value: str) -> list[tuple[str, str]]:
    if name not in ("margin", "padding"):
        return [(name, value)]
    p = value.split()
    if not 1 <= len(p) <= 4:
        return []
    top, right = p[0], p[1] if len(p) > 1 else p[0]
    bottom, left = p[2] if len(p) > 2 else top, p[3] if len(p) > 3 else right
    return [(f"{name}-{s}", v) for s, v in zip(SIDES, (top, right, bottom, left))]


def parse_declarations(text: str) -> list[tuple[str, str, bool]]:
    """(name, value, important); reutilizado por las reglas y por style="..."."""
    out = []
    for decl in tinycss2.parse_declaration_list(text, skip_comments=True, skip_whitespace=True):
        if decl.type == "declaration" and decl.name:
            value = tinycss2.serialize(decl.value).strip()
            out.extend((n, v, bool(decl.important))
                       for n, v in _expand(decl.lower_name, value))
    return out


def parse_stylesheet(text: str) -> list[Rule]:
    rules: list[Rule] = []
    for rule in tinycss2.parse_stylesheet(text, skip_comments=True, skip_whitespace=True):
        if rule.type != "qualified-rule":
            continue
        declarations = tuple(parse_declarations(tinycss2.serialize(rule.content)))
        if not declarations:
            continue
        prelude = tinycss2.serialize(rule.prelude).strip()
        for selector in cssselect2.compile_selector_list(prelude):
            if selector.pseudo_element is None and not selector.never_matches:
                rules.append(Rule(selector, declarations))
    return rules


def parse_stylesheets(texts: list[str]) -> list[Rule]:
    return [rule for text in texts for rule in parse_stylesheet(text)]
