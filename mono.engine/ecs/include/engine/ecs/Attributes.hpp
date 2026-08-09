#pragma once

// Arbitrary named values on an instance, which is the one thing a component
// column cannot be.
//
// **A resource, not a component, and that is the whole decision.** Every other
// per-entity fact in this engine is a column: fixed layout, dense, iterated. An
// attribute is a name an author invents at run time mapped to a value whose type
// they also pick, and there is no column shape for that — a component holding a
// map would be a heap allocation per row on `Instance`, which is every row in
// the world, and `AGENTS.md` rule 3 forbids it outright for anything that has to
// survive being memcpy'd across a process boundary.
//
// So the table is one-of-a-kind world state, which is `ecs/AGENTS.md`'s
// definition of a resource, and the cost lands where it belongs: **an entity with
// no attributes costs nothing at all**, not even a byte on its row. That is the
// case that matters, because it is almost every entity.
//
// **The key is `(Entity, Name)` and the lookup is a hash.** Not a column read —
// so this is deliberately *not* something to read per frame per entity. It is a
// script surface: `part:GetAttribute("Health")` on the frame something is hit,
// not `for every part, read its health` in a system. Anything iterated wants a
// component, and the day a game's attribute becomes hot is the day it should
// become one.
//
// **The value types are `ecs::PropertyType`'s and not a new list.** An attribute
// and a property are the same question — "what can userland hold" — asked at run
// time and at declaration time, and two answers would mean two marshallers in
// each binding and two widgets in the properties panel. What an attribute cannot
// be is `Reference`: a handle is meaningless outside the world holding it, and an
// attribute crosses a save file.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

	class Store;

	// One attribute's value, whatever type it is.
	//
	// **A struct with every field rather than a variant**, which is
	// `game::PropertyValue`'s trade and is made for the same reason: this crosses
	// between a store, a script and a document with its type carried alongside,
	// and a variant would cost a visitor at each of the three.
	//
	// It is the widest type in this header by far — the two sequences are 656
	// bytes between them — and that is affordable only because attributes are
	// stored in a hash map rather than in a column. A `PropertyValue`-shaped
	// thing in an archetype would be the mistake this whole file exists to avoid.
	//
	// @since v0.10
	struct AttributeValue {
		// Which of the fields below is meaningful.
		//
		// `Opaque` is what an unset attribute reads as, which is how
		// `GetAttribute` says "there is no such thing" without a second return.
		PropertyType Type = PropertyType::Opaque;

		// The plain scalar cases, one per width `PropertyType` names.
		//
		// **Read only when `Type` says so**, which is the whole discipline of a
		// struct-with-every-field: nothing here is cleared when another case is
		// written, so a reader that trusts `Float` without checking `Type` gets
		// whatever the last `Float` attribute on this entity happened to be.
		//@{
		bool Bool = false;
		int32_t Int32 = 0;
		int64_t Int64 = 0;
		float Float = 0.0f;
		double Double = 0.0;
		//@}

		// Used for both `Name` and `Enum`, exactly as `game::PropertyValue` does.
		core::Name Name;

		// The owning-string case. **The one an author actually reaches for**, and
		// the reason `PropertyType::String` exists at all: a value a game
		// *computes* must be able to go away, where a `core::Name` never does.
		std::string String;

		// The datatype cases — every `core/types` value a script can author.
		//
		// **The two sequences are 656 bytes between them and are carried
		// anyway**, which is the trade this struct's header opens with: an
		// attribute lives in a hash map rather than in a column, so width costs
		// the entities that have one instead of every entity in the world.
		//
		// Each is named for its type so a reader never has to look up which
		// field a `PropertyType` selects — the enum member and the field spell
		// the same word.
		//@{
		core::Vector3 Vector3;
		core::CFrame CFrame;
		core::Color3 Color3;
		core::Vector2 Vector2;
		core::UDim UDim;
		core::UDim2 UDim2;
		core::Rect Rect;
		core::NumberRange NumberRange;
		core::NumberSequence NumberSequence;
		core::ColorSequence ColorSequence;
		//@}
	};

	// Reports whether a type may be stored as an attribute.
	//
	// **`Reference` is the only refusal, and it is not an oversight.** An
	// `ecs::Entity` is a handle within one world and an attribute survives a save
	// file, so storing one would write a number that means a different row when it
	// is read back — rule 4's hazard with no name to fall back on. A game that
	// wants to point at something stores its instance's name.
	//
	// `Opaque` is refused too, because it is the absence of a value rather than
	// one.
	//
	// @param type The type to test.
	// @return `true` when an attribute may hold it.
	bool AttributeTypeAllowed(PropertyType type);

	// Every attribute in one world.
	//
	// A resource. See the header for why this is not a component.
	//
	// @since v0.10
	struct AttributeTable {
		// One entity's attributes, keyed by name.
		//
		// **A map per entity that has any, rather than one map keyed by a pair.**
		// A pair key means every `GetAttributes` is a scan of the whole table; a
		// map of maps means it is one lookup and then a walk of what is there. The
		// outer map only ever holds entities somebody has actually set something
		// on, which is the property that keeps an untouched world's table empty.
		std::unordered_map<uint32_t, std::unordered_map<uint32_t, AttributeValue>> Entities;
	};

	// Reads one attribute.
	//
	// @param store    The world.
	// @param instance The instance.
	// @param name     The attribute's name.
	// @param out      Filled in on success; left alone otherwise.
	// @return `false` when the instance has no such attribute.
	bool GetAttribute(const Store &store, Entity instance, core::Name name, AttributeValue &out);

	// Writes one attribute, creating the table if the world has none.
	//
	// **Setting a value of type `Opaque` removes the attribute**, which is
	// Roblox's `SetAttribute(name, nil)` and is the only spelling that lets a
	// script take one back. A separate `RemoveAttribute` would be a second way to
	// say it, and the two would drift the first time one of them started firing a
	// signal the other did not.
	//
	// @param store    The world.
	// @param instance The instance. Must be alive.
	// @param name     The attribute's name.
	// @param value    What to store. `Opaque` removes.
	// @return `false` for a dead instance, a refused type, or an adopt-only store.
	bool SetAttribute(Store &store, Entity instance, core::Name name, const AttributeValue &value);

	// Every attribute an instance carries, by name.
	//
	// **Sorted by name rather than in hash order**, because the callers are a
	// script iterating and a document being written, and both want the same
	// answer twice — `mono.tools/bindings`' argument for sorting a component set,
	// applied to a value somebody saves.
	//
	// @param store    The world.
	// @param instance The instance.
	// @return The names, sorted. Empty for an instance with none.
	std::vector<core::Name> AttributeNames(const Store &store, Entity instance);

	// Drops every attribute an instance carries.
	//
	// **Not the destroy path.** An instance being destroyed is cleaned up by
	// `StoreState`'s `DropAttributes`, which reaches the table by component id
	// because it sits below this header — one hook rather than two, for the
	// reason written there. This is the deliberate call: a scripting surface
	// clearing an instance's attributes in one step rather than naming each.
	//
	// @param store    The world.
	// @param instance The instance.
	// @return How many were dropped.
	size_t ClearAttributes(Store &store, Entity instance);

	// Registers the `AttributeTable` resource under an explicit name.
	//
	// Idempotent and process-wide, like every other registration.
	void RegisterAttributeComponents();
}
