// Late-created instances must arrive even while every mover keeps changing.

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/PlayLink.hpp>
#include <vector>

TEST_SUITE_ID("studio.replicationstress")
TEST_DEPENDS("studio.playlink")

namespace {
	constexpr float TICK_SECONDS = 1.0f / 60.0f;

	struct Fixture {
		engine::world::Universe Worlds;
		engine::world::WorldId Authority;
		studio::PlayLink Link;

		Fixture() {
			engine::parallel::Jobs::Start(1);
			engine::scene::RegisterSceneClasses();
			Authority = Worlds.Create(
				{.Name = engine::core::Name("replication-stress-test"),
				 .TickRate = 60.0,
				 .IdleTickRate = 60.0}
			);
			Worlds.Enter(Authority, [](engine::ecs::Store &store) {
				engine::scene::InstallServices(store);
				store.Observe<engine::scene::Transform>();
			});
			std::string error;
			REQUIRE(Link.Start(Worlds, Authority, 60.0, error));
			for (int tick = 0; tick < 4; ++tick)
				Step();
		}

		~Fixture() {
			Link.Stop(Worlds);
			engine::parallel::Jobs::Stop();
		}

		void Step() {
			Link.Step(Worlds);
			Worlds.Tick(TICK_SECONDS);
		}
	};
}

TEST_CASE("all twenty thousand late movers acquire their complete creation state", "[replication-stress]") {
	Fixture fixture;
	std::vector<engine::ecs::Entity> movers;
	engine::ecs::Entity workspace;
	fixture.Worlds.Enter(fixture.Authority, [&](engine::ecs::Store &store) {
		workspace = engine::scene::WorkspaceOf(store);
		for (size_t index = 0; index < 20000; ++index) {
			const auto part = store.CreateInstance(engine::scene::PartClass());
			REQUIRE(store.SetInstanceName(part, "Mover"));
			REQUIRE(store.SetParent(part, workspace));
			movers.push_back(part);
		}
	});
	fixture.Step();
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
		size_t alive = 0;
		for (const auto part : movers)
			alive += store.Alive(part);
		REQUIRE(alive == movers.size());
	});

	size_t complete = 0;
	for (int tick = 0; tick < 256 && complete != movers.size(); ++tick) {
		fixture.Worlds.Enter(fixture.Authority, [&](engine::ecs::Store &store) {
			for (size_t index = 0; index < movers.size(); ++index) {
				const float x = static_cast<float>(index % 200) * 0.55f;
				const float z = static_cast<float>(index / 200) * 0.55f;
				store.Set<engine::scene::Transform>(
					movers[index], {engine::core::CFrame(engine::core::Vector3{x, 2.0f + (tick % 2), z})}
				);
			}
		});
		fixture.Step();
		fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
			complete = 0;
			for (const auto part : movers) {
				if (store.ClassOf(part) == engine::scene::PartClass() &&
					store.InstanceNameOf(part) == engine::core::Name("Mover") &&
					store.Has<engine::scene::Transform>(part) && store.Has<engine::scene::Bounds>(part) &&
					store.Has<engine::scene::Visual>(part) && store.ParentOf(part) == workspace)
					++complete;
			}
		});
	}
	size_t classes = 0, names = 0, transforms = 0, bounds = 0, visuals = 0, parents = 0;
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
		for (const auto part : movers) {
			classes += store.ClassOf(part) == engine::scene::PartClass();
			names += store.InstanceNameOf(part) == engine::core::Name("Mover");
			transforms += store.Has<engine::scene::Transform>(part);
			bounds += store.Has<engine::scene::Bounds>(part);
			visuals += store.Has<engine::scene::Visual>(part);
			parents += store.ParentOf(part) == workspace;
		}
	});
	INFO(
		"classes=" << classes << " names=" << names << " transforms=" << transforms << " bounds=" << bounds
				   << " visuals=" << visuals << " parents=" << parents
	);
	INFO(
		"complete late-created Parts: " << complete << " tick=" << fixture.Link.Report().Tick
										<< " applied=" << fixture.Link.Report().Applied
	);
	CHECK(complete == movers.size());
	CHECK(fixture.Link.Report().ServerEntities >= movers.size());
	CHECK(fixture.Link.Report().ClientEntities >= movers.size());
}
