//! Layout de bloque y flex (row/column) con line boxes de texto.
//!
//! Puerto directo de `layout.py`: modelo border-box, bloque con
//! `cursor_y = child.bottom + margin_bottom`, flex row/column con `gap`,
//! `justify-content` y `align-items`, y una linea de texto por nodo antes de
//! sus hijos. Las metricas se inyectan (`MetricsFn`) para que layout no
//! dependa del rasterizador.
//!
//! El texto propio de un nodo se coloca en `content_x`,
//! `baseline = content_y + ascender`, y avanza `ascender + descender` en y.
//! El pintado es pre-orden: fondo del nodo, su texto y despues sus hijos.

use crate::cascade::ComputedStyle;
use crate::dom::{Dom, NodeId};

const MAX_DEPTH: usize = 512;

/// `(font-family, size-px) -> (ascender, descender)` en px.
pub type MetricsFn<'a> = &'a dyn Fn(&str, f64) -> Result<(f64, f64), String>;

#[derive(Clone)]
pub struct TextRun {
    pub text: String,
    pub x: f64,
    pub baseline: f64,
    pub font_family: String,
    pub font_size: f64,
    pub color: String,
}

pub struct RectOp {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub fill: String,
}

/// Op de pintado en orden de documento; las runs se shapean despues.
pub enum PaintOp {
    Rect(RectOp),
    Run(TextRun),
}

pub struct LayoutBox {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub background: Option<String>,
    pub margin_bottom: f64,
    pub children: Vec<LayoutBox>,
    pub text_runs: Vec<TextRun>,
}

impl LayoutBox {
    pub fn bottom(&self) -> f64 {
        self.y + self.height
    }

    pub fn translate(&mut self, dx: f64, dy: f64) {
        self.x += dx;
        self.y += dy;
        for run in &mut self.text_runs {
            run.x += dx;
            run.baseline += dy;
        }
        for child in &mut self.children {
            child.translate(dx, dy);
        }
    }
}

