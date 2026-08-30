#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/gui/Services.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Postbox.hpp>

#include <cmath>
#include <cstring>
#include <server/Simulation.hpp>
#include <string>
#include <vector>

namespace server {

	using engine::core::CFrame;
	using engine::core::Color3;
	using engine::core::Random;
	using engine::core::Vector3;
	using engine::ecs::Components;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::ecs::SystemOrder;
	using engine::scene::Bounds;
	using engine::scene::Motion;
	using engine::scene::Transform;
	using engine::scene::Visual;
	using engine::scene::WorldBounds;

	namespace {
		// This used to be a copy of the mixer in mono.client/src/Demo.cpp, with
		// a comment saying so. Both are engine::core::Random now - the reason
		// was always "a seeded standard generator may differ between standard
		// libraries", which was never a client concern or a server one.

		// --- systems -------------------------------------------------------
		//
		// Plain functions, capturing nothing. Everything they need is in the
		// world: the delta comes from its clock and the box comes from its
		// resources.

		// Parallel within the tick. The body reads and writes one row and
		// nothing else, which is the whole contract - anything structural
		// would abort on the store's affinity check from a worker, and that is
		// the intended outcome rather than a limitation.
		//
		// It still blocks until every entity is done, so the tick remains one
		// thing that starts and finishes and a recorded run still replays.
		void Integrate(Store &store) {
			const float delta = store.Time().Delta;

			// `Transform` is a whole `CFrame`, so only its position is written
			// here: the placeholder world has no angular velocity to apply and
			// touching the rotation would be inventing one. `Motion::Angular`
			// is what a physics step at L8 will integrate, and it is deliberately
			// left alone until that exists rather than half-implemented here.
			//
			// **Takes `Jobs::DEFAULT_GRAIN` deliberately, and the resemblance to
			// `physics::INTEGRATE_GRAIN` is the trap rather than the argument.**
			// That constant is 1024 because *its* body carries a whole `CFrame`
			// through a quaternion product and a normalise - about forty flops
			// and a reciprocal square root - and crosses over at 8,000 rows. This
			// body is the line below: three multiply-adds and a store, which is
			// exactly the cheap body `engine.ecs.bench.iteration` measures at a
			// crossover near 262,144 rows. The two load the same two components
			// and do a twentieth of the work, and it is the work that sets the
			// crossover.
			//
			// Borrowing 1024 would put the floor at 8192 rows, where this loop's
			// serial cost is a couple of microseconds against a handover
			// `engine.parallel.bench.dispatch` fits at 6.2 us to wake the pool
			// plus 0.19 us a range - a loss bought on the strength of a
			// measurement of somebody else's body, which is the mistake
			// `Jobs::DEFAULT_GRAIN`'s own comment is about. Of the two constants
			// the default's floor of 32,768 is the nearer, and unmeasured either
			// way. The day this loop integrates `Motion::Angular` is the day it
			// becomes the `CFrame` body and the day to re-take the number.
			store.EachParallel<Transform, const Motion>(
				[delta](Entity, Transform &transform, const Motion &motion) {
					transform.Frame.Position = transform.Frame.Position + motion.Linear * delta;
				}
			);

			// Every position just moved, and nothing else knows. An iteration
			// hands out a reference and the store never sees the write, so a
			// replication delta built from the dirty bits would carry nothing at
			// all - which is a client that joins, receives a snapshot, and then
			// watches a frozen world. Costs nothing when nobody is observing
			// `Transform`, which is the case whenever `--listen` was not given.
			store.MarkAllChanged<Transform>();
		}

		void Bounce(Store &store) {
			// Read once for the whole world rather than once per entity. This
			// was a *component* before it was a resource, holding the same four
			// bytes on every row - a column in the archetype and a load in this
			// loop's inner body for a number the loop already knew.
			const float halfExtent = store.Resource<WorldBounds>()->HalfExtent;

			// Default grain, for the reason `Integrate` above gives at length:
			// six compares and an occasional pair of stores is *cheaper* per row
			// than the `CFrame` body `physics::INTEGRATE_GRAIN` was measured
			// against, not more expensive, so borrowing that constant would
			// dispatch this four times too early rather than correcting
			// anything. Unmeasured in either direction -
			// `engine.ecs.bench.iteration` over this body is what would say.
			store.EachParallel<Transform, Motion>([halfExtent](Entity, Transform &transform, Motion &motion) {
				// Reflect off each wall independently. The position is
				// clamped as well as the velocity flipped, because
				// flipping alone lets an entity that overshot on a long
				// tick sit outside the box flipping every tick.
				Vector3 &position = transform.Frame.Position;
				float *axis[] = {&position.X, &position.Y, &position.Z};
				float *speed[] = {&motion.Linear.X, &motion.Linear.Y, &motion.Linear.Z};

				for (int index = 0; index < 3; index++) {
					if (*axis[index] > halfExtent) {
						*axis[index] = halfExtent;
						*speed[index] = -std::abs(*speed[index]);
					} else if (*axis[index] < -halfExtent) {
						*axis[index] = -halfExtent;
						*speed[index] = std::abs(*speed[index]);
					}
				}
			});

			// Both, and both over-report: `Transform` moved on every row anyway,
			// and `Motion` changed only on the few rows that hit a wall. A
			// system that could name those rows should mark those rows - the
			// cost of marking all of them is a delta carrying values that
			// already match at the other end, which is bandwidth rather than
			// error. Under-reporting a bounce would be a client that watches an
			// entity keep going through the wall until the next snapshot.
			store.MarkAllChanged<Transform>();
			store.MarkAllChanged<Motion>();
		}

