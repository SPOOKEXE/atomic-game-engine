#pragma once

// What this module puts into the process-wide tables.
//
// Two calls, in this order, and both idempotent. `scene/Registration.hpp` is
// the same pair for the same reasons: components before classes, because a
// class set names component ids; and idempotent because several programs in one
// process - a studio hosting a play session - each want to be sure the tables
// are up rather than to know who got there first.
//
// @tier L7 · shared

#include <engine/ecs/Instance.hpp>

#include <span>
#include <string_view>

namespace engine::gui {

	// Registers every component this module declares, under stable names.
	//
	// The names cross a save file, a wire and a recording, so
	// `gui/tests/Registration.cpp` pins every one of them.
	void RegisterGuiComponents();

	// Registers the class tree, the enums and the property surface.
	//
	// Calls `RegisterGuiComponents` first, so a caller that only wants classes
	// need not know the order.
	//
	// @return The `GuiObject` class id, which is the one most callers want -
	//         "is this instance part of a UI" is `store.IsA(x, GuiObjectClass())`.
	ecs::ClassId RegisterGuiClasses();

	// The registered id of one of this module's classes, by name.
	//
	// **A lookup rather than a stored id per class**, because the alternative is
	// twenty accessors that all have to be kept in step with the tree. Interned
	// once per distinct name by `ecs::Classes::Lookup`, so the cost is a hash of
	// a short string on a path nobody runs per row.
	//
	// @param name The class name, as a script spells it.
	// @return The id, or an invalid id when the tree has not been registered.
	ecs::ClassId GuiClass(std::string_view name);

	// Every class name this module registers, in registration order.
	//
	// Exported so a test, an insert palette and the bindings manifest read one
	// list rather than three that drift.
	//
	// @return The names, valid for the life of the program.
	std::span<const std::string_view> GuiClassNames();
}
