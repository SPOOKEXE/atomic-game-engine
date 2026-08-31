#pragma once

// The data model behind Studio's multi-selection property grid.
//
// Kept free of Dear ImGui so the important decisions can be tested without a
// window: the grid is the union of every selected class, inherited properties
// stay under the class that declared them, and disagreement is explicit rather
// than whichever entity happened to be first.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>

#include <span>
#include <vector>

namespace studio {
	// One property projected across every selected instance where it applies.
	struct SelectionPropertyRow {
		// Descriptor, representative value, coverage counts, and disagreement state.
		//@{
		const engine::ecs::PropertyDescriptor *Descriptor = nullptr;
		engine::game::PropertyValue Value;
		size_t Applicable = 0;
		size_t Readable = 0;
		bool Mixed = false;
		//@}
	};

	// Properties grouped beneath the class that first declared them.
	struct SelectionPropertyGroup {
		// Declaring class, applicable selection count, and projected rows.
		//@{
		engine::ecs::ClassId Owner;
		size_t Applicable = 0;
		std::vector<SelectionPropertyRow> Rows;
		//@}
	};

	// Which ancestor first declares a property for this class.
	engine::ecs::ClassId DeclaringPropertyClass(engine::ecs::ClassId klass, engine::core::Name property);

	// Whether a class carries the exact row represented by a union entry.
	bool SelectionPropertyApplies(
		engine::ecs::ClassId klass,
		engine::ecs::ClassId owner,
		engine::core::Name property,
		engine::ecs::PropertyType type
	);

	// Builds the root-first union of properties exposed by the live instances.
	std::vector<SelectionPropertyGroup>
	BuildPropertySelection(const engine::ecs::Store &store, std::span<const engine::ecs::Entity> instances);
}
