//! Cascade: selector -> computed style per node, with inheritance and `style=""`.
//!
//! Reproduces Python's `cascade.compute_styles`: priority key
//! `(!important, inline, specificity, rule order, declaration index)`.

use std::collections::HashMap;

use crate::parse::css::{
    parse_declarations, Rule, STATE_ACTIVE, STATE_DISABLED, STATE_FOCUS, STATE_HOVER,
};
use crate::parse::dom::{ua_display, Dom, NodeId};
use crate::style::color::{length, parse_color, percentage};
use crate::style::grid::{
    parse_grid_auto_rows, parse_grid_template_columns, GridAutoRows, GridTemplateColumns,
};
use crate::style::state::{StateStyle, StateStyles};
use crate::style::style_ext::{
    parse_box_shadows, parse_number, parse_rel_length, resolve_border_radius, resolve_border_side,
    BorderOut, LengthCtx, RelLength, StyleV3,
};

const SIDES: [&str; 4] = ["top", "right", "bottom", "left"];

const INHERITED: [&str; 7] = [
    "color",
    "font-family",
    "font-size",
    "font-weight",
    "line-height",
    "text-align",
    "white-space",
];

type Key = (u8, u8, (u32, u32, u32), u32, u32);

/// `color`/`font_size`/`font_family` are computed to keep cascade parity even
/// though this round does not emit text.
#[allow(dead_code)]
pub(crate) struct ComputedStyle {
    pub(crate) raw: HashMap<String, String>,
    pub(crate) display: String,
    /// top, right, bottom, left
    pub(crate) margin: [f64; 4],
    pub(crate) padding: [f64; 4],
    pub(crate) width: Option<f64>,
    pub(crate) height: Option<f64>,
    /// `width: 30%` -> 30.0; 0.0 when there is no percentage (`UiNode` contract).
    pub(crate) width_pct: f64,
    pub(crate) height_pct: f64,
    pub(crate) background: Option<String>,
    pub(crate) color: String,
    pub(crate) font_size: f64,
    pub(crate) font_family: String,
    pub(crate) font_weight: String,
    /// `left | center | right` (E1). Inherited.
    pub(crate) text_align: String,
    /// `line-height` resolved to px; 0.0 = `normal` (font metrics).
    pub(crate) line_height: f64,
    /// `margin-*: auto` per side (top, right, bottom, left). E1.
    pub(crate) margin_auto: [bool; 4],
    /// `box-sizing: border-box` (E8/A). false = content-box, the CSS default.
    pub(crate) box_sizing_border: bool,
    /// `flex-grow` / `flex-shrink` / `flex-basis` (px, -1 = auto). E8.
    pub(crate) flex_grow: f64,
    pub(crate) flex_shrink: f64,
    pub(crate) flex_basis: f64,
    /// `opacity` (0..1, clamped). Not inherited: a node with `opacity < 1`
    /// becomes a group that fades its own box and its whole subtree.
    pub(crate) opacity: f64,
    /// `position`: `static | relative | absolute`. `fixed`/`sticky` are
    /// diagnosed by the subset and fall back to `static`.
    pub(crate) position: String,
    /// top, right, bottom, left. Percentages are kept as percentage numbers
    /// (not baked) so the runtime resolves them against the right box.
    pub(crate) inset: [f64; 4],
    /// bit 0..3 = side is declared (not `auto`); bit 4..7 = that side is a
    /// percentage. 0 means all four insets are `auto`.
    pub(crate) inset_mask: u32,
    /// Blob v5: CSS `z-index`; `None` is `auto`. Only applied by the runtime
    /// to positioned nodes (relative/absolute). Negative values are valid.
    pub(crate) z_index: Option<i32>,
    pub(crate) flex_direction: String,
    pub(crate) justify_content: String,
    pub(crate) align_items: String,
    pub(crate) flex_wrap: String,
    pub(crate) gap: f64,
    /// v2: `gap` resolved per axis (`row-gap`/`column-gap` win over `gap`).
    pub(crate) row_gap: f64,
    pub(crate) column_gap: f64,
    /// v2: only `repeat(auto-fill, <px>)`; anything else stays `None`.
    pub(crate) grid_template_columns: Option<GridTemplateColumns>,
    /// v2: `<px>` or `auto`.
    pub(crate) grid_auto_rows: Option<GridAutoRows>,
    /// v2: `hidden | auto | scroll`; `visible`/unknown -> `None`.
    pub(crate) overflow: Option<String>,
    /// v2: `fill | cover | contain`; emitted inside `image.object_fit`.
    pub(crate) object_fit: Option<String>,
    /// Raw `data-hover` / `data-press` / `data-selected` / `data-action`.
    /// The cascade carries them because the runtime needs them per node.
    pub(crate) data_hover: Option<String>,
    pub(crate) data_press: Option<String>,
    pub(crate) data_selected: Option<String>,
    pub(crate) data_action: Option<String>,
    /// v3 CSS extensions (borders, radius, shadows, relative units).
    pub(crate) css3: StyleV3,
    /// Blob v6: effective `:hover`/`:active`/`:focus`/`:disabled` colors.
    pub(crate) states: StateStyles,
    /// `disabled` attribute on a `button`/`input`: the initial runtime
    /// WN_STATE_DISABLED bit (blob v6 states record).
    pub(crate) initial_disabled: bool,
}

