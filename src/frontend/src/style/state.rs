//! Effective CSS state styles (blob v6).
//!
//! A state style only carries the fields a state rule actually overrides:
//! `background`, text `color` and the uniform border color. Layout-affecting
//! properties are deliberately excluded from this lane; only colors travel to
//! the runtime.
//!
//! The four states are stored independently. Press is resolved with hover and
//! active both true, so its `background`/`text`/`border` already include the
//! hover declarations (press > hover at paint time; the combination is baked
//! here). Focus and disabled only include their own state.

/// One effective state. `None` means "keep the base value".
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StateStyle {
    /// Normalized `#rrggbb` background override.
    pub background: Option<String>,
    /// Normalized `#rrggbb` text color override.
    pub text: Option<String>,
    /// Normalized `#rrggbb` uniform border color override.
    pub border: Option<String>,
}

impl StateStyle {
    pub fn is_empty(&self) -> bool {
        self.background.is_none() && self.text.is_none() && self.border.is_none()
    }

    pub fn text_only(text: Option<String>) -> Option<Self> {
        text.map(|text| StateStyle {
            text: Some(text),
            ..StateStyle::default()
        })
    }
}

/// The four effective states of a node. A node only carries a state when at
/// least one of its color fields is overridden.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StateStyles {
    pub hover: Option<StateStyle>,
    /// `:active` + `:hover` (press).
    pub press: Option<StateStyle>,
    pub focus: Option<StateStyle>,
    /// Stored only for nodes that can be disabled (`button`/`input`); the
    /// runtime `WN_STATE_DISABLED` bit decides when it applies.
    pub disabled: Option<StateStyle>,
}

impl StateStyles {
    pub fn is_empty(&self) -> bool {
        self.hover.is_none()
            && self.press.is_none()
            && self.focus.is_none()
            && self.disabled.is_none()
    }

    /// Stable order used by the dump and the blob writer. The codes match the
    /// runtime `R2_STATE_STYLE_*` constants.
    pub fn entries(&self) -> [(&'static str, u8, &Option<StateStyle>); 4] {
        [
            ("hover", 0, &self.hover),
            ("press", 1, &self.press),
            ("focus", 2, &self.focus),
            ("disabled", 3, &self.disabled),
        ]
    }

    /// Copies only the text color of each state (used for generated text runs,
    /// which follow the state of their owning element).
    pub fn text_only(&self) -> StateStyles {
        StateStyles {
            hover: StateStyle::text_only(self.hover.as_ref().and_then(|s| s.text.clone())),
            press: StateStyle::text_only(self.press.as_ref().and_then(|s| s.text.clone())),
            focus: StateStyle::text_only(self.focus.as_ref().and_then(|s| s.text.clone())),
            disabled: StateStyle::text_only(
                self.disabled.as_ref().and_then(|s| s.text.clone()),
            ),
        }
    }
}
