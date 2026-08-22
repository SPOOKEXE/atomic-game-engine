#pragma once

// What is playing on a rig, and what it is playing.
//
// **Three rows rather than one, because they have three lifetimes.** A clip is
// content and outlives the world; an animator belongs to a body and lasts as
// long as the body does; a track lasts as long as somebody is playing it. One
// component holding all three would be rewritten on every play and stop for the
// sake of two fields nobody touches, which is the split `PlayerIdentity` and
// `PlayerCharacter` already make one file along.
//
// **A track is an instance here and a userdata in Roblox, and that is a
// deliberate departure.** Roblox's `AnimationTrack` is opaque because Roblox's
// `Animator` is a black box; `ROADMAP.md` v0.24 says in as many words that this
// engine's is not going to be one - "make humanoid a shim for character
// controller (so not a black box)" and "more modular than roblox standard
// humanoid". A row in the store is what modular means here: it saves, it
// replicates, a script can read it, and a debugger can show it. The cost is that
// playing a clip is `Instance.new` rather than a method call, which is the same
// trade `Sound.Playing` already makes for the same reason.
//
// **Nothing in this module evaluates a track.** `scene` is `shared` and holds
// data and one resolver, exactly as it holds a `Sound` without a mixer. What
// samples a clip and writes `Bone::Transform` is the animation handler at v0.24;
// what it needs from here is somewhere to read the play head from, and that is
// what these are.
//
// arch-waiver public-header: forward API for the v0.24 animation handler.
// `docs/FUTURE_COMPONENTS.md` says what reads these and in what order. Decision
// 16.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Skinning.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// How a track's channels combine with the ones already playing.
	//
	// Roblox's `AnimationPriority`, and the numbers are its numbers: a saved
	// place and a `.rbxl` import both carry them, and `Enums.hpp`'s `NormalId`
	// records the same decision for the same reason.
	//
	// @since v0.19
	enum class AnimationPriority : uint8_t {
		// Anything with nothing better to do. A breathing idle.
		Core = 0,

		// Walking, running, swimming.
		Idle = 1,

		// A deliberate movement, which beats locomotion.
		Movement = 2,

		// A one-off the game asked for, which beats everything below it.
		Action = 3,

		// Reserved above `Action` for a game that needs a layer of its own,
		// which is Roblox's `Action2` through `Action4` collapsed into one: four
		// numbered layers with no meaning between them is a set nobody can name,
		// and this engine has no content that uses more than one.
		Override = 4,
	};

	// What an `Animation` instance holds.
	//
	// **Two names, and the second is what makes a mismatch refusable.** A clip is
	// authored against one rig: its channels name joint slots, and those slots
	// mean nothing on a different skeleton. Without `Rig` the failure is a
	// character folding itself inside out with nothing in the file to explain it;
	// with it, the handler refuses at the point the track is created.
	//
	// @since v0.19
	struct AnimationClip {
		// Which clip. Roblox's `AnimationId`, and a `core::Name` for rule 4's
		// reason: it crosses a save file, a wire and a manifest.
		core::Name Asset;

		// Which `Skeleton::Rig` this clip's channels were authored against. An
		// invalid name means the author has not said, which plays anywhere.
		core::Name Rig;
	};

	// What drives a rig's pose, on an `Animator` instance.
	//
	// **Roblox puts it under a `Humanoid` and this one may sit anywhere**, which
	// is the modularity the roadmap asks for: an animator over a door hinge, a
	// flag or a crane arm is the same machinery, and requiring a humanoid to
	// reach it would be requiring a health bar to open a door.
	//
	// @since v0.19
	struct Animator {
		// The entity carrying the `Skeleton` this poses, or a null entity for the
		// nearest rig at or above its own parent.
		//
		// **A handle rather than a search every frame**, and null rather than a
		// second component to mean "the obvious one": the obvious one is what an
		// author gets by parenting the animator into the model, and a handle is
		// what a game with two rigs on one model needs.
		//
		// **Widest first**, so the object representation a snapshot writes holds
		// no padding between this and the fields below it.
		ecs::Entity Rig;

		// How much of a clip's root channel is turned into movement of the rig
		// itself rather than of its root joint, 0 to 1.
		//
		// Roblox's `Animator.RootMotionWeight`. Zero is animation in place, which
		// is what a character driven by a controller wants; one is animation that
		// drives the body, which is what a cutscene wants.
		float RootMotionWeight = 0.0f;

		// Whether the root channel moves the rig at all.
		//
		// Separate from the weight for `CameraController::Enabled`'s reason: a
		// game that switches root motion off and back on keeps whatever blend it
		// had chosen, where a weight driven to zero has lost it.
		bool RootMotion = false;

		// Whether the pose may be evaluated less often than once a tick when
		// nobody is close enough to see it.
		//
		// Roblox's `EvaluationThrottled`. Stored rather than decided by the
		// handler, because it is a per-rig authoring decision: a boss animation
		// that stutters at distance is a bug and a background crowd that does not
		// is a waste.
		bool EvaluationThrottled = true;

		// Explicit padding, for the reason `Components.hpp` opens with.
		uint8_t Reserved[2] = {};
	};

	// One clip being played on one animator, on an `AnimationTrack` instance.
	//
	// **The play head is stored and the pose is not.** What a track holds is
	// where in the clip it is and how loudly it is speaking; what that produces
	// is `Bone::Transform`, which is a different row and is derived. Storing a
	// sampled pose here would be the second copy of a derived fact this module
	// refuses everywhere else.
	//
	// @since v0.19
	struct AnimationTrack {
		// The `Animation` instance carrying the clip this plays, or a null entity
		// for a track nothing has been loaded into yet.
		ecs::Entity Clip;

		// How far into the clip the play head is, in seconds.
		//
		// **Seconds and not a fraction**, because a track's speed changes and a
		// fraction of an unknown length is not a position anybody can reason
		// about. `Length` is the clip's and is not stored here for that reason:
		// it is a fact about content, exactly as a mesh's triangle count is, and
		// `MeshCatalogue`'s argument applies unchanged.
		float TimePosition = 0.0f;

		// How fast the play head advances, as a multiple of real time. Negative
		// plays backwards.
		float Speed = 1.0f;

		// How much of this track reaches the pose right now, 0 to 1.
		float Weight = 0.0f;

		// What `Weight` is moving towards.
		//
		// **Two fields, because a fade is the ordinary way a track starts and
		// stops.** Collapsing them would make every fade a script's job and every
		// game's fade slightly different.
		float WeightTarget = 1.0f;

		// How long the current fade takes, in seconds. Zero snaps.
		float FadeTime = 0.1f;

		// Which layer this track's channels land on.
		AnimationPriority Priority = AnimationPriority::Core;

		// Whether the play head wraps at the end of the clip.
		bool Looped = false;

		// Whether the play head is advancing at all.
		//
		// **A flag rather than the track's existence**, which is `Sound.Playing`'s
		// arrangement and is here for its reason: a game that stops a track and
		// starts it again wants the same row back, and destroying and recreating
		// an instance per stop is a structural change per stop on the wire.
		bool Playing = false;

		// Explicit padding, for the reason `Components.hpp` opens with.
		uint8_t Reserved[1] = {};
	};

	// The animator that should pose a rig, resolved from the tree.
	//
	// **A function rather than a handle on the `Skeleton`**, because the tree
	// already says it: an animator names its rig or sits inside the model that
	// holds one, and a back-pointer would be the second copy that goes stale the
	// first time somebody reparents one.
	//
	// **Searches the rig's own subtree and its parent's, and never the world.**
	// Roblox's arrangement is an `Animator` under a `Humanoid` under the
	// character `Model`, with the skinned mesh a sibling of that humanoid, so
	// those two subtrees cover every rig anybody authors. An animator further
	// away is reached from its own side through `Animator::Rig`, which is what
	// the handle is for; scanning every animator in the world to answer that case
	// would cost a walk of the world per call.
	//
	// @param store The world.
	// @param rig   The entity carrying the `Skeleton`.
	// @return The animator posing it, or a null entity when nothing does.
	ecs::Entity AnimatorFor(const ecs::Store &store, ecs::Entity rig);

	// The rig an animator poses.
	//
	// `Animator::Rig` when it names one, and otherwise the nearest `Skeleton` at
	// or above the animator's parent - which is what parenting an animator into a
	// character model means.
	//
	// @param store    The world.
	// @param animator The animator instance.
	// @return The rig's entity, or a null entity when it reaches none.
	ecs::Entity RigFor(const ecs::Store &store, ecs::Entity animator);

	// Whether a clip may be played on a rig.
	//
	// **The one refusal this module makes about animation**, and it is here
	// rather than in the handler because both the handler and an editor's
	// "preview this clip" need the same answer, and two statements of it would
	// disagree about the permissive case.
	//
	// A clip that names no rig plays on anything, which is what an author gets
	// before anybody has said otherwise.
	//
	// @param clip     The clip.
	// @param skeleton The rig it would be played on.
	// @return `true` when the clip was authored for that rig or for none.
	bool ClipFitsRig(const AnimationClip &clip, const Skeleton &skeleton);

	// The `Animator` class id, registering the scene tree on first call.
	//
	// @return The class id.
	ecs::ClassId AnimatorClass();
}
