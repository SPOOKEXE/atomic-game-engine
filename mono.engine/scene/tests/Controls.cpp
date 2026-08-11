// The camera and character controllers, which had no suite at all.
//
// **Every line of `Controls.cpp` was reachable only by running the client**, so
// the arithmetic that decides where a player's camera points and how fast their
// character walks was checked by looking at it. The cases below are the ones a
// player would find first: the pitch clamp that stops the view flipping over,
// the zoom that becomes first person, the diagonal that must not be faster, and
// the jump that must not fire twice.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

TEST_SUITE_ID("engine.scene.controls")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::ActiveCamera;
using engine::scene::CameraController;
using engine::scene::CameraMode;
using engine::scene::Humanoid;
using engine::scene::InputState;
using engine::scene::KeyCode;
using engine::scene::Motion;
using engine::scene::MouseBehavior;
using engine::scene::MouseButton;
using engine::scene::PlaceCamera;
using engine::scene::StepCharacters;
using engine::scene::Transform;
using engine::scene::UpdateCameraControl;
using engine::scene::UpdateCharacterControl;

namespace {
	// A world with the scene classes registered and the two control resources
	// present, which is the state `client::InstallControls` leaves behind.
	struct World {
		Store Store_{"controls-test"};

		World() {
			engine::scene::RegisterSceneClasses();
			Store_.SetResource(InputState{});
			Store_.SetResource(CameraController{});
		}

		InputState &Input() {
			return *Store_.ResourceMutable<InputState>();
		}

		CameraController &Camera() {
			return *Store_.ResourceMutable<CameraController>();
		}
	};

	// Turning needs the right button held unless the pointer is locked, so a
	// case that only set a delta would be testing nothing.
	void HoldTurn(InputState &input) {
		input.Buttons = static_cast<uint8_t>(1u << static_cast<uint8_t>(MouseButton::Right));
	}

	constexpr float PITCH_LIMIT = std::numbers::pi_v<float> * 0.5f - 0.01f;
}

TEST_CASE("the mouse turns the camera only while the button is held", "[scene][controls]") {
	World world;
	world.Input().MouseDelta = {100.0f, 0.0f};

	// **No button, no turn.** A camera that turned on every pixel of motion
	// makes clicking anything impossible.
	CHECK_FALSE(UpdateCameraControl(world.Store_));
	CHECK(world.Camera().Angles.Y == Approx(0.0f));

	HoldTurn(world.Input());
	CHECK(UpdateCameraControl(world.Store_));
	CHECK(world.Camera().Angles.Y != Approx(0.0f));
}

TEST_CASE("a locked pointer turns without a button", "[scene][controls]") {
	// The locked case is the one that matters: under `LockCenter` the pointer
	// does not move, so requiring a button would make the camera unturnable in
	// exactly the mode built for turning it.
	World world;
	world.Input().Behaviour = MouseBehavior::LockCenter;
	world.Input().MouseDelta = {0.0f, 25.0f};

	CHECK(UpdateCameraControl(world.Store_));
	CHECK(world.Camera().Angles.X != Approx(0.0f));
}

TEST_CASE("pitch clamps short of straight up and straight down", "[scene][controls]") {
	// **The gimbal-lock guard.** At exactly a right angle the look direction is
	// parallel to world up and `LookAt` cannot choose a roll, so the view spins
	// about its own axis — which reads as the camera flipping over.
	World world;
	HoldTurn(world.Input());

	world.Input().MouseDelta = {0.0f, -100000.0f};
	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Angles.X <= PITCH_LIMIT);
	CHECK(world.Camera().Angles.X == Approx(PITCH_LIMIT));

	world.Input().MouseDelta = {0.0f, 200000.0f};
	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Angles.X >= -PITCH_LIMIT);
	CHECK(world.Camera().Angles.X == Approx(-PITCH_LIMIT));
}

