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
#include <engine/testing/Bench.hpp>

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
