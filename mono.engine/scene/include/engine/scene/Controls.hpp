#pragma once

// The camera the player drives, and the character they drive it around.
//
// **The arithmetic is here and the events are not**, which is the same split
// `replication::DistancePriority` makes and for the same reason: `input` is
// `client` tier, so a camera that consumed SDL events would be a camera only a
// client could have - and the studio, a replay and a headless test all need one.
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
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Instance.hpp>

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
		// controller, and the one that ran last would win - the bug
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
		// ambiguous at the poles - so the authoritative angles are these, and the
		// frame is what they produce. That is also what makes the pitch clamp
		// below possible at all.
		core::Vector2 Angles;

		// How far back the camera sits, in metres.
		float Distance = 12.0f;

		// The closest it may come.
		//
		// **Zero, and reaching it is what switches to first person.** Roblox does
		// the same, and the alternative - a separate key for first person - makes
		// the transition a jump rather than the continuous thing a player expects.
		float MinimumDistance = 0.0f;

		// The furthest it may go.
		float MaximumDistance = 40.0f;

		// How far one notch of the wheel moves it, in metres.
		float ZoomStep = 2.0f;

		// How fast `I` and `O` move it while held, in metres per second.
		//
		// **A rate rather than a step, because a key held has no "notch" to
		// count.** `I`/`O` are the pair a player without a wheel - a laptop
		// trackpad with scroll disabled, a controller mapped to the keyboard -
		// still has, and Roblox's own remap of the same idea. Zoomed the same
		// direction the wheel forward-scrolls: `I` in, `O` out, matching the
		// mnemonic "in"/"out" rather than the letters' keyboard order.
		//
		// @since v0.18
		float KeyZoomSpeed = 8.0f;

		// How far the camera turns per pixel of mouse motion, in radians.
		float Sensitivity = 0.0035f;

		// How far above the subject's origin the camera looks.
		//
		// A head offset. Without it a first-person camera sits at the character's
		// feet, which is the first thing anybody notices.
		float HeadHeight = 1.5f;

		// How far to the side a shift-locked camera sits, in metres.
		float ShoulderOffset = 2.0f;

		// The distance `PlaceCamera` uses in place of `Distance`, or negative
		// to use `Distance` itself.
		//
		// **A second field rather than a temporary write to `Distance`, and
		// that is the whole of why a poppercam needs one.** `Distance` is the
		// player's own setting - what the wheel and `I`/`O` change - and a
		// pass that shortened it to clear a wall would have nothing to
		// lengthen it back to once the wall was gone, short of remembering the
		// old value somewhere else. This *is* somewhere else: a client-tier
		// pass with a raycast, which `scene` cannot have on its own, writes
		// this every frame it runs and clears it to negative the moment
		// nothing is in the way - `Distance` never moves either way.
		//
		// **Negative rather than a bool beside it**, because the two states -
		// "clear" and "the distance is exactly zero" - are both real: a
		// poppercam that has pushed the eye all the way to the subject writes
		// zero here, and it has to read back as zero rather than as clear.
		//
		// @since v0.18
		float OccludedDistance = -1.0f;

		// Which of the subject's portal crossings this camera has already
		// turned for.
		//
		// **The consumer's half of `scene::PortalTransit`**, and it is on the
		// controller rather than beside the counter because it is a fact about
		// this *viewer*: two clients watching the same character each have to
		// turn their own eye once, and a flag on the body would let the first
		// one to read it clear the news for everybody else. Zero is "seen
		// nothing", which is what a camera holds before its subject has been
		// anywhere.
		//
		// @since v0.15
		uint32_t SeenTransit = 0;

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
		//
		// **Six rather than two**, because `Subject` is eight-byte aligned and
		// so the whole struct is: two left four bytes the compiler filled and
		// nobody initialised, and those four went into every save and every
		// delta. `engine.ecs.invariants` is what says so now, and
		// `engine.scene.registration` is what asks it about this module.
		uint8_t Reserved[6] = {};
	};

	// What makes a part a character somebody drives.
	//
	// **Deliberately small.** `ROADMAP.md` says "basic character controls" and the
	// instruction beside it says to spend the time on the particles instead, so
	// this is a capsule, a speed, a jump, a life and whether the ground is under
	// it - and not a state machine, an animator, a ladder or a swim.
	//
	// **The life arrived at v0.15 and it is what "dead" means.** Until then a
	// character died by being destroyed and `UpdateRespawns` measured the delay
	// from the tick a player was first seen with no body, which is Roblox's
	// *delay* with Roblox's *trigger* missing. `scene::TakeDamage` and
	// `scene::IsDead` are the two functions that gap needed;
	// `Characters.hpp` carries what happens at zero.
	//
	// @since v0.10
	struct Humanoid {
		// The part this steers, or a null entity when it steers the entity it
		// is on.
		//
		// **Roblox puts a `Humanoid` beside the parts rather than on one**, so
		// the thing that holds the walk speed and the thing the solver pushes
		// are two different rows - see `Characters.hpp`. A null handle means the
		// older arrangement, where a single part carries both, and that is what
		// every scripted NPC in the repository and every case in
		// `tests/Controls.cpp` still is.
		//
		// **Widest first**, so the object representation a snapshot writes holds
		// no padding between this and the vector below it.
		ecs::Entity RootPart;

		// Which way the character has been told to go, in world space.
		//
		// **A direction and not a velocity**, which is Roblox's `MoveDirection`
		// and is the right split: how fast it goes is `WalkSpeed`, and a
		// controller that wrote a velocity would make every speed change a change
		// to whatever last wrote the direction.
		core::Vector3 MoveDirection;

		// How fast it walks, in metres per second.
		//
		// **Roblox's number is 16 and it is 16 *studs*.** A stud is about 0.28 m,
		// so the same feel in this engine's units is nearer 4.5 - see `JumpSpeed`
		// below for why the two are stated together and why the figure here is
		// not simply converted. Left where a run of the examples put it, because
		// what a character *feels* like is an authoring decision and not a units
		// bug the way the jump was.
		float WalkSpeed = 16.0f;

		// How fast it leaves the ground when it jumps, in metres per second.
		//
		// **50 was Roblox's `JumpPower`, and Roblox's gravity is 196.2.** That
		// pairing gives a jump about 6 studs high. `scene::Gravity` deliberately
		// does *not* copy 196.2 - this engine measures a part sized `2` as two
		// metres, so it uses Earth's 9.81 and says so at length - and 50 m/s
		// against 9.81 m/s² is an apex of a hundred and thirty metres, reached
		// five seconds after take-off.
		//
		// That is the bug somebody reports as "jump freezes in mid-air": the
		// character is not stuck, it is coasting through the top of an arc so
		// slow and so tall that a second of it looks like a hang, and the ground
		// it left is off the bottom of the screen. Nothing in the solver, the
		// sleep heuristic or the ground query is involved.
		//
		// The figure is chosen against the height rather than converted: half of
		// `CHARACTER_HEIGHT` is a jump you can read, and `v = sqrt(2 g h)` with
		// h = 2.5 m gives 7.
		float JumpSpeed = 7.0f;

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

		// How much life is left. Zero is dead.
		//
		// **The authority's number, and a client's is a copy of it.**
		// `scene.Humanoid` replicates by the ordinary `scene.` prefix rule, so a
		// client reads this off its own store to draw a bar - and
		// `ecs::Store::SetProperty` refuses a write in a replica, which is where
		// this engine already answers "who owns a row" for every property there
		// is. `scene::TakeDamage` makes the same refusal for the C++ door, so
		// there is one answer rather than a script rule and a code rule.
		float Health = 100.0f;

		// The ceiling `Health` is clamped against.
		//
		// Roblox's hundred, so a game that sets neither gets a bar that starts
		// full and a hit that means a readable fraction of it.
		float MaxHealth = 100.0f;

		// Whether something is under it, as of the last step.
		bool Grounded = false;

		// Whether it has been told to jump. Cleared once the jump is applied.
		//
		// **A request rather than an act**, because the tick that reads input and
		// the tick that moves a body are not the same one - a controller that
		// applied the impulse where the key was read would apply it outside the
		// physics step.
		bool JumpRequested = false;

		// Whether the controller may move it.
		bool Enabled = true;

		// Whether the body turns to face where it is walking.
		//
		// **Roblox's field, and Roblox's default.** A humanoid with no facing
		// logic at all is not "facing forward" - it is frozen at whatever
		// orientation it spawned with, because nothing but this flag decides
		// what `StepCharacters` does with `Motion::Angular.Y`. Off is a real
		// case: a vehicle seat or a turret drives its own facing and would
		// otherwise fight this every tick.
		//
		// @since v0.18
		bool AutoRotate = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		//
		// **Four bytes rather than one, and that is what the two health floats
		// paid for.** An `ecs::Entity` aligns to eight, so this object rounds up
		// to a multiple of eight whatever is in it - and at one byte the round-up
		// was four *unnamed* bytes on the end, which a snapshot writes and nobody
		// initialises. Widening the named run to reach the boundary exactly is
		// how the rest of this module states the same rule, and `just
		// determinism` is what checks it: two runs of one scene writing
		// different bytes is what uninitialised padding looks like from outside.
		//
		// **`AutoRotate` took one of the five this used to be**, matching the
		// shape `Visual::Reserved` already went through as fields arrived.
		uint8_t Reserved[4] = {};
	};

	// Whether a humanoid has run out of life.
	//
	// **One spelling of the comparison, because three passes ask it.**
	// `StepCharacters` refuses to drive a dead body, `UpdateRespawns` starts the
	// respawn clock from it and `TakeDamage` uses it to make a death happen once
	// - and `<= 0` written out three times is three chances for one of them to
	// become `< 0` and quietly stop agreeing with the other two.
	//
	// @param humanoid The row to ask about.
	// @return `true` when `Health` has reached zero.
	// @since v0.15
	bool IsDead(const Humanoid &humanoid);

	// Turns this frame's input into camera angles and a zoom distance.
	//
	// **Reads `InputState` and writes `CameraController`, and touches no
	// `Transform`.** Placing the camera is `PlaceCamera` below; splitting the two
	// is what lets a test drive the angles without a camera existing, and what
	// lets a replay feed recorded input through the same arithmetic.
	//
	// Does nothing when the controller is disabled or `Scriptable`.
	//
	// **Except for the portal turn, which happens first and happens anyway.**
	// See `FollowPortalTransit`: a disabled camera still belongs to a body that
	// may have gone through a hole, and a scripted one still hands its yaw to
	// `ReadMoveIntent`. Skipping it for either would leave the yaw pointing at
	// the room the player came from.
	//
	// @param store The world.
	// @return `true` when the angles or the distance moved.
	bool UpdateCameraControl(ecs::Store &store);

	// Turns the camera by however far a portal turned the body it follows.
	//
	// **The client end of `scene::PortalTransit`, and it is a separate function
	// because it is a separate machine.** `CrossPortals` maps a crossing body
	// and its velocity on whichever host simulates it; the yaw a player steers
	// by lives in `CameraController::Angles`, which is a resource on whichever
	// host is *looking*. In a studio Play or against a real server those are two
	// worlds. So the crossing is recorded on the body, replication carries it
	// across with everything else the body owns, and this reads it where the eye
	// actually is.
	//
	// What it fixes is unmistakable when it is missing: you walk forward through
	// a hole whose pair turns a corner, and on the far side the view is still
	// pointing the way you came in - ninety degrees off your own body, facing a
	// wall, with W walking you sideways because `ReadMoveIntent` is relative to
	// this yaw.
	//
	// **Once per crossing per viewer.** `CameraController::SeenTransit` is the
	// counter this has already acted on, so a client that misses the frame a
	// delta lands still turns on the next one, and one that runs twice in a
	// frame does not turn twice.
	//
	// Called by `UpdateCameraControl` before anything else it does, so every
	// host that installs the camera pass gets it without a second entry in the
	// schedule.
	//
	// @param store The world.
	// @return `true` when the yaw was turned.
	// @since v0.15
	bool FollowPortalTransit(ecs::Store &store);

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

	// What the player is asking their character to do this frame.
	//
	// @since v0.14
	struct MoveIntent {
		// Where they want to go, in world space, already normalised.
		core::Vector3 Direction;

		// Whether the jump key went down this frame.
		bool Jump = false;
	};

	// Reads the keyboard into an intent, and applies it to nothing.
	//
	// **Split out of `UpdateCharacterControl` because a connected client needs
	// the intent without the application.** A client does not simulate its own
	// character - the host does, and the client sends what it wants through
	// `game::MoveInput` - so the arithmetic that turns W into "away from the
	// camera" has to be reachable on its own. The alternative was a second copy
	// of it in `mono.client`, which is where the diagonal-speed bug would come
	// back.
	//
	// Const, because it writes nothing at all.
	//
	// @param store The world.
	// @return The intent. Zero direction and no jump when the window is not
	//         focused or the world has no `InputState`.
	MoveIntent ReadMoveIntent(const ecs::Store &store);

	// What the player is aiming at, and whether they asked to act on it.
	//
	// @since v0.15
	struct AimIntent {
		// Where the eye is and which way it is pointing, in world space.
		core::Ray Ray;

		// Whether the primary button went down since a tick last consumed the
		// edges.
		//
		// **A tap and not a hold**, which is `MoveIntent::Jump`'s distinction
		// and matters more here: a hold would fire once per tick for as long as
		// the button is down, and a client cannot be trusted with the rate.
		bool Fired = false;

		// Whether there was a camera to aim from at all.
		//
		// **Separate from `Fired`, because they fail differently.** A world with
		// no live camera and a player who did not click both produce "no shot",
		// and only one of them is a bug.
		bool Aimed = false;
	};

	// Reads the live camera and the pointer into an aim, and applies it to
	// nothing.
	//
	// **Here rather than in `mono.client`, for `ReadMoveIntent`'s reason.** A
	// client sends where it aimed and never what it hit - `Server::ApplyInputs`
	// states that division - so the arithmetic that turns a camera into a ray
	// has to be reachable without the thing that acts on it. The studio's
	// `PlayLink` needs the same ray with no socket in the middle, and two
	// copies of "which way is the player looking" is the shape that drifts and
	// drifts first in the editor.
	//
	// **The ray is the camera's and not the character's.** A shot starts at the
	// eye because that is what the player aimed with; a game that wants it to
	// start at a muzzle offsets it, and it is the *direction* that must not be
	// re-derived.
	//
	// Const, because it writes nothing at all. The consuming of the tap is the
	// caller's, through `InputState::ConsumeTaps`, exactly as it is for a jump.
	//
	// @param store The world.
	// @return The aim. `Aimed` is false when there is no live camera or no
	//         `InputState`, and `Fired` is false whenever the window is not
	//         focused.
	AimIntent ReadAimIntent(const ecs::Store &store);

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
	// either phase - it only writes an intent - but this must not.
	//
	// **A dead humanoid is skipped, which is what makes a body stay where it
	// fell.** Roblox leaves the corpse for `Player.RespawnTime` and then replaces
	// it; the gate is here rather than on the intent because this is the half
	// that reaches a `Motion` - a dead character keeps whatever momentum killed
	// it and then falls, instead of walking on with the last direction its owner
	// pressed.
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
	// `scene::CameraClass` - it lives here rather than in `Part.hpp` because a
	// humanoid is the thing this file steers, and `StepCharacters` is the
	// reason the class exists at all.
	//
	// @return The class id.
	ecs::ClassId HumanoidClass();
}