		// PreRender on a headless server is where replication will hang:
		// deriving what to send is the same shape as deriving what to draw, and
		// neither may mutate simulation state.
		//
		// The count comes from the world. It used to be captured at build time
		// because asking the store cost a fresh query every tick and dominated
		// the measurement; CountMatching now keeps its query, so the world can
		// be asked and the second copy of the number is gone.
		void Report(Store &store) {
			engine::core::Metrics::Count(
				"world.entities", static_cast<double>(store.CountMatching<Transform>())
			);
		}
	}

	void PrepareSimulation(Store &store, Scheduler &scheduler, double physicsTickRate) {
		// The cell size is measured rather than authored, for the reason the
		// studio gives: `PreparePhysicsWorld` with no size means "measure it",
		// and a constant tuned now against a synthetic slab would be the wrong
		// number for whichever world actually gains a tick.
		engine::physics::PreparePhysicsWorld(store);

		// **The world's rate, pushed down rather than read up.** `physics` sits
		// above `world` and could name `WorldSettings`, but a physics library
		// that knows what a universe world is has stopped being a physics
		// library - so the host that owns both hands the number over, exactly
		// as it does for the cell size.
		engine::physics::SetPhysicsTickRate(store, physicsTickRate);

		engine::physics::RegisterPhysicsSystems(scheduler);

		// Non-overwriting, so a world that authored its own vector - under
		// water, on the moon - keeps it and a world that said nothing gets
		// Earth's.
		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(scheduler);

		// **This process is the authority, so this process reclaims.** A body
		// handed to a player who then leaves is owned by a dead entity, and
		// nothing would ever simulate it again. Registered even though nothing
		// reads ownership yet, because the day something does is the day this
		// being absent is a bug rather than a gap.
		engine::scene::RegisterOwnershipSystem(scheduler);

		// **Who a teleport brings in, and it must not depend on scripts.** A
		// destination is chosen by a script in *another* world, so a world can be
		// somebody's destination without containing a line of code - and
		// admitting used to happen inside the Luau runtime's own delivery pump.
		// A world with no runtime took the payload into its inbox and left it
		// there: destroyed in the world you left, never built in the world you
		// went to. `script::RegisterTeleportAdmission` carries the argument.
		engine::script::RegisterTeleportAdmission(scheduler);

		// **A server grounds, steps and poses its own characters.** Until this
		// the three lived in `mono.client` alone, so a character on a dedicated
		// server walked and could never jump - `Humanoid::Grounded` had no
		// writer. The input half stays out: a server has no keyboard, and what
		// moves a client-owned character here is its submitted `Motion`.
		engine::physics::RegisterCharacterSystems(scheduler);

		// **The respawn loop, and it is here rather than in `physics` because
		// half of it is `gui`'s.** A spawn is two things: a new body, which is
		// `scene::UpdateRespawns`, and a fresh copy of `StarterGui` in the
		// player's own `PlayerGui`, which is `gui::ResetPlayerGui` - and `scene`
		// may not link `gui`, so the two meet in whoever hosts. That is the same
		// split `ResetPlayerGui`'s own header states from the other side, and it
		// is why the pass hands back who it spawned instead of doing it all.
		//
		// **`PreSimulation`, after the character link.** `LinkPlayerCharacters`
		// is what releases a player whose model was destroyed, so running before
		// it would measure the delay from a tick later than the death - and
		// `RegisterCharacterSystems` above registers that one first, which is the
		// ordering the scheduler gives between two entries only by their being
		// in the same phase. Composed into its own entry rather than into
		// `character.link` because it reads that system's writes and nothing
		// else reads its own.
		scheduler.Add(
			"player.respawn",
			Phase::PreSimulation,
			[](Store &store) {
				std::vector<engine::ecs::Entity> spawned;
				if (engine::scene::UpdateRespawns(store, spawned) == 0) {
					return;
				}

				for (const engine::ecs::Entity player : spawned) {
					(void)engine::gui::ResetPlayerGui(store, player);
				}
			},
			SystemOrder{{}, {"character.link"}}
		);

		// **The authority derives the derived half of an `Attachment` too**, and
		// until v0.19 only `mono.client`'s presentation did. Nothing here draws a
		// beam, so this is not about the cache: `ResolveAttachments` is what
		// *reports* the write, and a reported write is what makes
		// `Attachment.WorldCFrame` and `WorldPosition` fire their change signal.
		// Unscheduled, a server script's `GetPropertyChangedSignal("WorldCFrame")`
		// never fired at all while the same script on a client did - one fact
		// with two answers, which is rule 2 with a part moving under it.
		//
		// **`PostSimulation`, which is the phase the authority's answer is
		// finished in.** The parts have stopped moving for this tick,
		// `World::Step` flushes signals immediately after this phase, and
		// extraction reads the change bits after that - so the frame that is
		// signalled, the frame that is stored and the frame that crosses are all
		// the one this tick ended at. `PreSimulation` - which is where the client
		// registers its first copy, because `refresh-emitters` needs a spawn
		// point before the tick moves anything - would resolve against last
		// tick's transforms here and there is nothing on a server that wants
		// that.
		//
		// **A world with no attachments pays an archetype match and nothing
		// else**, which is what makes it safe to register unconditionally.
		//
		// **It does put bytes on the wire, and that is worth knowing.**
		// `scene.Attachment` is replicated whole, so an attachment whose part
		// moves is now a dirty row every tick where it was dirty never - 56
		// bytes an attachment per tick that a client then recomputes anyway,
		// against its own interpolated transforms. The row that is *authored*
		// has to cross; the derived half arguably does not, and taking it off
		// would mean a hand-written pair for this one component. Left as it is
		// until a stress run says the traffic matters: `just stress` is where
		// that number would come from.
		scheduler.Add("resolve-attachments", Phase::PostSimulation, [](Store &store) {
			(void)engine::scene::ResolveAttachments(store);
		});
	}

