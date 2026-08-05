#pragma once

// The spacing scale, and the handful of sizes the layout is built from.
//
// **Ported from the reference this editor's look is based on**, and the comment
// there says the thing worth keeping: everything picks from these rather than
// inventing a number, so gaps line up across panels that know nothing about
// each other — a tree row's padding matches a toolbar's, a panel's inset
// matches its own button row.
//
// **A value between two steps is a mistake, not a choice.** That is the whole
// discipline: an interface built from five numbers looks designed, and one
// built from thirty looks like nobody was in charge. When something needs a
// size that is not here, the question is which of these it should have been.
//
// Everything is in logical pixels before `InterfaceSettings::Scale`. `Scaled`
// applies it, so a caller never multiplies by hand and never forgets to.
//
// @tier L12 · client

namespace engine::ui {

	// The spacing steps. Nothing between them.
	//
	// @since v0.7
	struct Space {
		// Between a glyph and the box around it.
		static constexpr float Tiny = 2.0f;

		// Between two things that belong together.
		static constexpr float Small = 4.0f;

		// The default gap between controls.
		static constexpr float Medium = 6.0f;

		// Between groups.
		static constexpr float Large = 8.0f;

		// A panel's own inset, and the gap between sections.
		static constexpr float Huge = 12.0f;
	};

	// How round a thing is, which is how much it reads as a control.
	//
	// @since v0.7
	struct Radius {
		// Buttons, fields, pills — anything you click.
		static constexpr float Control = 3.0f;

		// Panels and popups, which read as surfaces rather than controls.
		static constexpr float Panel = 4.0f;

		// Fully rounded, for a chip or a badge.
		static constexpr float Chip = 999.0f;
	};

	// The sizes a layout is made of.
	//
	// @since v0.7
	struct Size {
		// One line in any list: a tree row, a property row, a menu row.
		static constexpr float Row = 22.0f;

		// Buttons and text fields.
		static constexpr float Control = 24.0f;

		// A square icon button in a toolbar.
		static constexpr float Button = 26.0f;

		// A strip holding controls: a control plus its padding.
		static constexpr float Bar = Control + Space::Small * 2.0f;

		// The status line, which holds text rather than controls.
		static constexpr float Status = 20.0f;

		// One level of tree indentation.
		static constexpr float Indent = 16.0f;

		// How wide a panel may get before it stops being a panel.
		static constexpr float PanelMinimum = 180.0f;

		// How wide the field in a one-question modal is.
		//
		// **A number rather than `-1`, and that is the whole point of it
		// being here.** A negative width means "the content region minus
		// this", and the content region of an auto-resizing window is
		// derived from what the window measured *last* frame — so the field
		// shrinks the window, the smaller window shrinks the field, and the
		// modal deflates a few pixels per frame until it bottoms out on the
		// widest thing left in it. See `studio::PathPrompt`.
		static constexpr float Prompt = 520.0f;
	};

	// The scale every metric is multiplied by, set once by `ApplyEditorTheme`.
	//
	// **Held here rather than passed to every call**, because the alternative is
	// a `scale` parameter on every widget in the editor and one place that
	// forgets it.
	//
	// @return The current scale, 1 until a theme is applied.
	float Scale();

	// A metric at the current scale.
	//
	// @param value The unscaled metric, from one of the tables above.
	// @return The value in real pixels.
	float Scaled(float value);
}
