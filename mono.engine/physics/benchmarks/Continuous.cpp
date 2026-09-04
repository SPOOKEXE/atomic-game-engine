// Cost of the full-motion conservative advance used by continuous collision.

#include "ConvexQuery.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.continuous")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::ConvexSweep;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::ShapeInstance;
using engine::physics::SweepConvexMotion;
using engine::physics::SweepFastBodies;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::ShapeKind;
using engine::scene::Simulated;
using engine::scene::Transform;
using engine::testing::Consume;

namespace continuous_bench {
	constexpr size_t PAIR_COUNT = 4096;

	const ShapeInstance FIRST{CFrame{Vector3{-3.0f, 0.0f, 0.0f}}, Vector3{0.5f, 0.5f, 0.5f}, ShapeKind::Box};
	const ShapeInstance SECOND{CFrame{Vector3{3.0f, 0.0f, 0.0f}}, Vector3{0.5f, 0.5f, 0.5f}, ShapeKind::Box};
	const ShapeInstance BAR{CFrame{Vector3::Zero}, Vector3{0.05f, 2.0f, 0.05f}, ShapeKind::Box};
	const ShapeInstance BLOCK{CFrame{Vector3{-1.5f, 0.0f, 0.0f}}, Vector3{0.1f, 0.1f, 0.1f}, ShapeKind::Box};

	struct SparseWorld {
		Store World{"physics.bench.continuous.sparse"};
		Entity First;
		Entity Second;

		SparseWorld() {
			PreparePhysicsWorld(World, 4.0f);
			Collider collider;
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			for (size_t body = 0; body < 8192; body++) {
				const Entity entity = World.Create();
				World.Set<Transform>(
					entity, Transform{CFrame{Vector3{static_cast<float>(body) * 3.0f, 20.0f, 0.0f}}}
				);
				World.Set<Motion>(entity, Motion{});
				World.Set<Collider>(entity, collider);
				World.Set<Simulated>(entity, Simulated{});
			}
			First = World.Create();
			Second = World.Create();
			for (const Entity entity : {First, Second}) {
				World.Set<Motion>(entity, Motion{});
				World.Set<Collider>(entity, collider);
				World.Set<Simulated>(entity, Simulated{});
			}
		}

		void Reset() {
			World.Set<Transform>(First, Transform{CFrame{Vector3{2.0f, 0.0f, 0.0f}}});
			World.Set<Transform>(Second, Transform{CFrame{Vector3{-2.0f, 0.0f, 0.0f}}});
			World.GetMutable<Motion>(First)->Linear = Vector3{300.0f, 0.0f, 0.0f};
			World.GetMutable<Motion>(Second)->Linear = Vector3{-300.0f, 0.0f, 0.0f};
		}
	};

	struct SweptGrid {
		static constexpr size_t BODY_COUNT = 8192;

		std::vector<engine::spatial::Proxy> Proxies;
		std::vector<uint64_t> Candidates = std::vector<uint64_t>(BODY_COUNT);
		engine::spatial::HashGrid Index{2.0f};

		SweptGrid() {
			Proxies.reserve(BODY_COUNT);
			for (size_t body = 0; body < BODY_COUNT; body++) {
				const float x = static_cast<float>(body % 32) * 0.75f;
				const float y = static_cast<float>((body / 32) % 16) * 0.75f;
				const float z = static_cast<float>(body / (32 * 16)) * 0.75f;
				Proxies.push_back(
					engine::spatial::Proxy{
						static_cast<uint64_t>(body),
						engine::core::AABB::FromCentre(Vector3{x, y, z}, Vector3{0.5f, 0.5f, 0.5f}),
						engine::spatial::LayerMask::All(),
					}
				);
			}
			Index.Rebuild(Proxies);
		}
	};

	struct CascadeWorld {
		Store World{"physics.bench.continuous.cascade"};
		Entity Late;
		Entity Middle;
		Entity Early;

		CascadeWorld() {
			PreparePhysicsWorld(World, 4.0f);
			Collider collider;
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			for (Entity *body : {&Late, &Middle, &Early}) {
				*body = World.Create();
				World.Set<Motion>(*body, Motion{});
				World.Set<Collider>(*body, collider);
				World.Set<Simulated>(*body, Simulated{});
			}
		}

