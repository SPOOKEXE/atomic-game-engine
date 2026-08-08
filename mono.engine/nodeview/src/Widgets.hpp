#pragma once

// The two widgets both canvases are made of.
//
// Internal to `nodeview`: the Render Pipeline and Assets Pipeline canvases are
// the same boxes with different contents, and a second copy of a `Frame` with a
// `Background` on it is the kind of duplication that drifts into two editors
// looking subtly unlike each other.

#include <engine/core/types/Color3.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>

#include <string>
#include <string_view>

namespace engine::nodeview {

	inline ecs::Entity MakeFrame(
		ecs::Store &store,
		ecs::Entity parent,
		std::string_view name,
		const core::UDim2 &position,
		const core::UDim2 &size,
		const core::Color3 &colour
	) {
		const ecs::Entity made = store.CreateInstance(gui::GuiClass("Frame"), name);
		store.SetParent(made, parent);

		gui::Element element;
		element.Position = position;
		element.Size = size;
		store.Set(made, element);

		gui::Background background;
		background.Color = colour;
		store.Set(made, background);

		return made;
	}

	inline ecs::Entity MakeLabel(
		ecs::Store &store,
		ecs::Entity parent,
		std::string_view name,
		std::string text,
		const core::Color3 &colour
	) {
		const ecs::Entity made = store.CreateInstance(gui::GuiClass("TextLabel"), name);
		store.SetParent(made, parent);

		// Fills its box. A label that sized itself would need a measured
		// font, which is the glyph atlas's business and is a device away.
		gui::Element element;
		element.Position = core::UDim2{0.0f, 0.0f, 0.0f, 0.0f};
		element.Size = core::UDim2{1.0f, 0.0f, 1.0f, 0.0f};
		store.Set(made, element);

		gui::Label label;
		label.Text = std::move(text);
		label.Color = colour;
		store.Set(made, label);

		return made;
	}
}
