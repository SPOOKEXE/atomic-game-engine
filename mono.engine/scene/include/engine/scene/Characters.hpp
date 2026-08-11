#pragma once

// The blocky thing a player drives, and how it is put together.
//
// **One body and five passengers.** A character looks like six parts and is
// simulated as one: `HumanoidRootPart` is the whole capsule — a box the full
// height of the character — and the head, torso, arms and legs are anchored
// parts carried along by `PoseCharacters`. Roblox welds six colliders together
// with `Motor6D`s and lets the solver hold them in formation; that needs joints,
// a constraint solver that respects them, and a mass distribution nobody
// authored. None of it changes what a player feels, and all of it is a way for
// a character to fall apart on a slope.
//
// So the limbs are `CharacterLimb` rows: a root and a rest offset, resolved in
// one flat pass. That is `Attachments.hpp`'s design and its argument applies
// unchanged — a limb's parent is a part, a part's transform is already world
// space, so a limb's world frame is one `CFrame` product with no chain to walk.
//
// **The pass runs wherever the character is drawn, not only where it is
// simulated.** A client receives the root's transform through interpolation and
// poses its own limbs from it; limbs that arrived over the wire on their own
// would each interpolate separately and the character would come apart at speed
// for one frame every time a delta was split. `client::InstallControls` and the
// server's simulation both install it.
//
// **A character is content, so it is built from the class table.** `MakeCharacter`
// goes through `CreateInstance` and `MakePart` like everything else, which means
// a character round-trips through a game file, appears in the explorer, and is
// replicated by the ordinary `scene.` prefix rule with nothing added.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// What a character is, held on its `Model`.
	//
	// **On the model rather than on the root part**, because the model is what
	// a script is handed — `Player.Character` is a model in Roblox and the
	// engine has no reason to disagree. The two entities it names are how the
	// rest of the engine reaches the parts without walking children by name; a
	// name lookup per frame is how `FindFirstChild("Humanoid")` becomes a
	// profile entry.
	//
	// @since v0.14
	struct Character {
		// The one part physics moves. Everything else follows it.
		ecs::Entity Root;

		// The `Humanoid` instance steering it.
		ecs::Entity Humanoid;

		// The `Player` this belongs to, or a null entity for an NPC.
		//
		// **A character with no owner is the ordinary case for anything that is
		// not a person at a keyboard**, and the field exists so that a server
		// admitting a client can find the character it spawned again without a
		// second table beside the store.
		ecs::Entity Owner;
	};

	// Which character a player is driving, held on the `Player`.
	//
	// **Roblox's `Player.Character`.** A component rather than a lookup by name
	// under `Workspace`, because the name is an author's to change and a
	// character renamed mid-game would silently stop being anybody's.
	//
	// @since v0.14
	struct PlayerCharacter {
		// The `Model`, or a null entity between a death and a respawn.
		ecs::Entity Model;
	};

	// A part carried by a character's root.
	//
	// **The character's own `Model` carries one too, with an identity offset.**
	// A model's `Transform` is what `PivotTo` and a camera subject read, so a
	// model left where it spawned would be a character whose handle stayed
	// behind — and giving it a limb row rather than a special case in the pass
	// is one rule instead of two.
	//
	// @since v0.14
	struct CharacterLimb {
		// The rest pose, in the root's own frame.
		core::CFrame Offset;

		// The root part this hangs off.
		ecs::Entity Root;

		// Explicit padding, so the object representation a snapshot writes
		// holds no uninitialised bytes. The reason every other `Reserved` here
		// gives.
		uint32_t Reserved = 0;
	};

	// Everything that decides what one character looks like.
	//
	// @since v0.14
	struct CharacterDesc {
		// Where the character stands. **The feet, not the centre** — an author
		// placing a character places it on a floor, and asking them to add half
		// its height is the conversion that is wrong in one direction forever.
		core::CFrame Frame;

		// What to call the model.
		std::string_view Name = "Character";

		// The shirt.
		core::Color3 TorsoColour{0.13f, 0.33f, 0.60f};

		// The trousers.
		core::Color3 LegColour{0.11f, 0.18f, 0.30f};

		// The skin.
		core::Color3 SkinColour{0.96f, 0.80f, 0.19f};
	};

	// How tall a character is, in metres, from its feet to the top of its head.
	//
	// **Stated once here and read by everything that has to agree with it.**
	// `Humanoid::Height` defaults to the same figure and `GroundCharacters`
	// takes half of it to find the feet — a rig whose parts added up to
	// something else would cast its ground ray from inside its own shins, which
	// is a character that can never jump and gives no clue why.
	inline constexpr float CHARACTER_HEIGHT = 5.0f;

	// Builds a character and parents it to the world's `Workspace`.
	//
	// The model holds a `HumanoidRootPart`, a `Humanoid`, and five limbs named
	// as Roblox names them, so a script written against `Head` or
	// `HumanoidRootPart` finds them.
	//
	// @param store The world.
	// @param desc  What it looks like and where it stands.
	// @return The `Model` instance, or `ecs::NULL_ENTITY` when the world has no
	//         `Workspace` to put it in.
	ecs::Entity MakeCharacter(ecs::Store &store, const CharacterDesc &desc);

	// Gives a player a character, replacing whatever they had.
	//
	// **Roblox's `Player:LoadCharacter()`, and it destroys the old one first**
	// — a player holding two characters is two things a camera could follow and
	// two humanoids the controls would drive at once.
	//
	// The character is named after the player, which is what makes
	// `workspace:FindFirstChild(player.Name)` the lookup a game script expects.
	// Network ownership of the root goes to the player, so a client may move
	// its own body and no one else's.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @param frame  Where to stand them, feet first. Defaults to whatever
	//        `FindSpawn` offers.
	// @return The new `Model`, or `ecs::NULL_ENTITY` when `player` is not a
	//         live `Player` or the world has no `Workspace`.
	ecs::Entity LoadCharacter(ecs::Store &store, ecs::Entity player, const core::CFrame &frame);

	// The same, standing them wherever `FindSpawn` says.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @return The new `Model`, or `ecs::NULL_ENTITY`.
	ecs::Entity LoadCharacter(ecs::Store &store, ecs::Entity player);

	// Destroys a player's character, if they have one.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @return `true` when there was one to remove.
	bool RemoveCharacter(ecs::Store &store, ecs::Entity player);

	// Binds a model to a player, which is what `Player.Character = model` does.
	//
	// **The one hook a game needs, and everything downstream keys off it.** A
	// client submits a move only once `CharacterOf` answers and the model
	// carries a `Character`; a host applies one under the same two conditions;
	// `FollowOwnCharacter` aims the camera at the rig's root. All three read the
	// pair this writes, so a game that assigns `Player.Character` gets controls,
	// a camera and server-side movement with nothing else to remember.
	//
	// **The rig is resolved from the model rather than supplied**, so a character
	// a *script* built works exactly as one `MakeCharacter` built. The humanoid
	// is the model itself when it carries one, and otherwise the first descendant
	// that does; the root is `Humanoid::RootPart`, or a descendant named
	// `HumanoidRootPart`, or the model. `Humanoid::RootPart` is written back so
	// `StepCharacters` and `GroundCharacters` resolve the same body this did.
	//
	// **A model with no `Humanoid` is refused**, for `Portal.Destination`'s
	// reason: a mistake refused where it was made beats one discovered by
	// pressing W and watching nothing happen.
	//
	// **Clearing is allowed and it stops the body.** `player.Character = nil`
	// zeroes the old humanoid's `MoveDirection` and drops its latched jump —
	// without that a character released mid-stride keeps walking for ever,
	// because the pass that would have cleared it no longer has an owner to
	// clear it for.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @param model  The `Model` to drive, or `ecs::NULL_ENTITY` to release.
	// @return `false` when `player` is not a live `Player`, or when `model` is
	//         neither null nor a live model with a humanoid in it.
	bool SetPlayerCharacter(ecs::Store &store, ecs::Entity player, ecs::Entity model);

	// Keeps every player's rig in step with the model they were assigned.
	//
	// **The polling half of `SetPlayerCharacter`, and it exists because the
	// assignment is not always local.** A client never runs the setter: it
	// receives `PlayerCharacter` over the wire and has to build the same link
	// from its own store. A model destroyed under a player is the other case —
	// the reference dangles, and a humanoid still holding a `MoveDirection` is a
	// corpse that walks.
	//
	// Idempotent and cheap: a player whose model already carries a `Character`
	// naming them costs one component read.
	//
	// @param store The world.
	// @return How many players were re-linked or released.
	size_t LinkPlayerCharacters(ecs::Store &store);

	// Destroys characters whose owner is no longer alive.
	//
	// **The other direction of `LinkPlayerCharacters`, and the one nothing was
	// doing.** That function handles a model destroyed under a player. This is a
	// player destroyed under a model — and a `Character` is not a child of the
	// `Player`, it is a `Model` under Workspace, so nothing takes it along.
	//
	// **A rule rather than a line in each caller, because there were only ever
	// two callers and both had to remember.** `studio::PlayLink::Stop` and
	// `TeleportService:Teleport` each call `RemoveCharacter` by hand before
	// destroying the instance, and the comment in the first says why it must:
	// *"`RemoveCharacter` is what `DestroyInstance` on a `Player` cannot do for
	// itself"*. Every other way a player can stop existing — a script calling
	// `player:Destroy()`, an author deleting one in the explorer, a host
	// dropping a client that timed out — left a rig standing on the spawn with
	// a `Humanoid` nobody drives, a root part the solver keeps awake and six
	// limbs `PoseCharacters` follows for the rest of the session.
	//
	// The two hand-written calls stay: releasing the body in the same breath as
	// the player is tidier than leaving it for the next tick, and this is the
	// backstop for everything that does not.
	//
	// **`Character::Owner` is the test**, which is the same field
	// `UpdateCharacterControl` reads to decide whose keyboard drives what — an
	// NPC leaves it null and is never collected.
	//
	// @param store The world.
	// @return How many characters were destroyed.
	size_t ReclaimOrphanedCharacters(ecs::Store &store);

	// The model a player is driving, or a null entity.
	//
	// @param store  The world.
	// @param player The `Player` instance.
	// @return The `Model`, or `ecs::NULL_ENTITY`.
	ecs::Entity CharacterOf(const ecs::Store &store, ecs::Entity player);

	// Where a character should be put when nobody has said.
	//
	// **A part named `SpawnLocation` if the world has one, and the origin
	// otherwise.** Roblox has a `SpawnLocation` class with teams and a forcefield
	// on it; none of that exists here yet and inventing the class before the
	// behaviour would be a class with a footnote. A name is what an author can
	// already express in the explorer.
	//
	// The frame returned is *feet* height — the top face of the spawn part, so
	// a character put there stands on it rather than in it.
	//
	// @param store The world.
	// @return Where to stand the next character.
	core::CFrame FindSpawn(const ecs::Store &store);

	// Points the camera at the local player's own body.
	//
	// **One rule, run every frame, rather than a line in `LoadCharacter`.** The
	// character a viewer follows may appear three different ways: spawned in
	// this process, arriving over the wire into a replica, or replaced by a
	// respawn. A camera aimed at the moment of spawning gets the first and
	// misses the other two — a connected client would follow nothing at all,
	// because nothing on a client ever calls `LoadCharacter`.
	//
	// **The subject is the root and not the model**, because the root is what
	// the solver moves; `PoseCharacters` puts the model there a phase later, and
	// a camera a phase behind its own character is visible judder.
	//
	// Does nothing where there is no `LocalPlayer`, which is every world an
	// editor and every test has.
	//
	// @param store The world.
	// @return `true` when the subject changed.
	bool FollowOwnCharacter(ecs::Store &store);

	// Puts every limb where its root says it should be.
	//
	// **One flat pass over one component type**, for `ResolveAttachments`'
	// reason: a limb's root is a part whose transform is already world space, so
	// this is a product and never a walk. A limb whose root is gone is left
	// alone rather than moved to the origin — a dead root is a character being
	// torn down, and a pile of limbs at the origin is a worse picture than a
	// pile where it died.
	//
	// @param store The world.
	// @return How many limbs were placed.
	size_t PoseCharacters(ecs::Store &store);

	// The `Model` class id, registering the scene tree on first call.
	//
	// **A `Model` is a `PVInstance` and not a `BasePart`.** It has a place in
	// the world — that is what makes `PivotTo` on a character mean something —
	// and it is not drawn, bounded or collided. Roblox's arrangement, for the
	// reason `MeshPart` gives: a class tree that differs from the one scripts
	// expect is a migration nobody asked for.
	//
	// @return The class id.
	ecs::ClassId ModelClass();
}
