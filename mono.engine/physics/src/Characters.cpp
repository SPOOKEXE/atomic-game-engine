#include "ConvexQuery.hpp"
#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Portals.hpp>
#include <engine/physics/Query.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <cmath>
#include <optional>
#include <vector>

namespace engine::physics {

	namespace {
		// A blocker wearing this tag is walked straight through, up to
		// `POPPERCAM_IGNORE_LIMIT` of them in one cast. `UpdatePoppercam`'s
		// own header carries the reason a tag rather than a query argument.
		const core::Name &PoppercamIgnoreTag() {
			static const core::Name tag("IgnorePoppercam");
			return tag;
		}

		constexpr int POPPERCAM_IGNORE_LIMIT = 8;

		// How far short of the blocker the eye stops, in metres.
		//
		// **Not zero**, because a raycast hit is a surface and an eye placed
		// exactly on one clips into it the moment the camera's own near plane
		// - which has real thickness - crosses the same point. A hair of
		// clearance is cheaper than a per-frame fight with the near plane.
		constexpr float POPPERCAM_MARGIN = 0.15f;

		// How see-through a blocker becomes. **Partial rather than
		// invisible**, which is what the goal this closes asks for by name:
		// a wall thinned enough to steer by and still readable as a wall,
		// rather than a hole in the world with a floor plan behind it.
		constexpr float POPPERCAM_FADE = 0.6f;
	}

	// What the previous call faded, so the next one can clear exactly that
	// and nothing else.
	//
	// **A resource rather than a field on `CameraController`**, because it is
	// this pass's own bookkeeping and nothing else has a reason to read it -
	// `scene` may not even know this module exists. Self-installing the same
	// way `scene::MaterialCatalogue` is, because there is exactly one of it
	// and nothing needs to have set it up in advance.
	struct PoppercamState {
		ecs::Entity FadedBlocker = ecs::NULL_ENTITY;
	};

	namespace {
		// How level a face has to be for a character to walk up it, as the Y of
		// its unit normal.
		//
		// **0.6 is about 53 degrees**, which is steeper than anything a person
		// walks up and gentle enough that a scree slope in a heightfield is
		// still ground. Above it the walk is turned along the surface and the
		// character climbs at the speed it walks; below it the face is left to
		// gravity, which is what makes a cliff a cliff rather than a ramp.
		//
		// Roblox's `MaxSlopeAngle` is the same idea as a per-humanoid figure.
		// This is the engine's floor under it and can become one the day a
		// scene needs to disagree.
		constexpr float MINIMUM_WALKABLE_NORMAL = 0.6f;

		// How far above its feet a character finds its own ground.
		//
		// **A step, and the reason it is not a tolerance.** `Humanoid::
		// GroundTolerance` asks whether the feet are on something and is a
		// sixth of a stud; this asks what they should be standing on, and a
		// surface a stud higher than the feet is a kerb to step onto rather
		// than a wall to stop at. One stud is a fifth of the default character
		// and about a real step; two - Roblox's - lets a body walk onto things
		// it visibly should not.
		constexpr float CHARACTER_STEP_HEIGHT = 1.0f;
	}

	size_t GroundCharacters(ecs::Store &store) {
		// **Silent in a world with no solver, exactly as
		// `ClipCharacterVelocity` below is and for the same reason.** This is
		// registered by `RegisterCharacterSystems`, which a client installs on
		// every world it presents - including its own, which is presented and
		// never simulated and therefore never given a `PhysicsWorld`. The
		// grounding ray then went through `physics::Raycast`, which asks the
		// loud accessor, and put `physics has no PhysicsWorld resource` in the
		// log once per character per tick.
		//
		// A character with nothing to stand on is ungrounded, which is the
		// honest answer for a world with no floors in it as far as this module
		// is concerned. See `WorldResource.cpp` for why the accessor is loud
		// where a solver step really is expected.
		if (!PhysicsWorldRegistered() || store.Resource<PhysicsWorld>() == nullptr) {
			return 0;
		}

		size_t tested = 0;

		store.Each<scene::Humanoid>([&](ecs::Entity row, scene::Humanoid &humanoid) {
			if (!humanoid.Enabled) {
				return;
			}

			// The body is not always the row - `scene::Humanoid::RootPart`
			// carries the argument, and `scene::StepCharacters` resolves it the
			// same way. Casting from the humanoid instance instead would be
			// casting from a row with no place in the world.
			const ecs::Entity body = humanoid.RootPart == ecs::NULL_ENTITY ? row : humanoid.RootPart;

			const scene::Transform *placement = store.Get<scene::Transform>(body);
			if (placement == nullptr) {
				return;
			}

			const core::Vector3 feet =
				placement->Frame.Position - core::Vector3{0.0f, humanoid.Height * 0.5f, 0.0f};

			const core::Ray ray{feet + core::Vector3{0.0f, 0.1f, 0.0f}, core::Vector3{0.0f, -1.0f, 0.0f}};

			// **The body is skipped by the query and not by testing the
			// answer**, and that correction is what makes a character rig
			// grounded at all. The ray starts *inside* the feet on purpose - a
			// ray that begins exactly on a face is a coin flip about whether it
			// hits it, and the coin lands differently on two machines - so with
			// a root collider the full height of the character, the nearest hit
			// is always the character. Comparing `hit->Owner != body`
			// afterwards then reads "not grounded" while it is standing on a
			// floor, for ever: the floor was never the answer that came back.
			//
			// It cost a studio test to find and the symptom was a character
			// resting perfectly still on a plate it could not jump off.
			// **Through any pane in the way, which is what keeps a body in the
			// seam standing on something.** A pane is a hole and a character may
			// be halfway through one, so the floor under its feet is the far
			// room's for as long as the near room's stops at the doorway - and a
			// ray that stopped at the glass reported "not grounded" for a
			// character everybody can see standing on a floor. What that looks
			// like is falling out of the world in the one metre where you should
			// not.
			//
			// **Which side is answered by the sign, not by a blend.** The ray
			// starts at the feet, so a body more than halfway through is a body
			// whose feet are past the plane and whose ground is the far room's;
			// one less than halfway keeps the near room's. That is the same
			// threshold `CrossPortals` moves the body on, and the two agreeing is
			// what makes the crossing tick look like every other one.
			const auto hit = RaycastThroughPortals(
				store, ray, 0.1f + humanoid.GroundTolerance, spatial::LayerMask::All(), body
			);

			// **A trigger is not a floor**, for `ClipCharacterVelocity`'s
			// reason: `CanCollide = false` means a body falls through, and a
			// character reported as standing on water is one that will not
			// jump and will not fall.
			const scene::Collider *stoodOn =
				hit.has_value() ? store.Get<scene::Collider>(hit->Owner) : nullptr;

			humanoid.Grounded = hit.has_value() && (stoodOn == nullptr || !stoodOn->Trigger);

			tested++;
		});

		return tested;
	}