	void RegisterPlaceholderComponents() {
		// The placeholder's first system opens a Postbox. Register its resources
		// here with the rest of the program's stable component names so a test or
		// tool that prepares the scheduler before constructing a Universe cannot
		// mint them under compiler spellings.
		engine::world::RegisterMailboxTypes();

		// The shared set first, and under `scene`'s names rather than this
		// program's. A client registers the same strings, which is what makes a
		// snapshot resolve on the far side without a translation layer - there
		// used to be one, and keeping two declarations of one wire type in step
		// by hand is what it cost.
		engine::scene::RegisterSceneComponents();

		// `PhysicsWorld` is a resource, and a resource is keyed by a component
		// id like any other - so one never registered here is minted by the
		// first `SetResource`, under whatever the compiler calls the type, and
		// aborts outright once the table is sealed. Registered on every path
		// including the placeholder's, because the cost is a hash lookup and
		// the failure is a crash in a program that did nothing wrong.
		engine::physics::RegisterPhysicsComponents();

		// `Chatter` holds a `core::Name`, which is a process-local id - writing
		// it as an object representation would restore as whatever name
		// happened to take that id in the reading process. So it is written as
		// text, which is the rule every name on the wire follows.
		Components::Register<Chatter>(
			"server.Chatter",
			[](engine::core::ByteWriter &writer, const void *values, size_t count) {
				const auto *chatter = static_cast<const Chatter *>(values);
				for (size_t index = 0; index < count; index++) {
					writer.WriteName(chatter[index].Topic);
				}
			},
			[](engine::core::ByteReader &reader, void *values, size_t count) {
				auto *chatter = static_cast<Chatter *>(values);
				for (size_t index = 0; index < count; index++) {
					chatter[index].Topic = reader.ReadName();
				}
			}
		);
		Components::Register<Heard>(
			"server.Heard",
			[](engine::core::ByteWriter &writer, const void *values, size_t count) {
				const auto *heard = static_cast<const Heard *>(values);
				for (size_t index = 0; index < count; index++) {
					writer.WriteUInt64(heard[index].Count);
					writer.WriteName(heard[index].From);
				}
			},
			[](engine::core::ByteReader &reader, void *values, size_t count) {
				auto *heard = static_cast<Heard *>(values);
				for (size_t index = 0; index < count; index++) {
					heard[index].Count = reader.ReadUInt64();
					heard[index].From = reader.ReadName();
				}
			}
		);
	}