pub(crate) fn compute_styles(
    dom: &Dom,
    rules: &[Rule],
    viewport: (f64, f64),
) -> Vec<Option<ComputedStyle>> {
    let mut styles: Vec<Option<ComputedStyle>> = Vec::with_capacity(dom.nodes.len());
    styles.resize_with(dom.nodes.len(), || None);
    // `rem` resolves against the computed `html` font-size. Pre-order
    // guarantees the root is computed before body and its descendants.
    let mut root_font_size = 16.0f64;

    for id in dom.preorder() {
        let mut raw: HashMap<String, String> = HashMap::new();
        let mut parent_font_size = 16.0f64;
        if let Some(parent) = dom.nodes[id].parent {
            if let Some(parent_style) = styles[parent].as_ref() {
                parent_font_size = parent_style.font_size;
                for name in INHERITED {
                    // `font-size` is inherited as a computed value, not as a
                    // token: a child without its own font-size must not
                    // re-resolve the parent's `em` against itself.
                    if name == "font-size" {
                        continue;
                    }
                    if let Some(value) = parent_style.raw.get(name) {
                        raw.insert(name.to_string(), value.clone());
                    }
                }
            }
        }

        // Base cascade: no state pseudo-class is active.
        let winners = collect_winners(dom, id, rules, 0);
        for (name, (_, value, _)) in winners {
            raw.insert(name, value);
        }
        let mut style = resolve(dom, id, raw, parent_font_size, root_font_size, viewport);
        let parent_style = dom.nodes[id].parent.and_then(|parent| styles[parent].as_ref());
        style.states = compute_state_styles(dom, id, rules, &style, parent_style);
        if dom.nodes[id].tag == "html" {
            root_font_size = style.font_size;
        }
        styles[id] = Some(style);
    }
    styles
}

/// Winner map for one state mask. `bool` is true when the winning declaration
/// comes from a rule with a state pseudo-class, so the value is a state
/// override (and not just the base/inline value re-evaluated for the state).
type Winners = HashMap<String, (Key, String, bool)>;

fn collect_winners(dom: &Dom, id: NodeId, rules: &[Rule], state_mask: u8) -> Winners {
    let mut winners: Winners = HashMap::new();
    for (order, rule) in rules.iter().enumerate() {
        if !rule.selector.matches_with_state(dom, id, state_mask) {
            continue;
        }
        let specificity = rule.selector.specificity();
        let from_state = rule.selector.has_state_pseudo();
        for (declaration_index, declaration) in rule.declarations.iter().enumerate() {
            let key: Key = (
                declaration.important as u8,
                0,
                specificity,
                order as u32,
                declaration_index as u32,
            );
            offer(
                &mut winners,
                &declaration.name,
                &declaration.value,
                key,
                from_state,
            );
        }
    }
    if let Some(inline) = dom.attr(id, "style") {
        for (declaration_index, declaration) in parse_declarations(inline).iter().enumerate() {
            let key: Key = (
                declaration.important as u8,
                1,
                (0, 0, 0),
                0,
                declaration_index as u32,
            );
            offer(
                &mut winners,
                &declaration.name,
                &declaration.value,
                key,
                false,
            );
        }
    }
    winners
}

fn offer(
    map: &mut Winners,
    name: &str,
    value: &str,
    key: Key,
    from_state: bool,
) {
    match map.get(name) {
        Some((current, _, _)) if *current > key => {}
        _ => {
            map.insert(name.to_string(), (key, value.to_string(), from_state));
        }
    }
}

