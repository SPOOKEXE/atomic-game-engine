#pragma once

// Script-authored actions shown in a client's ESC settings menu.
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::gui {

	// One script-authored action exposed in a client's ESC settings menu.
	struct SettingsMenuAction {
		// Stable action identifier used when the row is activated.
		core::Name Id;

		// User-facing row label.
		std::string Label;

		// Compares the identifier and label for change detection.
		bool operator==(const SettingsMenuAction &) const = default;
	};

	// The bounded, ECS-owned list of script-authored settings-menu actions.
	class SettingsMenuExtensions {
	  public:
		// Maximum actions retained by one world.
		static constexpr size_t MAXIMUM_ACTIONS = 12;

		// Maximum UTF-8 bytes retained for one action label.
		static constexpr size_t MAXIMUM_LABEL_BYTES = 64;

		// Adds or relabels an action, refusing invalid or over-capacity input.
		bool Set(core::Name id, std::string_view label);

		// Removes one action by identifier.
		bool Remove(core::Name id);

		// Removes every script-authored action.
		void Clear();

		// Returns the actions in their menu order.
		std::span<const SettingsMenuAction> Actions() const {
			return Entries;
		}

	  private:
		std::vector<SettingsMenuAction> Entries;
	};

	// Returns the world's action list, installing an empty one when absent.
	SettingsMenuExtensions &ReachSettingsMenuExtensions(ecs::Store &store);

	// Returns the world's actions, or an empty span when the resource is absent.
	std::span<const SettingsMenuAction> SettingsMenuActionsOf(const ecs::Store &store);
}