TEST_CASE("yaw is not clamped, because turning around is not an error", "[scene][controls]") {
	World world;
	HoldTurn(world.Input());
	world.Input().MouseDelta = {100000.0f, 0.0f};

	UpdateCameraControl(world.Store_);
	CHECK(std::abs(world.Camera().Angles.Y) > PITCH_LIMIT);
}

TEST_CASE("the wheel zooms between the two distances", "[scene][controls]") {
	World world;
	world.Camera().Distance = 12.0f;
	world.Camera().MaximumDistance = 40.0f;

	world.Input().WheelDelta = -1000.0f;
	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Distance == Approx(40.0f));

	world.Input().WheelDelta = 1000.0f;
	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Distance == Approx(world.Camera().MinimumDistance));
}

TEST_CASE("zooming all the way in is first person, and back out is not", "[scene][controls]") {
	// Roblox's behaviour, and the reason it is a mode rather than a key: the
	// transition is continuous, so a player scrolls in until the character
	// disappears.
	World world;
	world.Input().WheelDelta = 1000.0f;
	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Mode == CameraMode::LockFirstPerson);

	world.Input().WheelDelta = -1000.0f;
	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Mode == CameraMode::Classic);
}

TEST_CASE("shift-lock survives the wheel", "[scene][controls]") {
	// A mode a script or a keybinding chose, and letting the wheel drop out of
	// it would make the shoulder camera impossible to zoom.
	World world;
	world.Camera().Mode = CameraMode::ShiftLock;
	world.Input().WheelDelta = 1000.0f;

	UpdateCameraControl(world.Store_);
	CHECK(world.Camera().Mode == CameraMode::ShiftLock);
}

TEST_CASE("a scriptable camera is left alone even when enabled", "[scene][controls]") {
	// **`Scriptable` is checked before `Enabled`**, because a script that took
	// the camera should keep it even if something re-enables player control.
	World world;
	world.Camera().Mode = CameraMode::Scriptable;
	world.Camera().Enabled = true;
	HoldTurn(world.Input());
	world.Input().MouseDelta = {100.0f, 100.0f};

	CHECK_FALSE(UpdateCameraControl(world.Store_));
	CHECK(world.Camera().Angles.X == Approx(0.0f));
	CHECK(world.Camera().Angles.Y == Approx(0.0f));
}

TEST_CASE("a disabled camera keeps its angles for when it comes back", "[scene][controls]") {
	World world;
	world.Camera().Angles = {0.25f, 0.5f};
	world.Camera().Enabled = false;
	HoldTurn(world.Input());
	world.Input().MouseDelta = {100.0f, 100.0f};

	CHECK_FALSE(UpdateCameraControl(world.Store_));
	CHECK(world.Camera().Angles.X == Approx(0.25f));
	CHECK(world.Camera().Angles.Y == Approx(0.5f));
}

TEST_CASE("a world with no controller reports nothing rather than crashing", "[scene][controls]") {
	// The empty case: a world nobody installed controls into still ticks.
	Store bare{"bare"};
	engine::scene::RegisterSceneClasses();
	CHECK_FALSE(UpdateCameraControl(bare));
	CHECK_FALSE(PlaceCamera(bare));
	CHECK(UpdateCharacterControl(bare) == 0);
	CHECK(StepCharacters(bare, 1.0f / 60.0f) == 0);
}