/// Builds the four effective state styles for one node.
///
/// Combination rule (documented in `docs/wnui-format.md`):
/// - hover: `:hover` only.
/// - press: `:hover` and `:active` active together, so press includes the
///   hover declarations through the normal cascade.
/// - focus: `:focus` only.
/// - disabled: `:disabled` only, and only for nodes that can be disabled
///   (`button`/`input`); the runtime bit decides when it applies.
fn compute_state_styles(
    dom: &Dom,
    id: NodeId,
    rules: &[Rule],
    base: &ComputedStyle,
    parent: Option<&ComputedStyle>,
) -> StateStyles {
    let tag = dom.nodes[id].tag.as_str();
    let can_disable = tag == "button" || tag == "input";
    StateStyles {
        hover: compute_state(dom, id, rules, base, parent, STATE_HOVER),
        press: compute_state(
            dom,
            id,
            rules,
            base,
            parent,
            STATE_HOVER | STATE_ACTIVE,
        ),
        focus: compute_state(dom, id, rules, base, parent, STATE_FOCUS),
        disabled: if can_disable {
            compute_state(dom, id, rules, base, parent, STATE_DISABLED)
        } else {
            None
        },
    }
}

fn compute_state(
    dom: &Dom,
    id: NodeId,
    rules: &[Rule],
    base: &ComputedStyle,
    parent: Option<&ComputedStyle>,
    state_mask: u8,
) -> Option<StateStyle> {
    let winners = collect_winners(dom, id, rules, state_mask);
    let parent_text = parent.and_then(|style| parent_state_text(style, state_mask));
    let background = state_color(&winners, "background-color")
        .or_else(|| state_color(&winners, "background"));
    // `color` is inherited. A node with no own color declaration follows the
    // parent's effective color for the same state; a node with its own base
    // color keeps it (no override is needed).
    let text = if let Some(value) = state_color(&winners, "color") {
        Some(value)
    } else if !winners.contains_key("color") {
        parent_text.map(str::to_string)
    } else {
        None
    };
    let border = state_border(&winners, base, text.as_deref());
    let style = StateStyle {
        background,
        text,
        border,
    };
    if style.is_empty() {
        None
    } else {
        Some(style)
    }
}

/// Color override declared by an *active state rule*; `None` for base/inline
/// winners (those are already part of the base style).
fn state_color(winners: &Winners, name: &str) -> Option<String> {
    let (_, value, from_state) = winners.get(name)?;
    if !from_state {
        return None;
    }
    parse_color(Some(value))
}

fn parent_state_text(style: &ComputedStyle, state_mask: u8) -> Option<&str> {
    let states = &style.states;
    let state = match state_mask {
        STATE_HOVER => states.hover.as_ref(),
        mask if mask == (STATE_HOVER | STATE_ACTIVE) => states.press.as_ref(),
        STATE_FOCUS => states.focus.as_ref(),
        STATE_DISABLED => states.disabled.as_ref(),
        _ => None,
    };
    state.and_then(|state| state.text.as_deref())
}

/// Recolors the first visible base border side when a state rule changes it.
/// Only the color travels: state rules do not change border width/style in
/// this lane.
fn state_border(winners: &Winners, base: &ComputedStyle, state_text: Option<&str>) -> Option<String> {
    for (side, border) in base.css3.border.sides() {
        if !border.visible() {
            continue;
        }
        let name = format!("border-{side}-color");
        let Some((_, value, true)) = winners.get(&name) else {
            continue;
        };
        if value.trim().eq_ignore_ascii_case("currentcolor") {
            return Some(
                state_text
                    .map(str::to_string)
                    .unwrap_or_else(|| base.color.clone()),
            );
        }
        return parse_color(Some(value));
    }
    None
}

