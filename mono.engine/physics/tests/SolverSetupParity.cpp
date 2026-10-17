// Full pipeline outputs and the warm-start cache are exported only on explicit opt-in.
#include "PipelineInternals.hpp"
#include "fixtures/CellSizeParity.hpp"
#include "fixtures/SteppingScene.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

TEST_SUITE_ID("engine.physics.solver-setup-parity")
TEST_DEPENDS("engine.physics.solver")
TEST_DEPENDS("engine.physics.continuous")

TEST_CASE(
	"solver setup preserves mixed-contact pipeline output over warm starts", "[physics][solver-setup-parity]"
) {
	using namespace engine;
	using namespace physics::testing;
	bool sawMultiplePoints = false;
	bool sawSpeculative = false;
	bool sawWarmStart = false;
	for (const Layout layout : {Layout::Pile, Layout::Stacks, Layout::Scattered, Layout::Fast}) {
		CAPTURE(static_cast<int>(layout));
		auto store = BuildScene(layout, 64, 4.0f);
		// An isolated near-floor pair starts speculative; gravity then brings it to contact.
		const ecs::Entity nearFloor = store->Create();
		store->Set<scene::Transform>(nearFloor, scene::Transform{core::CFrame{core::Vector3{50, .5015f, 0}}});
		scene::Collider box;
		box.Extent = core::Vector3{.5f, .5f, .5f};
		store->Set<scene::Collider>(nearFloor, box);
		store->Set<scene::RigidBody>(nearFloor, scene::RigidBody{});
		scene::Motion approach;
		approach.Linear.Y = 9.81f / 60 - .03f;
		store->Set<scene::Motion>(nearFloor, approach);
		store->Set<scene::Simulated>(nearFloor, scene::Simulated{});
		for (size_t tick = 0; tick < 23; tick++) {
			CAPTURE(tick);
			store->Each<scene::Motion>([](ecs::Entity, scene::Motion &motion) {
				motion.Linear.Y -= 9.81f / 60;
			});
			Tick(*store);
			auto &world = *store->ResourceMutable<physics::PhysicsWorld>();
			for (const auto &manifold : world.Manifolds())
				sawMultiplePoints |= manifold.PointCount > 1;
			sawSpeculative |= !physics::PipelineInternals::SpeculativeManifolds(world).empty();
			std::vector<uint64_t> impulses;
			for (const auto &impulse : physics::PipelineInternals::ImpulseCache(world)) {
				impulses.push_back(impulse.A.Id);
				impulses.push_back(impulse.B.Id);
				impulses.push_back(impulse.Feature);
				Append(impulses, impulse.Normal);
				Append(impulses, impulse.Tangent[0]);
				Append(impulses, impulse.Tangent[1]);
				Append(impulses, impulse.SpeculativeClosingSpeed);
				sawWarmStart |= tick > 0 && impulse.Normal != 0;
			}
			const auto snapshot = Capture(*store);
			REQUIRE(store->CountMatching<scene::Transform>() == 66);
			store->Query<const scene::Transform>().Each([](ecs::Entity, const scene::Transform &transform) {
				REQUIRE(std::isfinite(transform.Frame.Position.X));
				REQUIRE(std::isfinite(transform.Frame.Position.Y));
				REQUIRE(std::isfinite(transform.Frame.Position.Z));
			});
			DumpSnapshot("mixed", static_cast<size_t>(layout), tick + 1, snapshot);
			DumpWords("mixed", static_cast<size_t>(layout), tick + 1, "impulses", impulses);
		}
	}
	CHECK(sawMultiplePoints);
	CHECK(sawSpeculative);
	CHECK(sawWarmStart);
}
