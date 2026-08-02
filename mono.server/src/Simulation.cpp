#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/world/Postbox.hpp>

#include <cmath>
#include <cstring>
#include <server/Simulation.hpp>
#include <string>
#include <vector>

namespace server {

	using engine::core::Random;
	using engine::core::Vector3;
	using engine::ecs::Components;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;

	namespace {
		// This used to be a copy of the mixer in mono.client/src/Demo.cpp, with
		// a comment saying so. Both are engine::core::Random now — the reason
		// was always "a seeded standard generator may differ between standard
		// libraries", which was never a client concern or a server one.

		// --- systems -------------------------------------------------------
		//
		// Plain functions, capturing nothing. Everything they need is in the
		// world: the delta comes from its clock and the box comes from its
		// resources.

		// Parallel within the tick. The body reads and writes one row and
		// nothing else, which is the whole contract — anything structural
		// would abort on the store's affinity check from a worker, and that is
		// the intended outcome rather than a limitation.
		//
		// It still blocks until every entity is done, so the tick remains one
		// thing that starts and finishes and a recorded run still replays.
		void Integrate(Store &store) {
			const float delta = store.Time().Delta;

			store.EachParallel<Position, const Velocity>(
				[delta](Entity, Position &position, const Velocity &velocity) {
					position.Value = position.Value + velocity.Value * delta;
				}
			);
		}

		void Bounce(Store &store) {
			// Read once for the whole world rather than once per entity. When
			// this was a component it was the same four bytes on every row, and
			// the loop paid a load for a number it already knew.
			const float halfExtent = store.Resource<WorldBounds>()->HalfExtent;

			store.EachParallel<Position, Velocity>(
				[halfExtent](Entity, Position &position, Velocity &velocity) {
					// Reflect off each wall independently. The position is
					// clamped as well as the velocity flipped, because
					// flipping alone lets an entity that overshot on a long
					// tick sit outside the box flipping every tick.
					float *axis[] = {&position.Value.X, &position.Value.Y, &position.Value.Z};
					float *speed[] = {&velocity.Value.X, &velocity.Value.Y, &velocity.Value.Z};

					for (int index = 0; index < 3; index++) {
						if (*axis[index] > halfExtent) {
							*axis[index] = halfExtent;
							*speed[index] = -std::abs(*speed[index]);
						} else if (*axis[index] < -halfExtent) {
							*axis[index] = -halfExtent;
							*speed[index] = std::abs(*speed[index]);
						}
					}
				}
			);
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
				"world.entities", static_cast<double>(store.CountMatching<Position>())
			);
		}
	}

	void RegisterPlaceholderComponents() {
		Components::Register<Position>("server.Position");
		Components::Register<Velocity>("server.Velocity");
		Components::Register<WorldBounds>("server.WorldBounds");

		// `Chatter` holds a `core::Name`, which is a process-local id — writing
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
			// comes back — which is the honest answer, since the subscription
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

		store.SetResource(WorldBounds{HALF_EXTENT});

		for (uint32_t index = 0; index < count; index++) {
			const Entity entity = store.Create();

			store.Set<Position>(
				entity,
				Position{Vector3{
					Random::Range(index, 2u, -HALF_EXTENT, HALF_EXTENT),
					Random::Range(index, 3u, -HALF_EXTENT, HALF_EXTENT),
					Random::Range(index, 5u, -HALF_EXTENT, HALF_EXTENT),
				}}
			);
			store.Set<Velocity>(
				entity,
				Velocity{Vector3{
					Random::Range(index, 7u, -10.0f, 10.0f),
					Random::Range(index, 11u, -10.0f, 10.0f),
					Random::Range(index, 13u, -10.0f, 10.0f),
				}}
			);
		}

		ENGINE_INFO("placeholder world: {} entities", count);
		RegisterPlaceholderSystems(store, scheduler);
	}
}
