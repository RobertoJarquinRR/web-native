//! Emission of the `--emit-ui` style tree, the `.wnui` blob, the `ui_data.h`
//! header and shared JSON helpers.

pub(crate) mod blob;
pub(crate) mod emit;
pub(crate) mod header;
pub(crate) mod ui_tree;

pub(crate) use self::emit::round_py;
