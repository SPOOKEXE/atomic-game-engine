#pragma once

#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {
	// Owned focus and text-edit state used to validate delayed input.
	struct InputContinuity {
		// Conservative allowance for allocator overhead.
		static constexpr size_t ALLOCATION_ALLOWANCE = 128;
		// Incarnation of the Store that owns these entities.
		uint64_t StoreIdentity = 0;
		// GUI service entity, if present.
		ecs::Entity Service;
		// Focus stored on the service before eligibility checks.
		ecs::Entity RawFocus;
		// Eligible focused text box, if any.
		ecs::Entity Focus;
		// Current semantic selection.
		ecs::Entity Selection;
		// Whether the service has its state component.
		bool HasServiceState = false;
		// Whether automatic selection is enabled.
		bool AutoSelect = false;
		// Whether the GUI menu is open.
		bool MenuOpen = false;
		// Whether the focused entity has an entry component.
		bool HasEntry = false;
		// Whether the focused entity has a label component.
		bool HasLabel = false;
		// Focused entry caret position.
		int32_t Cursor = -1;
		// Focused entry selection anchor.
		int32_t SelectionStart = -1;
		// Whether the focused entry accepts edits.
		bool Editable = false;
		// Whether the focused entry accepts multiple lines.
		bool Multiline = false;
		// Owned text from the focused label.
		std::string Text;
		// Whether an input-method composition exists.
		bool HasComposition = false;
		// Text box that owns the composition.
		ecs::Entity CompositionBox;
		// Selected offset within the preedit text.
		int32_t CompositionStart = -1;
		// Selected length within the preedit text.
		int32_t CompositionLength = -1;
		// Revision of the platform's composition update.
		uint64_t CompositionRevision = 0;
		// Owned input-method preedit text.
		std::string CompositionText;

		// Estimated retained size, including string capacity and allocation allowance.
		// @return Estimated bytes owned by this witness.
		size_t RetainedBytes() const {
			return sizeof(InputContinuity) + Text.capacity() + CompositionText.capacity() + 2 +
				   ALLOCATION_ALLOWANCE;
		}
	};

	// Copies typing state within the budget. The Store must not be reentered during capture.
	// @param store Store containing the focused entry.
	// @param byteBudget Maximum retained bytes for the witness.
	// @return The owned witness, or no value if it cannot be captured within the budget.
	std::optional<InputContinuity> CaptureInputContinuity(const ecs::Store &store, size_t byteBudget);
	// Compares without allocating or writing the Store.
	// @param store Store to compare with the witness.
	// @param witness Previously captured input state.
	// @return True when the input state still matches.
	bool MatchesInputContinuity(const ecs::Store &store, const InputContinuity &witness);
}
