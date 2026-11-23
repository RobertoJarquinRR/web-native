"""HTML -> Node tree con html5lib (nodos implícitos html/head/body incluidos)."""
from pathlib import Path

import html5lib

from .tree import Node

NON_RENDERED = {"head", "title", "meta", "link", "style", "script", "base",
                "template", "noscript", "source", "track"}


def parse_html(text: str) -> Node:
    tree = html5lib.parse(text, treebuilder="etree", namespaceHTMLElements=False)
    root = tree.getroot() if hasattr(tree, "getroot") else tree

    def convert(el, parent):
        node = Node(str(el.tag).lower(), dict(el.attrib), parent, text=el.text or "")
        for child in el:
            if isinstance(child.tag, str):  # ignora comentarios
                node.append(convert(child, node))
        return node

    return convert(root, None)


def parse_html_file(path: str | Path) -> Node:
    return parse_html(Path(path).read_text(encoding="utf-8"))


def stylesheet_texts(root: Node, base_dir: str | Path) -> list[str]:
    """CSS en orden de documento: <link rel=stylesheet> e <style> inline."""
    texts, base = [], Path(base_dir)
    for node in root.walk():
        if node.tag == "style":
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
