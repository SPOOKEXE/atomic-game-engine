#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace engine::scene {

	namespace {
		using core::CFrame;
		using core::Vector2;
		using core::Vector3;

		// How far the camera may look up or down.
		//
		// **Just under a right angle, and the gap is the whole reason for the
		// constant.** At exactly ninety degrees the look direction is parallel to
		// world up, so the `LookAt` that builds the frame has no way to choose a
		// roll and the camera spins about its own axis — a gimbal lock that reads
		// as the view flipping over. A hundredth of a radian of clearance costs
		// nothing anybody can see and removes the case entirely.
		constexpr float PITCH_LIMIT = std::numbers::pi_v<float> * 0.5f - 0.01f;
	}

	bool FollowPortalTransit(ecs::Store &store) {
		auto *controller = store.ResourceMutable<CameraController>();
		if (controller == nullptr || controller->Subject == ecs::NULL_ENTITY) {
			return false;
		}

		const PortalTransit *went = store.Get<PortalTransit>(controller->Subject);
		if (went == nullptr || went->Serial == controller->SeenTransit) {
			return false;
		}

		// **The counter is taken whether or not the angle is used**, so a
		// crossing is never applied twice and a viewer that arrives late does
		// not owe a turn for every crossing since the world began.
		controller->SeenTransit = went->Serial;

		// **Added, not assigned.** A portal turns the world under the player
		// rather than deciding where they are looking: whatever they were
		// aiming at, they are now aiming at the same thing through the hole. Two
		// rotations about the same axis add, so for the pairs a floor and a
		// ceiling allow this is exact.
		controller->Angles.Y += went->Turn;
		return true;
	}

	bool UpdateCameraControl(ecs::Store &store) {
		// **Before the guards below, and that is deliberate.** A camera that is
		// disabled or scriptable still belongs to a body that may have just gone
		// through a hole, and its yaw is still what `ReadMoveIntent` steers by.
		// Turning it is not "camera control" in the sense the guards are about —
		// it is keeping the eye pointing at the thing it was already pointing at.
		const bool turned = FollowPortalTransit(store);

		auto *controller = store.ResourceMutable<CameraController>();
		const auto *input = store.Resource<InputState>();
		if (controller == nullptr || input == nullptr) {
			return turned;
		}

		// **`Scriptable` is checked before `Enabled`**, because they mean
		// different things and a script that took the camera should keep it even
		// if something re-enables player control.
		if (controller->Mode == CameraMode::Scriptable || !controller->Enabled) {
			return turned;
		}

		bool moved = turned;

		// **Turning needs the right button held, unless the pointer is locked.**
		// That is Roblox's rule and it is what makes a mouse usable for anything
		// else: a camera that turned on every pixel of motion would make clicking
		// a button impossible. A locked pointer has nothing else to do, so it
		// turns freely.
		const bool turning =
			input->Behaviour != MouseBehavior::Default || input->IsButtonDown(MouseButton::Right);

		if (turning && (input->MouseDelta.X != 0.0f || input->MouseDelta.Y != 0.0f)) {
			// **The delta and not the position**, which is what makes a locked
			// pointer work at all — see `InputState::MouseDelta`.
			controller->Angles.Y -= input->MouseDelta.X * controller->Sensitivity;
			controller->Angles.X -= input->MouseDelta.Y * controller->Sensitivity;
			controller->Angles.X = std::clamp(controller->Angles.X, -PITCH_LIMIT, PITCH_LIMIT);
			moved = true;
		}

		if (input->WheelDelta != 0.0f) {
			controller->Distance = std::clamp(
				controller->Distance - input->WheelDelta * controller->ZoomStep,
				controller->MinimumDistance,
				controller->MaximumDistance
			);
			moved = true;
		}

		// **Zooming all the way in *is* first person**, rather than a separate
		// key. Roblox's behaviour, and the reason is that the transition is
		// continuous: a player scrolls in until the character disappears, which is
		// what "first person" means to them.
		//
		// Shift-lock is left alone by this. It is a mode a script or a keybinding
		// chooses, and letting the wheel drop out of it would make the shoulder
		// camera impossible to zoom.
		if (controller->Mode != CameraMode::ShiftLock) {
			const CameraMode wanted = controller->Distance <= controller->MinimumDistance + 0.01f
										  ? CameraMode::LockFirstPerson
										  : CameraMode::Classic;
			if (controller->Mode != wanted) {
				controller->Mode = wanted;
				moved = true;
			}
		}

		return moved;
	}

	bool PlaceCamera(ecs::Store &store) {
		const auto *controller = store.Resource<CameraController>();
		const auto *active = store.Resource<ActiveCamera>();
		if (controller == nullptr || active == nullptr) {
			return false;
		}
		if (controller->Mode == CameraMode::Scriptable) {
			return false;
		}

		const Transform *subject = store.Get<Transform>(controller->Subject);
		if (subject == nullptr || !store.Alive(active->Entity)) {
			// No subject is a free camera, which is what an editor has. Left
			// where it is rather than moved to the origin.
			return false;
		}

		// Where the eyes are. Everything below is relative to this.
		const Vector3 head = subject->Frame.Position + Vector3{0.0f, controller->HeadHeight, 0.0f};

		// The look direction, from the two angles. **Built here rather than
		// carried**, because the angles are the authority — `CameraController::
		// Angles` says why.
		const float pitch = controller->Angles.X;
		const float yaw = controller->Angles.Y;
		Vector3 forward{
			-std::sin(yaw) * std::cos(pitch),
			std::sin(pitch),
			-std::cos(yaw) * std::cos(pitch),
		};

		Vector3 eye = head;
		if (controller->Mode != CameraMode::LockFirstPerson) {
			eye = head - forward * controller->Distance;

			if (controller->Mode == CameraMode::ShiftLock) {
				// Over the shoulder. The side vector is the forward turned a
				// quarter turn about world up, which is exact rather than a cross
				// product because the yaw is already known.
				const Vector3 side{std::cos(yaw), 0.0f, -std::sin(yaw)};
				eye = eye + side * controller->ShoulderOffset;
			}
		}

		// **The arm goes through a portal if one is in the way of it**, and
		// leaving that out is what makes a hole somebody can walk through look
		// broken from the outside. The body crosses on the tick its own segment
		// changes side; the eye is metres behind it and does not, so for as long
		// as the arm straddles the pane the camera watches its subject from the
		// room it just left — the character reads as teleporting away and
		// turning as it goes, which is exactly the report this closes.
		//
		// Put through the same map as a body's placement and velocity, so the
		// eye arrives where the picture in the pane says it should be: behind
		// the character, on the far side, looking back through the hole. First
		// person has no arm and therefore no crossing, which is why this is
		// after the branch above rather than inside it.
		// **`Point` for the eye and `Rotate` for the aim**, because a hole may
		// change size as well as place: the arm's far end is a position and
		// scales with the room it lands in, and the look direction is a unit
		// vector that must stay one. Scaling the aim would give `LookAt` a
		// longer forward, which is not wrong so much as it is one edit away from
		// being wrong.
		SeamTransform carried;
		if (engine::scene::PortalCrossing(store, head, eye, carried)) {
			eye = carried.Point(eye);
			forward = carried.Rotate(forward);
		}

		// **And never left standing in the glass.** The crossing above answers
		// which room the eye is in; it does not stop the eye landing *inside*
		// the pane, which an arm swung into a doorway or a first-person walk up
		// to one both do. A surface camera constructed from a viewpoint in its
		// own plane has no half-space to clip and no bounded fit, and the pane
		// fills the screen with a smear. `ClearOfPanes` is the same rule a body
		// already gets from its landing clearance, applied to the eye.
		//
		// **After the crossing rather than before it**, because it is the eye's
		// final resting place that has to be out of the seam — pushing it clear
		// first and then mapping it through a hole would put it back in.
		(void)engine::scene::ClearOfPanes(store, eye);

		// **`LookAt` towards a point along the forward rather than at the head**,
		// which matters in first person and in shift-lock: aiming at the head from
		// the head is a zero-length direction, and aiming at the head from over a
		// shoulder points the camera *at the character* rather than past it.
		store.Set(active->Entity, Transform{CFrame::LookAt(eye, eye + forward)});
		return true;
	}

	MoveIntent ReadMoveIntent(const ecs::Store &store) {
		const auto *input = store.Resource<InputState>();
		const auto *controller = store.Resource<CameraController>();
		if (input == nullptr) {
			return {};
		}

		// **Relative to the camera's yaw**, which is why this reads the
		// controller. W is "away from the camera", not "along -Z" — a game whose
		// forward key stopped meaning forward when the camera turned is the one
		// thing every player notices immediately.
		const float yaw = controller == nullptr ? 0.0f : controller->Angles.Y;
		const Vector3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
		const Vector3 side{std::cos(yaw), 0.0f, -std::sin(yaw)};

		Vector3 wanted;
		if (input->Focused) {
			// **Only when focused.** Alt-tabbing away while holding W must not
			// leave a character walking forever, which is the bug every engine
			// ships once. The client also clears the keys, and this is the second
			// half of the same belt: a recorded input stream replayed into an
			// unfocused world should behave the same way.
			if (input->IsKeyDown(KeyCode::W) || input->IsKeyDown(KeyCode::Up)) {
				wanted = wanted + forward;
			}
			if (input->IsKeyDown(KeyCode::S) || input->IsKeyDown(KeyCode::Down)) {
				wanted = wanted - forward;
			}
			if (input->IsKeyDown(KeyCode::D) || input->IsKeyDown(KeyCode::Right)) {
				wanted = wanted + side;
			}
			if (input->IsKeyDown(KeyCode::A) || input->IsKeyDown(KeyCode::Left)) {
				wanted = wanted - side;
			}
		}

		MoveIntent intent;

		// **Normalised, so diagonal movement is not faster.** Two keys held gives
		// a vector of length √2, and a controller that used it directly would make
		// running diagonally forty per cent quicker — which is the oldest movement
		// bug there is and the one players find first.
		intent.Direction = wanted.Magnitude() > 0.0f ? wanted.Unit() : Vector3{};

		// **`WasKeyTapped` and not `WasKeyPressed`**, because this is read on a
		// tick and the other question is about a frame. See `InputState::Pressed`
		// — a space bar tapped between two ticks is pressed on a frame no tick
		// inspects, and reading the frame-shaped edge here is what made both
		// hosts grow a private `PendingJump` beside this function instead of
		// through it.
		intent.Jump = input->Focused && input->WasKeyTapped(KeyCode::Space);
		return intent;
	}

	size_t UpdateCharacterControl(ecs::Store &store) {
		if (store.Resource<InputState>() == nullptr) {
			return 0;
		}

		const MoveIntent intent = ReadMoveIntent(store);
		const Vector3 direction = intent.Direction;
		const bool jump = intent.Jump;

		// **Which humanoid this keyboard is allowed to move**, and getting this
		// wrong is not cosmetic: with two clients in one world, a `Each` over
		// every humanoid means each machine walks *both* characters and the two
		// fight over one body every tick.
		//
		// **A world with no `LocalPlayer` drives all of them**, which is not a
		// second behaviour bolted on — it is the case where the question has no
		// answer. A test world, a single-part scripted character, an examples
		// scene: none of them has a `Players` service, so "the local player's
		// character" names nothing and driving what is there is the only useful
		// reading. The moment a host admits a player it stops applying.
		const LocalPlayer *local = store.Resource<LocalPlayer>();
		ecs::Entity mine = ecs::NULL_ENTITY;
		if (local != nullptr) {
			const Character *character = store.Get<Character>(CharacterOf(store, local->Instance));
			if (character == nullptr) {
				// A local player between a death and a respawn has no body, and
				// keys pressed into that gap must not reach somebody else's.
				return 0;
			}
			mine = character->Humanoid;
		}

		// **A character somebody owns is never the fallback's to drive**, and
		// this is the correction that makes a hosted world work at all.
		//
		// The fallback above — no `LocalPlayer`, so drive every humanoid — is
		// right for the worlds it was written for and catastrophic for an
		// authority. A studio playing both halves, or a dedicated server, has no
		// `LocalPlayer` and no keyboard: `InputState` sits there unfocused, so
		// `ReadMoveIntent` returns a zero direction, and the fallback wrote that
		// zero over the direction a client had just sent through
		// `game::ApplyMoveInput` — every tick, after the message arrived and
		// before `StepCharacters` read it. `MoveDirection` was therefore always
		// zero by the time it mattered and no client could walk.
		//
		// **Jump was unaffected, which is what made the bug so hard to place.**
		// `JumpRequested` below is latched with `||` and survives being written
		// by a keyboard that is not pressing anything; the direction is assigned
		// and does not. A character that jumps perfectly and refuses to walk is
		// the exact signature of this line being missing.
		//
		// `Character::Owner` is the test rather than a `Players` lookup: it is
		// already the field that says "a person at a keyboard somewhere holds
		// this", and an NPC leaves it null and stays drivable.
		std::vector<ecs::Entity> spoken;
		if (local == nullptr) {
			store.Each<const Character>([&spoken](ecs::Entity, const Character &character) {
				if (character.Owner != ecs::NULL_ENTITY && character.Humanoid != ecs::NULL_ENTITY) {
					spoken.push_back(character.Humanoid);
				}
			});
		}

		size_t driven = 0;
		store.Each<Humanoid>([&](ecs::Entity entity, Humanoid &humanoid) {
			if (!humanoid.Enabled || (local != nullptr && entity != mine)) {
				return;
			}
			if (local == nullptr && std::find(spoken.begin(), spoken.end(), entity) != spoken.end()) {
				return;
			}
			humanoid.MoveDirection = direction;

			// **Latched rather than assigned**, so a jump pressed between two
			// simulation ticks is not lost. The step clears it.
			humanoid.JumpRequested = humanoid.JumpRequested || jump;
			driven++;
		});

		return driven;
	}

	size_t StepCharacters(ecs::Store &store, float delta) {
		(void)delta;

		size_t moved = 0;
		store.Each<Humanoid>([&](ecs::Entity entity, Humanoid &humanoid) {
			if (!humanoid.Enabled) {
				return;
			}

			// **The body is not always the row.** A character rig puts the
			// humanoid beside the parts and names one of them; a scripted NPC
			// puts the humanoid on the part itself. `Humanoid::RootPart` says
			// which arrangement this is, and resolving it here is what lets both
			// go through one function.
			const ecs::Entity body = humanoid.RootPart == ecs::NULL_ENTITY ? entity : humanoid.RootPart;

			Motion *found = store.GetMutable<Motion>(body);
			if (found == nullptr) {
				return;
			}
			Motion &motion = *found;

			const Vector3 wanted = humanoid.MoveDirection * humanoid.WalkSpeed;

			// **The horizontal velocity is replaced and the vertical is kept**,
			// which is what makes a character controller a controller rather than
			// a body. Adding a force would leave momentum from the last frame and
			// make stopping take a second; replacing Y as well would cancel
			// gravity and the fall.
			motion.Linear.X = wanted.X;
			motion.Linear.Z = wanted.Z;

			if (humanoid.JumpRequested && humanoid.Grounded) {
				motion.Linear.Y = humanoid.JumpSpeed;
				humanoid.Grounded = false;
			}

			// **Upright, and this is the line that stops a character lying
			// down.** Nothing here runs a balance controller, so a body free to
			// spin tips over the first time a corner catches it — and a
			// character face-down on the floor still walks, which reads as a
			// physics bug rather than a missing feature. Zeroing the rate is
			// enough: a box that is never given angular velocity never acquires
			// an angle, so there is no orientation to correct afterwards.
			motion.Angular = Vector3{};

			// **Cleared whether or not it was used.** A request that survived
			// being on the ground would fire the moment the character landed,
			// which reads as an input that took a second to arrive.
			humanoid.JumpRequested = false;
			moved++;
		});

		return moved;
	}

	ecs::ClassId HumanoidClass() {
		// The shape every class accessor in this module shares; `Part.hpp`
		// carries the argument for both halves. A humanoid derives from the
		// instance root rather than from a part — what it shares with
		// `PartClass` is the registration, which is why that is what is called.
		static const ecs::ClassId humanoid = (EnsureClassTree(), ecs::Classes::Find(core::Name("Humanoid")));
		return humanoid;
	}
}
