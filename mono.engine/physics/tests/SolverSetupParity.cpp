#include "ImpulseCacheCursor.hpp"
// Full pipeline outputs and the warm-start cache are exported only on explicit opt-in.
#include "PipelineInternals.hpp"
#include "fixtures/CellSizeParity.hpp"
#include "fixtures/SteppingScene.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.solver-setup-parity")
TEST_DEPENDS("engine.physics.solver")
TEST_DEPENDS("engine.physics.continuous")

TEST_CASE("solver owner order matches complete entity ids", "[physics][solver-setup-parity]") {
	using namespace engine;
	ecs::Store store("solver-owner-order");
	physics::PreparePhysicsWorld(store, 4.0f);
	auto &world = *store.ResourceMutable<physics::PhysicsWorld>();
	std::vector<std::vector<uint64_t>> cases{
		{},
		{17, 17},
		{0, 0, 0, 0},
		{std::numeric_limits<uint64_t>::max(), 0, uint64_t{1} << 63, 1, uint64_t{1} << 32, 1},
	};
	// Keep every other bit constant, including across radix and generation boundaries.
	for (size_t bit = 0; bit < 64; bit++) {
		const uint64_t common = ~(uint64_t{1} << bit);
		cases.push_back({common | (uint64_t{1} << bit), common, common, common | (uint64_t{1} << bit)});
	}
	for (size_t digits = 2; digits < 6; digits++) {
		std::vector<uint64_t> boundaries{0, 0};
		for (size_t digit = 0; digit < digits; digit++) {
			boundaries.push_back(uint64_t{1} << (digit * 11));
			boundaries.push_back(uint64_t{1} << (digit * 11));
		}
		std::reverse(boundaries.begin(), boundaries.end());
		cases.push_back(std::move(boundaries));
	}
	uint64_t randomId = 0x9e3779b97f4a7c15ULL;
	std::vector<uint64_t> mixed;
	for (size_t index = 0; index < 1024; index++) {
		randomId ^= randomId << 13;
		randomId ^= randomId >> 7;
		randomId ^= randomId << 17;
		mixed.push_back(randomId);
		mixed.push_back(index % 3 == 0 ? randomId : ~randomId);
	}
	cases.push_back(std::move(mixed));
	cases.push_back({2, 1});
	cases.push_back({});

	for (const auto &ids : cases) {
		CAPTURE(ids);
		auto expected = ids;
		std::sort(expected.begin(), expected.end());
		expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
		auto &manifolds = physics::PipelineInternals::Manifolds(world);
		manifolds.clear();
		// Missing handles and zero-point manifolds isolate ordering from body arithmetic.
		for (size_t index = 0; index < ids.size(); index += 2) {
			physics::ContactManifold manifold;
			manifold.A = ecs::Entity(std::min(ids[index], ids[index + 1]));
			manifold.B = ecs::Entity(std::max(ids[index], ids[index + 1]));
			manifolds.push_back(manifold);
		}
		physics::Solve(store);
		const auto &owners = physics::PipelineInternals::BodyOwners(world);
		REQUIRE(owners.size() == expected.size());
		for (size_t index = 0; index < expected.size(); index++) {
			CHECK(owners[index].Id == expected[index]);
		}
	}
}

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

TEST_CASE("solver cache cursor matches whole-cache pair search", "[physics][solver-cache-cursor]") {
	using namespace engine;
	std::vector<physics::ContactImpulse> cache;
	for (const uint64_t first : {uint64_t{3}, uint64_t{1} << 32, uint64_t{1} << 63}) {
		for (const uint64_t second : {uint64_t{7}, UINT64_MAX}) {
			for (const uint32_t feature : {0u, 2u, UINT32_MAX}) {
				cache.push_back({ecs::Entity(first), ecs::Entity(second), feature});
			}
		}
	}
	for (const size_t start : {size_t{0}, size_t{1}, size_t{8}, size_t{17}}) {
		physics::ImpulseCacheCursor cursor{cache};
		for (size_t index = start; index < cache.size(); index++) {
			const auto &key = cache[index];
			const auto found = cursor.Pair(key.A, key.B);
			REQUIRE(found.size() == 3);
			CHECK(found.front().A == key.A);
			CHECK(found.front().B == key.B);
			CHECK(found.front().Feature == 0);
			CHECK(found.back().Feature == UINT32_MAX);
		}
		CHECK(cursor.Pair(ecs::Entity(UINT64_MAX), ecs::Entity(UINT64_MAX)).empty());
	}
	physics::ImpulseCacheCursor absent{cache};
	CHECK(absent.Pair(ecs::Entity(0), ecs::Entity(1)).empty());
	CHECK(absent.Pair(ecs::Entity(3), ecs::Entity(8)).empty());
	const auto afterHole = absent.Pair(ecs::Entity(3), ecs::Entity(UINT64_MAX));
	REQUIRE(afterHole.size() == 3);
	CHECK(afterHole.front().A == ecs::Entity(3));
	CHECK(afterHole.front().B == ecs::Entity(UINT64_MAX));
	CHECK(afterHole.back().Feature == UINT32_MAX);
	CHECK(absent.Pair(ecs::Entity(4), ecs::Entity(0)).empty());
	CHECK(absent.Pair(ecs::Entity(UINT64_MAX), ecs::Entity(0)).empty());
	physics::ImpulseCacheCursor empty{{}};
	CHECK(empty.Pair(ecs::Entity(0), ecs::Entity(1)).empty());
}