fn resolve(
    dom: &Dom,
    id: NodeId,
    raw: HashMap<String, String>,
    parent_font_size: f64,
    root_font_size: f64,
    viewport: (f64, f64),
) -> ComputedStyle {
    let tag = dom.nodes[id].tag.as_str();
    let font_size = raw
        .get("font-size")
        .and_then(|value| resolve_font_size(value, parent_font_size, root_font_size))
        .unwrap_or(parent_font_size);
    // Context for resolving `em`/`rem`/`vh`/`vw` to px at build time.
    let ctx = LengthCtx {
        font_size,
        root_font_size,
        viewport_width: viewport.0,
        viewport_height: viewport.1,
    };

    let mut margin = [0.0f64; 4];
    let mut padding = [0.0f64; 4];
    let mut margin_rel = [None; 4];
    let mut padding_rel = [None; 4];
    let mut margin_auto = [false; 4];
    for (index, side) in SIDES.iter().enumerate() {
        let (margin_px, _, margin_unit) =
            resolve_box_length(raw.get(&format!("margin-{side}")), &ctx);
        let (padding_px, _, padding_unit) =
            resolve_box_length(raw.get(&format!("padding-{side}")), &ctx);
        margin[index] = margin_px.unwrap_or(0.0);
        padding[index] = padding_px.unwrap_or(0.0);
        margin_rel[index] = margin_unit;
        padding_rel[index] = padding_unit;
        margin_auto[index] = raw
            .get(&format!("margin-{side}"))
            .is_some_and(|value| value.trim().eq_ignore_ascii_case("auto"));
    }

    let (width, width_pct, width_rel) = resolve_box_length(raw.get("width"), &ctx);
    let (height, height_pct, height_rel) = resolve_box_length(raw.get("height"), &ctx);
    let box_sizing_border = raw
        .get("box-sizing")
        .is_some_and(|value| value.trim().eq_ignore_ascii_case("border-box"));
    let (flex_grow, flex_shrink, flex_basis) = resolve_flex(&raw);
    let (min_width, min_width_pct, min_width_rel) = resolve_box_length(raw.get("min-width"), &ctx);
    let (max_width, max_width_pct, max_width_rel) = resolve_box_length(raw.get("max-width"), &ctx);
    let (min_height, min_height_pct, min_height_rel) =
        resolve_box_length(raw.get("min-height"), &ctx);
    let (max_height, max_height_pct, max_height_rel) =
        resolve_box_length(raw.get("max-height"), &ctx);

    // `gap` with the same "truthy" short-circuit as Python (0.0 is falsy).
    // `row_gap`/`column_gap` are the effective per-axis values: the longhand
    // wins over the shorthand (CSS), and `gap` is still computed as in v1.
    let (gap_direct, _, gap_rel) = resolve_box_length(raw.get("gap"), &ctx);
    let (row_gap_decl, _, row_gap_rel_decl) = resolve_box_length(raw.get("row-gap"), &ctx);
    let (column_gap_decl, _, column_gap_rel_decl) = resolve_box_length(raw.get("column-gap"), &ctx);
    let gap = match gap_direct {
        Some(value) => value,
        None => {
            let row = row_gap_decl.filter(|v| *v != 0.0);
            let column = column_gap_decl.filter(|v| *v != 0.0);
            row.or(column).unwrap_or(0.0)
        }
    };
    let row_gap = row_gap_decl.or(gap_direct).unwrap_or(0.0);
    let column_gap = column_gap_decl.or(gap_direct).unwrap_or(0.0);
    // The effective relative unit follows the same precedence as the px value.
    let row_gap_rel = if row_gap_decl.is_some() {
        row_gap_rel_decl
    } else {
        gap_rel
    };
    let column_gap_rel = if column_gap_decl.is_some() {
        column_gap_rel_decl
    } else {
        gap_rel
    };

    let overflow = keyword_value(raw.get("overflow"), &["hidden", "auto", "scroll"]);
    let object_fit = keyword_value(raw.get("object-fit"), &["fill", "cover", "contain"]);
    let grid_template_columns = raw
        .get("grid-template-columns")
        .and_then(|value| parse_grid_template_columns(value));
    let grid_auto_rows = raw
        .get("grid-auto-rows")
        .and_then(|value| parse_grid_auto_rows(value));

    let display = match raw.get("display") {
        Some(value) if !value.is_empty() => value.trim().to_ascii_lowercase(),
        _ => ua_display(tag).to_string(),
    };

    let background_value = match raw.get("background-color") {
        Some(value) if !value.is_empty() => Some(value.as_str()),
        _ => raw.get("background").map(String::as_str),
    };
    let background = parse_color(background_value);

    let color =
        parse_color(raw.get("color").map(String::as_str)).unwrap_or_else(|| "#000000".to_string());

    let side = |name: &str| resolve_side(&raw, name, &ctx, &color);
    let border = BorderOut {
        top: side("top"),
        right: side("right"),
        bottom: side("bottom"),
        left: side("left"),
    };

    let (border_radius, border_radius_rel) =
        resolve_border_radius(raw.get("border-radius").map(String::as_str), &ctx);
    let box_shadow = raw
        .get("box-shadow")
        .and_then(|value| parse_box_shadows(value, &ctx, &color))
        .unwrap_or_default();
    let opacity = raw
        .get("opacity")
        .and_then(|value| parse_number(value))
        .map(|value| value.clamp(0.0, 1.0))
        .unwrap_or(1.0);
    // Only relative/absolute are part of the contract; fixed/sticky are
    // diagnosed by `subset` and behave as static here so a page still lays out.
    let position = match raw.get("position") {
        Some(value) if value.trim().eq_ignore_ascii_case("relative") => "relative".to_string(),
        Some(value) if value.trim().eq_ignore_ascii_case("absolute") => "absolute".to_string(),
        _ => "static".to_string(),
    };
    let (inset, inset_mask) = resolve_insets(&raw);
    // Only the integer grammar is valid for z-index; `auto` and invalid
    // declarations stay `auto` (CSS drops an unsupported value). The subset
    // reports malformed values separately.
    let z_index = raw.get("z-index").and_then(|value| parse_z_index(value));
    let states = StateStyles::default();
    let initial_disabled = (tag == "button" || tag == "input")
        && dom.attr(id, "disabled").is_some();

    // E1: text-align is discrete and inherited; line-height resolves to px
    // (`normal` -> 0.0 so the runtime uses ascender + descender).
    let text_align = keyword_value(raw.get("text-align"), &["left", "center", "right"])
        .unwrap_or_else(|| "left".to_string());
    let line_height = resolve_line_height(raw.get("line-height"), font_size, root_font_size);

    ComputedStyle {
        position,
        inset,
        inset_mask,
        z_index,
        color,
        font_family: raw
            .get("font-family")
            .cloned()
            .unwrap_or_else(|| "sans-serif".to_string()),
        font_weight: raw
            .get("font-weight")
            .filter(|value| !value.trim().is_empty())
            .cloned()
            .unwrap_or_else(|| "400".to_string()),
        text_align,
        line_height,
        margin_auto,
        box_sizing_border,
        flex_grow,
        flex_shrink,
        flex_basis,
        opacity,
        flex_direction: keyword(&raw, "flex-direction", "row"),
        justify_content: keyword(&raw, "justify-content", "flex-start"),
        align_items: keyword(&raw, "align-items", "stretch"),
        flex_wrap: keyword(&raw, "flex-wrap", "nowrap"),
        display,
        margin,
        padding,
        width,
        height,
        width_pct,
        height_pct,
        background,
        font_size,
        gap,
        row_gap,
        column_gap,
        grid_template_columns,
        grid_auto_rows,
        overflow,
        object_fit,
        data_hover: dom.attr(id, "data-hover").map(str::to_string),
        data_press: dom.attr(id, "data-press").map(str::to_string),
        data_selected: dom.attr(id, "data-selected").map(str::to_string),
        data_action: dom.attr(id, "data-action").map(str::to_string),
        css3: StyleV3 {
            width_rel,
            height_rel,
            min_width,
            min_width_pct,
            min_width_rel,
            max_width,
            max_width_pct,
            max_width_rel,
            min_height,
            min_height_pct,
            min_height_rel,
            max_height,
            max_height_pct,
            max_height_rel,
            margin_rel,
            padding_rel,
            gap_rel,
            row_gap_rel,
            column_gap_rel,
            border,
            border_radius,
            border_radius_rel,
            box_shadow,
        },
        states,
        initial_disabled,
        raw,
    }
}

