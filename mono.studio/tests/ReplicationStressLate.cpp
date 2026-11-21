// Late-created instances must arrive even while every mover keeps changing.
//
// A suite of its own so the runner can run it beside ReplicationStress.cpp:
// each case steps 20000 replicated parts for over 200 ticks.

#include "fixtures/ReplicationStressFixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("studio.replicationstress-late")
TEST_DEPENDS("studio.playlink")
TEST_DEPENDS("engine.examples.scene")

using studio::testing::Fixture;

TEST_CASE(
	"all twenty thousand late movers acquire their complete creation state", "[slow][replication-stress]"
) {
	Fixture fixture;
	const engine::core::Name moverName("Mover");
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
					store.InstanceNameOf(part) == moverName && store.Has<engine::scene::Transform>(part) &&
					store.Has<engine::scene::Bounds>(part) && store.Has<engine::scene::Visual>(part) &&
					store.ParentOf(part) == workspace)
					++complete;
			}
		});
	}
	size_t classes = 0, names = 0, transforms = 0, bounds = 0, visuals = 0, parents = 0;
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
		for (const auto part : movers) {
			classes += store.ClassOf(part) == engine::scene::PartClass();
			names += store.InstanceNameOf(part) == moverName;
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

	std::vector<engine::core::Vector3> finalPositions;
	finalPositions.reserve(movers.size());
	fixture.Worlds.Enter(fixture.Authority, [&](const engine::ecs::Store &store) {
		for (const auto part : movers) {
			const auto *transform = store.Get<engine::scene::Transform>(part);
			REQUIRE(transform != nullptr);
			finalPositions.push_back(transform->Frame.Position);
		}
	});

	// A completed creation does not prove a moving value survived the crowded
	// delta stream. Stop moving the authority and give its final poses time to land.
	constexpr float positionTolerance = engine::scene::WIRE_POSITION_ERROR_METRES * 1.75f;
	size_t current = 0;
	for (int tick = 0; tick < 256 && current != movers.size(); ++tick) {
		fixture.Step();
		fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
			current = 0;
			for (size_t index = 0; index < movers.size(); ++index) {
				const auto *transform = store.Get<engine::scene::Transform>(movers[index]);
				if (transform != nullptr &&
					(transform->Frame.Position - finalPositions[index]).Magnitude() <= positionTolerance)
					++current;
			}
		});
	}
	size_t intact = 0;
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
		for (const auto part : movers) {
			intact += store.ClassOf(part) == engine::scene::PartClass() &&
					  store.InstanceNameOf(part) == moverName && store.Has<engine::scene::Bounds>(part) &&
					  store.Has<engine::scene::Visual>(part) && store.ParentOf(part) == workspace;
		}
	});
	INFO(
		"final poses=" << current << " intact parts=" << intact
					   << " applied=" << fixture.Link.Report().Applied
	);
	CHECK(current == movers.size());
	CHECK(intact == movers.size());
}
