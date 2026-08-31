// The path a key press takes to a character, end to end within this tier.
//
// **`Controls.cpp`'s suite tests the arithmetic; this one tests the plumbing**,
// and the difference is where the bugs actually were. Every case in that file
// passed while a character in the studio refused to walk, because each of them
// drives `UpdateCharacterControl` on a world holding exactly one humanoid and
// exactly one keyboard - the arrangement in which nothing can be overwritten by
// anything.
//
// The two failures below are what a real host looks like instead:
//
//   * a key tapped between two ticks, which the frame-shaped edge in
//     `WasKeyPressed` cannot see and `InputState::Pressed` exists to carry, and
//   * an authority world with characters on it and no keyboard of its own,
//     where the local input pass used to write its empty direction over the one
//     a client had just sent.
//
// Both are about a *second* writer, which is why neither could appear in a
// single-writer test.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

TEST_SUITE_ID("engine.scene.moveinput")
TEST_DEPENDS("engine.scene.controls")

using Catch::Approx;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::CameraController;
using engine::scene::Character;
using engine::scene::ControllerAxis;
using engine::scene::ControllerButton;
using engine::scene::ControllerState;
using engine::scene::Humanoid;
using engine::scene::InputState;
using engine::scene::KeyCode;
using engine::scene::MoveIntent;
using engine::scene::ReadMoveIntent;
using engine::scene::UpdateCharacterControl;

namespace {
	// A world with the scene classes and both control resources, as
	// `client::InstallControls` leaves it.
	struct World {
		Store Store_{"move-input-test"};

		World() {
			engine::scene::RegisterSceneClasses();
			Store_.SetResource(InputState{});
			Store_.SetResource(ControllerState{});
			Store_.SetResource(CameraController{});
		}

		InputState &Input() {
			return *Store_.ResourceMutable<InputState>();
		}

		// A bare humanoid nobody owns - an NPC, or a scripted character in an
		// examples scene.
		Entity Npc(const char *name = "Npc") {
			const Entity row = Store_.CreateInstance(engine::scene::PartClass(), name);
			Store_.Set(row, Humanoid{});
			return row;
		}

		// A humanoid that belongs to somebody, which is what a host spawns for
		// a client that joined. The owner only has to exist; nothing here reads
		// through it.
		Entity Owned(const char *name = "Player") {
			const Entity humanoid = Npc(name);
			const Entity model = Store_.CreateInstance(engine::scene::PartClass(), "Model");
			const Entity owner = Store_.CreateInstance(engine::scene::PartClass(), "Owner");
			Store_.Set(model, Character{model, humanoid, owner});
			return humanoid;
		}

		Vector3 Direction(Entity humanoid) {
			return Store_.Get<Humanoid>(humanoid)->MoveDirection;
		}

		// One frame of a writer: roll last frame's keys under, set this
		// frame's, and keep the press edges for whichever tick reads them. The
		// same three steps `Editor::DrivePlayer` and `client::Client` both do.
		void Frame(std::initializer_list<KeyCode> down) {
			Input().Previous = Input().Down;
			Input().Down = {};
			for (const KeyCode key : down) {
				Input().Down.Set(key, true);
			}
			Input().LatchPresses();
		}
	};
}

// --- the keyboard, and the gap between a frame and a tick --------------------

TEST_CASE("a key tapped between two ticks still reaches the tick", "[scene][moveinput]") {
	// The bug both hosts had grown a private latch to hide. Frames outnumber
	// ticks, so a press and its release can land entirely inside one tick's
	// gap - `WasKeyPressed` is true on a frame nobody sampled and false by the
	// time the simulation asks.
	World world;

	world.Frame({KeyCode::Space});
	world.Frame({});

	// The frame-shaped question has already forgotten it, which is the whole
	// problem and is asserted so that a change to `WasKeyPressed` cannot make
	// this case pass for the wrong reason.
	CHECK_FALSE(world.Input().WasKeyPressed(KeyCode::Space));
	CHECK(world.Input().WasKeyTapped(KeyCode::Space));
}

