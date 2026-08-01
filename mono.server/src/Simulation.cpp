#include <server/Simulation.hpp>

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Random.hpp>

#include <cmath>

namespace server {

	using engine::core::Random;
	using engine::core::Vector3;
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
					float *axis[] = { &position.Value.X, &position.Value.Y, &position.Value.Z };
					float *speed[] = { &velocity.Value.X, &velocity.Value.Y, &velocity.Value.Z };

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
			engine::core::Metrics::Count("world.entities",
				static_cast<double>(store.CountMatching<Position>()));
		}
	}

	void BuildPlaceholderWorld(Store &store, Scheduler &scheduler, uint32_t count) {
		constexpr float HALF_EXTENT = 64.0f;

		store.SetResource(WorldBounds { HALF_EXTENT });

		for (uint32_t index = 0; index < count; index++) {
			const Entity entity = store.Create();

			store.Set<Position>(entity, Position { Vector3 {
				Random::Range(index, 2u, -HALF_EXTENT, HALF_EXTENT),
				Random::Range(index, 3u, -HALF_EXTENT, HALF_EXTENT),
				Random::Range(index, 5u, -HALF_EXTENT, HALF_EXTENT),
			} });
			store.Set<Velocity>(entity, Velocity { Vector3 {
				Random::Range(index, 7u, -10.0f, 10.0f),
				Random::Range(index, 11u, -10.0f, 10.0f),
				Random::Range(index, 13u, -10.0f, 10.0f),
			} });
		}

		ENGINE_INFO("placeholder world: {} entities", count);

		scheduler.Add("integrate", Phase::Simulation, Integrate);
		scheduler.Add("bounce", Phase::PostSimulation, Bounce);
		scheduler.Add("report", Phase::PreRender, Report);
	}
}
