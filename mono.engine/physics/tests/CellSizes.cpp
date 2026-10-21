#include "../../spatial/tests/fixtures/GridDiagnostics.hpp"
#include "PipelineInternals.hpp"
#include "fixtures/CellSizeJobs.hpp"
#include "fixtures/CellSizeParity.hpp"
#include "fixtures/SteppingScene.hpp"

#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using engine::physics::testing::JobPoolFixture;

namespace {

	struct SerialComputeFixture {
		const bool Previous = engine::parallel::ForceSerialCompute();

		explicit SerialComputeFixture(bool serial) {
			engine::parallel::SetForceSerialCompute(serial);
		}
		~SerialComputeFixture() {
			engine::parallel::SetForceSerialCompute(Previous);
		}

		SerialComputeFixture(const SerialComputeFixture &) = delete;
		SerialComputeFixture &operator=(const SerialComputeFixture &) = delete;
	};
}

TEST_SUITE_ID("engine.physics.cell-sizes")
TEST_DEPENDS("engine.physics.broadphase")
TEST_DEPENDS("engine.physics.narrowphase")
TEST_DEPENDS("engine.physics.solver")
TEST_DEPENDS("engine.parallel.jobs")

TEST_CASE(
	"cell size preserves ordered contacts and body state in stress scenes", "[slow][physics][cell-sizes]"
) {
	JobPoolFixture jobPool;
	using namespace engine::physics::testing;
	constexpr std::array cellSizes{4.0f, 2.0f, 1.0f};
	for (const Layout layout : {Layout::Pile, Layout::Stacks, Layout::Scattered, Layout::MixedScale}) {
		CAPTURE(static_cast<int>(layout));
		std::array floors{
			BuildScene(layout, 0, cellSizes[0]),
			BuildScene(layout, 0, cellSizes[1]),
			BuildScene(layout, 0, cellSizes[2])
		};
		std::array worlds{
			BuildScene(layout, 4000, cellSizes[0]),
			BuildScene(layout, 4000, cellSizes[1]),
			BuildScene(layout, 4000, cellSizes[2])
		};
		// Cover initial overlap resolution, warm-started contacts and settled contact ticks.
		for (size_t tick = 0; tick < 23; tick++) {
			// Change visit order within the same stress run to expose shared scratch leaking between worlds.
			const bool reverse = tick % 2 != 0;
			CAPTURE(tick, reverse);
			const size_t worldCount = worlds.size() * 2;
			for (size_t visit = 0; visit < worldCount; visit++) {
				const size_t index = reverse ? worldCount - 1 - visit : visit;
				const size_t variant = index / 2;
				// The 4m reference stays serial; finer grids exercise real workers.
				const SerialComputeFixture compute(variant == 0);
				Tick(index % 2 == 0 ? *floors[variant] : *worlds[variant]);
			}
			const CellSizeSnapshot expectedFloor = Capture(*floors[0]);
			const CellSizeSnapshot expected = Capture(*worlds[0]);
			for (size_t variant = 1; variant < worlds.size(); variant++) {
				CAPTURE(cellSizes[variant]);
				const CellSizeSnapshot actualFloor = Capture(*floors[variant]);
				REQUIRE(actualFloor.Pairs == expectedFloor.Pairs);
				REQUIRE(actualFloor.Manifolds == expectedFloor.Manifolds);
				REQUIRE(actualFloor.Events == expectedFloor.Events);
				REQUIRE(actualFloor.Bodies == expectedFloor.Bodies);
				const CellSizeSnapshot actual = Capture(*worlds[variant]);
				REQUIRE(actual.Pairs == expected.Pairs);
				REQUIRE(actual.Manifolds == expected.Manifolds);
				REQUIRE(actual.Events == expected.Events);
				REQUIRE(actual.Bodies == expected.Bodies);
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