TEST_CASE("a tap is consumed once and does not fire again", "[scene][moveinput]") {
	// A latch that is never cleared is a character that jumps for ever, which
	// is the failure mode on the other side of the one above.
	World world;

	world.Frame({KeyCode::Space});
	REQUIRE(world.Input().WasKeyTapped(KeyCode::Space));

	world.Input().ConsumeTaps();
	CHECK_FALSE(world.Input().WasKeyTapped(KeyCode::Space));

	// Still held, and holding is not tapping again - a jump has to be released
	// and pressed to fire twice.
	world.Frame({KeyCode::Space});
	CHECK_FALSE(world.Input().WasKeyTapped(KeyCode::Space));

	world.Frame({});
	world.Frame({KeyCode::Space});
	CHECK(world.Input().WasKeyTapped(KeyCode::Space));
}

TEST_CASE("two taps between one pair of ticks are one request", "[scene][moveinput]") {
	// Latching must not count. A player mashing space during a slow frame
	// should jump once when the tick arrives, not bank presses.
	World world;

	world.Frame({KeyCode::Space});
	world.Frame({});
	world.Frame({KeyCode::Space});
	world.Frame({});

	CHECK(world.Input().WasKeyTapped(KeyCode::Space));
	world.Input().ConsumeTaps();
	CHECK_FALSE(world.Input().WasKeyTapped(KeyCode::Space));
}

// --- the intent a keyboard produces -----------------------------------------

