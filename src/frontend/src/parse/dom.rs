//! HTML -> DOM using html5ever with a custom `TreeSink`.
//!
//! The resulting tree mimics `html5lib` with `treebuilder="etree"`: elements
//! only, `text` holds the node's own text before its children, and comment and
//! doctype nodes are dropped.

use std::borrow::Cow;
use std::cell::{Cell, RefCell};
use std::fmt;
use std::fs;
use std::path::Path;

use html5ever::interface::tree_builder::{ElementFlags, NodeOrText, QuirksMode, TreeSink};
use html5ever::interface::ElemName;
use html5ever::tendril::{StrTendril, TendrilSink};
use html5ever::{parse_document, Attribute, LocalName, Namespace, QualName};

pub(crate) type NodeId = usize;

const NON_RENDERED: [&str; 10] = [
    "head", "title", "meta", "link", "style", "script", "base", "template", "noscript", "track",
];

/// Owned (cloned) name so it can be returned out of a `RefCell`.
pub(crate) struct OwnedName {
    ns: Namespace,
    local: LocalName,
}

impl ElemName for OwnedName {
    fn ns(&self) -> &Namespace {
        &self.ns
    }
    fn local_name(&self) -> &LocalName {
        &self.local
    }
}

impl fmt::Debug for OwnedName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", self.local)
    }
}

pub(crate) struct Node {
    pub(crate) tag: String,
    pub(crate) attrs: Vec<(String, String)>,
    pub(crate) parent: Option<NodeId>,
    pub(crate) children: Vec<NodeId>,
    pub(crate) text: String,
}

impl Node {
    pub(crate) fn attr(&self, name: &str) -> Option<&str> {
        self.attrs
            .iter()
            .find(|(key, _)| key == name)
            .map(|(_, value)| value.as_str())
    }

    pub(crate) fn classes(&self) -> impl Iterator<Item = &str> {
        self.attr("class").unwrap_or("").split_whitespace()
    }
}

pub(crate) struct Dom {
    pub(crate) nodes: Vec<Node>,
    pub(crate) root: NodeId,
}

impl Dom {
    pub(crate) fn preorder(&self) -> Vec<NodeId> {
        let mut out = Vec::with_capacity(self.nodes.len());
        let mut stack = vec![self.root];
        while let Some(id) = stack.pop() {
            out.push(id);
            for child in self.nodes[id].children.iter().rev() {
                stack.push(*child);
            }
        }
        out
    }

    pub(crate) fn attr(&self, id: NodeId, name: &str) -> Option<&str> {
        self.nodes[id].attr(name)
    }
}

enum RawKind {
    Document,
    Doctype,
    Text(String),
    Comment,
    Element {
        name: QualName,
        attrs: Vec<(String, String)>,
        template: Option<NodeId>,
    },
}

struct RawNode {
    kind: RawKind,
    parent: Option<NodeId>,
    first: Option<NodeId>,
    last: Option<NodeId>,
    prev: Option<NodeId>,
    next: Option<NodeId>,
}

impl RawNode {
    fn new(kind: RawKind) -> Self {
        RawNode {
            kind,
            parent: None,
            first: None,
            last: None,
            prev: None,
            next: None,
        }
    }
}

struct Sink {
    nodes: RefCell<Vec<RawNode>>,
    document: NodeId,
    quirks: Cell<QuirksMode>,
    errors: Cell<u32>,
}

impl Sink {
    fn new() -> Self {
        Sink {
            nodes: RefCell::new(vec![RawNode::new(RawKind::Document)]),
            document: 0,
            quirks: Cell::new(QuirksMode::NoQuirks),
            errors: Cell::new(0),
        }
    }

    fn new_node(&self, kind: RawKind) -> NodeId {
        let mut nodes = self.nodes.borrow_mut();
        nodes.push(RawNode::new(kind));
        nodes.len() - 1
    }

    fn detach(nodes: &mut [RawNode], node: NodeId) {
        let parent = nodes[node].parent.take();
        let prev = nodes[node].prev.take();
        let next = nodes[node].next.take();
        if let Some(next) = next {
            nodes[next].prev = prev;
        } else if let Some(parent) = parent {
            nodes[parent].last = prev;
        }
        if let Some(prev) = prev {
            nodes[prev].next = next;
        } else if let Some(parent) = parent {
            nodes[parent].first = next;
        }
    }

    fn append_child(&self, parent: NodeId, child: NodeId) {
        let mut nodes = self.nodes.borrow_mut();
        Self::detach(&mut nodes, child);
        nodes[child].parent = Some(parent);
        let last = nodes[parent].last;
        nodes[child].prev = last;
        match last {
            Some(last) => nodes[last].next = Some(child),
            None => nodes[parent].first = Some(child),
        }
        nodes[parent].last = Some(child);
    }

