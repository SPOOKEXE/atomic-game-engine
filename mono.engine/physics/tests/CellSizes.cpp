#include "../../spatial/tests/fixtures/GridDiagnostics.hpp"
#include "PipelineInternals.hpp"
#include "fixtures/CellSizeParity.hpp"
#include "fixtures/SteppingScene.hpp"

#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

TEST_SUITE_ID("engine.physics.cell-sizes")
TEST_DEPENDS("engine.physics.broadphase")
TEST_DEPENDS("engine.physics.narrowphase")
TEST_DEPENDS("engine.physics.solver")

TEST_CASE("cell size preserves ordered contacts and body state in stress scenes", "[physics][cell-sizes]") {
	using namespace engine::physics::testing;
	for (const Layout layout : {Layout::Pile, Layout::Stacks, Layout::Scattered}) {
		CAPTURE(static_cast<int>(layout));
		std::array worlds{
			BuildScene(layout, 4000, 4.0f), BuildScene(layout, 4000, 2.0f), BuildScene(layout, 4000, 1.0f)
		};
		// Include the dense initial pile and its ten-step warm start, then the eight runner warmups and five
		// measured ticks.
		for (size_t tick = 0; tick < 23; tick++) {
			CAPTURE(tick);
			for (auto &world : worlds)
				Tick(*world);
			const CellSizeSnapshot expected = Capture(*worlds[0]);
			for (size_t variant = 1; variant < worlds.size(); variant++) {
				CAPTURE(variant);
				const CellSizeSnapshot actual = Capture(*worlds[variant]);
				REQUIRE(actual.Pairs == expected.Pairs);
				REQUIRE(actual.Manifolds == expected.Manifolds);
				REQUIRE(actual.Events == expected.Events);
				REQUIRE(actual.Bodies == expected.Bodies);
			}
		}
	}
}

TEST_CASE("representative cell-size scenes preserve exact output in both orders", "[physics][cell-sizes]") {
	using namespace engine;
	using namespace engine::physics::testing;
	for (const Layout layout : {Layout::Pile, Layout::Stacks, Layout::Scattered, Layout::MixedScale}) {
		for (const bool reverse : {false, true}) {
			CAPTURE(static_cast<int>(layout), reverse);
			auto floor4 = BuildScene(layout, 0, 4.0f);
			auto scene4 = BuildScene(layout, 4000, 4.0f);
			auto floor2 = BuildScene(layout, 0, 2.0f);
			auto scene2 = BuildScene(layout, 4000, 2.0f);
			std::array<ecs::Store *, 4> worlds{floor4.get(), scene4.get(), floor2.get(), scene2.get()};
			const std::array<size_t, 4> order =
				reverse ? std::array<size_t, 4>{2, 3, 0, 1} : std::array<size_t, 4>{0, 1, 2, 3};
			for (size_t tick = 0; tick < 23; tick++) {
				CAPTURE(tick);
				for (const size_t index : order)
					Tick(*worlds[index]);
				REQUIRE(SameSnapshot(Capture(*floor4), Capture(*floor2)));
				REQUIRE(SameSnapshot(Capture(*scene4), Capture(*scene2)));
			}
		}
	}
}

