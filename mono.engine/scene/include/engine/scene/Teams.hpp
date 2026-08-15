#pragma once

// Sides, and the pads that belong to them.
//
// **A team is a thing in the tree, which is what makes it a feature rather
// than a coloured label.** `docs/DEFERRED.md` D00119 refused `Player.Team`
// while the only thing behind it would have been a string on a row: a side
// nothing acts on is a field, and a field that looks like a feature is worse
// than an absence. What acts on it is here - a `SpawnLocation` carries a
// `TeamColor` and `FindSpawn` filters on it, so joining a team changes where
// you appear.
//
// **A `SpawnLocation` is a class now and was a *name* until v0.15.**
// `scene::FindSpawn` looked for a part called `SpawnLocation`, which
// `Characters.hpp` recorded as a deliberate stop - a class with teams and a
// forcefield on it would have been a class with a footnote while neither
// existed. The name still works and is meant to: a plain `Part` still called
// `SpawnLocation` is read as an enabled, neutral spawn, so every scene in
// `mono.engine/examples` keeps spawning people with nothing edited. What the
// class adds is the three fields a name cannot carry.
//
// **Colours are compared, not ids.** Roblox matches a `Team` to a
// `SpawnLocation` by `BrickColor`, which is a palette index; this engine has
// no palette, so the match is `Color3` against `Color3` within
// `TEAM_COLOUR_TOLERANCE`. See `SameTeamColour` for why that is a tolerance
// and not an equality.
//
// @tier L7 · shared