    fn insert_before(&self, sibling: NodeId, node: NodeId) {
        let mut nodes = self.nodes.borrow_mut();
        Self::detach(&mut nodes, node);
        let parent = nodes[sibling].parent;
        nodes[node].parent = parent;
        nodes[node].next = Some(sibling);
        let prev = nodes[sibling].prev.take();
        nodes[node].prev = prev;
        match prev {
            Some(prev) => nodes[prev].next = Some(node),
            None => {
                if let Some(parent) = parent {
                    nodes[parent].first = Some(node);
                }
            }
        }
        nodes[sibling].prev = Some(node);
    }

    fn append_common(&self, parent_prev: Option<NodeId>, parent: NodeId, child: NodeOrText<NodeId>) {
        match child {
            NodeOrText::AppendText(text) => {
                if let Some(prev) = parent_prev {
                    let mut nodes = self.nodes.borrow_mut();
                    if let RawKind::Text(contents) = &mut nodes[prev].kind {
                        contents.push_str(&text);
                        return;
                    }
                }
                let id = self.new_node(RawKind::Text(text.to_string()));
                self.append_child(parent, id);
            }
            NodeOrText::AppendNode(node) => self.append_child(parent, node),
        }
    }
}

impl TreeSink for Sink {
    type Handle = NodeId;
    type Output = Dom;
    type ElemName<'a> = OwnedName;

    fn finish(self) -> Dom {
        build_dom(self.nodes.into_inner(), self.document)
    }

    fn parse_error(&self, _msg: Cow<'static, str>) {
        self.errors.set(self.errors.get() + 1);
    }

    fn get_document(&self) -> NodeId {
        self.document
    }

    fn elem_name<'a>(&'a self, target: &'a NodeId) -> OwnedName {
        let nodes = self.nodes.borrow();
        match &nodes[*target].kind {
            RawKind::Element { name, .. } => OwnedName {
                ns: name.ns.clone(),
                local: name.local.clone(),
            },
            _ => panic!("elem_name sobre un nodo que no es elemento"),
        }
    }

    fn create_element(
        &self,
        name: QualName,
        attrs: Vec<Attribute>,
        flags: ElementFlags,
    ) -> NodeId {
        let template = if flags.template {
            Some(self.new_node(RawKind::Document))
        } else {
            None
        };
        let attrs = attrs
            .into_iter()
            .map(|attr| {
                (
                    attr.name.local.to_string(),
                    attr.value.to_string(),
                )
            })
            .collect();
        self.new_node(RawKind::Element {
            name,
            attrs,
            template,
        })
    }

    fn create_comment(&self, _text: StrTendril) -> NodeId {
        self.new_node(RawKind::Comment)
    }

    fn create_pi(&self, _target: StrTendril, _data: StrTendril) -> NodeId {
        self.new_node(RawKind::Comment)
    }

    fn append(&self, parent: &NodeId, child: NodeOrText<NodeId>) {
        let prev = self.nodes.borrow()[*parent].last;
        self.append_common(prev, *parent, child);
    }

    fn append_before_sibling(&self, sibling: &NodeId, child: NodeOrText<NodeId>) {
        match child {
            NodeOrText::AppendText(text) => {
                let prev = self.nodes.borrow()[*sibling].prev;
                if let Some(prev) = prev {
                    let mut nodes = self.nodes.borrow_mut();
                    if let RawKind::Text(contents) = &mut nodes[prev].kind {
                        contents.push_str(&text);
                        return;
                    }
                }
                let id = self.new_node(RawKind::Text(text.to_string()));
                self.insert_before(*sibling, id);
            }
            NodeOrText::AppendNode(node) => self.insert_before(*sibling, node),
        }
    }

    fn append_based_on_parent_node(
        &self,
        element: &NodeId,
        prev_element: &NodeId,
        child: NodeOrText<NodeId>,
    ) {
        let has_parent = self.nodes.borrow()[*element].parent.is_some();
        if has_parent {
            self.append_before_sibling(element, child);
        } else {
            self.append(prev_element, child);
        }
    }

    fn append_doctype_to_document(
        &self,
        _name: StrTendril,
        _public_id: StrTendril,
        _system_id: StrTendril,
    ) {
        let id = self.new_node(RawKind::Doctype);
        self.append_child(self.document, id);
    }

    fn get_template_contents(&self, target: &NodeId) -> NodeId {
        let nodes = self.nodes.borrow();
        match &nodes[*target].kind {
            RawKind::Element {
                template: Some(contents),
                ..
            } => *contents,
            _ => panic!("get_template_contents sobre un nodo que no es template"),
        }
    }

    fn same_node(&self, x: &NodeId, y: &NodeId) -> bool {
        x == y
    }

    fn set_quirks_mode(&self, mode: QuirksMode) {
        self.quirks.set(mode);
    }

    fn add_attrs_if_missing(&self, target: &NodeId, attrs: Vec<Attribute>) {
        let mut nodes = self.nodes.borrow_mut();
        if let RawKind::Element { attrs: existing, .. } = &mut nodes[*target].kind {
            for attr in attrs {
                let name = attr.name.local.to_string();
                if !existing.iter().any(|(key, _)| *key == name) {
                    existing.push((name, attr.value.to_string()));
                }
            }
        }
    }

    fn remove_from_parent(&self, target: &NodeId) {
        let mut nodes = self.nodes.borrow_mut();
        Self::detach(&mut nodes, *target);
    }

    fn reparent_children(&self, node: &NodeId, new_parent: &NodeId) {
        loop {
            let child = {
                let nodes = self.nodes.borrow();
                nodes[*node].first
            };
            match child {
                Some(child) => self.append_child(*new_parent, child),
                None => break,
            }
        }
    }
}

