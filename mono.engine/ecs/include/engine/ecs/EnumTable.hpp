#pragma once

// The named sets a property may be one of.
//
// **Why this is a table and not a C++ enum.** `scene::BodyKind` and
// `scene::ShapeKind` are closed sets the engine switches on, and they are the
// right shape for that - adding a case is a compiler error everywhere it
// matters. A *property* enum is a different thing: `AlphaMode` is a set userland
// picks from, a game may extend, and a manifest has to describe. A C++ enum can
// do none of those, because the set has to be readable at run time by a binding
// generator that never saw the header.
//
// So a property enum is a name and a list of member names, and both cross as
// **text** - rule 4. What `PropertyType::Enum` adds is not a new storage form.
// It is that userland gets a value it can compare and be told when it is wrong:
//
//     part.AlphaMode = "Clipp"   -- refused, naming the enum
//     part.AlphaMode = Enum.AlphaMode.Clip
//
// A `PropertyType::Name` property accepts any string at all, so that typo lands
// in a component and surfaces as a part that draws every cut-out plane opaque
// for reasons nobody can see.
//
// **The engine registered a seventeen-member `Material` here until v0.10 and no
// longer does**, which is worth a line because it was this file's own example:
// the membership check was the only thing it did, and a material is content now
// rather than a word. `scene/Materials.hpp` carries that change. A game that
// wants its own named set still registers one.
//
// **Process-wide, like `Components` and `Classes`, and for the same reason**: a
// property's enum has to mean the same thing in every world, because a snapshot
// taken in one is restored into another.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {

	// Registers named sets and answers questions about them.
	//
	// @since v0.6
	// @threadsafe
	class EnumTable {
	  public:
		// Registers a member of an enum, creating the enum if it is new.
		//
		// Registering the same member twice is a no-op rather than an error:
		// two modules may both declare that `AlphaMode` has a `Clip`, and the
		// second is agreeing rather than conflicting.
		//
		// **Order is registration order and it is what a binding lists.** Not
		// sorted: the order an author reads them in should be the order they
		// were declared, which is usually meaningful - `Static`, `Kinematic`,
		// `Dynamic` is a progression and alphabetical is not.
		//
		// @param enumName The set's name, as a script spells it.
		// @param member   The member's name.
		static void Register(std::string_view enumName, std::string_view member);

		// Registers several members at once.
		//
		// @param enumName The set's name.
		// @param members  The members, in the order they should be listed.
		static void Register(std::string_view enumName, std::span<const std::string_view> members);

		// Reports whether an enum has a member.
		//
		// @param enumName The set to look in.
		// @param member   The member to look for.
		// @return `true` when the enum is registered and holds that member.
		static bool Has(core::Name enumName, core::Name member);

		// Reports whether an enum is registered at all.
		//
		// Distinct from having no members: a property declaring an enum nobody
		// registered is a bug in the declaration, and a caller needs to be able
		// to tell that from a set that is genuinely empty.
		//
		// @param enumName The set to look for.
		// @return `true` when something registered it.
		static bool Known(core::Name enumName);

		// The members of one enum, in registration order.
		//
		// **By value, not a span**, and the reason is the one that makes a span
		// dangerous here: members are appended, so the vector behind them
		// reallocates, and a span handed out before a late registration would
		// point at freed memory. Registration does happen at startup in
		// practice - but "in practice" is a convention, and rule 6 says a
		// constraint the build does not check is documentation. A copy of a
		// handful of interned ids costs nothing next to being wrong once.
		//
		// @param enumName The set to list.
		// @return The members, empty for an unregistered enum.
		static std::vector<core::Name> MembersOf(core::Name enumName);

		// The member at an ordinal, for a property whose storage is the number.
		//
		// **The other direction, and a table that could not answer it was half a
		// table.** `Has` says whether a name is a member, which is everything a
		// *checked string* property needs - and a component cannot hold a string,
		// so the ones that matter store the ordinal and have to convert both ways
		// on every read and write. Without this each of them would keep its own
		// list of the names in order, which is the same fact recorded twice and
		// wrong the first time somebody registers a member in between.
		//
		// Ordinals are registration order, which is why registration order is
		// worth being deliberate about: `scene` registers `NormalId` in Roblox's
		// order so that a saved number means the same thing in both engines.
		//
		// @param enumName The set.
		// @param ordinal  Which member, from zero.
		// @return The member, or an invalid `Name` when the enum is unregistered
		//         or the ordinal is past its end.
		static core::Name MemberAt(core::Name enumName, size_t ordinal);

		// Where a member sits in its set.
		//
		// @param enumName The set.
		// @param member   The member to find.
		// @param ordinal  Filled with its position when this returns true.
		// @return `false` when the enum is unregistered or holds no such member,
		//         leaving `ordinal` untouched - which is what makes
		//         `part.Face = "Frnot"` a refusal at the assignment rather than a
		//         face nobody chose.
		static bool OrdinalOf(core::Name enumName, core::Name member, size_t &ordinal);

		// Every registered enum, in registration order.
		//
		// What the bindings manifest walks. By value, for the reason
		// `MembersOf` gives.
		//
		// @return The enum names.
		static std::vector<core::Name> Names();

		// The number of registered enums.
		//
		// @return The current registration count.
		static size_t Count();
	};
}
