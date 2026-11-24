//! Avisos de R1: features que solo existen en R2 (`--emit-ui`).
//!
//! El IR de R1 es un display list post-layout de bloque/flex. No implementa
//! layout grid, scroll ni imagenes, asi que una pagina que use esas features
//! produciria rects incompletos (o ninguno) sin avisar. Este modulo recorre el
//! DOM + la cascada **despues** de calcular estilos y, solo en el modo R1,
//! imprime un `warning[r1]` por feature. No altera el IR: el layout sigue
//! exactamente igual (las features se ignoran, no se inventan rects).
//!
//! En `--emit-ui` no se ejecuta: ahi el arbol de estilos v2 es la salida.

use crate::cascade::ComputedStyle;
use crate::dom::Dom;

pub struct Warning {
    pub context: String,
    pub message: String,
}

/// Features de R2 presentes en la pagina (pre-orden, sin duplicados por nodo
/// salvo propiedades distintas).
pub fn collect(dom: &Dom, styles: &[Option<ComputedStyle>]) -> Vec<Warning> {
    let mut out = Vec::new();
    for id in dom.preorder() {
        let Some(style) = styles.get(id).and_then(Option::as_ref) else {
            continue;
        };
        // Un nodo dentro de un subarbol `display: none` no llega a R1.
        if !is_rendered(dom, styles, id) {
            continue;
        }
        let node = &dom.nodes[id];
        let name = node_name(node.tag.as_str(), node.attr("id"), node.classes().next());

        if style.display == "grid" {
            out.push(Warning {
                context: name.clone(),
                message: "'display: grid' solo se implementa en R2 (--emit-ui); el IR de R1 no calcula layout grid".to_string(),
            });
        } else if style.grid_template_columns.is_some() || style.grid_auto_rows.is_some() {
            out.push(Warning {
                context: name.clone(),
                message: "propiedades grid sin 'display: grid': solo se emiten en R2 (--emit-ui)"
                    .to_string(),
            });
        }
        if let Some(overflow) = style.overflow.as_deref() {
            if overflow == "auto" || overflow == "scroll" {
                out.push(Warning {
                    context: name.clone(),
                    message: format!(
                        "'overflow: {overflow}' (scroll) solo existe en R2 (--emit-ui); R1 no recorta ni desplaza"
                    ),
                });
            }
        }
        if style.css3.border.any_visible() {
            out.push(Warning {
                context: name.clone(),
                message: "'border' solo se renderiza en R2 (--emit-ui); el IR de R1 pinta la caja sin borde"
                    .to_string(),
            });
        }
        if style.css3.border_radius.iter().any(|value| *value > 0.0) {
            out.push(Warning {
                context: name.clone(),
                message:
                    "'border-radius' solo se renderiza en R2 (--emit-ui); R1 no redondea esquinas"
                        .to_string(),
            });
        }
        if !style.css3.box_shadow.is_empty() {
            out.push(Warning {
                context: name.clone(),
                message: "'box-shadow' solo se renderiza en R2 (--emit-ui); R1 no pinta sombras"
                    .to_string(),
            });
        }
        if node.tag == "img" {
            out.push(Warning {
                context: name,
                message: "<img> no se renderiza en el IR de R1; R2 (--emit-ui) emite kind:image"
                    .to_string(),
            });
        }
    }
    out
}

pub fn print(warnings: &[Warning]) {
    for warning in warnings {
        eprintln!("warning[r1] {}: {}", warning.context, warning.message);
    }
    if !warnings.is_empty() {
        let what = if warnings.len() == 1 {
            "feature solo-R2 ignorada"
        } else {
            "features solo-R2 ignoradas"
        };
        eprintln!(
            "r1: {} {} en el IR (usa --emit-ui para el arbol v2)",
            warnings.len(),
            what
        );
    }
}

/// `true` si el nodo y todos sus ancestros se renderizan (sin `display: none`).
fn is_rendered(dom: &Dom, styles: &[Option<ComputedStyle>], id: usize) -> bool {
    let mut current = Some(id);
    while let Some(node) = current {
        if let Some(style) = styles.get(node).and_then(Option::as_ref) {
            if style.display == "none" {
                return false;
            }
        }
        current = dom.nodes[node].parent;
    }
    true
}

fn node_name(tag: &str, id: Option<&str>, first_class: Option<&str>) -> String {
    let mut name = tag.to_string();
    if let Some(id) = id {
        if !id.is_empty() {
            name.push('#');
            name.push_str(id);
        }
    }
    if let Some(class) = first_class {
        name.push('.');
        name.push_str(class);
    }
    name
}
