#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/InputContinuity.hpp>
#include <engine/gui/Services.hpp>

#include <new>

namespace engine::gui {
	std::optional<InputContinuity> CaptureInputContinuity(const ecs::Store &store, size_t byteBudget) {
		if (byteBudget < sizeof(InputContinuity) + InputContinuity::ALLOCATION_ALLOWANCE) return {};
		byteBudget -= sizeof(InputContinuity) + InputContinuity::ALLOCATION_ALLOWANCE;
		InputContinuity witness;
		witness.StoreIdentity = store.Identity();
		witness.Service = GuiServiceOf(store);
		const auto *state = store.Get<GuiServiceState>(witness.Service);
		if (state != nullptr) {
			witness.HasServiceState = true;
			witness.RawFocus = state->FocusedTextBox;
			witness.Selection = state->SelectedObject;
			witness.AutoSelect = state->AutoSelectGuiEnabled;
			witness.MenuOpen = state->MenuIsOpen;
		}
		witness.Focus = FocusedTextBox(store);
		const auto *entry = store.Get<Entry>(witness.Focus);
		const auto *label = store.Get<Label>(witness.Focus);
		const auto *composition = store.Get<TextCompositionState>(witness.Service);
		const size_t textBytes = label != nullptr ? label->Text.size() : 0;
		const size_t compositionBytes = composition != nullptr ? composition->Text.size() : 0;
		if (textBytes > byteBudget || compositionBytes > byteBudget - textBytes) return {};
		try {
			if (entry != nullptr) {
				witness.HasEntry = true;
				witness.Cursor = entry->CursorPosition;
				witness.SelectionStart = entry->SelectionStart;
				witness.Editable = entry->TextEditable;
				witness.Multiline = entry->MultiLine;
			}
			if (label != nullptr) {
				witness.HasLabel = true;
				witness.Text = label->Text;
			}
			if (composition != nullptr) {
				witness.HasComposition = true;
				witness.CompositionBox = composition->TextBox;
				witness.CompositionStart = composition->Start;
				witness.CompositionLength = composition->Length;
				witness.CompositionRevision = composition->Revision;
				witness.CompositionText = composition->Text;
			}
			if (witness.Text.capacity() >= byteBudget ||
				witness.CompositionText.capacity() >= byteBudget - witness.Text.capacity() - 1)
				return {};
		} catch (const std::bad_alloc &) {
			return {};
		}
		return witness;
	}

	bool MatchesInputContinuity(const ecs::Store &store, const InputContinuity &witness) {
		if (store.Identity() != witness.StoreIdentity || GuiServiceOf(store) != witness.Service ||
			FocusedTextBox(store) != witness.Focus)
			return false;
		const auto *state = store.Get<GuiServiceState>(witness.Service);
		if ((state != nullptr) != witness.HasServiceState) return false;
		if (state != nullptr &&
			(state->FocusedTextBox != witness.RawFocus || state->SelectedObject != witness.Selection ||
			 state->AutoSelectGuiEnabled != witness.AutoSelect || state->MenuIsOpen != witness.MenuOpen))
			return false;
		const auto *entry = store.Get<Entry>(witness.Focus);
		const auto *label = store.Get<Label>(witness.Focus);
		if ((entry != nullptr) != witness.HasEntry || (label != nullptr) != witness.HasLabel) return false;
		if (entry != nullptr &&
			(entry->CursorPosition != witness.Cursor || entry->SelectionStart != witness.SelectionStart ||
			 entry->TextEditable != witness.Editable || entry->MultiLine != witness.Multiline))
			return false;
		if (label != nullptr && label->Text != witness.Text) return false;
		const auto *composition = store.Get<TextCompositionState>(witness.Service);
		if ((composition != nullptr) != witness.HasComposition) return false;
		if (composition != nullptr && (composition->TextBox != witness.CompositionBox ||
									   composition->Start != witness.CompositionStart ||
									   composition->Length != witness.CompositionLength ||
									   composition->Revision != witness.CompositionRevision ||
									   composition->Text != witness.CompositionText))
			return false;
		return true;
	}
}