fn resolve_side(
    raw: &HashMap<String, String>,
    side: &str,
    ctx: &LengthCtx,
    color: &str,
) -> crate::style::style_ext::BorderSide {
    resolve_border_side(
        raw.get(&format!("border-{side}-width")),
        raw.get(&format!("border-{side}-style")),
        raw.get(&format!("border-{side}-color")),
        ctx,
        color,
    )
}

/// Resolves a box property to `(px, pct, declared unit)`.
///
/// - `30%` -> `(None, 30.0, None)` (the v1 contract keeps the percentage).
/// - `2em` -> `(resolved px, 0.0, Some(2em))`.
/// - `12px`/`12` -> `(Some(12.0), 0.0, None)`.
/// - `auto`/`none`/unknown -> `(None, 0.0, None)`.
fn resolve_box_length(
    value: Option<&String>,
    ctx: &LengthCtx,
) -> (Option<f64>, f64, Option<RelLength>) {
    let Some(raw) = value else {
        return (None, 0.0, None);
    };
    let text = raw.trim();
    if text.is_empty() || text.eq_ignore_ascii_case("auto") || text.eq_ignore_ascii_case("none") {
        return (None, 0.0, None);
    }
    if text.ends_with('%') {
        return (None, percentage(Some(text)), None);
    }
    if let Some(px) = length(Some(text)) {
        return (Some(px), 0.0, None);
    }
    if let Some(rel) = parse_rel_length(text) {
        return (Some(ctx.resolve(rel)), 0.0, Some(rel));
    }
    (None, 0.0, None)
}