	bool UpdatePoppercam(ecs::Store &store) {
		PoppercamState *state = store.ResourceMutable<PoppercamState>();
		if (state == nullptr) {
			store.SetResource(PoppercamState{});
			state = store.ResourceMutable<PoppercamState>();
		}

		// Clears whatever the previous frame faded and drops the occlusion,
		// so every early return below leaves the world in the state a caller
		// with nothing to occlude expects: the player's own distance, and
		// nothing translucent that should not be.
		const auto clear = [&]() -> bool {
			bool changed = false;
			auto *controller = store.ResourceMutable<scene::CameraController>();
			if (controller != nullptr && controller->OccludedDistance >= 0.0f) {
				controller->OccludedDistance = -1.0f;
				changed = true;
			}
			if (state->FadedBlocker != ecs::NULL_ENTITY) {
				scene::SetLocalTransparency(store, state->FadedBlocker, 0.0f);
				state->FadedBlocker = ecs::NULL_ENTITY;
				changed = true;
			}
			return changed;
		};

		auto *controller = store.ResourceMutable<scene::CameraController>();
		const auto *active = store.Resource<scene::ActiveCamera>();
		if (controller == nullptr || active == nullptr || !controller->Enabled ||
			controller->Mode == scene::CameraMode::Scriptable ||
			controller->Mode == scene::CameraMode::LockFirstPerson) {
			// **First person is not "zero distance", it is nothing to occlude
			// at all.** The eye sits at the head with no arm reaching back
			// from it, so there is no point between the two for anything to
			// stand in.
			return clear();
		}

		const scene::Transform *subject = store.Get<scene::Transform>(controller->Subject);
		if (subject == nullptr) {
			return clear();
		}

		// The same arithmetic `PlaceCamera` uses to find where the eye wants
		// to be, repeated here rather than shared - that function is `scene`
		// and takes no query, and duplicating four lines of trigonometry is
		// cheaper than a callback for what the query decides. Portal seams
		// are deliberately not accounted for: a poppercam correcting for a
		// wall on the far side of a hole it has not been told about is a
		// sharper edge than one that is a frame late clearing a wall that
		// was, and the ordinary case - no portal in the shot - pays nothing
		// extra either way.
		const core::Vector3 head =
			subject->Frame.Position + core::Vector3{0.0f, controller->HeadHeight, 0.0f};
		const float pitch = controller->Angles.X;
		const float yaw = controller->Angles.Y;
		const core::Vector3 forward{
			-std::sin(yaw) * std::cos(pitch),
			std::sin(pitch),
			-std::cos(yaw) * std::cos(pitch),
		};

		core::Vector3 desired = head - forward * controller->Distance;
		if (controller->Mode == scene::CameraMode::ShiftLock) {
			const core::Vector3 side{std::cos(yaw), 0.0f, -std::sin(yaw)};
			desired = desired + side * controller->ShoulderOffset;
		}

		const core::Vector3 toEye = desired - head;
		const float wanted = toEye.Magnitude();
		if (wanted <= POPPERCAM_MARGIN) {
			// Already closer than the margin allows - nothing to pull in to.
			return clear();
		}
		const core::Vector3 direction = toEye / wanted;

		// **A loop of single-hit casts rather than a filtered query**, because
		// `Raycast` refuses a general ignore list by design - see its own
		// header. Each pass starts just past the last hit, which is the same
		// "inside rather than on the surface" trick `GroundCharacters` uses
		// above, for the identical reason: a ray beginning exactly on a face
		// is a coin flip about whether it hits it again.
		float travelled = 0.0f;
		std::optional<ColliderHit> blocking;
		for (int pass = 0; pass < POPPERCAM_IGNORE_LIMIT && travelled < wanted; pass++) {
			const core::Ray ray{head + direction * travelled, direction};
			const auto hit =
				Raycast(store, ray, wanted - travelled, spatial::LayerMask::All(), controller->Subject);
			if (!hit.has_value()) {
				break;
			}

			if (scene::HasTag(store, hit->Owner, PoppercamIgnoreTag())) {
				travelled += hit->Distance + 0.01f;
				continue;
			}

			blocking = hit;
			blocking->Distance += travelled;
			break;
		}

		if (!blocking.has_value()) {
			return clear();
		}

		bool changed = false;

		const float occluded = std::max(0.0f, blocking->Distance - POPPERCAM_MARGIN);
		if (controller->OccludedDistance != occluded) {
			controller->OccludedDistance = occluded;
			changed = true;
		}

		if (state->FadedBlocker != blocking->Owner) {
			if (state->FadedBlocker != ecs::NULL_ENTITY) {
				scene::SetLocalTransparency(store, state->FadedBlocker, 0.0f);
			}
			scene::SetLocalTransparency(store, blocking->Owner, POPPERCAM_FADE);
			state->FadedBlocker = blocking->Owner;
			changed = true;
		}

		return changed;
	}

