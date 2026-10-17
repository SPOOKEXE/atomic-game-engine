#pragma once

// arch-waiver public-header: forward studio API. Property tools exchange this
// complete selection contract.

// The data model behind Studio's multi-selection property grid.
//
// Kept free of Dear ImGui so the important decisions can be tested without a
// window: the grid is the intersection of every selected live class, inherited
// properties stay under the class that declared them, and disagreement is
// explicit rather than whichever entity happened to be first.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>

#include <span>
#include <vector>

namespace studio {
	// One property shared by every selected live instance.
	struct SelectionPropertyRow {
		// Descriptor and declaring owner, representative value, coverage counts, and disagreement state.
		//@{
		const engine::ecs::PropertyDescriptor *Descriptor = nullptr;
		engine::ecs::ClassId Owner;
		engine::game::PropertyValue Value;
		size_t Applicable = 0;
		size_t Readable = 0;
		bool Mixed = false;
		//@}
	};

	// Shared properties projected by declaring class or by inspector tag.
	struct SelectionPropertyGroup {
		// Declaring class for owner groups, inspector tag for tagged groups, coverage and rows.
		//@{
		engine::ecs::ClassId Owner;
		engine::core::Name PropertiesTag;
		size_t Applicable = 0;
		std::vector<SelectionPropertyRow> Rows;
		//@}
	};

	// Which ancestor first declares a property for this class.
	engine::ecs::ClassId DeclaringPropertyClass(engine::ecs::ClassId klass, engine::core::Name property);

	// Whether a class carries the exact row represented by a shared entry.
	bool SelectionPropertyApplies(
		engine::ecs::ClassId klass,
		engine::ecs::ClassId owner,
		engine::core::Name property,
		engine::ecs::PropertyType type
	);

	// Builds the root-first intersection of properties exposed by every selected live instance.
	std::vector<SelectionPropertyGroup>
	BuildPropertySelection(const engine::ecs::Store &store, std::span<const engine::ecs::Entity> instances);
	// Groups the shared property surface by explicit inspector metadata. Row owners stay unchanged.
	std::vector<SelectionPropertyGroup> BuildTaggedPropertySelection(
		const engine::ecs::Store &store, std::span<const engine::ecs::Entity> instances
	);
}
