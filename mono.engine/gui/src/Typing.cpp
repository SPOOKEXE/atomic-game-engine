#include "Utf8.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>

#include <algorithm>
#include <optional>

namespace engine::gui {

	namespace {
		constexpr size_t MAXIMUM_PREEDIT_BYTES = 4096;

		void ApplyComposition(ecs::Store &store, ecs::Entity instance, const Typing &typing) {
			const ecs::Entity service = GuiServiceOf(store);
			TextCompositionState *state =
				service != ecs::NULL_ENTITY ? store.GetMutable<TextCompositionState>(service) : nullptr;
			if (state == nullptr || state->Revision == typing.PreeditRevision) {
				return;
			}

			state->Revision = typing.PreeditRevision;
			state->TextBox = ecs::NULL_ENTITY;
			state->Text.clear();
			state->Start = -1;
			state->Length = -1;
			if (typing.Preedit.empty() || typing.Preedit.size() > MAXIMUM_PREEDIT_BYTES ||
				store.Get<Entry>(instance) == nullptr || !store.Get<Entry>(instance)->TextEditable) {
				return;
			}

			state->TextBox = instance;
			state->Text = typing.Preedit;
			state->Start = typing.PreeditStart;
			state->Length = typing.PreeditLength;
		}
	}

	std::string TextWithComposition(const ecs::Store &store, ecs::Entity instance, std::string_view text) {
		const ecs::Entity service = GuiServiceOf(store);
		const TextCompositionState *state =
			service != ecs::NULL_ENTITY ? store.Get<TextCompositionState>(service) : nullptr;
		const Entry *entry = store.Get<Entry>(instance);
		if (state == nullptr || entry == nullptr || state->TextBox != instance || state->Text.empty()) {
			return std::string(text);
		}

		const int32_t characters = static_cast<int32_t>(Characters(text));
		const int32_t cursor = std::clamp(entry->CursorPosition, 1, characters + 1);
		const int32_t anchor =
			entry->SelectionStart < 1 ? cursor : std::clamp(entry->SelectionStart, 1, characters + 1);
		const int32_t from = std::min(anchor, cursor);
		const int32_t to = std::max(anchor, cursor);
		std::string resolved(text);
		const size_t start = ByteOffset(resolved, from);
		const size_t end = ByteOffset(resolved, to);
		resolved.replace(start, end - start, state->Text);
		return resolved;
	}