TEST_CASE("both cell sizes keep falling bodies on promoted and residual floors", "[physics][cell-sizes]") {
	using namespace engine;
	using namespace physics::testing;
	// The pile fixture has no gravity. This scene applies weight explicitly and
	// initializes the static index before the fast body's first sweep.
	for (const float floorHalf : {4096.0f, 1'000'000.0f}) {
		for (const float offset : {0.0f, 600'000.0f}) {
			for (const bool fast : {false, true}) {
				CAPTURE(floorHalf, offset, fast);
				std::array worlds{BuildScene(Layout::Pile, 1, 4.0f), BuildScene(Layout::Pile, 1, 2.0f)};
				std::array<ecs::Entity, 2> floors{};
				std::array<ecs::Entity, 2> bodies{};
				std::array<bool, 2> admitted{};
				std::array<bool, 2> contacted{};
				for (size_t variant = 0; variant < worlds.size(); variant++) {
					auto &store = *worlds[variant];
					store.Query<const scene::Transform>().Each([&](ecs::Entity entity,
																   const scene::Transform &) {
						if (store.Has<scene::Simulated>(entity))
							bodies[variant] = entity;
						else
							floors[variant] = entity;
					});
					store.GetMutable<scene::Transform>(floors[variant])->Frame.Position =
						core::Vector3{offset, -1.0f, 0.0f};
					store.GetMutable<scene::Collider>(floors[variant])->Extent =
						core::Vector3{floorHalf, 1.0f, floorHalf};
					store.GetMutable<scene::Transform>(bodies[variant])->Frame.Position =
						core::Vector3{offset, fast ? 3.0f : 1.0f, 0.0f};
					store.GetMutable<scene::Motion>(bodies[variant])->Linear =
						core::Vector3{0.0f, fast ? -300.0f : 0.0f, 0.0f};
					physics::SyncBroadphase(store);
					const auto &grid =
						physics::PipelineInternals::StaticIndex(*store.Resource<physics::PhysicsWorld>());
					std::vector<std::array<int32_t, 3>> scratch;
					const auto placement = spatial::testing::ReadGrid(grid, scratch);
					if (floorHalf == 1'000'000.0f)
						REQUIRE(placement.ResidualProxies == 1);
					else {
						REQUIRE(placement.ResidualProxies == 0);
						REQUIRE(placement.Levels[0].Proxies == 0);
					}
					const auto hit = physics::Raycast(
						store,
						core::Ray{core::Vector3{offset, 5.0f, 0.0f}, -core::Vector3::YAxis},
						10.0f,
						spatial::LayerMask::All(),
						bodies[variant]
					);
					REQUIRE(hit.has_value());
					REQUIRE(hit->Owner == floors[variant]);
				}
				for (size_t tick = 0; tick < 23; tick++) {
					CAPTURE(tick);
					for (size_t variant = 0; variant < worlds.size(); variant++) {
						auto &store = *worlds[variant];
						if (auto *motion = store.GetMutable<scene::Motion>(bodies[variant]))
							motion->Linear.Y -= 9.81f / 60.0f;
						Tick(store);
						const auto &world = *store.Resource<physics::PhysicsWorld>();
						const auto position = store.Get<scene::Transform>(bodies[variant])->Frame.Position;
						REQUIRE(std::isfinite(position.X));
						REQUIRE(std::isfinite(position.Y));
						REQUIRE(std::isfinite(position.Z));
						REQUIRE(position.Y >= 0.48f);
						if (fast && tick == 0) REQUIRE(world.SweptBodies() == 1);
						for (const auto &pair : world.Pairs())
							admitted[variant] = admitted[variant] ||
												(pair.A == floors[variant] && pair.B == bodies[variant]) ||
												(pair.B == floors[variant] && pair.A == bodies[variant]);
						for (const auto &event : world.Events())
							contacted[variant] = contacted[variant] ||
												 (event.A == floors[variant] && event.B == bodies[variant]) ||
												 (event.B == floors[variant] && event.A == bodies[variant]);
					}
					const auto expected = Capture(*worlds[0]);
					const auto actual = Capture(*worlds[1]);
					REQUIRE(actual.Pairs == expected.Pairs);
					REQUIRE(actual.Manifolds == expected.Manifolds);
					REQUIRE(actual.Events == expected.Events);
					REQUIRE(actual.Bodies == expected.Bodies);
				}
				for (size_t variant = 0; variant < worlds.size(); variant++) {
					REQUIRE(admitted[variant]);
					REQUIRE(contacted[variant]);
				}
			}
		}
	}
}
