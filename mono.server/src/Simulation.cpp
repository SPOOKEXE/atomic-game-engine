#include <engine/core/Metrics.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/WeatherGround.hpp>
#include <engine/gui/Services.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/PlayerGui.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/world/Postbox.hpp>

#include <cstring>
#include <server/Simulation.hpp>
#include <string>
#include <vector>

namespace server {

	using engine::ecs::Components;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::ecs::SystemOrder;
	using engine::scene::Transform;

	namespace {
		void Report(Store &store) {
			engine::core::Metrics::Count(
				"world.entities", static_cast<double>(store.CountMatching<Transform>())
			);
		}
	}

	void PrepareSimulation(Store &store, Scheduler &scheduler, double physicsTickRate) {
		const bool restoredClock = store.Resource<engine::physics::PhysicsClock>() != nullptr;
		if (store.Resource<engine::physics::PhysicsWorld>() == nullptr) {
			// A fresh world measures its cell size from the colliders it gains.
			engine::physics::PreparePhysicsWorld(store);
		} else {
			// Restoring a resource does not restore the store's observation setup.
			// The broad phase needs changed Transform and Collider rows.
			store.Observe<engine::scene::Transform>();
			store.Observe<engine::scene::Collider>();
		}
		// The host owns the world rate. Replay keeps the serialized clock,
		// including its accumulator, rather than resetting it here.
		if (!restoredClock) {
			if (store.Resource<engine::physics::PhysicsClock>() == nullptr)
				store.SetResource(engine::physics::PhysicsClock{});
			engine::physics::SetPhysicsTickRate(store, physicsTickRate);
		}

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
		engine::weather::RegisterWeatherClockSystem(scheduler);
		engine::game::RegisterWeatherGroundSystem(scheduler);

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
		// player's own `PlayerGui`, which is `script::ResetPlayerGui` - and `scene`
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
					(void)engine::script::ResetPlayerGui(store, player);
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

	void RegisterServerComponents() {
		engine::weather::RegisterWeatherRuntimeComponents();
		// The optional bus diagnostic opens a Postbox. Register its resources
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
		// Factory packages can open a script runtime after the server seals the
		// component table. Its effect probes need these stable ids beforehand.
		engine::effects::RegisterEffectComponents();

		// `PhysicsWorld` is a resource, and a resource is keyed by a component
		// id like any other - so one never registered here is minted by the
		// first `SetResource`, under whatever the compiler calls the type, and
		// aborts outright once the table is sealed. Registered on every path
		// including the default scene, because the cost is a hash lookup and
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

	void RegisterServerDiagnostics(Scheduler &scheduler) {
		scheduler.Add("chat", Phase::PreSimulation, Chat);
		scheduler.Add("report", Phase::PreRender, Report);
	}
}
