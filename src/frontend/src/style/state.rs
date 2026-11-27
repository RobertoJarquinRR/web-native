//! Effective CSS state styles (blob v6).
//!
//! A state style only carries the fields a state rule actually overrides:
//! `background`, text `color`, the uniform border color and `font-weight`.
//! Layout-affecting properties other than the weight are deliberately excluded
//! from this lane.
//!
//! The six states are stored independently. Press is resolved with hover and
//! active both true, so its `background`/`text`/`border` already include the
//! hover declarations (press > hover at paint time; the combination is baked
//! here). Focus-visible folds `:focus` declarations in the same cascade, and
//! focus-within only includes its own state. Focus and disabled only include
//! their own state.

/// One effective state. `None` means "keep the base value".
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StateStyle {
    /// Normalized `#rrggbb` background override.
    pub background: Option<String>,
    /// Normalized `#rrggbb` text color override.
    pub text: Option<String>,
    /// Normalized `#rrggbb` uniform border color override.
    pub border: Option<String>,
    /// Blob v6 additive field: state `font-weight` (100..=900). It travels in
    /// the record's reserved u16, so the 16-byte entry stays unchanged.
    pub font_weight: Option<i32>,
}

impl StateStyle {
    pub fn is_empty(&self) -> bool {
        self.background.is_none()
            && self.text.is_none()
            && self.border.is_none()
            && self.font_weight.is_none()
    }

    /// Text runs follow their owning element: only the text color and the
    /// weight have to travel to the generated run.
    pub fn text_and_weight(&self) -> Option<Self> {
        if self.text.is_none() && self.font_weight.is_none() {
            return None;
        }
        Some(StateStyle {
            text: self.text.clone(),
            font_weight: self.font_weight,
            ..StateStyle::default()
        })
    }
}

/// The six effective states of a node. A node only carries a state when at
/// least one field is overridden.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StateStyles {
    pub hover: Option<StateStyle>,
    /// `:active` + `:hover` (press).
    pub press: Option<StateStyle>,
    pub focus: Option<StateStyle>,
    /// Stored only for nodes that can be disabled (`button`/`input`); the
    /// runtime `WN_STATE_DISABLED` bit decides when it applies.
    pub disabled: Option<StateStyle>,
    /// `:focus-visible` (+ `:focus` declarations through the cascade).
    pub focus_visible: Option<StateStyle>,
    /// `:focus-within`, applied to the focused node and its ancestors.
    pub focus_within: Option<StateStyle>,
}

impl StateStyles {
    pub fn is_empty(&self) -> bool {
        self.hover.is_none()
            && self.press.is_none()
            && self.focus.is_none()
            && self.disabled.is_none()
            && self.focus_visible.is_none()
            && self.focus_within.is_none()
    }

    /// Stable order used by the dump and the blob writer. The codes match the
    /// runtime `R2_STATE_STYLE_*` constants.
    pub fn entries(&self) -> [(&'static str, u8, &Option<StateStyle>); 6] {
        [
            ("hover", 0, &self.hover),
            ("press", 1, &self.press),
            ("focus", 2, &self.focus),
            ("disabled", 3, &self.disabled),
            ("focus-visible", 4, &self.focus_visible),
            ("focus-within", 5, &self.focus_within),
        ]
    }

    /// Copies only the text color and weight of each state (used for generated
    /// text runs, which follow the state of their owning element).
    pub fn text_only(&self) -> StateStyles {
        StateStyles {
            hover: self.hover.as_ref().and_then(StateStyle::text_and_weight),
            press: self.press.as_ref().and_then(StateStyle::text_and_weight),
            focus: self.focus.as_ref().and_then(StateStyle::text_and_weight),
            disabled: self
                .disabled
                .as_ref()
                .and_then(StateStyle::text_and_weight),
            focus_visible: self
                .focus_visible
                .as_ref()
                .and_then(StateStyle::text_and_weight),
            focus_within: self
                .focus_within
                .as_ref()
                .and_then(StateStyle::text_and_weight),
        }
    }
}
