#pragma once

// A thing a character can hold, and what holding it means.
//
// **Equipping is a reparent, exactly as it is in Roblox, and the tree is the
// only place it is written down.** A `Tool` inside a `Player.Backpack` is
// stowed; a `Tool` that is a direct child of a character `Model` is equipped.
// There is no `Equipped` flag beside that, because a flag would be a second
// copy of a fact the hierarchy already states - rule 2 - and it is the copy
// that goes stale the first time a script reparents one without going through
// this file.
//
// **What carries the handle is a `CharacterLimb` and deliberately not an
// `Attachment`.** `docs/DEFERRED.md` D00120 held this entry open on "a joint,
// or an attachment strong enough to carry a handle", and named
// `scene::Attachment` as the first thing to look at. It is the wrong half:
// `ResolveAttachments` runs in `PreRender` and *resolves a frame* rather than
// moving a part, and `Attachments.hpp` says in as many words that a caller
// wanting a weld is asking for something that pass does not promise. What this
// engine already has instead is the thing a character rig is made of - a
// `CharacterLimb` is an anchored part carried along by a root at a fixed
// offset, resolved in one flat `CFrame` product by `PoseCharacters`. That is
// the joint substitute `Characters.hpp` chose over `Motor6D` for the whole
// body, and a held object is one more part in the same formation. So a tool
// needs no constraint solver, no new pass and no new component: it needs the
// row a limb already has.
//
// **Three things fall out of that and none of them had to be built.** The
// offsets replicate, because `scene.CharacterLimb` replicates; the handle's
// per-tick `Transform` stops crossing, because that component is already
// `replication`'s suppressor for `scene.Transform`; and a client poses the
// handle from the root it interpolated, because it already poses five limbs
// that way. `D00115` bought all of it and this is its second consumer.
//
// **Who may equip is the authority and the refusal is not new either.**
// `EquipTool` and `UnequipTool` refuse on `ecs::Store::AdoptOnly`, which is
// `scene::TakeDamage`'s door, and a script's `tool.Parent = character` is an
// ordinary property write that `ecs::Store::SetProperty` already refuses in a
// replica. That is one rule with two doors rather than a third statement of it
// - see `EquipTool`.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// What a `Tool` instance is: where its handle sits in the hand.
	//
	// **One field, because one field has a reader.** That is the bar
	// `docs/DEFERRED.md` D00119 set for a class existing at all, and Roblox's
	// `Tool` has six more that would fail it here - `CanBeDropped` needs a drop
	// input nothing sends, `Enabled` and the `Activated` pair need an activation
	// channel that does not exist, and `ToolTip` and `TextureId` need an
	// inventory interface. A property nothing acts on is what this class was
	// held back for; adding six would be the same mistake inside the fix.
	//
	// @since v0.15
	struct Tool {
		// Where the handle's centre sits, relative to the grip point.
		//
		// **Composed onto the grip rather than replacing it**, so the identity
		// - which is what `Instance.new("Tool")` starts with - puts the handle's
		// centre in the hand and an author only says how it differs from that.
		// Roblox's `Tool.Grip` is the same idea expressed as a `Motor6D`'s `C1`.
		core::CFrame Grip;
	};

	// What a tool's handle is called.
	//
	// **A name, and Roblox's.** A `Tool` is a `Model` and the part that goes in
	// the hand is the direct child called `Handle` - which is the same kind of
	// lookup `ResolveRig` makes for `HumanoidRootPart` and not the kind
	// `scene/AGENTS.md` refuses: that rule is about *services*, which a script
	// can rename out of existence, and this is content an author names on
	// purpose.
	//
	// A tool with no `Handle` still equips and simply has nothing to place,
	// which is Roblox's `RequiresHandle = false` without a field to say it.
	//
	// @since v0.15
	inline constexpr std::string_view TOOL_HANDLE_NAME = "Handle";

	// The `Tool` class id, registering the scene tree on first call.
	//
	// **Derives from `Model`, and `BackpackItem` is deliberately skipped.**
	// Roblox puts an abstract `BackpackItem` between the two; registering it
	// here would put an instantiable class that does nothing into the insert
	// palette, which is the exact objection D00120 held this whole entry open
	// on. A `Model` is what a tool *is* - a container with a place in the world
	// and parts under it - and `:IsA("Model")` answers what a script would
	// actually ask.
	//
	// @return The class id.
	// @since v0.15
	ecs::ClassId ToolClass();

	// The part a tool is held by, or a null entity.
	//
	// @param store The world.
	// @param tool  The `Tool` instance.
	// @return Its direct child named `Handle`, or `ecs::NULL_ENTITY`.
	// @since v0.15
	ecs::Entity ToolHandle(const ecs::Store &store, ecs::Entity tool);

	// The tool a character is holding, or a null entity.
	//
	// **A search of the model's own children rather than a stored handle**,
	// which is `Player.Backpack`'s call one file over: a field naming the
	// equipped tool would be a second place the answer lived and would go stale
	// the moment a script reparented one. A character has a handful of children.
	//
	// @param store     The world.
	// @param character The character `Model`.
	// @return The `Tool`, or `ecs::NULL_ENTITY`.
	// @since v0.15
	ecs::Entity EquippedTool(const ecs::Store &store, ecs::Entity character);

	// Where a handle sits, in the character root's own frame.
	//
	// **Read off the rig rather than written down as a constant**, so a
	// character built with different proportions holds its tools in its own
	// hand: the answer is the `Right Arm` limb's rest offset dropped by that
	// part's half-height, which is the bottom face of the arm and is where
	// Roblox's `RightGrip` attachment is.
	//
	// @param store     The world.
	// @param character The character `Model`.
	// @return The grip frame, or the identity for anything that is not a
	//         character or has no right arm - which puts a handle at the root.
	// @since v0.15
	core::CFrame ToolGrip(const ecs::Store &store, ecs::Entity character);

	// Puts a tool in a character's hand.
	//
	// **Roblox's model kept whole: this reparents and nothing else decides.**
	// The tool becomes a direct child of the character `Model`, which is both
	// what "equipped" means and what makes it stop being private on the wire -
	// `scene::PlayerOwning` answers the owner for anything under a `Player` and
	// nothing for something under `Workspace`, so a stowed tool reaches one
	// client and a held one reaches everybody, with no rule added anywhere.
	//
	// **The authority's call, refused in a replica rather than documented as
	// one.** A client that can reparent its own tool can duplicate it: the write
	// survives until the next delta contradicts it, which presents as an
	// inventory that works sometimes. `ecs::Store::SetProperty` already refuses
	// every property write in a replica - that is where this engine answers "who
	// owns a row", and it is what refuses a `LocalScript`'s `tool.Parent = ...`
	// - and this is the same refusal for the C++ door, so a hosted script and a
	// hosted subsystem get one answer instead of two. `scene::TakeDamage` is the
	// same pair for the same reason.
	//
	// **One hand, so equipping puts back whatever was already in it** - and
	// refuses the swap when it cannot. Roblox's rule, and here it is also the
	// arithmetic: two handles sharing one grip offset is two parts drawn inside
	// each other. A hand that cannot be emptied is a character with no owner to
	// have a `Backpack`; see `UnequipTool`.
	//
	// @param store     The world. Must be the authority's.
	// @param character The character `Model`, which must carry a `Character`.
	// @param tool      The `Tool` instance.
	// @return `false` in a replica, when either argument is not live and of the
	//         right kind, or when the tool already held cannot be put away.
	// @since v0.15
	bool EquipTool(ecs::Store &store, ecs::Entity character, ecs::Entity tool);

	// Takes a tool out of a character's hand and back into its owner's
	// `Backpack`.
	//
	// **The `Backpack` and never the `StarterGear`**, which is the direction
	// `Services.hpp` states: gear survives a death by being in `StarterGear`,
	// and a respawn refills the backpack from it. Putting an unequipped tool in
	// the persistent list would make picking something up and dropping it a
	// permanent grant.
	//
	// **A tool an NPC is holding has nowhere to go, and is refused rather than
	// dropped.** `Character::Owner` is null for anything that is not a person at
	// a keyboard, so there is no backpack to put it in; inventing one - parenting
	// it into `Workspace` at the character's feet - would be this module deciding
	// what dropping a tool looks like, which is a game's rule.
	//
	// @param store The world. Must be the authority's.
	// @param tool  The `Tool` instance.
	// @return `false` in a replica, when the tool is not held by a character, or
	//         when that character's owner has no `Backpack`.
	// @since v0.15
	bool UnequipTool(ecs::Store &store, ecs::Entity tool);

	// Hangs every equipped tool's handle off the character holding it, and
	// releases the ones that are no longer held.
	//
	// **The polling half of `EquipTool`, and it exists for
	// `LinkPlayerCharacters`' reason: the assignment is not always local.** A
	// game script equips by writing `tool.Parent = character` - which is how a
	// Roblox script does it and is a plain property write here - so the reparent
	// arrives with nobody having called `EquipTool`. On a client the reparent
	// arrives over the wire instead. Both leave a tree saying one thing and a
	// handle hanging off nothing, and this is what makes them agree.
	//
	// **Derived and therefore safe everywhere.** It reparents nothing and
	// destroys nothing - every write it makes is a function of where the tool
	// already is - so a replica running it computes the same answer the
	// authority did rather than fighting it. That is the line `EquipTool` is on
	// the other side of.
	//
	// **What it writes is a `CharacterLimb` on the handle, and a `Motion`
	// removal beside it.** A carried part is posed rather than integrated, and
	// taking `scene::Motion` away is how this ECS says that - the same archetype
	// move `physics` makes when it puts a body to sleep. Nothing else on the
	// handle is touched: `Anchored`, `CanCollide`, `CollisionGroup` and the
	// physical properties are all declared properties an author set, and an
	// equip that rewrote one would be the engine editing content on the way
	// past. A handle that should not shove its owner is authored
	// `CanCollide = false`, which is what a Roblox author already does.
	//
	// Run by `PoseCharacters`, so every host that draws a character already
	// calls it; exported because a caller that wants the effect without waiting
	// for a phase should not have to run the pose to get it.
	//
	// @param store The world.
	// @return How many handles were hung or released.
	// @since v0.15
	size_t UpdateToolGrips(ecs::Store &store);
}