#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// What a `Team` instance is: a colour, and whoever points at it.
	//
	// **The members are the whole of it, and the list is short on purpose.**
	// Roblox's `Team` also carries `AutoAssignable` and a `PlayerAdded` signal;
	// neither has a reader here, and a property nothing acts on is the thing
	// D00119 refused this class for in the first place. Who is on a team is the
	// `PlayerTeam` rows pointing at it rather than a list kept beside them -
	// rule 2, and the list would go stale the first time a player was
	// destroyed.
	//
	// @since v0.15
	struct Team {
		// The side's colour, which is also what a `SpawnLocation` is matched
		// against.
		//
		// Roblox's medium stone grey by default, so a team somebody made and
		// did not colour is visible rather than black.
		core::Color3 Colour{0.639f, 0.635f, 0.647f};
	};

	// Which side a player is on, held on the `Player`.
	//
	// **A component rather than a field on `PlayerIdentity`**, for
	// `PlayerCharacter`'s reason one file over: identity lasts as long as a
	// connection and a team changes mid-round, so a component holding both
	// would rewrite two fields that never change every time somebody swapped
	// sides.
	//
	// Absent for a player on no team, which is the state a world starts in.
	//
	// @since v0.15
	struct PlayerTeam {
		// The `Team` instance, or a null entity between teams.
		//
		// Spelled `Instance` rather than `Team` - `LocalPlayer::Instance`'s
		// name - because a member called `Team` inside a namespace that also
		// holds a *type* called `Team` is a shadow waiting for the first person
		// who writes a method on this struct.
		ecs::Entity Instance;
	};

	// What a `SpawnLocation` adds to a part.
	//
	// **Three fields, and each one has a reader in `FindSpawn`.** That is the
	// bar D00119 set for this class existing at all.
	//
	// @since v0.15
	struct SpawnLocation {
		// Which side's pad this is. Matched against `Team::Colour`.
		core::Color3 TeamColour{0.639f, 0.635f, 0.647f};

		// Whether a player on no team, or on the wrong one, may still use it.
		//
		// **True by default, which is Roblox's default and is also what keeps
		// every existing scene working.** A world that has never heard of teams
		// is a world of neutral pads, and everybody lands on them.
		bool Neutral = true;

		// Whether it is a spawn at all.
		//
		// A round that closes one half of a map turns these off rather than
		// destroying the pads, so the geometry stays and the spawn stops.
		bool Enabled = true;

		// Explicit padding, so the object representation a snapshot writes
		// holds no uninitialised bytes. The reason every other `Reserved` in
		// this module gives.
		uint8_t Reserved[2] = {};
	};

	// How far apart two team colours may be and still be one side.
	//
	// **A tolerance rather than an equality, because a colour here is a value
	// and not a palette index.** Roblox compares `BrickColor`s, which are
	// enumerated, so equality is exact by construction; a `Color3` reaches this
	// comparison through a property setter, a snapshot, and - in the studio -
	// a JSON file that writes floats as text and reads them back. Exact
	// equality survives the first two and not the third.
	//
	// One five-hundred-and-twelfth is finer than the gap between any two
	// distinct authored colours and coarser than anything a text round trip
	// introduces.
	inline constexpr float TEAM_COLOUR_TOLERANCE = 1.0f / 512.0f;

	// Whether two colours name the same side.
	//
	// @param left  One colour.
	// @param right The other.
	// @return `true` when every channel is within `TEAM_COLOUR_TOLERANCE`.
	// @since v0.15
	bool SameTeamColour(const core::Color3 &left, const core::Color3 &right);

	// The `Team` class id, registering the service tree on first call.
	//
	// **A `Team` derives from `Instance` and not from `Service`**, which is
	// `Player`'s arrangement and for the same reason: there are many of them,
	// a game makes and destroys them, and the class picker hides services as a
	// category. `Teams` is the service; a `Team` is content inside it.
	//
	// @return The class id.
	ecs::ClassId TeamClass();

	// The `SpawnLocation` class id, registering the scene tree on first call.
	//
	// **Derives from `Part`, because a spawn is a pad you can stand on.** Every
	// scene that has one builds it as a block and expects to see it, so a class
	// that was not drawn or collided would be a different thing wearing the same
	// name.
	//
	// @return The class id.
	ecs::ClassId SpawnLocationClass();

	// The world's `Teams` service, or a null entity when it has none.
	//
	// @param store The world.
	// @return The service instance.
	// @since v0.15
	ecs::Entity TeamsOf(const ecs::Store &store);

	// Creates one `Team` under the world's `Teams` service.
	//
	// **`AddPlayer`'s shape, and deliberately not automatic.** Which sides a
	// game has is the game's business, so furnishing a world invents none -
	// the same refusal `InstallServices` makes about occupants.
	//
	// @param store  The world.
	// @param name   What to call the side.
	// @param colour Its colour, which is what a `SpawnLocation` is matched
	//        against.
	// @return The `Team` instance, or `ecs::NULL_ENTITY` when the world has no
	//         `Teams`.
	// @since v0.15
	ecs::Entity AddTeam(ecs::Store &store, std::string_view name, const core::Color3 &colour);

	// Puts a player on a side, which is what `Player.Team = team` does.
	//
	// **Refused for anything that is not a `Team`**, for `Portal.Destination`'s
	// reason: a mistake refused where it was made beats one discovered by
	// respawning and landing somewhere unexplained.
	//
	// Clearing is allowed and is how a game returns somebody to no side.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @param team   The `Team`, or `ecs::NULL_ENTITY` to leave every side.
	// @return `false` when `player` is not a live `Player`, or when `team` is
	//         neither null nor a live `Team`.
	// @since v0.15
	bool SetPlayerTeam(ecs::Store &store, ecs::Entity player, ecs::Entity team);

	// Which side a player is on, or a null entity.
	//
	// **Liveness-checked, which is `CharacterOf`'s rule and matters more here.**
	// A `Team` destroyed mid-round leaves every member holding a handle to a row
	// that is gone, and a spawn filter reading it would match nothing and put
	// the whole side at the origin.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @return The `Team`, or `ecs::NULL_ENTITY`.
	// @since v0.15
	ecs::Entity TeamOf(const ecs::Store &store, ecs::Entity player);
}
