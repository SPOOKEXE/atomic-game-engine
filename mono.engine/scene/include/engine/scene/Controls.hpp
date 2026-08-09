#pragma once

// The camera the player drives, and the character they drive it around.
//
// **The arithmetic is here and the events are not**, which is the same split
// `replication::DistancePriority` makes and for the same reason: `input` is
// `client` tier, so a camera that consumed SDL events would be a camera only a
// client could have — and the studio, a replay and a headless test all need one.
// So the client writes `scene::InputState` and this reads it, which makes every
// line below runnable in a suite.
//
// **Roblox's vocabulary, and its defaults.** `CameraType.Custom` orbits at a zoom
// distance; `LockFirstPerson` is what full zoom-in becomes; shift-lock is a third
// mode rather than a flag on the first, because it is the one where the character
// turns with the camera instead of with its own velocity.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// How the live camera is driven.
	//
	// @since v0.10
	enum class CameraMode : uint8_t {
		// Orbits the subject at `Distance`, turned by the mouse. Roblox's
		// `Custom`, and what a player gets when nobody says otherwise.
		Classic = 0,

		// Sits at the subject's head and turns with the mouse. What full zoom-in
		// becomes, and what a shooter wants.
		//
		// **A mode rather than `Distance == 0`**, because the character's facing
		// rule differs: in first person the body turns with the camera, and a
		// classic camera at zero distance would leave it turning with its
		// velocity and looking wrong from the inside.
		LockFirstPerson = 1,

		// Third person, and the character faces where the camera faces.
		//
		// Roblox's shift-lock. The camera is over a shoulder rather than centred,
		// which is what makes it a distinct offset rather than only a facing rule.
		ShiftLock = 2,

		// Nothing here moves it. A script owns the `CFrame` outright.
		//
		// **What `workspace.CurrentCamera.CFrame = ...` needs to survive**: a
		// cutscene that set the camera every frame would otherwise fight this
		// controller, and the one that ran last would win — the bug
		// `client::BuildScriptedWorld` already records for `MoveCamera`.
		Scriptable = 3,
	};

	// How the player's camera is placed, and what it is looking at.
	//
	// A resource: there is one player at a keyboard. Which *camera* is live is
	// `ActiveCamera`; this is how it is being driven.
	//
	// @since v0.10
	struct CameraController {
		// What the camera orbits or sits in. A null entity leaves the camera
		// alone, which is what an editor's free-fly camera is.
		ecs::Entity Subject;

		// Where the camera is looking, in radians. X is pitch, Y is yaw.
		//
		// **Kept here rather than derived from the camera's `CFrame`.** A
		// `CFrame` is a quaternion and reading pitch back out of one is
		// ambiguous at the poles — so the authoritative angles are these, and the
		// frame is what they produce. That is also what makes the pitch clamp
		// below possible at all.
		core::Vector2 Angles;

		// How far back the camera sits, in metres.
		float Distance = 12.0f;

		// The closest it may come.
		//
		// **Zero, and reaching it is what switches to first person.** Roblox does
		// the same, and the alternative — a separate key for first person — makes
		// the transition a jump rather than the continuous thing a player expects.
		float MinimumDistance = 0.0f;

		// The furthest it may go.
		float MaximumDistance = 40.0f;

		// How far one notch of the wheel moves it, in metres.
		float ZoomStep = 2.0f;

		// How far the camera turns per pixel of mouse motion, in radians.
		float Sensitivity = 0.0035f;

		// How far above the subject's origin the camera looks.
		//
		// A head offset. Without it a first-person camera sits at the character's
		// feet, which is the first thing anybody notices.
		float HeadHeight = 1.5f;

		// How far to the side a shift-locked camera sits, in metres.
		float ShoulderOffset = 2.0f;

		// How it is driven.
		CameraMode Mode = CameraMode::Classic;

		// Whether the player may turn the camera at all.
		//
		// **The "enable/disable" half of `ROADMAP.md`'s controls item**, and it is
		// separate from `Scriptable`: a disabled camera keeps its mode and stops
		// responding, so re-enabling it puts the player back where they were. A
		// script that switched to `Scriptable` and back would have lost the
		// angles.
		bool Enabled = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// What makes a part a character somebody drives.
	//
	// **Deliberately small.** `ROADMAP.md` says "basic character controls" and the
	// instruction beside it says to spend the time on the particles instead, so
	// this is a capsule, a speed, a jump and whether the ground is under it —
	// and not a state machine, an animator, a ladder or a swim.
	//
	// @since v0.10
	struct Humanoid {
		// Which way the character has been told to go, in world space.
		//
		// **A direction and not a velocity**, which is Roblox's `MoveDirection`
		// and is the right split: how fast it goes is `WalkSpeed`, and a
		// controller that wrote a velocity would make every speed change a change
		// to whatever last wrote the direction.
		core::Vector3 MoveDirection;

		// How fast it walks, in metres per second.
		float WalkSpeed = 16.0f;

		// How fast it leaves the ground when it jumps.
		float JumpSpeed = 50.0f;

		// How tall the capsule is, in metres.
		float Height = 5.0f;

		// How wide it is.
		float Radius = 1.0f;

		// How far below the feet counts as standing on something.
		//
		// **Not zero, and this is the number that decides whether walking down a
		// slope feels right.** A ray exactly to the feet loses the ground for a
		// frame every time the character steps off a lip, so it falls and lands
		// repeatedly on a surface it never actually left. A tenth of a metre of
		// tolerance is what turns that into walking.
		float GroundTolerance = 0.15f;

		// Whether something is under it, as of the last step.
		bool Grounded = false;

		// Whether it has been told to jump. Cleared once the jump is applied.
		//
		// **A request rather than an act**, because the tick that reads input and
		// the tick that moves a body are not the same one — a controller that
		// applied the impulse where the key was read would apply it outside the
		// physics step.
		bool JumpRequested = false;

		// Whether the controller may move it.
		bool Enabled = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved = 0;
	};

	// Turns this frame's input into camera angles and a zoom distance.
	//
	// **Reads `InputState` and writes `CameraController`, and touches no
	// `Transform`.** Placing the camera is `PlaceCamera` below; splitting the two
	// is what lets a test drive the angles without a camera existing, and what
	// lets a replay feed recorded input through the same arithmetic.
	//
	// Does nothing when the controller is disabled or `Scriptable`.
	//
	// @param store The world.
	// @return `true` when the angles or the distance moved.
	bool UpdateCameraControl(ecs::Store &store);

	// Places the live camera from the controller's angles.
	//
	// **Separate from `UpdateCameraControl` for that function's reason**, and with
	// one more: a cutscene may want the placement without the input, and an editor
	// wants neither.
	//
	// A controller with no subject leaves the camera where it is, which is what
	// makes a free-fly camera possible without a fourth mode.
	//
	// @param store The world.
	// @return `true` when a camera was placed.
	bool PlaceCamera(ecs::Store &store);

	// Turns this frame's input into a `Humanoid::MoveDirection`.
	//
	// **Relative to the camera's yaw, which is why it is here and not in
	// `physics`.** W means "away from the camera" and not "along +Z", and the
	// camera's yaw is `CameraController::Angles`. A controller that did not know
	// about the camera would make every game write this itself.
	//
	// @param store The world.
	// @return How many humanoids were driven.
	size_t UpdateCharacterControl(ecs::Store &store);

	// Applies each humanoid's move direction and jump to its body.
	//
	// **In the simulation and against the fixed tick**, because it writes a
	// `Motion` the physics step integrates. `UpdateCharacterControl` may run in
	// either phase — it only writes an intent — but this must not.
	//
	// **Ground detection is a downward ray and the query is the caller's.** This
	// module may not link `physics`, so `Grounded` is read rather than computed:
	// whoever installs these systems runs the raycast and writes the flag, exactly
	// as `replication::DistancePriority::Blocked` takes its occlusion from
	// outside. `client::InstallControls` is that caller.
	//
	// @param store The world.
	// @param delta The tick's own delta, in seconds.
	// @return How many humanoids moved.
	size_t StepCharacters(ecs::Store &store, float delta);

	// The `Humanoid` class id, registering the scene tree on first call.
	//
	// The third of the tree's class accessors, beside `scene::PartClass` and
	// `scene::CameraClass` — it lives here rather than in `Part.hpp` because a
	// humanoid is the thing this file steers, and `StepCharacters` is the
	// reason the class exists at all.
	//
	// @return The class id.
	ecs::ClassId HumanoidClass();
}
