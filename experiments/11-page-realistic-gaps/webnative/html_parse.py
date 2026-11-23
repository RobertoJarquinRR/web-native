"""HTML -> Node tree con html5lib.

Diferencias con exp05: los nodos #text se conservan (incluidos los tails), de modo
que `text_content()` puede reconstruir el contenido inline de un elemento.
"""
from pathlib import Path

import html5lib

from .tree import Node

NON_RENDERED = {"head", "title", "meta", "link", "style", "script", "base",
                "template", "noscript", "source", "track"}


def parse_html(text: str) -> Node:
    tree = html5lib.parse(text, treebuilder="etree", namespaceHTMLElements=False)
    root = tree.getroot() if hasattr(tree, "getroot") else tree

    def convert(element, parent):
        if not isinstance(element.tag, str):  # comentarios / processing instructions
            return None
        node = Node(str(element.tag).lower(), dict(element.attrib), parent)
        if element.text:
            node.append(Node("#text", {}, node, text=element.text))
        for child in element:
            converted = convert(child, node)
            if converted is not None:
                node.append(converted)
            tail = getattr(child, "tail", None)
            if tail:
                node.append(Node("#text", {}, node, text=tail))
        return node

    return convert(root, None)


def parse_html_file(path: str | Path) -> Node:
    return parse_html(Path(path).read_text(encoding="utf-8"))


def stylesheet_texts(root: Node, base_dir: str | Path) -> list[str]:
    """CSS en orden de documento: <link rel=stylesheet> e <style> inline."""
    texts, base = [], Path(base_dir)
    for node in root.walk():
        if node.tag == "style" and node.text:
            texts.append(node.text)
        elif node.tag == "link":
            href = node.attrs.get("href", "")
            rel = node.attrs.get("rel", "").lower().split()
            if "stylesheet" in rel and href and not href.startswith(("http:", "https:", "//")):
                path = base / href
                if not path.is_file():
                    raise FileNotFoundError(f"stylesheet no encontrado: {path}")
                texts.append(path.read_text(encoding="utf-8"))
    return texts


def find_body(root: Node) -> Node:
    return next((node for node in root.walk() if node.tag == "body"), root)


def text_content(node: Node) -> str:
    """Concatena todos los #text del subárbol, en orden de documento."""
    parts: list[str] = []

    def visit(current: Node) -> None:
        if current.is_text:
            parts.append(current.text)
            return
        for child in current.children:
            visit(child)

    visit(node)
    return "".join(parts)
