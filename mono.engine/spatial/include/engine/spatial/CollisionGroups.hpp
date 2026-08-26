#pragma once

// Names for the thirty-two layers, and which of them meet.
//
// **`LayerMask` is thirty-two anonymous bits, and anonymous is the problem.**
// A game says "players do not collide with player projectiles"; the storage
// says bit 3 is absent from mask 7. Something has to join the two, and where
// that something lives was a real decision rather than a filing question:
//
// - **Not in `scene`.** `Collider` holds the bits, but a naming scheme is a
//   physics policy, and `scene` deciding one would be a component module
//   choosing how collision is configured for every game that ever uses it.
//   `scene/src/Part.cpp` says so in as many words about `CollisionGroup`.
// - **Not in `script`.** Then C++ and Luau would each have a set of names and
//   only one of them would be in a save file.
// - **Here**, beside `LayerMask`, because this module owns the bits and a name
//   for one is a fact about the bit.
//
// Roblox calls the surface `PhysicsService`, and that is what the script
// binding is called. This is the table underneath it.
//
// **Names cross, indices do not.** Rule 4 exactly: a group is identified by its
// string in a save file and on a wire, and the index it happens to hold depends
// on registration order - which is the order a game's files linked in. A
// `CollisionGroup` property therefore reads and writes a name.
//
// **Registration is process-wide**, like `Components` and `Classes`, because a
// group has to mean the same thing in every world: a snapshot taken in one is
// restored into another, and two worlds disagreeing about which bit is
// "Players" is a collision matrix that changes when a world migrates.
//
// @tier L6 · shared

#include <engine/core/Name.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::spatial {

	// The index no group holds.
	inline constexpr uint32_t NO_GROUP = LayerMask::LAYER_COUNT;

	// Named collision layers, and the matrix saying which pairs meet.
	//
	// @since v0.6
	// @threadsafe
	class CollisionGroups {
	  public:
		// The group every collider starts in.
		//
		// Index zero and always registered, so a game that never calls anything
		// here still has a name for what its parts are in. `Collider::Layer`
		// defaults to `Only(0)`, which is this.
		static constexpr std::string_view DEFAULT = "Default";

		// Registers a group, or returns the index it already has.
		//
		// **Refused rather than wrapped past thirty-two.** A thirty-third group
		// silently folded onto an existing bit would make two unrelated groups
		// collide as one, which presents as a physics bug in a scene neither
		// group's author was looking at.
		//
		// A new group starts colliding with **every** existing group, matching
		// Roblox: a group nobody has configured behaves like ordinary geometry,
		// and the alternative - a group that collides with nothing until told
		// otherwise - makes a new group look broken.
		//
		// @param name The group's name.
		// @return Its layer index, or `NO_GROUP` when all thirty-two are taken.
		static uint32_t Register(std::string_view name);

		// The index a name holds.
		//
		// @param name The group's name.
		// @return Its layer index, or `NO_GROUP` when nothing registered it.
		static uint32_t IndexOf(core::Name name);

		// The name an index holds.
		//
		// @param index The layer index.
		// @return The name, or an invalid name for an unregistered index.
		static core::Name NameOf(uint32_t index);

		// Sets whether two groups collide.
		//
		// **Symmetric, and that is not a convenience.** `LayerMask::Overlaps`
		// asks whether two sets share a bit, and a pair is considered only when
		// each side's layer is in the other's mask - so a one-sided setting
		// would produce a pair that one collider believes in and the other does
		// not, which the broad phase resolves differently depending on which it
		// visited first.
		//
		// @param first     One group.
		// @param second    The other. May be the same, for self-collision.
		// @param collidable Whether the pair is considered.
		// @return `false` when either name is not registered.
		static bool SetCollidable(core::Name first, core::Name second, bool collidable);

		// Reports whether two groups collide.
		//
		// @param first  One group.
		// @param second The other.
		// @return `true` when the pair is considered.
		static bool Collidable(core::Name first, core::Name second);

		// The mask a collider in one group should be tested against.
		//
		// What `Collider::Mask` is set to when a part joins a group: every group
		// this one collides with.
		//
		// @param index The group's layer index.
		// @return The mask, or `All()` for an unregistered index.
		static LayerMask MaskFor(uint32_t index);

		// Every registered group, in registration order.
		//
		// By value for the reason `ecs::EnumTable::MembersOf` gives: the backing
		// vector grows, so a span handed out before a late registration would
		// dangle.
		//
		// @return The group names.
		static std::vector<core::Name> Names();

		// How many groups are registered.
		//
		// @return The count, at least one - `Default` always exists.
		static uint32_t Count();

		// Forgets every group but `Default` and restores the full matrix.
		//
		// **For tests, and it is the only reason this exists.** A process-wide
		// registry with thirty-two slots and no reset would make the order test
		// suites happened to run in decide whether the thirty-third
		// registration in the process failed.
		static void Reset();
	};
}
