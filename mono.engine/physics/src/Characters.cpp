#include "WorldResource.hpp"

#include <engine/core/Name.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Portals.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
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

	size_t GroundCharacters(ecs::Store &store) {
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

			humanoid.Grounded = hit.has_value();
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
			// `character.control` below gives at length: the scheduler orders
			// nothing within a phase, and releasing a player and collecting an
			// orphan in either order against the same rows is a race worth not
			// having.
			(void)scene::ReclaimOrphanedCharacters(store);
		});

		// **Wake, ground and step are one system, and that is the whole fix.**
		// They were two - the first two here and `character.step` in
		// `Phase::Simulation` - and `RegisterPhysicsSystems` says in as many
		// words why that could not work: *"`ecs::Scheduler` gives no ordering
		// between two systems in one phase"*. `physics.simulation` is in
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
		// Composed rather than ordered by registration, which is the same
		// decision `physics.contacts` makes for the same reason - the contract
		// supports composition and does not support registration order. The
		// chain is: link the rig, wake the body so it *has* a `Motion`, ask what
		// is under it, then write the velocity. `Simulation` then integrates
		// what this left, in the tick that produced it.
		scheduler.Add("character.control", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)WakeMovingCharacters(store);
			(void)GroundCharacters(store);

			// **Still against the fixed tick**, which is all
			// `scene::StepCharacters` ever asked for - `PreSimulation` runs once
			// per tick exactly as `Simulation` does, so moving it here costs the
			// determinism nothing and buys the ordering everything.
			(void)scene::StepCharacters(store, static_cast<float>(store.Time().Delta));
		});

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
		scheduler.Add("portal.retire", ecs::Phase::PostSimulation, [](ecs::Store &store) {
			(void)RetirePortalProxies(store);
		});

		scheduler.Add("character.pose", ecs::Phase::PreRender, [](ecs::Store &store) {
			(void)scene::PoseCharacters(store);
		});
	}
}
