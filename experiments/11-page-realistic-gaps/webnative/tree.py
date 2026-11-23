"""DOM mínimo con nodos de texto explícitos (#text).

Los experimentos anteriores guardaban solo `node.text` (texto inicial) y perdían
los tails. Aquí el texto es un nodo más para que el flujo inline sea inspeccionable
y para no perder contenido entre elementos.
"""
from dataclasses import dataclass, field


@dataclass(eq=False)  # identidad: Node se usa como clave de dict
class Node:
    tag: str
    attrs: dict = field(default_factory=dict)
    parent: "Node | None" = field(default=None, repr=False)
    children: list["Node"] = field(default_factory=list, repr=False)
    text: str = ""

    def append(self, child: "Node") -> "Node":
        child.parent = self
        self.children.append(child)
        return child

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()

    @property
    def is_text(self) -> bool:
        return self.tag == "#text"

    def classes(self) -> set[str]:
        return set(self.attrs.get("class", "").split())