/// Colapsa whitespace como `white-space: normal` en una sola linea.
pub fn normalize_text(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

pub fn find_body(dom: &Dom) -> NodeId {
    dom.preorder()
        .into_iter()
        .find(|id| dom.nodes[*id].tag == "body")
        .unwrap_or(dom.root)
}

pub fn layout_tree(
    dom: &Dom,
    styles: &[Option<ComputedStyle>],
    viewport_width: f64,
    metrics: MetricsFn<'_>,
) -> Result<Option<LayoutBox>, String> {
    let body = find_body(dom);
    let Some(style) = styles.get(body).and_then(Option::as_ref) else {
        return Ok(None);
    };
    Ok(Some(layout_box(
        dom,
        styles,
        body,
        style,
        0.0,
        0.0,
        viewport_width,
        metrics,
        0,
    )?))
}

#[allow(clippy::too_many_arguments)]
fn layout_box(
    dom: &Dom,
    styles: &[Option<ComputedStyle>],
    id: NodeId,
    style: &ComputedStyle,
    origin_x: f64,
    origin_y: f64,
    available_width: f64,
    metrics: MetricsFn<'_>,
    depth: usize,
) -> Result<LayoutBox, String> {
    let margin = style.margin;
    let padding = style.padding;

    let width = style
        .width
        .unwrap_or_else(|| (available_width - margin[1] - margin[3]).max(0.0));
    let x = origin_x + margin[3];
    let y = origin_y + margin[0];
    let content_x = x + padding[3];
    let content_y = y + padding[0];
    let content_width = (width - padding[1] - padding[3]).max(0.0);

    let mut result = LayoutBox {
        x,
        y,
        width,
        height: 0.0,
        background: style.background.clone(),
        margin_bottom: margin[2],
        children: Vec::new(),
        text_runs: Vec::new(),
    };

    let mut cursor_y = content_y;

    let text = normalize_text(&dom.nodes[id].text);
    if !text.is_empty() {
        // R1 (display list post-layout) no modela E1: `font-weight` real,
        // `text-align`, `line-height` ni `margin: auto` cambian este run. Se
        // aplican solo en el arbol pre-layout / runtime R2 (`ui_tree.rs`), de
        // modo que el IR de R1 sigue siendo byte a byte compatible.
        let (ascender, descender) = metrics(&style.font_family, style.font_size)?;
        result.text_runs.push(TextRun {
            text,
            x: content_x,
            baseline: content_y + ascender,
            font_family: style.font_family.clone(),
            font_size: style.font_size,
            color: style.color.clone(),
        });
        cursor_y += ascender + descender;
    }

    let children: Vec<NodeId> = dom.nodes[id]
        .children
        .iter()
        .copied()
        .filter(|child| {
            matches!(
                styles[*child].as_ref().map(|style| style.display.as_str()),
                Some("block") | Some("flex")
            )
        })
        .collect();

    if depth < MAX_DEPTH {
        if style.display == "flex" && !children.is_empty() {
            let content_height = style
                .height
                .map(|height| (height - padding[0] - padding[2]).max(0.0));
            cursor_y += layout_flex_children(
                &mut result,
                dom,
                styles,
                &children,
                content_x,
                cursor_y,
                content_width,
                content_height,
                style,
                metrics,
                depth + 1,
            )?;
        } else {
            for child in children {
                let child_style = match styles[child].as_ref() {
                    Some(style) => style,
                    None => continue,
                };
                let child_box = layout_box(
                    dom,
                    styles,
                    child,
                    child_style,
                    content_x,
                    cursor_y,
                    content_width,
                    metrics,
                    depth + 1,
                )?;
                cursor_y = child_box.bottom() + child_box.margin_bottom;
                result.children.push(child_box);
            }
        }
    }

    let content_used = cursor_y - content_y;
    result.height = style
        .height
        .unwrap_or_else(|| (content_used + padding[0] + padding[2]).max(0.0));
    Ok(result)
}

#[allow(clippy::too_many_arguments)]
fn layout_flex_children(
    parent_box: &mut LayoutBox,
    dom: &Dom,
    styles: &[Option<ComputedStyle>],
    children: &[NodeId],
    content_x: f64,
    content_y: f64,
    content_width: f64,
    content_height: Option<f64>,
    parent: &ComputedStyle,
    metrics: MetricsFn<'_>,
    depth: usize,
) -> Result<f64, String> {
    if children.is_empty() {
        return Ok(0.0);
    }
    let gap = parent.gap.max(0.0);
    let row = parent.flex_direction != "column";

    // Se mide cada hijo en (0,0) y luego se traslada a su posicion final.
    let mut measured: Vec<(NodeId, LayoutBox, &ComputedStyle)> = Vec::with_capacity(children.len());
    for child in children {
        let child_style = match styles[*child].as_ref() {
            Some(style) => style,
            None => continue,
        };
        let child_box = layout_box(
            dom,
            styles,
            *child,
            child_style,
            0.0,
            0.0,
            content_width,
            metrics,
            depth,
        )?;
        measured.push((*child, child_box, child_style));
    }
    if measured.is_empty() {
        return Ok(0.0);
    }

    let count = measured.len();
    let mut main_sizes: Vec<f64> = Vec::with_capacity(count);
    let mut cross_sizes: Vec<f64> = Vec::with_capacity(count);
    for (_, child_box, _) in &measured {
        if row {
            main_sizes.push(child_box.width);
            cross_sizes.push(child_box.height);
        } else {
            main_sizes.push(child_box.height);
            cross_sizes.push(child_box.width);
        }
    }

    let cross_max = cross_sizes.iter().copied().fold(0.0f64, f64::max);
    let (extent, main_available) = if row {
        (content_height.unwrap_or(cross_max), content_width)
    } else {
        let summed: f64 = main_sizes.iter().sum::<f64>() + gap * (count as f64 - 1.0);
        (content_width, content_height.unwrap_or(summed))
    };

    let used: f64 = main_sizes.iter().sum::<f64>() + gap * (count as f64 - 1.0);
    let free = (main_available - used).max(0.0);
    let (start, spacing) = justify(&parent.justify_content, free, count, gap);

    let mut cursor = 0.0f64;
    for (_, mut child_box, child_style) in measured {
        let main_size = if row { child_box.width } else { child_box.height };
        let mut cross_size = if row { child_box.height } else { child_box.width };

        if parent.align_items == "stretch" || parent.align_items == "normal" {
            if row && child_style.height.is_none() {
                child_box.height = extent;
                cross_size = extent;
            } else if !row && child_style.width.is_none() {
                child_box.width = extent;
                cross_size = extent;
            }
        }

        let cross_offset = match parent.align_items.as_str() {
            "center" => (extent - cross_size) / 2.0,
            "flex-end" | "end" => extent - cross_size,
            _ => 0.0,
        };

        if row {
            child_box.translate(content_x + start + cursor, content_y + cross_offset);
        } else {
            child_box.translate(content_x + cross_offset, content_y + start + cursor);
        }
        parent_box.children.push(child_box);
        cursor += main_size + spacing;
    }

    if row {
        Ok(extent)
    } else {
        Ok(used)
    }
}

/// `(start_offset, spacing)` en el eje principal, igual que `_justify`.
fn justify(name: &str, free: f64, count: usize, gap: f64) -> (f64, f64) {
    if count <= 1 {
        let start = if name == "center" {
            free / 2.0
        } else if name == "flex-end" || name == "end" {
            free
        } else {
            0.0
        };
        return (start, gap);
    }
    if name == "center" {
        (free / 2.0, gap)
    } else if name == "flex-end" || name == "end" {
        (free, gap)
    } else if name == "space-between" {
        (0.0, gap + free / (count as f64 - 1.0))
    } else {
        (0.0, gap)
    }
}

/// Pre-orden: fondo del nodo, su texto y despues los hijos (documento).
pub fn paint(root: &LayoutBox, ops: &mut Vec<PaintOp>) {
    if let Some(fill) = &root.background {
        ops.push(PaintOp::Rect(RectOp {
            x: root.x,
            y: root.y,
            w: root.width,
            h: root.height,
            fill: fill.clone(),
        }));
    }
    for run in &root.text_runs {
        ops.push(PaintOp::Run(run.clone()));
    }
    for child in &root.children {
        paint(child, ops);
    }
}
