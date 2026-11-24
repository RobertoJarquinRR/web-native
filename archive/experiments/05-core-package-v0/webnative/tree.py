"""DOM mínimo, independiente del parser y del renderer. `text` reserva TextOp."""
from dataclasses import dataclass, field


@dataclass(eq=False)  # identidad: los nodos son claves de dict
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
