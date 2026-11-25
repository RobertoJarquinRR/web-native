//! Emision del arbol de estilos `--emit-ui`, del blob `.wnui` y helpers JSON
//! compartidos.

pub(crate) mod blob;
pub(crate) mod emit;
pub(crate) mod ui_tree;

pub(crate) use self::emit::round_py;