	TypeResult Type(ecs::Store &store, const Typing &typing) {
		ENGINE_PROFILE_CAT("gui type", engine::core::ProfileCategory::ECS);

		TypeResult result;
		result.Instance = FocusedTextBox(store);
		if (result.Instance == ecs::NULL_ENTITY) {
			return result;
		}

		ApplyComposition(store, result.Instance, typing);

		Entry *entry = store.GetMutable<Entry>(result.Instance);
		Label *label = store.GetMutable<Label>(result.Instance);
		if (entry == nullptr || label == nullptr) {
			return result;
		}

		// Edits can join or split graphemes across the edited boundary, so the
		// character count must be updated from the resulting text.
		auto characters = static_cast<int32_t>(Characters(label->Text));

		// **The caret is clamped before anything reads it.** A script sets
		// `TextBox.Text` through a plain field with no setter to hook, so a box
		// whose text has been replaced with something shorter arrives here with a
		// caret past the end. `Typing.hpp` carries the argument for clamping at
		// the reader rather than at the write.
		int32_t cursor = std::clamp(entry->CursorPosition, 1, characters + 1);
		if (cursor != entry->CursorPosition) {
			// The clamp is correct and silent, and a caret that is repeatedly
			// far out of range is a script writing `Text` and `CursorPosition`
			// out of step - which reads as typing landing in the wrong place.
			ENGINE_DEBUG_EVERY(
				5.0, "caret {} clamped to {} over {} grapheme(s)", entry->CursorPosition, cursor, characters
			);
		}
		int32_t anchor = entry->SelectionStart < 1 ? -1 : std::min(entry->SelectionStart, characters + 1);
		if (anchor == cursor) {
			anchor = -1;
		}

		// Removes whatever is selected and leaves the caret in the gap.
		//
		// **Both directions, because a selection is an anchor and a moving end
		// rather than a start and a length**: dragging left puts `SelectionStart`
		// after `CursorPosition`, which is Roblox's arrangement and the only one
		// that survives shift-left followed by shift-right.
		const auto updateCaret = [&](size_t offset) {
			characters = static_cast<int32_t>(Characters(label->Text));
			cursor = std::min(
				static_cast<int32_t>(Characters(std::string_view(label->Text).substr(0, offset))) + 1,
				characters + 1
			);
		};
		const auto dropSelection = [&]() -> std::optional<size_t> {
			if (anchor == -1) return {};
			const int32_t from = std::min(anchor, cursor);
			const int32_t to = std::max(anchor, cursor);
			const size_t start = ByteOffset(label->Text, from);
			const size_t end = ByteOffset(label->Text, to);
			label->Text.erase(start, end - start);
			updateCaret(start);
			anchor = -1;
			return start;
		};

		// **The characters first**, so a frame that typed a letter and then
		// pressed Backspace ends where it started. `Type`'s comment states the
		// whole order and why it is the one a person meant.
		if (!typing.Text.empty() && entry->TextEditable) {
			const auto selection = dropSelection();
			const size_t insertion = selection ? *selection : ByteOffset(label->Text, cursor);
			label->Text.insert(insertion, typing.Text);
			updateCaret(insertion + typing.Text.size());
			result.Changed = true;
		}

		if (typing.Backspace && entry->TextEditable) {
			if (dropSelection()) {
				result.Changed = true;
			} else if (cursor > 1) {
				// **One grapheme and never one byte.** Erasing a byte off the end
				// of an accented letter leaves its lead byte behind, which is not
				// text in any encoding and reads as the box corrupting itself.
				const size_t start = ByteOffset(label->Text, cursor - 1);
				const size_t end = ByteOffset(label->Text, cursor);
				label->Text.erase(start, end - start);

				updateCaret(start);
				result.Changed = true;
			}
		}

		// **The caret moves whatever `TextEditable` says.** A locked box is still
		// one a person can move through and select in, which is what the property
		// means where it comes from.
		if (typing.Caret != 0) {
			if (anchor != -1 && !typing.Extend) {
				// **An unshifted move onto a selection collapses onto the end it
				// went towards rather than stepping from the caret.** Every text
				// field does this, and the alternative loses the far edge of a
				// selection the moment somebody nudges it.
				cursor = typing.Caret < 0 ? std::min(anchor, cursor) : std::max(anchor, cursor);
				anchor = -1;
			} else {
				if (typing.Extend && anchor == -1) {
					anchor = cursor;
				}

				cursor = std::clamp(cursor + typing.Caret, 1, characters + 1);
				if (anchor == cursor) {
					anchor = -1;
				}
			}
		}

		if (typing.Submit) {
			// **A `MultiLine` box takes a line break and a single-line one is
			// done.** Roblox's rule, and the only one that leaves both usable -
			// see `Type`.
			if (!entry->MultiLine) {
				result.Released = true;
			} else if (entry->TextEditable) {
				const auto selection = dropSelection();
				const size_t insertion = selection ? *selection : ByteOffset(label->Text, cursor);
				label->Text.insert(insertion, 1, '\n');
				updateCaret(insertion + 1);
				result.Changed = true;
			}
		}

		entry->CursorPosition = cursor;
		entry->SelectionStart = anchor;

		// **Last, because releasing the focus is what puts the caret back to -1**
		// and a write after it would undo what "unfocused" means in that field.
		if (result.Released) {
			Focus(store, ecs::NULL_ENTITY);
		}

		return result;
	}
}