	size_t WakeMovingCharacters(ecs::Store &store) {
		// Silent like `GhostPortalBodies`, and guarded the same way: hosts
		// without a solver run this system, and the typed lookup would register
		// the resource under the compiler's spelling to say it found nothing -
		// see `WorldResource.hpp`.
		if (!PhysicsWorldRegistered()) {
			return 0;
		}
		auto *world = store.ResourceMutable<PhysicsWorld>();
		if (world == nullptr) {
			return 0;
		}

		size_t woken = 0;
		std::vector<ecs::Entity> pending;

		store.Each<const scene::Humanoid>([&](ecs::Entity row, const scene::Humanoid &humanoid) {
			if (!humanoid.Enabled) {
				return;
			}

			// **Told to do something, or with nothing under it.**
			//
			// The first is the intent, and it is the intent rather than a key
			// press on purpose: a server applying a `game::MoveInput` has no
			// keyboard, and neither does a scripted NPC. Both write
			// `MoveDirection`, so both wake.
			//
			// The second is the case that has no other answer. A sleeping body
			// carries no `scene::Motion` and is therefore never integrated, so a
			// character whose floor is deleted, or who is nudged off a ledge by
			// something that does not touch it, hangs in the air for ever -
			// `physics`' own wake pass only fires on a *contact*, and there is
			// no contact when the support simply stops existing.
			//
			// **Not "always awake", which is the other way this could have
			// gone.** A component that kept every character in the dynamic set
			// would put every idle player through the integrator and the broad
			// phase every tick, for ever, to hold still - and standing still is
			// what players spend most of a session doing. Sleeping exists for
			// exactly that body. These two conditions are what it costs to keep
			// it.
			if (humanoid.MoveDirection.Magnitude() <= 0.0f && !humanoid.JumpRequested && humanoid.Grounded) {
				return;
			}

			const ecs::Entity body = humanoid.RootPart == ecs::NULL_ENTITY ? row : humanoid.RootPart;

			if (world->Wake(body)) {
				woken++;
			}

			// **And the component back, because that is what waking *is* in this
			// ECS.** `physics::Publish` takes `scene::Motion` away when a body
			// sleeps - the archetype move is the mechanism, not a flag - so a
			// body without one is a body `IntegrateMotion` never visits and
			// `scene::StepCharacters` has nothing to write into.
			//
			// **Gathered here and written below**, for the reason
			// `scene::LinkPlayerCharacters` gives about its own list: adding a
			// component is an archetype move, and an archetype move under an
			// `Each` is the iteration invalidating itself. A rig keeps its
			// humanoid and its body on different rows so the two archetypes are
			// usually different and it usually got away with it - usually is not
			// a guarantee, and a scripted character carries both on one row,
			// where they are the same archetype and it never was.
			if (!store.Has<scene::Motion>(body)) {
				pending.push_back(body);
			}
		});

		// One `Set` per body that had none, outside the walk. Empty on every
		// tick where nothing woke, which is nearly all of them.
		for (const ecs::Entity body : pending) {
			store.Set(body, scene::Motion{});
		}

		return woken;
	}