/// `auto | <integer>` -> `Some(value)` or `None` (auto/invalid).
///
/// CSS requires an integer (no fraction, no unit). The blob stores int32 with
/// `INT32_MIN` reserved for `auto`, so that one explicit value is rejected.
fn parse_z_index(value: &str) -> Option<i32> {
    let text = value.trim();
    if text.is_empty() || text.eq_ignore_ascii_case("auto") {
        return None;
    }
    let integer = parse_css_integer(text)?;
    if integer == i64::from(i32::MIN) {
        return None;
    }
    i32::try_from(integer).ok()
}

/// Optional sign followed by one or more ASCII digits. No exponent or unit.
fn parse_css_integer(text: &str) -> Option<i64> {
    let mut chars = text.chars();
    let first = chars.next()?;
    let digits = match first {
        '+' | '-' => chars.as_str(),
        _ => text,
    };
    if digits.is_empty() || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    text.parse::<i64>().ok()
}

/// `top`/`right`/`bottom`/`left` -> `(values, mask)`.
///
/// Bit `i` means side `i` (top,right,bottom,left) is declared; bit `4+i` means
/// that value is a percentage. Percentages stay as numbers: the runtime
/// resolves them against the containing block, never the frontend. Values the
/// cascade cannot parse are treated as `auto` (the subset reports them).
fn resolve_insets(raw: &HashMap<String, String>) -> ([f64; 4], u32) {
    let mut inset = [0.0f64; 4];
    let mut mask = 0u32;
    for (index, side) in SIDES.iter().enumerate() {
        let Some(value) = raw.get(*side) else {
            continue;
        };
        let text = value.trim();
        if text.is_empty() || text.eq_ignore_ascii_case("auto") {
            continue;
        }
        if let Some(number) = text.strip_suffix('%') {
            if let Some(percent) = parse_number(number) {
                inset[index] = percent;
                mask |= 1 << index;
                mask |= 1 << (4 + index);
            }
            continue;
        }
        if let Some(px) = length(Some(text)) {
            inset[index] = px;
            mask |= 1 << index;
        }
        // `em`/`rem`/`vh`/`vw` are not part of this lane: the subset reports
        // them and the cascade leaves the side at `auto`.
    }
    (inset, mask)
}

/// Computed `font-size`: px/number, `em` (parent), `rem` (root) or `%` (parent).
fn resolve_font_size(value: &str, parent: f64, root: f64) -> Option<f64> {
    let text = value.trim().to_ascii_lowercase();
    if text.is_empty() {
        return None;
    }
    if let Some(px) = length(Some(&text)) {
        return Some(px);
    }
    if let Some(number) = text.strip_suffix('%') {
        return parse_number(number).map(|value| parent * value / 100.0);
    }
    if let Some(number) = text.strip_suffix("rem") {
        return parse_number(number).map(|value| root * value);
    }
    if let Some(number) = text.strip_suffix("em") {
        return parse_number(number).map(|value| parent * value);
    }
    None
}

fn keyword(raw: &HashMap<String, String>, name: &str, default: &str) -> String {
    match raw.get(name) {
        Some(value) if !value.is_empty() => value.trim().to_ascii_lowercase(),
        _ => default.to_string(),
    }
}

