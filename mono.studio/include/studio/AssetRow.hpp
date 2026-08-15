#pragma once

// One row of a content list: a picture, a name, and a single thing to click.
//
// **This exists because the row it replaces aborted the editor.** The picker
// drew its `Selectable`, rewound the cursor with `SetCursorPos`, submitted the
// picture and the label over the top, and then rewound once more to place the
// next row - and that last rewind is followed by nothing. Dear ImGui checks for
// exactly that at `EndChild`:
//
//     Code uses SetCursorPos()/SetCursorScreenPos() to extend window/parent
//     boundaries. Please submit an item e.g. Dummy() afterwards.
//
// It is an `IM_ASSERT_USER_ERROR`, so the editor did not misdraw - it called
// `abort()` the first frame the picker opened. Choosing a material was
// impossible because the process ended on the way to the list.
//
// ## The rule this file keeps
//
// **A row submits exactly one item, and everything else is painted.** The
// `Selectable` is the item; the picture, its border and the label go straight to
// the draw list, which advances no cursor. There is nothing to rewind, so there
// is nothing for the assert to catch.
//
// **What that does *not* fix is worth saying, because the first version of this
// comment claimed it did.** Items submitted over a `Selectable` were suspected
// of stealing its click; they do not - `tests/AssetRow.cpp` drives a click
// through the old shape and it lands. Overlapping items were a second copy of
// the row's geometry and one more item per row in a list of hundreds, and they
// are gone for those reasons. The crash was the whole of the bug.
//
// ## Why a template rather than a begin/end pair
//
// A `BeginAssetRow`/`EndAssetRow` pair would be a balance invariant nothing
// checks, which is a third one this program would carry. A callback cannot be
// unbalanced, and it keeps the painting - which needs the editor's thumbnail
// cache - outside a header that must not know about it.
//
// @tier client

#include <imgui.h>

#include <cstdint>

namespace studio {

	// What a click on a row meant.
	//
	// @since v0.10
	enum class RowAction : uint8_t {
		// Nothing happened this frame.
		None,

		// Clicked once: this row is now the selection.
		Chose,

		// Double-clicked, which is what a person tries first - `FilePrompt`
		// treats one the same way and for the same reason.
		Confirmed,
	};

	// Draws one row `side` pixels tall and paints `body` over it.
	//
	// **`body` is handed the row's top-left in *screen* space**, because that is
	// what an `ImDrawList` takes, and because a body that reached for the cursor
	// instead is the shape this file exists to stop.
	//
	// **`ImGui::IsItemHovered()` still answers about the row after this
	// returns**, which is what lets `Editor::HoverPreview` stay as it is at all
	// six of its call sites. It holds precisely because the body submits no
	// items - a body that submitted one would move `LastItemData` and the hover
	// preview would follow that instead. There is no way to enforce it from
	// here; `AGENTS.md` calls that a convention, and `tests/AssetRow.cpp` is
	// where it is pinned.
	//
	// @param selected Whether this row is the current choice.
	// @param side     How tall the row is, and how wide its picture is.
	// @param body     `void(ImVec2 corner)` - paints the row's contents.
	// @return What the click did, or `None`.
	template <class Body> RowAction DrawAssetRow(bool selected, float side, Body &&body) {
		const ImVec2 corner = ImGui::GetCursorScreenPos();

		RowAction action = RowAction::None;
		if (ImGui::Selectable(
				"##row", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, side)
			)) {
			// **Both answer, and only the second says `Confirmed`.** A picker
			// that reported the double alone would make a single click select
			// nothing, which reads as a list that ignores you.
			action = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? RowAction::Confirmed
																		: RowAction::Chose;
		}

		body(corner);
		return action;
	}
}