		void Reset() {
			World.Set<Transform>(Late, Transform{CFrame{Vector3{10.0f, 0.0f, 0.0f}}});
			World.Set<Transform>(Middle, Transform{CFrame{Vector3{10.0f, 0.0f, 0.0f}}});
			World.Set<Transform>(Early, Transform{CFrame{Vector3{3.0f, 0.0f, 0.0f}}});
			World.GetMutable<Motion>(Late)->Linear = Vector3{1200.0f, 0.0f, 0.0f};
			World.GetMutable<Motion>(Middle)->Linear = Vector3{600.0f, 0.0f, 0.0f};
			World.GetMutable<Motion>(Early)->Linear = Vector3::Zero;
		}
	};
}

using namespace continuous_bench;

BENCH("4096 crossing dynamic pairs", 8) {
	for (size_t pair = 0; pair < PAIR_COUNT; pair++) {
		const ConvexSweep hit = SweepConvexMotion(
			FIRST,
			Vector3{5.0f, 0.0f, 0.0f},
			Vector3::Zero,
			SECOND,
			Vector3{-5.0f, 0.0f, 0.0f},
			Vector3::Zero,
			1.0f
		);
		Consume(hit.Fraction);
	}
}

BENCH("4096 rotational impacts", 8) {
	for (size_t pair = 0; pair < PAIR_COUNT; pair++) {
		const ConvexSweep hit = SweepConvexMotion(
			BAR, Vector3::Zero, Vector3{0.0f, 0.0f, 2.0f}, BLOCK, Vector3::Zero, Vector3::Zero, 1.0f
		);
		Consume(hit.Fraction);
	}
}

BENCH("8192 still bodies and one fast pair", 4) {
	static SparseWorld sparse;
	for (int pass = 0; pass < 4; pass++) {
		sparse.Reset();
		SweepFastBodies(sparse.World);
		Consume(sparse.World.Resource<PhysicsWorld>()->SweptBodies());
	}
}

BENCH("frozen cascade resweeps", 4) {
	static CascadeWorld cascade;
	for (int pass = 0; pass < 64; pass++) {
		cascade.Reset();
		SweepFastBodies(cascade.World);
		Consume(cascade.World.Resource<PhysicsWorld>()->SweptBodies());
	}
}

BENCH("duplicate swept-grid gather", 4) {
	static SweptGrid grid;
	size_t gathered = 0;
	for (size_t pass = 0; pass < 8; pass++) {
		for (size_t body = 0; body < grid.Proxies.size(); body++) {
			gathered +=
				engine::spatial::OverlapBox(
					grid.Index, grid.Proxies[body].Bounds, engine::spatial::LayerMask::All(), grid.Candidates
				)
					.Written;
		}
	}
	Consume(gathered);
}

BENCH("ordered swept-grid gather", 4) {
	static SweptGrid grid;
	size_t gathered = 0;
	for (size_t pass = 0; pass < 8; pass++) {
		for (size_t body = 0; body < grid.Proxies.size(); body++) {
			gathered += engine::spatial::OverlapBoxAfterId(
							grid.Index,
							grid.Proxies[body].Bounds,
							engine::spatial::LayerMask::All(),
							static_cast<uint64_t>(body),
							grid.Candidates
			)
							.Written;
		}
	}
	Consume(gathered);
}

BENCH("ordered swept-grid shape gather", 4) {
	static SweptGrid grid;
	size_t gathered = 0;
	for (size_t pass = 0; pass < 8; pass++) {
		for (size_t body = 0; body < grid.Proxies.size(); body++) {
			gathered += engine::spatial::ShapeCastAfterId(
							grid.Index,
							grid.Proxies[body].Bounds,
							Vector3{1.5f, 1.5f, 0.0f},
							engine::spatial::LayerMask::All(),
							static_cast<uint64_t>(body),
							grid.Candidates
			)
							.Written;
		}
	}
	Consume(gathered);
}