/// `line-height: normal | <px> | <number> | <percentage>` -> px.
///
/// `normal` (or an unrecognized value) returns 0.0: the runtime then uses the
/// font's `ascender + descender`. A unitless number is a multiple of the
/// computed `font-size` (CSS), even though the subset only accepts px|normal.
fn resolve_line_height(value: Option<&String>, font_size: f64, root_font_size: f64) -> f64 {
    let Some(raw) = value else { return 0.0 };
    let text = raw.trim().to_ascii_lowercase();
    if text.is_empty() || text == "normal" {
        return 0.0;
    }
    // `24px` is absolute px; a bare number multiplies the font-size.
    if text.ends_with("px") {
        return match length(Some(&text)) {
            Some(px) if px > 0.0 => px,
            _ => 0.0,
        };
    }
    if let Some(number) = text.strip_suffix('%') {
        return parse_number(number)
            .map(|value| (font_size * value / 100.0).max(0.0))
            .unwrap_or(0.0);
    }
    if let Some((number, rest)) = parse_number_unitless(&text) {
        if rest.is_empty() {
            return (font_size * number).max(0.0);
        }
    }
    if let Some(px) = length(Some(&text)) {
        // Any other absolute length that `length` accepts.
        return if px > 0.0 { px } else { 0.0 };
    }
    // `1.5em`/`2rem`: a length relative to the node's computed font-size.
    if let Some(rel) = parse_rel_length(&text) {
        let ctx = LengthCtx {
            font_size,
            root_font_size,
            viewport_width: 0.0,
            viewport_height: 0.0,
        };
        return ctx.resolve(rel).max(0.0);
    }
    0.0
}

/// Unitless CSS number (accepts sign and decimals).
fn parse_number_unitless(text: &str) -> Option<(f64, &str)> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return None;
    }
    let mut end = 0usize;
    let bytes = trimmed.as_bytes();
    if bytes[0] == b'+' || bytes[0] == b'-' {
        end = 1;
    }
    let mut digits = 0usize;
    while end < bytes.len() && bytes[end].is_ascii_digit() {
        end += 1;
        digits += 1;
    }
    if end < bytes.len() && bytes[end] == b'.' {
        end += 1;
        while end < bytes.len() && bytes[end].is_ascii_digit() {
            end += 1;
            digits += 1;
        }
    }
    if digits == 0 {
        return None;
    }
    let value = trimmed[..end].parse::<f64>().ok()?;
    Some((value, &trimmed[end..]))
}

/// `flex-grow`, `flex-shrink` and `flex-basis` (px; -1 = auto), including the
/// `flex: <grow> [<shrink>] [<basis>]` shorthand.
fn resolve_flex(raw: &HashMap<String, String>) -> (f64, f64, f64) {
    let mut grow = raw
        .get("flex-grow")
        .and_then(|v| v.trim().parse::<f64>().ok())
        .unwrap_or(0.0);
    let mut shrink = raw
        .get("flex-shrink")
        .and_then(|v| v.trim().parse::<f64>().ok())
        .unwrap_or(1.0);
    let mut basis = flex_basis_px(raw.get("flex-basis")).unwrap_or(-1.0);

    if let Some(value) = raw.get("flex") {
        let text = value.trim();
        if !text.is_empty() && !text.eq_ignore_ascii_case("none") {
            let mut parts = text.split_whitespace();
            if let Some(first) = parts.next() {
                if let Some(number) = first.parse::<f64>().ok() {
                    grow = number;
                    // `flex: <grow>` alone means basis 0 (CSS one-value form).
                    basis = 0.0;
                    if let Some(second) = parts.next() {
                        if let Some(number) = second.parse::<f64>().ok() {
                            shrink = number;
                            if let Some(third) = parts.next() {
                                basis = flex_basis_px(Some(&third.to_string())).unwrap_or(basis);
                            }
                        } else {
                            basis = flex_basis_px(Some(&second.to_string())).unwrap_or(basis);
                        }
                    }
                } else {
                    basis = flex_basis_px(Some(&first.to_string())).unwrap_or(basis);
                }
            }
        }
    }
    (grow.max(0.0), shrink.max(0.0), basis)
}

fn flex_basis_px(value: Option<&String>) -> Option<f64> {
    let text = value?.trim();
    if text.is_empty() || text.eq_ignore_ascii_case("auto") {
        return None;
    }
    let (number, unit) = parse_number_unitless(text)?;
    let unit = unit.trim().to_ascii_lowercase();
    if unit.is_empty() || unit == "px" {
        Some(number)
    } else {
        None
    }
}

/// Normalized keyword if it belongs to `allowed`; otherwise `None`.
fn keyword_value(value: Option<&String>, allowed: &[&str]) -> Option<String> {
    let keyword = value?.trim().to_ascii_lowercase();
    if allowed.contains(&keyword.as_str()) {
        Some(keyword)
    } else {
        None
    }
}
