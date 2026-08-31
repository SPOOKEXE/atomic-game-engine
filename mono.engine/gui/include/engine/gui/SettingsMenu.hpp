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

	struct SettingsMenuAction {
		core::Name Id;
		std::string Label;

		bool operator==(const SettingsMenuAction &) const = default;
	};

	class SettingsMenuExtensions {
	  public:
		static constexpr size_t MAXIMUM_ACTIONS = 12;
		static constexpr size_t MAXIMUM_LABEL_BYTES = 64;

		bool Set(core::Name id, std::string_view label);
		bool Remove(core::Name id);
		void Clear();

		std::span<const SettingsMenuAction> Actions() const {
			return Entries;
		}

	  private:
		std::vector<SettingsMenuAction> Entries;
	};

	SettingsMenuExtensions &ReachSettingsMenuExtensions(ecs::Store &store);
	std::span<const SettingsMenuAction> SettingsMenuActionsOf(const ecs::Store &store);
}
