#pragma once

// Shared deterministic scenes for full physics tick tests and benchmarks.

#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::physics::testing {
	using core::CFrame;
	using core::Random;
	using core::Vector3;
	using ecs::Entity;
	using ecs::Store;
	using scene::Collider;
	using scene::Motion;
	using scene::RigidBody;
	using scene::Simulated;
	using scene::Transform;

	// Bodies per scene. A thousand is a busy room, four thousand a level, and
	// sixteen thousand is past what a game should ship and exactly where a
	// growth curve stops being a straight line if it is going to.
	inline constexpr size_t SMALL = 1000;
	inline constexpr size_t MEDIUM = 4000;
	inline constexpr size_t LARGE = 16'000;

	// Deterministic body layouts shared by the tick fixtures.
	enum class Layout {
		// Stacks on a wide floor, spread across it. The ordinary case.
		Stacks,

		// Bodies crowded into a three-metre cube. The pathological case.
		Pile,

		// Bodies eight metres apart, touching only the floor.
		Scattered,

		// Clusters pair one large collider with eight smaller neighbors.
		MixedScale,

		// Stacks, but every body given a velocity large enough that
		// `SweepFastBodies` has to sweep it.
		Fast,
	};

	// Keep the pipeline order without adding scheduler dispatch to measured work.
	inline void Tick(Store &store) {
		store.AdvanceTick(1.0f / 60.0f);
		IntegrateMotion(store);
		SweepFastBodies(store);
		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
		Solve(store);
		Publish(store);
	}

	// Where body `index` of `count` goes, under a layout.
	inline Vector3 PlaceOf(Layout layout, size_t index, size_t count) {
		const auto seed = static_cast<uint32_t>(index);
		switch (layout) {
		case Layout::Pile:
			// Dense overlap keeps candidate and contact generation busy at any cell size.
			return Vector3{
				Random::Range(seed, 3, -1.5f, 1.5f),
				0.5f + Random::Range(seed, 5, 0.0f, 3.0f),
				Random::Range(seed, 7, -1.5f, 1.5f),
			};

		case Layout::Scattered: {
			// Start at floor height so longer runs do not change from free fall to contact.
			const auto side = static_cast<size_t>(1 + std::sqrt(static_cast<double>(count)));
			return Vector3{
				static_cast<float>(index % side) * 8.0f,
				0.5f,
				static_cast<float>(index / side) * 8.0f,
			};
		}

		case Layout::MixedScale: {
			const size_t group = index / 9;
			const size_t member = index % 9;
			constexpr size_t columns = 24;
			const float groupX = static_cast<float>(group % columns) * 14.0f - 161.0f;
			const float groupZ = static_cast<float>(group / columns) * 14.0f - 126.0f;
			if (member == 0) return Vector3{groupX, 2.0f, groupZ};
			return Vector3{
				groupX + ((member & 1u) == 0 ? -2.25f : 2.25f),
				(member & 4u) == 0 ? 0.5f : 1.5f,
				groupZ + ((member & 2u) == 0 ? -2.25f : 2.25f),
			};
		}

		case Layout::Stacks:
		case Layout::Fast:
		default: {
			// Spread four-body stacks over an area proportional to count to hold density fixed.
			const size_t column = index / 4;
			const size_t level = index % 4;
			const auto columnSeed = static_cast<uint32_t>(column);
			const float half = 3.0f * std::sqrt(static_cast<float>(count) / 4.0f);
			return Vector3{
				Random::Range(columnSeed, 3, -half, half),
				0.5f + static_cast<float>(level) * 0.999f,
				Random::Range(columnSeed, 5, -half, half),
			};
		}
		}
	}

	// Build the authored scene; callers choose how many warmup ticks to run.
	inline std::unique_ptr<ecs::Store> BuildScene(Layout layout, size_t count, float cellSize) {
		auto store = std::make_unique<Store>("physics.bench.stepping");
		PreparePhysicsWorld(*store, cellSize);

		// Keep the same anchored floor in every layout. This fixture applies no
		// gravity; its pile cost measures overlap resolution and the authored fast
		// layout measures downward motion.
		const Entity floor = store->Create();
		store->Set<Transform>(floor, Transform{CFrame{Vector3{0.0f, -1.0f, 0.0f}}});
		Collider ground;
		ground.Extent = Vector3{4096.0f, 1.0f, 4096.0f};
		store->Set<Collider>(floor, ground);

		for (size_t index = 0; index < count; index++) {
			const Entity entity = store->Create();
			store->Set<Transform>(entity, Transform{CFrame{PlaceOf(layout, index, count)}});

			Collider collider;
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			if (layout == Layout::MixedScale && index % 9 == 0) {
				collider.Extent = Vector3{2.5f, 2.0f, 2.5f};
			}
			store->Set<Collider>(entity, collider);

			Motion motion;
			if (layout == Layout::Fast) {
				// Far enough per step that a body would pass through its own
				// depth, which is what `SweepFastBodies` exists for.
				motion.Linear = Vector3{0.0f, -60.0f, 0.0f};
			}
			store->Set<Motion>(entity, motion);
			store->Set<RigidBody>(entity, RigidBody{});

			// Simulated makes boxes movable by the solver; the floor stays anchored.
			store->Set<Simulated>(entity, Simulated{});
		}

		return store;
	}
}
