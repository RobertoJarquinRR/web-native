//! Emission of the pre-layout tree and the `.wnui` blob.

pub(crate) mod blob;
pub(crate) mod emit;
pub(crate) mod ui_tree;

pub(crate) use self::emit::round_py;