	// Subscribes once, then publishes this world's name and tick every tick.
	//
	// Runs in PreSimulation so a publish and the deliveries it produces sit on
	// either side of the barrier the way any other bus traffic does.
	void Chat(Store &store) {
		const Chatter *chatter = store.Resource<Chatter>();
		if (chatter == nullptr) {
			return;
		}

		engine::world::Postbox box(store);
		const std::string topic(chatter->Topic.Text());

		// Everything that arrived, before anything is sent. A world reading its
		// own publish back would mean the driver stopped filtering it out.
		Heard heard = store.Resource<Heard>() == nullptr ? Heard{} : *store.Resource<Heard>();
		for (const engine::world::Delivery &delivery : box.Deliveries()) {
			if (delivery.Key != chatter->Topic) {
				continue;
			}
			heard.Count++;
			heard.From = delivery.From;
		}
		store.SetResource(heard);

		if (store.Time().Tick <= 1) {
			// Takes effect at the next barrier, so nothing published this tick
			// comes back - which is the honest answer, since the subscription
			// did not exist when it was sent.
			box.Subscribe(topic);
			return;
		}

		const std::string message = std::string(store.Name()) + ":" + std::to_string(store.Time().Tick);
		std::vector<std::byte> payload(message.size());
		std::memcpy(payload.data(), message.data(), message.size());
		box.Publish(topic, payload);
	}

	void RegisterPlaceholderSystems(Store &, Scheduler &scheduler) {
		scheduler.Add("chat", Phase::PreSimulation, Chat);
		scheduler.Add("integrate", Phase::Simulation, Integrate);
		scheduler.Add("bounce", Phase::PostSimulation, Bounce);
		scheduler.Add("report", Phase::PreRender, Report);
	}

	void BuildPlaceholderWorld(Store &store, Scheduler &scheduler, uint32_t count) {
		constexpr float HALF_EXTENT = 64.0f;

		// **The world's size and the replication wire's position grid are one
		// decision made in two files**, and this is the line that keeps them
		// together. A world authored past the grid does not fail to replicate -
		// its entities are clamped and pile up against a wall that is not this
		// one - so the size is checked where it is chosen rather than
		// discovered per entity on a client. `scene/Wire.hpp` states the grid
		// and the error it introduces.
		static_assert(
			engine::scene::WireCoversWorld(HALF_EXTENT),
			"this world reaches further than the replication wire's position grid"
		);

		// Before the first `Set`, on every path. An unregistered component is
		// minted under whatever the compiler calls the type, which is not a
		// name a snapshot written by one build can be read back by another
		// under. Idempotent, so calling it here as well as in `Server::Run`'s
		// start-up costs a hash lookup.
		RegisterPlaceholderComponents();

		store.SetResource(WorldBounds{HALF_EXTENT});

		for (uint32_t index = 0; index < count; index++) {
			const Entity entity = store.Create();

			store.Set<Transform>(
				entity,
				Transform{CFrame{Vector3{
					Random::Range(index, 2u, -HALF_EXTENT, HALF_EXTENT),
					Random::Range(index, 3u, -HALF_EXTENT, HALF_EXTENT),
					Random::Range(index, 5u, -HALF_EXTENT, HALF_EXTENT),
				}}}
			);

			// Linear only. The placeholder scene has never tumbled its
			// entities, and giving them an angular velocity nothing integrates
			// would be a field that reads as live and is not.
			store.Set<Motion>(
				entity,
				Motion{
					Vector3{
						Random::Range(index, 7u, -10.0f, 10.0f),
						Random::Range(index, 11u, -10.0f, 10.0f),
						Random::Range(index, 13u, -10.0f, 10.0f),
					},
					Vector3::Zero,
				}
			);

			// **What a headless server is doing with a size and a colour.** It
			// is not drawing them - this binary contains no renderer. It is
			// describing what these things *are*, which is what a client
			// replicating this world needs in order to draw it, and what the
			// draw list a hosted world publishes would carry. Before v0.4 a
			// joining client received two vectors and had nothing to make a
			// scene out of.
			store.Set<Bounds>(entity, Bounds{});

			Visual visual;
			visual.Tint = Color3::FromLinear(
				Random::Range(index, 19u, 0.15f, 0.90f),
				Random::Range(index, 23u, 0.20f, 0.80f),
				Random::Range(index, 29u, 0.35f, 0.95f)
			);
			store.Set<Visual>(entity, visual);
		}

		ENGINE_INFO("placeholder world: {} entities", count);
		RegisterPlaceholderSystems(store, scheduler);
	}
}
