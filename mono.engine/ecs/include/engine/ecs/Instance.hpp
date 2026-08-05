#pragma once

// What an instance is, underneath.
//
// Userland gets Roblox-style instancing — `Instance.new`, `Parent`,
// `GetChildren()`, `:IsA()`, `:Destroy()`. None of that is an object here. An
// instance is an entity, a class is a set of components, a property is a field
// in a column, and every system iterates rows. This header holds the three
// components that make the façade possible.
//
// **The tree is not a transform hierarchy, and that is the whole saving.** In
// Roblox a part's `CFrame` is world-space and parenting is organisational — a
// model is a folder, not a coordinate frame. Keeping that means no propagation
// pass, no dirty-transform cascade, physics reading `Transform` with no resolve
// step, and a tree that costs four entity handles per node instead of a scene
// graph.
//
// **Sibling order is insertion order**, because `GetChildren()` returning a
// different order on two runs would make replication and replay disagree about
// a thing neither of them changed.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Enums.hpp>

#include <cstdint>

namespace engine::ecs {

	// A dense process-local handle for one registered class.
	//
	// @since v0.2
	struct ClassId {
		// The value no registration produces.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The registration index, or INVALID.
		uint32_t Index = INVALID;

		// Creates an invalid id.
		constexpr ClassId() = default;

		// Creates an id from a registration index.
		//
		// @param index The registration index to wrap.
		constexpr explicit ClassId(uint32_t index) : Index(index) {}

		// Reports whether this id names a registered class.
		//
		// @return `true` when the id came from a registration.
		constexpr bool IsValid() const {
			return Index != INVALID;
		}

		// Compares registration indices for equality.
		//
		// @param other The id to compare.
		// @return `true` when both name the same class.
		constexpr bool operator==(const ClassId &other) const {
			return Index == other.Index;
		}

		// Compares registration indices for inequality.
		//
		// @param other The id to compare.
		// @return `true` when the ids name different classes.
		constexpr bool operator!=(const ClassId &other) const {
			return Index != other.Index;
		}
	};

	// Which class an entity was created as.
	//
	// A component rather than a lookup table, so `ClassName` and `:IsA` are a
	// column read — and so a query can ask for every instance of a class
	// without the store keeping a second index of who is what.
	//
	// @since v0.2
	struct InstanceClass {
		// The class, or invalid for an entity that is not an instance.
		ClassId Class;
	};

	// One node's place in the tree.
	//
	// Four handles and no allocation. A child list would be a vector per node —
	// an allocation for every instance in the world, most of which have no
	// children at all.
	//
	// Every field is an `Entity`, so a snapshot carries the tree for free: the
	// directory is restored exactly, and handles inside a component stay valid.
	//
	// @since v0.2
	struct Hierarchy {
		// The parent, or NULL_ENTITY for a root.
		Entity Parent;

		// The first child, or NULL_ENTITY when there are none.
		Entity FirstChild;

		// The last child, so appending is O(1) rather than a walk.
		//
		// Insertion order is what `GetChildren()` returns, and keeping it
		// without this would mean walking the whole sibling list per add —
		// which turns building a model out of a thousand parts into a million
		// steps.
		Entity LastChild;

		// The next sibling in insertion order, or NULL_ENTITY at the end.
		Entity NextSibling;

		// The previous sibling, or NULL_ENTITY at the start. Present so that
		// unparenting is O(1) instead of a walk from the front.
		Entity PreviousSibling;
	};

	// An instance's name.
	//
	// A `core::Name` rather than a string, because names repeat heavily — a
	// thousand parts called "Part" intern to one — and because comparing names
	// is then an integer compare.
	//
	// This is not `Store::Create(name)`, which is the store's own lookup of
	// singular things. An instance name is not unique: siblings may share one,
	// exactly as they may in Roblox.
	//
	// @since v0.2
	struct InstanceName {
		// The interned name, or an invalid Name for an unnamed instance.
		core::Name Value;
	};

	// Present on an instance that `Clone` must not copy.
	//
	// **A tag, and the sense is inverted, and both follow from what this
	// costs.** Roblox spells it `Archivable` and defaults it to true, so a
	// component holding a bool would be one byte per instance in every
	// archetype in the engine, set the same way on all but a handful — which is
	// the exact shape `ecs/AGENTS.md` names as belonging somewhere else.
	//
	// A tag is presence rather than bytes: it costs nothing on the instances
	// that do not have it, and the ones that do are rare enough that the
	// archetype move each one pays is not a cost anybody meets in a loop.
	//
	// So `Archivable` is `!Has<NotArchivable>`, which is the same fact read the
	// cheap way round.
	//
	// @since v0.75
	struct NotArchivable {};
}