TEST_CASE("WASD reads as a direction relative to where the camera looks", "[scene][moveinput]") {
	// W is "away from the camera" and not "along -Z". A quarter turn has to
	// take the same key somewhere else, or the forward key stops meaning
	// forward the moment the player turns.
	World world;
	world.Frame({KeyCode::W});

	const MoveIntent ahead = ReadMoveIntent(world.Store_);
	CHECK(ahead.Direction.X == Approx(0.0f).margin(1e-5));
	CHECK(ahead.Direction.Z == Approx(-1.0f));

	world.Store_.ResourceMutable<CameraController>()->Angles.Y = std::numbers::pi_v<float> * 0.5f;

	const MoveIntent turned = ReadMoveIntent(world.Store_);
	CHECK(turned.Direction.X == Approx(-1.0f));
	CHECK(turned.Direction.Z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("the jump in an intent comes from the latch, not the frame", "[scene][moveinput]") {
	// The seam that put jump back on the shared path: `ReadMoveIntent` is
	// called once per tick, so it has to ask the tick-shaped question.
	World world;

	world.Frame({KeyCode::Space});
	world.Frame({KeyCode::W});

	const MoveIntent intent = ReadMoveIntent(world.Store_);
	CHECK(intent.Jump);
	CHECK(intent.Direction.Z == Approx(-1.0f));
}

TEST_CASE("an unfocused panel produces neither a step nor a jump", "[scene][moveinput]") {
	// A viewport that lost the pointer must let go of the character, including
	// a jump it had already latched - otherwise clicking away mid-press jumps.
	World world;
	world.Frame({KeyCode::W, KeyCode::Space});
	world.Input().Focused = false;

	const MoveIntent intent = ReadMoveIntent(world.Store_);
	CHECK(intent.Direction.Magnitude() == Approx(0.0f));
	CHECK_FALSE(intent.Jump);
}

TEST_CASE("the first connected controller drives movement and jump", "[scene][moveinput][gamepad]") {
	World world;
	auto &slot = world.Store_.ResourceMutable<ControllerState>()->Slots[0];
	slot.Connected = true;
	slot.Axes[static_cast<size_t>(ControllerAxis::LeftX)] = 0.5f;
	slot.Axes[static_cast<size_t>(ControllerAxis::LeftY)] = -1.0f;
	slot.Buttons = 1u << static_cast<uint8_t>(ControllerButton::A);
	world.Store_.ResourceMutable<ControllerState>()->LatchPresses();

	const MoveIntent intent = ReadMoveIntent(world.Store_);
	CHECK(intent.Direction.X > 0.0f);
	CHECK(intent.Direction.Z < 0.0f);
	CHECK(intent.Direction.Magnitude() == Approx(1.0f));
	CHECK(intent.Jump);
}

TEST_CASE("the controller that spoke last drives when several are connected", "[scene][moveinput][gamepad]") {
	World world;
	auto *controllers = world.Store_.ResourceMutable<ControllerState>();
	controllers->Slots[0].Connected = true;
	controllers->Slots[1].Connected = true;
	controllers->Slots[1].Axes[static_cast<size_t>(ControllerAxis::LeftX)] = -1.0f;
	controllers->Slots[1].PressedButtons = 1u << static_cast<uint8_t>(ControllerButton::A);
	world.Input().LastSource = engine::scene::InputSource::Gamepad2;

	const MoveIntent intent = ReadMoveIntent(world.Store_);
	CHECK(intent.Direction.X == Approx(-1.0f));
	CHECK(intent.Jump);
}

// --- who a keyboard is allowed to write --------------------------------------

TEST_CASE("a keyboardless authority leaves an owned character alone", "[scene][moveinput]") {
	// **The bug.** A studio playing both halves, and a dedicated server, have no
	// `LocalPlayer` and no keyboard. The local input pass took that to mean
	// "drive every humanoid" and wrote its empty direction over the one
	// `game::ApplyMoveInput` had just delivered - every tick, between the
	// message arriving and `StepCharacters` reading it.
	//
	// Jump survived because it is latched with `||` and a direction is
	// assigned, so the character jumped perfectly and would not walk a step.
	// That asymmetry is asserted here too, because it is the signature somebody
	// will recognise if this ever comes back.
	World world;
	const Entity mine = world.Owned();

	// What the host just applied on this client's behalf.
	world.Store_.GetMutable<Humanoid>(mine)->MoveDirection = Vector3{1.0f, 0.0f, 0.0f};
	world.Store_.GetMutable<Humanoid>(mine)->JumpRequested = true;

	// The authority's own keyboard, which nobody is touching.
	REQUIRE(world.Store_.Resource<engine::scene::LocalPlayer>() == nullptr);
	UpdateCharacterControl(world.Store_);

	CHECK(world.Direction(mine).X == Approx(1.0f));
	CHECK(world.Store_.Get<Humanoid>(mine)->JumpRequested);
}

TEST_CASE("a keyboardless world still drives characters nobody owns", "[scene][moveinput]") {
	// The fallback is not removed, only narrowed. A test world, an examples
	// scene and a scripted NPC have no `Players` service at all, so "the local
	// player's character" names nothing and driving what is there is the only
	// useful reading.
	World world;
	const Entity npc = world.Npc();
	const Entity mine = world.Owned();

	world.Store_.GetMutable<Humanoid>(mine)->MoveDirection = Vector3{1.0f, 0.0f, 0.0f};
	world.Frame({KeyCode::W});
	UpdateCharacterControl(world.Store_);

	// The NPC answered the keyboard; the owned character kept what its client
	// sent. One pass, two rules, and the ownership is what separates them.
	CHECK(world.Direction(npc).Z == Approx(-1.0f));
	CHECK(world.Direction(mine).X == Approx(1.0f));
}

TEST_CASE("an owned character with no owner entity is still an NPC", "[scene][moveinput]") {
	// `Character::Owner` is the test rather than the presence of a `Character`,
	// because a host spawns the component for scenery characters too. A null
	// owner means nobody is at a keyboard for it.
	World world;
	const Entity humanoid = world.Npc();
	const Entity model = world.Store_.CreateInstance(engine::scene::PartClass(), "Model");
	world.Store_.Set(model, Character{model, humanoid, NULL_ENTITY});

	world.Frame({KeyCode::W});
	UpdateCharacterControl(world.Store_);

	CHECK(world.Direction(humanoid).Z == Approx(-1.0f));
}
