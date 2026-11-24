"""CSS -> reglas con selector compilado por cssselect2.

Se expanden los shorthands que el motor entiende (margin, padding, gap) y el
resto queda como declaración cruda. Las propiedades desconocidas no fallan:
llegan a la cascada y simplemente no se leen.
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
    if name in ("margin", "padding"):
        parts = value.split()
        if not 1 <= len(parts) <= 4:
            return []
        top, right = parts[0], parts[1] if len(parts) > 1 else parts[0]
        bottom = parts[2] if len(parts) > 2 else top
        left = parts[3] if len(parts) > 3 else right
        return [(f"{name}-{s}", v) for s, v in zip(SIDES, (top, right, bottom, left))]
    if name == "gap":
        parts = value.split()
        if len(parts) == 1:
            return [("row-gap", parts[0]), ("column-gap", parts[0])]
        if len(parts) == 2:
            return [("row-gap", parts[0]), ("column-gap", parts[1])]
        return []
    return [(name, value)]


def parse_declarations(text: str) -> list[tuple[str, str, bool]]:
    """(name, value, important); reutilizado por reglas y por style="..."."""
    out: list[tuple[str, str, bool]] = []
    for decl in tinycss2.parse_declaration_list(
            text, skip_comments=True, skip_whitespace=True):
        if decl.type == "declaration" and decl.name:
            value = tinycss2.serialize(decl.value).strip()
            out.extend((n, v, bool(decl.important))
                       for n, v in _expand(decl.lower_name, value))
    return out


def parse_stylesheet(text: str) -> list[Rule]:
    rules: list[Rule] = []
    for rule in tinycss2.parse_stylesheet(text, skip_comments=True, skip_whitespace=True):
        if rule.type != "qualified-rule":
            continue  # @media / @font-face / @supports se ignoran en v0
        declarations = tuple(parse_declarations(tinycss2.serialize(rule.content)))
        if not declarations:
            continue
        prelude = tinycss2.serialize(rule.prelude).strip()
        try:
            selectors = cssselect2.compile_selector_list(prelude)
        except cssselect2.SelectorError:
            continue
        for selector in selectors:
            if selector.pseudo_element is None and not selector.never_matches:
                rules.append(Rule(selector, declarations))
    return rules


def parse_stylesheets(texts: list[str]) -> list[Rule]:
    return [rule for text in texts for rule in parse_stylesheet(text)]
