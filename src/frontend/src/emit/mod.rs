//! Emision: JSON del IR y arbol de estilos --emit-ui.
//!
//! emit serializa el display list de R1 y ui_tree el arbol de estilos de
//! R2. Los nombres que el binario ya usaba como crate::emit::* se reexportan
//! aqui porque el modulo interno se llama igual que la carpeta.

pub(crate) mod emit;
pub(crate) mod ui_tree;

pub(crate) use self::emit::{atlas_json, ir_json, round_py, timings_inline, timings_json, IrOp, Timings};