TEST_CASE("first person puts the eye at the head and third person behind it", "[scene][controls]") {
	World world;
	const Entity subject = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	world.Store_.Set(subject, Transform{CFrame{Vector3{0.0f, 0.0f, 0.0f}}});

	const Entity eye = world.Store_.CreateInstance(engine::scene::CameraClass(), "Eye");
	world.Store_.Set(eye, Transform{});
	world.Store_.SetResource(ActiveCamera{eye});

	world.Camera().Subject = subject;
	world.Camera().Mode = CameraMode::LockFirstPerson;
	world.Camera().HeadHeight = 1.5f;

	REQUIRE(PlaceCamera(world.Store_));
	const Vector3 first = world.Store_.Get<Transform>(eye)->Frame.Position;
	CHECK(first.Y == Approx(1.5f));

	// Third person sits back along the look direction by exactly the distance.
	world.Camera().Mode = CameraMode::Classic;
	world.Camera().Distance = 10.0f;
	REQUIRE(PlaceCamera(world.Store_));
	const Vector3 third = world.Store_.Get<Transform>(eye)->Frame.Position;
	CHECK((third - Vector3{0.0f, 1.5f, 0.0f}).Magnitude() == Approx(10.0f));
}

TEST_CASE("a camera with no subject is left where it is", "[scene][controls]") {
	// No subject is a free camera, which is what an editor has — moving it to
	// the origin would yank an author's viewpoint away.
	World world;
	const Entity eye = world.Store_.CreateInstance(engine::scene::CameraClass(), "Eye");
	world.Store_.Set(eye, Transform{CFrame{Vector3{5.0f, 6.0f, 7.0f}}});
	world.Store_.SetResource(ActiveCamera{eye});

	CHECK_FALSE(PlaceCamera(world.Store_));
	CHECK(world.Store_.Get<Transform>(eye)->Frame.Position.X == Approx(5.0f));
}

TEST_CASE("a diagonal is not faster than a straight line", "[scene][controls]") {
	// The oldest movement bug there is: two keys held gives a vector of length
	// root two, and using it directly makes running diagonally 40% quicker.
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	world.Store_.Set(character, Humanoid{});

	world.Input().Down.Set(KeyCode::W, true);
	REQUIRE(UpdateCharacterControl(world.Store_) == 1);
	const float straight = world.Store_.Get<Humanoid>(character)->MoveDirection.Magnitude();

	world.Input().Down.Set(KeyCode::D, true);
	REQUIRE(UpdateCharacterControl(world.Store_) == 1);
	const float diagonal = world.Store_.Get<Humanoid>(character)->MoveDirection.Magnitude();

	CHECK(straight == Approx(1.0f));
	CHECK(diagonal == Approx(1.0f));
}

TEST_CASE("opposed keys cancel to a standstill rather than a NaN", "[scene][controls]") {
	// The guarded normalise: W and S together is a zero vector, and dividing it
	// by its own magnitude is the NaN that reaches physics three calls later.
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	world.Store_.Set(character, Humanoid{});

	world.Input().Down.Set(KeyCode::W, true);
	world.Input().Down.Set(KeyCode::S, true);
	REQUIRE(UpdateCharacterControl(world.Store_) == 1);

	const Vector3 direction = world.Store_.Get<Humanoid>(character)->MoveDirection;
	CHECK(direction.X == Approx(0.0f));
	CHECK(direction.Y == Approx(0.0f));
	CHECK(direction.Z == Approx(0.0f));
	CHECK_FALSE(std::isnan(direction.X));
}

TEST_CASE("an unfocused window walks nobody", "[scene][controls]") {
	// Alt-tabbing away while holding W must not leave a character walking for
	// ever, and this is the second belt — the client also clears the keys, and
	// a recording replayed into an unfocused world should behave the same way.
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	world.Store_.Set(character, Humanoid{});

	world.Input().Down.Set(KeyCode::W, true);
	world.Input().Focused = false;
	REQUIRE(UpdateCharacterControl(world.Store_) == 1);
	CHECK(world.Store_.Get<Humanoid>(character)->MoveDirection.Magnitude() == Approx(0.0f));

	world.Input().Down.Set(KeyCode::Space, true);
	UpdateCharacterControl(world.Store_);
	CHECK_FALSE(world.Store_.Get<Humanoid>(character)->JumpRequested);
}

