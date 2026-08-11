#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <vector>
#include <engine/spatial/LayerMask.hpp>

namespace engine::physics {

	size_t GroundCharacters(ecs::Store &store) {
		size_t tested = 0;

		store.Each<scene::Humanoid>([&](ecs::Entity row, scene::Humanoid &humanoid) {
			if (!humanoid.Enabled) {
				return;
			}

			// The body is not always the row — `scene::Humanoid::RootPart`
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
			// grounded at all. The ray starts *inside* the feet on purpose — a
			// ray that begins exactly on a face is a coin flip about whether it
			// hits it, and the coin lands differently on two machines — so with
			// a root collider the full height of the character, the nearest hit
			// is always the character. Comparing `hit->Owner != body`
			// afterwards then reads "not grounded" while it is standing on a
			// floor, for ever: the floor was never the answer that came back.
			//
			// It cost a studio test to find and the symptom was a character
			// resting perfectly still on a plate it could not jump off.
			const auto hit =
				Raycast(store, ray, 0.1f + humanoid.GroundTolerance, spatial::LayerMask::All(), body);

			humanoid.Grounded = hit.has_value();
			tested++;
		});

		return tested;
	}

	size_t WakeMovingCharacters(ecs::Store &store) {
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
			// something that does not touch it, hangs in the air for ever —
			// `physics`' own wake pass only fires on a *contact*, and there is
			// no contact when the support simply stops existing.
			//
			// **Not "always awake", which is the other way this could have
			// gone.** A component that kept every character in the dynamic set
			// would put every idle player through the integrator and the broad
			// phase every tick, for ever, to hold still — and standing still is
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
			// sleeps — the archetype move is the mechanism, not a flag — so a
			// body without one is a body `IntegrateMotion` never visits and
			// `scene::StepCharacters` has nothing to write into.
			//
			// **Gathered here and written below**, for the reason
			// `scene::LinkPlayerCharacters` gives about its own list: adding a
			// component is an archetype move, and an archetype move under an
			// `Each` is the iteration invalidating itself. A rig keeps its
			// humanoid and its body on different rows so the two archetypes are
			// usually different and it usually got away with it — usually is not
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
		});

		// **Wake, ground and step are one system, and that is the whole fix.**
		// They were two — the first two here and `character.step` in
		// `Phase::Simulation` — and `RegisterPhysicsSystems` says in as many
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
		// the same tick — before anything had integrated the velocity. The next
		// tick `WakeMovingCharacters` handed back a *zero* `Motion`,
		// `StepCharacters` wrote the walk speed into it again, and the cycle
		// repeated for as long as the key was held: `scene::Motion` appearing
		// and vanishing every frame, `Humanoid::MoveDirection` reading as the
		// commanded direction on some ticks and zero on others, and a body that
		// never went anywhere.
		//
		// Composed rather than ordered by registration, which is the same
		// decision `physics.contacts` makes for the same reason — the contract
		// supports composition and does not support registration order. The
		// chain is: link the rig, wake the body so it *has* a `Motion`, ask what
		// is under it, then write the velocity. `Simulation` then integrates
		// what this left, in the tick that produced it.
		scheduler.Add("character.control", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)WakeMovingCharacters(store);
			(void)GroundCharacters(store);

			// **Still against the fixed tick**, which is all
			// `scene::StepCharacters` ever asked for — `PreSimulation` runs once
			// per tick exactly as `Simulation` does, so moving it here costs the
			// determinism nothing and buys the ordering everything.
			(void)scene::StepCharacters(store, static_cast<float>(store.Time().Delta));
		});

		// **In `PreRender`, beside `ResolveAttachments`, and on whatever machine
		// draws.** A limb's place is derived from a root the solver moved this
		// tick on a server and that interpolation moved this *frame* on a
		// client — posing in the simulation would leave a client's limbs a frame
		// behind their own body, which reads as a character sliding out of its
		// own arms.
		// **After the solver, because what is tested is where the tick ended.**
		// A crossing is the segment between where a body started the tick and
		// where it finished it — `scene::CrossPortals` says why a position test
		// alone misses at walking speed — so this has to run once the body has
		// actually been moved.
		scheduler.Add("character.portal", ecs::Phase::PostSimulation, [](ecs::Store &store) {
			(void)scene::CrossPortals(store);
		});

		scheduler.Add("character.pose", ecs::Phase::PreRender, [](ecs::Store &store) {
			(void)scene::PoseCharacters(store);
		});
	}
}
