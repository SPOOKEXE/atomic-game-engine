#pragma once
#include "NavigationSelector.hpp"

#include <engine/core/types/Vector2.hpp>
#include <engine/gui/Components.hpp>

#include <cstddef>
#include <span>

namespace engine::gui::detail {
	// Commands are already restricted to the selected collector and modal scope.
	template <typename Command, typename Facts> struct NavigationAccess {
		std::span<const Command *const> Commands;
		const Facts &Source;
		size_t CommandCount() const {
			return Commands.size();
		}
		ecs::Entity CommandSource(size_t index) const {
			return Commands[index]->Source;
		}
		bool Selectable(ecs::Entity entity) const {
			const auto *element = Source.ElementOf(entity);
			return element && element->Visible && element->Selectable && element->Interactable;
		}
		const Selection *SelectionOf(ecs::Entity entity) const {
			return Source.SelectionOf(entity);
		}
		bool Centre(ecs::Entity entity, core::Vector2 &out) const {
			for (const auto *command : Commands) {
				if (command->Source == entity && command->Bounds.Min.X < command->Clip.Max.X &&
					command->Bounds.Max.X > command->Clip.Min.X &&
					command->Bounds.Min.Y < command->Clip.Max.Y &&
					command->Bounds.Max.Y > command->Clip.Min.Y) {
					out = {
						(command->Bounds.Min.X + command->Bounds.Max.X) * 0.5f,
						(command->Bounds.Min.Y + command->Bounds.Max.Y) * 0.5f
					};
					return true;
				}
			}
			return false;
		}
	};
}