TEST_CASE("a disabled humanoid is not driven", "[scene][controls]") {
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	Humanoid off;
	off.Enabled = false;
	world.Store_.Set(character, off);

	world.Input().Down.Set(KeyCode::W, true);
	CHECK(UpdateCharacterControl(world.Store_) == 0);
	CHECK(world.Store_.Get<Humanoid>(character)->MoveDirection.Magnitude() == Approx(0.0f));
}

TEST_CASE("the step replaces horizontal velocity and keeps vertical", "[scene][controls]") {
	// What makes a character controller a controller rather than a body: adding
	// a force leaves momentum from the last frame and makes stopping take a
	// second, and replacing Y as well cancels gravity and the fall.
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	Humanoid humanoid;
	humanoid.MoveDirection = Vector3{1.0f, 0.0f, 0.0f};
	humanoid.WalkSpeed = 16.0f;
	world.Store_.Set(character, humanoid);
	world.Store_.Set(character, Motion{Vector3{-99.0f, -25.0f, -99.0f}, Vector3{}});

	REQUIRE(StepCharacters(world.Store_, 1.0f / 60.0f) == 1);
	const Motion *motion = world.Store_.Get<Motion>(character);
	CHECK(motion->Linear.X == Approx(16.0f));
	CHECK(motion->Linear.Z == Approx(0.0f));
	CHECK(motion->Linear.Y == Approx(-25.0f));
}

TEST_CASE("a jump needs the ground and is spent once", "[scene][controls]") {
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	Humanoid humanoid;
	humanoid.JumpSpeed = 50.0f;
	humanoid.JumpRequested = true;
	humanoid.Grounded = false;
	world.Store_.Set(character, humanoid);
	world.Store_.Set(character, Motion{});

	// Airborne: the request is cleared rather than held, so it does not fire
	// the moment the character lands — which reads as input arriving late.
	REQUIRE(StepCharacters(world.Store_, 1.0f / 60.0f) == 1);
	CHECK(world.Store_.Get<Motion>(character)->Linear.Y == Approx(0.0f));
	CHECK_FALSE(world.Store_.Get<Humanoid>(character)->JumpRequested);

	// Grounded: it launches, and leaving the ground is part of the same step so
	// a second tick cannot jump again from the same contact.
	Humanoid *live = world.Store_.GetMutable<Humanoid>(character);
	live->Grounded = true;
	live->JumpRequested = true;
	REQUIRE(StepCharacters(world.Store_, 1.0f / 60.0f) == 1);
	CHECK(world.Store_.Get<Motion>(character)->Linear.Y == Approx(50.0f));
	CHECK_FALSE(world.Store_.Get<Humanoid>(character)->Grounded);
	CHECK_FALSE(world.Store_.Get<Humanoid>(character)->JumpRequested);
}

TEST_CASE("a jump pressed between two ticks is latched, not lost", "[scene][controls]") {
	// The control pass runs in `PreSimulation` and the step in `Simulation`, so
	// a press that lands between them has to survive the gap.
	World world;
	const Entity character = world.Store_.CreateInstance(engine::scene::PartClass(), "Character");
	world.Store_.Set(character, Humanoid{});

	// **Through `LatchPresses`, which is what a writer does.** A press only
	// becomes a tap once the thing filling `Down` has recorded the edge — see
	// `InputState::Pressed`. Setting the bit alone is a frame nobody wrote.
	world.Input().Down.Set(KeyCode::Space, true);
	world.Input().LatchPresses();
	REQUIRE(UpdateCharacterControl(world.Store_) == 1);
	CHECK(world.Store_.Get<Humanoid>(character)->JumpRequested);

	// The key goes up before the step runs, and the latch still holds.
	world.Input().Previous = world.Input().Down;
	world.Input().Down.Set(KeyCode::Space, false);
	world.Input().LatchPresses();
	UpdateCharacterControl(world.Store_);
	CHECK(world.Store_.Get<Humanoid>(character)->JumpRequested);
}