	// Removes the part of a character's commanded velocity that points into
	// something solid, so the walk slides along a wall instead of pressing
	// through it.
	//
	// **The bug this exists for.** `scene::StepCharacters` hard-assigns
	// `Motion::Linear.X/Z` from `MoveDirection * WalkSpeed` every
	// `PreSimulation`, and its comment defends that at length: replacing rather
	// than adding is what makes a character controller a controller, and it is
	// right about responsiveness. What it also does is throw away the contact
	// impulse the solver produced last tick, unintegrated. The solver's only
	// remaining answer is position correction, which is capped at
	// `MAXIMUM_CORRECTION_SPEED` - 3 m/s - and a default `WalkSpeed` of 16 beats
	// that better than five to one. So a character walked at a wall advances
	// into it at the difference and comes out the far side, which is the
	// "you phase through blocks" report. Measured: net advance into a step was
	// 0.2167 studs a tick against a commanded 0.2667, and the missing 0.05 is
	// exactly 3 m/s over a sixtieth of a second. At `WalkSpeed` 2 - under the
	// cap - the same character stops dead at the face and stays there.
	//
	// **Why the continuous sweep does not already catch it.** `SweepFastBodies`
	// admits a body only when its step is long relative to its own thinnest
	// half-extent (`Continuous.cpp:95`), because that is the tunnelling
	// question. A walk step is 0.267 against a half-extent of 0.5, so a
	// character never qualifies - it is not passing *through* the block in one
	// tick, it is leaning on it for twenty.
	//
	// **Here rather than in `scene`, and that is the layering.** `scene` is L7
	// and cannot see a collider index; this module is L8 and already owns the
	// character pass. So the intent is formed in `scene` and clipped here,
	// immediately after, before the integrator has run.
	//
	// **Two passes, because a corner is two walls.** Removing the component
	// along the first normal can leave the velocity pointing into the second;
	// running the sweep again on the clipped vector is standard
	// collide-and-slide and is where it stops - a third pass only matters for a
	// wedge sharper than anything a character can stand in.
	//
	// @param store The world.
	// @return How many characters were clipped.
	size_t ClipCharacterVelocity(ecs::Store &store) {
		// **Silent like `WakeMovingCharacters`, and guarded the same way.**
		// `PreparedWorldMutable` complains once per call by design - a world
		// with no solver produces no contacts at all, so `WorldResource.cpp`
		// would rather say so every tick than let one startup line scroll away.
		// That is right for a step that needs a solver and wrong for this one:
		// a client's own world is presented and never simulated, and this pass
		// runs on it through `character.control` like every other. Asking the
		// loud accessor put `physics has no PhysicsWorld resource` in the log
		// sixty times a second for a world that was never meant to have one.
		//
		// The name lookup first, for the reason that file gives: the typed
		// lookup would *register* the resource under the compiler's spelling in
		// the act of finding it missing.
		if (!PhysicsWorldRegistered()) {
			return 0;
		}
		PhysicsWorld *world = store.ResourceMutable<PhysicsWorld>();
		if (world == nullptr) {
			return 0;
		}

		const spatial::HashGrid &index = PipelineInternals::StaticIndex(*world);
		const std::vector<ColliderRecord> &records = PipelineInternals::StaticRecords(*world);
		if (records.empty()) {
			return 0;
		}

		const float delta = PhysicsStepSeconds(store);
		if (!(delta > 0.0f)) {
			return 0;
		}

		std::vector<uint64_t> &candidates = PipelineInternals::CandidateBuffer(*world);
		if (candidates.size() < records.size()) {
			candidates.resize(records.size());
		}

		const ecs::Store &reader = store;
		size_t clipped = 0;

		// **The baked shapes, resolved once for the walk.** Without them a
		// `Hull` or a `Mesh` collider is demoted to a box the size of the part
		// - `ShapeInstance` says so where it takes a null pointer - and for a
		// terrain chunk that box is the whole chunk, the full height of it. A
		// character standing on such a chunk starts every sweep *inside* that
		// box, which the zero-fraction skip below then throws away: so a
		// script-built heightfield clipped nothing at all, and a walk into a
		// mountainside went into the mountainside.
		const scene::CollisionShapes *baked = scene::CollisionShapesOf(store);

		// Gathered before the walk, because clipping writes `Motion` on a body
		// that is usually not the row the humanoid sits on - a rig puts the
		// humanoid beside the parts. Writing through a second handle while an
		// `Each` over `Humanoid` is running is the structural change the store
		// refuses.
		// The body, plus the three figures the slope projection below needs off
		// the humanoid it belongs to.
		struct Walker {
			ecs::Entity Body;
			float Height = 0.0f;
			float GroundTolerance = 0.0f;
		};

		std::vector<Walker> bodies;
		store.Each<const scene::Humanoid>([&](ecs::Entity row, const scene::Humanoid &humanoid) {
			if (!humanoid.Enabled || scene::IsDead(humanoid)) {
				return;
			}
			bodies.push_back(
				Walker{
					humanoid.RootPart == ecs::NULL_ENTITY ? row : humanoid.RootPart,
					humanoid.Height,
					humanoid.GroundTolerance,
				}
			);
		});

		for (const auto &[body, height, tolerance] : bodies) {
			const scene::Transform *placement = reader.Get<scene::Transform>(body);
			const scene::Collider *collider = reader.Get<scene::Collider>(body);
			scene::Motion *motion = store.GetMutable<scene::Motion>(body);
			if (placement == nullptr || collider == nullptr || motion == nullptr) {
				continue;
			}
			if (collider->Trigger || !reader.Has<scene::Simulated>(body)) {
				continue;
			}

			// **The walk is turned along the ground before it is swept against
			// anything**, and its absence is why a character could not climb a
			// hill. `scene::StepCharacters` writes the commanded speed into
			// `Linear.X` and `Linear.Z` and leaves `Linear.Y` to gravity, which
			// is right on a floor and wrong on a slope: the horizontal drive
			// pushes the body *into* the rising ground every tick, and the only
			// thing lifting it out is contact resolution, which is capped. On
			// anything steeper than about a quarter the body loses a little
			// ground each tick, and once its centre is under the surface there
			// is no contact at all - a triangle soup is a surface and not a
			// solid - so it falls through the world.
			//
			// Projecting the walk onto the plane the feet are standing on is
			// the standard answer and it is what makes a slope a slope: the
			// same commanded speed becomes an up-slope velocity, so a character
			// climbs at the pace it walks and descends without leaving the
			// ground.
			//
			// **Only while grounded, and only on ground worth standing on.** A
			// face steeper than `MINIMUM_WALKABLE_NORMAL` is left to gravity,
			// which is what makes a cliff a cliff rather than a ramp; and a
			// body that is not grounded is falling or jumping, where the
			// vertical is nobody's business but the integrator's.
			// **The ground under the feet, found once and used twice: to put the
			// body on it, and to turn the walk along it.**
			//
			// Its absence is why a character could not climb a hill.
			// `scene::StepCharacters` writes the commanded speed into
			// `Linear.X` and `Linear.Z` and leaves the vertical to gravity,
			// which is right on a floor and wrong on a slope: the drive pushes
			// the body *into* the rising ground every tick and the only thing
			// lifting it out is contact resolution, which is capped. On
			// anything steeper than about a quarter the body loses a little
			// each tick, and once its centre is under the surface there is no
			// contact at all - a triangle soup is a surface and not a solid -
			// so it walks along inside the hill and then out of the world.
			//
			// **Not while rising.** A jump is the one case where the vertical is
			// deliberately not the ground's, and a snap on the frame it started
			// would put the body straight back down.
			{
				const core::Vector3 feet =
					placement->Frame.Position - core::Vector3{0.0f, height * 0.5f, 0.0f};

				// **From a step above the feet, which is what makes it a step
				// rather than a ground test.** `GroundCharacters` asks whether
				// the feet are *on* something and casts from just above them;
				// this asks what the feet should be on, and ground half a stud
				// higher than they are is a kerb to walk up rather than a wall.
				//
				// The downward reach is the step plus this tick's travel,
				// because that is how far the surface can have moved under the
				// body since the last one - which is what keeps a character on
				// a descending slope instead of launching off every rise.
				const float travelled =
					core::Vector3{motion->Linear.X, 0.0f, motion->Linear.Z}.Magnitude() * delta;
				const float below = tolerance + travelled;

				const core::Ray under{
					feet + core::Vector3{0.0f, CHARACTER_STEP_HEIGHT, 0.0f}, core::Vector3{0.0f, -1.0f, 0.0f}
				};
				const auto ground = RaycastThroughPortals(
					store, under, CHARACTER_STEP_HEIGHT + below, spatial::LayerMask::All(), body
				);

				// **A trigger is not a floor.** `CanCollide = false` is
				// `Collider::Trigger`, which means "report the contact and
				// apply no impulse" - so a body falls through one, and a
				// character that snapped onto one would stand on it while
				// everything else fell past. The case that found it is water: a
				// sea slab is a trigger by construction, and a walker ended up
				// standing on the surface of it.
				const scene::Collider *floorCollider =
					ground.has_value() ? reader.Get<scene::Collider>(ground->Owner) : nullptr;
				const bool solidFloor =
					ground.has_value() && (floorCollider == nullptr || !floorCollider->Trigger);

				if (solidFloor) {
					const core::Vector3 walk{motion->Linear.X, 0.0f, motion->Linear.Z};
					const float speed = walk.Magnitude();
					const core::Vector3 &face = ground->Normal;

					// The commanded walk with the part that points into the
					// surface taken out of it: collide-and-slide against the
					// ground rather than against a wall.
					const core::Vector3 along = walk - face * walk.Dot(face);

					const float surface = feet.Y + CHARACTER_STEP_HEIGHT - ground->Distance;
					const float lift = surface - feet.Y;

					// **A jump is the one case that is left alone**, and it is
					// recognised by where the body is rather than by a flag:
					// rising while the ground is at or below the feet is a
					// jump, and snapping it back down would be a character that
					// cannot jump - which is exactly what
					// `server.replication`'s jump case caught, because
					// `StepCharacters` writes the jump speed on the tick the
					// feet are still touching and `lift` is zero there.
					//
					// Rising while the surface is *above* the feet is the
					// solver ejecting a body from geometry it is buried in, and
					// putting the feet on top is the answer to that rather than
					// something to stand out of the way of.
					const bool jumping = motion->Linear.Y > 0.0f && lift < 1e-3f;

					if (face.Y > MINIMUM_WALKABLE_NORMAL && !jumping) {
						// **The feet are put on the face.** Position and not
						// force, which is what every character controller does
						// and why one can climb a stair that a crate cannot:
						// the solver's correction is capped at a speed and a
						// slope is not a speed.
						if (std::abs(lift) > 1e-4f) {
							if (scene::Transform *moved = store.GetMutable<scene::Transform>(body)) {
								moved->Frame.Position.Y += lift;
							}
						}

						// **And the walk follows the face**, rescaled to the
						// commanded speed rather than left as its horizontal
						// shadow, or a character walks slower the steeper the
						// ground - which reads as the hill being sticky.
						const float length = along.Magnitude();
						if (speed > 1e-4f && length > 1e-4f) {
							const core::Vector3 slope = along * (speed / length);
							motion->Linear.X = slope.X;
							motion->Linear.Z = slope.Z;
						}

						// The vertical is the snap's now. Left as gravity it
						// would accumulate a fall the snap has to undo every
						// tick, which is the jitter this replaces.
						motion->Linear.Y = 0.0f;
					} else if (!jumping) {
						// **Too steep to walk up, so it is not walked into.**
						// The drive keeps only what runs across the face and
						// the vertical stays gravity's, so a character pressing
						// into a cliff slides down it rather than burrowing
						// through. Not rescaled: pushing into a wall should
						// cost speed.
						if (speed > 1e-4f) {
							motion->Linear.X = along.X;
							motion->Linear.Z = along.Z;
						}

						// **And back out of the face, along the face.**
						//
						// Projecting the walk is not enough on its own, and the
						// reason is at the top of this function: a character's
						// velocity is hard-assigned every tick, so the solver's
						// contact impulse is thrown away unintegrated and the
						// only thing left resolving an overlap is position
						// correction, capped at `MAXIMUM_CORRECTION_SPEED`. A
						// body sliding across a hillside at ten studs a second
						// gains depth faster than three metres a second takes
						// it away, and the projection is onto the face under
						// the *feet* while the face it is pressing into is the
						// one in front - so a little is left pointing in every
						// tick and it accumulates.
						//
						// What that cost was a character walking into a
						// mountain and out of the world. The probe above is a
						// ray from a step over the feet: once the feet are a
						// full step under the surface, the ray starts *inside*
						// the hill, points down, and finds nothing - so the
						// slide stops, the ground is reported as absent, and
						// the body falls through a solid landscape for ever.
						// Measured, it took about ninety seconds of walking.
						//
						// **Along the normal and never straight up**, which is
						// what separates this from the snap above: a push along
						// the face of a cliff moves a body *away* from the
						// cliff, where a vertical one would walk it up. The
						// distance is the vertical burial scaled by the face's
						// own tilt, which is that burial measured perpendicular
						// to the surface - the whole overlap on flat ground,
						// and nothing at all on a vertical wall, where a
						// downward ray has nothing to say anyway.
						//
						// **Only ever outward.** A negative lift is a body
						// above the surface, and pulling it down onto a slope
						// it cannot stand on is the sticky cliff this is meant
						// to prevent.
						if (lift > 1e-4f) {
							if (scene::Transform *moved = store.GetMutable<scene::Transform>(body)) {
								moved->Frame.Position = moved->Frame.Position + face * (lift * face.Y);
							}
						}
					}
				}

				// **And what is in front, which the probe above cannot see.**
				// A downward ray finds the floor and says nothing about the
				// cliff the floor runs into: walking at a vertical face, the
				// ground under the feet is walkable right up to the moment the
				// body is inside the hill. The sweep further down would catch
				// it and cannot - `SweepConvex` is a convex query and a
				// triangle soup is not convex, so a mesh collider is the one
				// shape it has no answer for.
				//
				// A ray does have an answer for one, so this asks the question
				// a ray can: is there a face across the walk within this tick's
				// travel. It is a line and not a box, so it misses a pillar
				// narrower than the body - but a hillside is not narrow, and
				// this is the difference between a character stopping at a
				// mountain and walking into it.
				const core::Vector3 walk{motion->Linear.X, 0.0f, motion->Linear.Z};
				const float walking = walk.Magnitude();
				if (walking > 1e-4f) {
					const core::Vector3 heading = walk * (1.0f / walking);

					// From the knee rather than the centre, so a kerb the step
					// above would climb is not read as a wall.
					const core::Vector3 knee =
						placement->Frame.Position -
						core::Vector3{0.0f, height * 0.5f - CHARACTER_STEP_HEIGHT * 1.5f, 0.0f};

					const auto ahead = RaycastThroughPortals(
						store,
						core::Ray{knee, heading},
						walking * delta + collider->Extent.X,
						spatial::LayerMask::All(),
						body
					);

					const scene::Collider *wall =
						ahead.has_value() ? reader.Get<scene::Collider>(ahead->Owner) : nullptr;

					if (ahead.has_value() && (wall == nullptr || !wall->Trigger) &&
						ahead->Normal.Y <= MINIMUM_WALKABLE_NORMAL) {
						const core::Vector3 across = walk - ahead->Normal * walk.Dot(ahead->Normal);
						motion->Linear.X = across.X;
						motion->Linear.Z = across.Z;
					}
				}
			}

			const ColliderRecord self{body, collider->Layer, collider->Mask};
			bool touched = false;

			for (int pass = 0; pass < 2; pass++) {
				// **Horizontal only, and that is not a simplification.** The
				// vertical axis already works: gravity pulls, the solver's
				// contact resolves, and a character lands and rests correctly
				// without this pass existing. Sweeping the full velocity asks
				// the question anyway, and a body resting exactly flush on a
				// slab is the case a sweep answers worst - measured against a
				// 240-stud floor it returned the slab's *Z face*, normal
				// (0, 0.081, 0.997), at fraction 0.4967, for a character
				// standing on its top and walking along it. Clipping on that
				// stops the walk against the ground it is standing on.
				//
				// The reported bug is horizontal: a walk driven into a block at
				// a speed position correction cannot answer. So this asks only
				// the horizontal question and leaves the vertical to the parts
				// of the pipeline that already get it right.
				const core::Vector3 travel{motion->Linear.X * delta, 0.0f, motion->Linear.Z * delta};
				const float distance = travel.Magnitude();
				if (!(distance > 1e-5f)) {
					break;
				}

				core::CFrame ahead = placement->Frame;
				ahead.Position = placement->Frame.Position + travel;

				const core::AABB envelope =
					ShapeWorldBounds(*collider, placement->Frame).Union(ShapeWorldBounds(*collider, ahead));

				const spatial::QueryResult found =
					spatial::OverlapBox(index, envelope, collider->Mask, candidates);
				if (found.Written == 0) {
					break;
				}

				const ShapeInstance moving{placement->Frame, collider->Extent, collider->Shape};

				// The earliest hit, tie-broken by entity id for the reason
				// `SweepFastBodies` gives: the grid walk's order is a property
				// of the index rather than of the scene, so a body in a corner
				// must not be clipped against whichever wall was reached first.
				float earliest = 1.0f;
				bool blocked = false;
				core::Vector3 normal;
				ecs::Entity against;

				for (size_t at = 0; at < found.Written; at++) {
					const ColliderRecord &other = records[static_cast<size_t>(candidates[at])];
					if (other.Owner == body || !PairAdmitted(self, other)) {
						continue;
					}

					const scene::Transform *placed = reader.Get<scene::Transform>(other.Owner);
					const scene::Collider *shape = reader.Get<scene::Collider>(other.Owner);
					if (placed == nullptr || shape == nullptr || shape->Trigger) {
						continue;
					}

					const collision::ConvexHull *hull = nullptr;
					const collision::TriangleMesh *soup = nullptr;
					if (baked != nullptr) {
						if (shape->Shape == scene::ShapeKind::Hull) {
							hull = baked->FindHull(shape->Geometry);
						} else if (shape->Shape == scene::ShapeKind::Mesh) {
							soup = baked->FindMesh(shape->Geometry);
						}
					}

					const ShapeInstance fixed{placed->Frame, shape->Extent, shape->Shape, hull, soup};
					const ConvexSweep hit = SweepConvex(moving, travel, fixed);
					if (!hit.Hit) {
						continue;
					}

					// **A hit at fraction zero is an overlap that already
					// existed, and its normal cannot be trusted.** A character
					// resting on a floor penetrates it by the solver's slop
					// every tick, so the sweep starts inside it and reports
					// contact immediately - with whichever face of the slab the
					// algorithm reached, which measured as the floor's *-X side*
					// while the character walked +X along the top of it. Clipping
					// on that cancels the walk against the ground it is standing
					// on, and a character that could not phase through a wall
					// could not move at all.
					//
					// Resolving an existing overlap is position correction's
					// job. What this pass is for is the other question: is the
					// step about to *enter* something. That is a hit strictly
					// along the travel, so a zero fraction is skipped rather
					// than taken as the earliest.
					if (hit.Fraction <= 1e-4f) {
						continue;
					}

					// **Ground is not a wall, and telling them apart is what
					// keeps a walk moving.** Since v0.19 a sweep can see a
					// triangle mesh - before that a mesh collider was demoted to
					// its bound and this pass never hit terrain at all - and the
					// first thing it sees is the ground the character is walking
					// *on*: a surface rising a few centimetres over one step is
					// a hit at a real fraction with a floor's normal. Clipping
					// on that stopped a character three studs from its spawn.
					//
					// What the ground does to a walk is handled above, by the
					// projection onto the face and the snap onto it. This pass
					// is for the other question: is the step about to enter
					// something it has to go around.
					if (hit.Normal.Y > MINIMUM_WALKABLE_NORMAL) {
						continue;
					}

					if (!blocked || hit.Fraction < earliest ||
						(hit.Fraction == earliest && other.Owner.Id < against.Id)) {
						earliest = hit.Fraction;
						normal = hit.Normal;
						against = other.Owner;
						blocked = true;
					}
				}

				if (!blocked) {
					break;
				}

				// **Flattened, so that a sloped face cannot take the fall
				// away.** A normal with any Y in it would otherwise remove part
				// of the downward velocity too, which is a character that stops
				// falling because it brushed a wall.
				const core::Vector3 flat{normal.X, 0.0f, normal.Z};
				const float length = flat.Magnitude();
				if (!(length > 1e-4f)) {
					// Purely vertical: a floor or a ceiling, and neither is this
					// pass's business.
					break;
				}
				const core::Vector3 wall = flat / length;

				// **Only the part pointing *into* the surface.** A normal the
				// velocity is already moving away from is a surface being left,
				// and removing anything there would stop a character walking out
				// of a doorway it has just entered.
				const core::Vector3 horizontal{motion->Linear.X, 0.0f, motion->Linear.Z};
				const float into = horizontal.Dot(wall);
				if (into >= 0.0f) {
					break;
				}

				const core::Vector3 slid = horizontal - wall * into;
				motion->Linear.X = slid.X;
				motion->Linear.Z = slid.Z;
				touched = true;
			}

			if (touched) {
				clipped++;
			}
		}

		return clipped;
	}