fn build_dom(raw: Vec<RawNode>, document: NodeId) -> Dom {
    let mut root = None;
    let mut cursor = raw[document].first;
    while let Some(id) = cursor {
        if matches!(raw[id].kind, RawKind::Element { .. }) {
            root = Some(id);
            break;
        }
        cursor = raw[id].next;
    }
    let root = root.unwrap_or(document);
    let mut nodes: Vec<Node> = Vec::new();
    let root_index = convert(&raw, root, None, &mut nodes);
    Dom {
        nodes,
        root: root_index,
    }
}

fn convert(raw: &[RawNode], id: NodeId, parent: Option<NodeId>, out: &mut Vec<Node>) -> NodeId {
    let (tag, attrs) = match &raw[id].kind {
        RawKind::Element { name, attrs, .. } => (
            name.local.to_string().to_ascii_lowercase(),
            attrs.clone(),
        ),
        _ => unreachable!("convert() solo recibe elementos"),
    };
    let index = out.len();
    out.push(Node {
        tag,
        attrs,
        parent,
        children: Vec::new(),
        text: String::new(),
    });

    let mut text = String::new();
    let mut seen_element = false;
    let mut cursor = raw[id].first;
    while let Some(child) = cursor {
        match &raw[child].kind {
            RawKind::Text(contents) => {
                if !seen_element {
                    text.push_str(contents);
                }
            }
            RawKind::Element { .. } => {
                seen_element = true;
                let child_index = convert(raw, child, Some(index), out);
                out[index].children.push(child_index);
            }
            _ => {}
        }
        cursor = raw[child].next;
    }
    out[index].text = text;
    index
}

pub(crate) fn parse_html(text: &str) -> Dom {
    // html5lib defaults to scripting=False; html5ever starts with true.
    let options = html5ever::driver::ParseOpts {
        tree_builder: html5ever::tree_builder::TreeBuilderOpts {
            scripting_enabled: false,
            ..Default::default()
        },
        ..Default::default()
    };
    let sink = Sink::new();
    parse_document(sink, options)
        .from_utf8()
        .one(text.as_bytes())
}

/// CSS in document order: inline `<style>` and `<link rel=stylesheet>`.
pub(crate) fn stylesheet_texts(dom: &Dom, base_dir: &Path) -> Result<Vec<String>, String> {
    let mut texts = Vec::new();
    for id in dom.preorder() {
        let node = &dom.nodes[id];
        match node.tag.as_str() {
            "style" => texts.push(node.text.clone()),
            "link" => {
                let href = node.attr("href").unwrap_or("");
                let rel = node.attr("rel").unwrap_or("").to_ascii_lowercase();
                let is_sheet = rel.split_whitespace().any(|item| item == "stylesheet");
                if !is_sheet || href.is_empty() {
                    continue;
                }
                if href.starts_with("http:")
                    || href.starts_with("https:")
                    || href.starts_with("//")
                {
                    continue;
                }
                let path = base_dir.join(href);
                if !path.is_file() {
                    return Err(format!("stylesheet no encontrado: {}", path.display()));
                }
                let text = fs::read_to_string(&path)
                    .map_err(|err| format!("{}: {err}", path.display()))?;
                texts.push(text);
            }
            _ => {}
        }
    }
    Ok(texts)
}

/// Nodes not rendered by the browser's UA stylesheet (same as Python).
pub(crate) fn ua_display(tag: &str) -> &'static str {
    if NON_RENDERED.contains(&tag) {
        "none"
    } else if INLINE.contains(&tag) {
        "inline"
    } else {
        "block"
    }
}

const INLINE: [&str; 22] = [
    "a", "abbr", "b", "bdi", "bdo", "br", "cite", "code", "em", "i", "img", "label", "q", "s",
    "small", "span", "strong", "sub", "sup", "u", "var", "wbr",
];
