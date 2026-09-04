// Cost of stopping close, fast pairs before their first overlap.
//
// Each pair has a one-millimetre gap and closes by two centimetres in one
// 60 Hz step. Pairs are spaced apart so the row grows linearly and measures
// speculative contact work rather than a quadratic pile-up.

#include "PipelineInternals.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Bench.hpp>

#include <memory>

TEST_SUITE_ID("engine.physics.bench.speculative-contacts")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::physics::PreparePhysicsWorld;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::Transform;
using engine::testing::Consume;

namespace speculative_contacts_bench {
	constexpr size_t PAIR_COUNT = 4096;

	Store &ClosingPairs() {
		static std::unique_ptr<Store> store;
		if (store != nullptr) {
			return *store;
		}

		store = std::make_unique<Store>("physics.bench.speculative-contacts");
		PreparePhysicsWorld(*store, 4.0f);
		for (size_t pair = 0; pair < PAIR_COUNT; pair++) {
			const float base = static_cast<float>(pair) * 3.0f;
			for (size_t side = 0; side < 2; side++) {
				const Entity entity = store->Create();
				store->Set<Transform>(
					entity, Transform{CFrame{Vector3{base + static_cast<float>(side) * 1.001f, 0.0f, 0.0f}}}
				);
				Collider collider;
				collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
				store->Set<Collider>(entity, collider);
				Motion motion;
				motion.Linear = Vector3{side == 0 ? 0.6f : -0.6f, 0.0f, 0.0f};
				store->Set<Motion>(entity, motion);
			}
		}

		SyncBroadphase(*store);
		BroadPhase(*store);
		return *store;
	}
}

using namespace speculative_contacts_bench;

BENCH("Narrow phase · 4096 separated closing pairs", 30) {
	Store &store = ClosingPairs();
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	for (int pass = 0; pass < 30; pass++) {
		NarrowPhase(store);
		Consume(PipelineInternals::SpeculativeManifolds(world).size());
	}
}

BENCH("Sync, broad and narrow · 4096 separated closing pairs", 20) {
	Store &store = ClosingPairs();
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	for (int pass = 0; pass < 20; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
		Consume(PipelineInternals::SpeculativeManifolds(world).size());
	}
}