	void RegisterCharacterComponents() {
		// Named `physics.` rather than left to `TypeNameOf`, for rule 4's
		// reason: the automatic name is the compiler's spelling and this type
		// lives in an anonymous namespace, so the spelling is neither stable
		// across compilers nor meaningful to a reader of a snapshot.
		ecs::Components::Register<PoppercamState>("physics.PoppercamState");
	}

	void RegisterCharacterSystems(ecs::Scheduler &scheduler) {
		// **Before everything else in the phase, because everything else needs
		// the link it makes.** `Player.Character = model` is the assignment a
		// game writes; on the machine that ran it the setter has already done
		// the work and this costs two reads, and on a machine that only
		// *received* the assignment over the wire this is what builds the same
		// rig locally. Without it a client holds a `PlayerCharacter` naming a
		// model with no `Character` on it, and `client::SubmitMove` refuses to
		// send a single key press because it cannot find one.
		scheduler.Add("character.link", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)scene::LinkPlayerCharacters(store);

			// **And the reverse, which is the same concern and was nobody's
			// job.** `LinkPlayerCharacters` releases a player whose model was
			// destroyed; this destroys a model whose player was. A character is
			// a `Model` under Workspace rather than a child of the `Player`, so
			// nothing collects it - and the two places that remembered to call
			// `RemoveCharacter` by hand were the only reason it ever happened.
			//
			// Composed rather than a second `Add`, for the reason
			// `character.control` below gives at length: these operations share
			// storage and form one indivisible system.
			(void)scene::ReclaimOrphanedCharacters(store);
		});

		// **Wake, ground and step are one system, and that is the whole fix.**
		// They were two - the first two here and `character.step` in
		// `Phase::Simulation`. `physics.simulation` is in
		// `Simulation` too and is registered first by every host, so
		// `IntegrateMotion` ran *before* `StepCharacters` on every tick. The
		// velocity a key press produced was therefore written immediately after
		// the only thing that would have moved it.
		//
		// On its own that is a tick of lag nobody would notice. What made it a
		// character that does not move at all is the other end: a character
		// standing still is a body the solver rests, `physics::Publish` takes
		// `scene::Motion` away from a resting body, and it did so at the end of
		// the same tick - before anything had integrated the velocity. The next
		// tick `WakeMovingCharacters` handed back a *zero* `Motion`,
		// `StepCharacters` wrote the walk speed into it again, and the cycle
		// repeated for as long as the key was held: `scene::Motion` appearing
		// and vanishing every frame, `Humanoid::MoveDirection` reading as the
		// commanded direction on some ticks and zero on others, and a body that
		// never went anywhere.
		//
		// Composed rather than split into smaller scheduled operations, which is
		// the same decision `physics.contacts` makes for the same reason. The
		// chain is: link the rig, wake the body so it *has* a `Motion`, ask what
		// is under it, then write the velocity. `Simulation` then integrates
		// what this left, in the tick that produced it.
		scheduler.Add(
			"character.control",
			ecs::Phase::PreSimulation,
			[](ecs::Store &store) {
				(void)WakeMovingCharacters(store);
				(void)GroundCharacters(store);

				// **Still against the fixed tick**, which is all
				// `scene::StepCharacters` ever asked for - `PreSimulation` runs once
				// per tick exactly as `Simulation` does, so moving it here costs the
				// determinism nothing and buys the ordering everything.
				(void)scene::StepCharacters(store, static_cast<float>(store.Time().Delta));

				// **Immediately after, and that ordering is the whole fix.**
				// `StepCharacters` is the last writer of the walk before the
				// integrator, and it writes an intent rather than a force - so
				// anything solid in the way has to be taken out of that intent
				// here, before the integrator acts on it. See
				// `ClipCharacterVelocity`.
				(void)ClipCharacterVelocity(store);
			},
			ecs::SystemOrder{{}, {"character.link"}, {"scene.gravity"}, {"character-control"}}
		);

		// **In `PreRender`, beside `ResolveAttachments`, and on whatever machine
		// draws.** A limb's place is derived from a root the solver moved this
		// tick on a server and that interpolation moved this *frame* on a
		// client - posing in the simulation would leave a client's limbs a frame
		// behind their own body, which reads as a character sliding out of its
		// own arms.
		// **After the solver, because what is tested is where the tick ended.**
		// A crossing is the segment between where a body started the tick and
		// where it finished it - `scene::CrossPortals` says why a position test
		// alone misses at walking speed - so this has to run once the body has
		// actually been moved.
		// **Before the solver, because it decides what the solver does.** A
		// portal's pane is an ordinary part and an ordinary part collides, so
		// a character walked into the picture and stopped there - and
		// `character.portal` below, which tests whether a body's step *crossed*
		// the plane, could never fire for a body the solver had parked on it.
		//
		// **Its own entry rather than composed, and the phase is what makes
		// that safe.** Nothing else in `PreSimulation` reads `Collider::Trigger`
		// - physics reads it in `Simulation` and `PostSimulation` - so the
		// ordering the scheduler does not give is ordering nothing here needs.
		// The composition argument below applies to systems that read each
		// other's writes within a phase, and this reads nobody's.
		scheduler.Add("portal.open", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)scene::OpenPortals(store);
		});

		// **The far room's floor, under anything standing in a hole.** Beside
		// `portal.open` because it is the other half of what lets a body be in a
		// pane: that one stops the pane solving contacts, and this one gives the
		// half that has gone through something to solve against. Before the
		// broadphase, which is in `Simulation`, so a proxy is indexed on the tick
		// it exists for.
		scheduler.Add("portal.ghost", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)GhostPortalBodies(store);
		});

		scheduler.Add("character.portal", ecs::Phase::PostSimulation, [](ecs::Store &store) {
			(void)scene::CrossPortals(store);
		});

		// **After the crossing**, so a body that walked through this tick is
		// already on the far side when its proxies go - and unconditionally, or a
		// proxy outlives the seam that explains it and becomes a wall nobody can
		// see.
		scheduler.Add(
			"portal.retire",
			ecs::Phase::PostSimulation,
			[](ecs::Store &store) { (void)RetirePortalProxies(store); },
			ecs::SystemOrder{{}, {"character.portal"}}
		);

		scheduler.Add("character.pose", ecs::Phase::PreRender, [](ecs::Store &store) {
			(void)scene::PoseCharacters(store);
		});
	}
}
