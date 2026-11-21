// The ReplicationStress demo keeps its client camera above every mover.

#include "fixtures/ReplicationStressFixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("studio.replicationstress")
TEST_DEPENDS("studio.playlink")
TEST_DEPENDS("engine.examples.scene")

using studio::testing::Fixture;

TEST_CASE(
	"actual replication stress keeps its client camera above the moving field",
	"[slow][replication-stress][demo]"
) {
	Fixture fixture(true);
	const engine::core::Name moverName("Mover");
	std::vector<engine::ecs::Entity> authorityMovers;
	fixture.Worlds.Enter(fixture.Authority, [&](const engine::ecs::Store &store) {
		store.EachDescendant(engine::scene::WorkspaceOf(store), [&](engine::ecs::Entity entity) {
			if (store.InstanceNameOf(entity) == moverName) authorityMovers.push_back(entity);
		});
	});
	REQUIRE(authorityMovers.size() == 20000);
	size_t complete = 0;
	for (int tick = 0; tick < 1024 && (tick < 180 || complete != authorityMovers.size()); ++tick) {
		fixture.Step();
		// All 180 ticks must run before completeness can end the loop.
		if (tick < 179) continue;
		fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
			complete = 0;
			for (const auto entity : authorityMovers) {
				const auto *bounds = store.Get<engine::scene::Bounds>(entity);
				const auto *transform = store.Get<engine::scene::Transform>(entity);
				if (store.ClassOf(entity) == engine::scene::PartClass() &&
					store.InstanceNameOf(entity) == moverName && transform != nullptr && bounds != nullptr &&
					store.Has<engine::scene::Visual>(entity) &&
					(bounds->HalfExtent - engine::core::Vector3{.175f, .175f, .175f}).Magnitude() < .0001f &&
					transform->Frame.Position.Y >= 1.9f && transform->Frame.Position.Y <= 5.25f)
					++complete;
			}
		});
	}
	CHECK(complete == authorityMovers.size());
	fixture.Worlds.Enter(fixture.Authority, [&](const engine::ecs::Store &store) {
		INFO(
			"server posed=" << fixture.Link.Report().ServerEntities << " client posed="
							<< fixture.Link.Report().ClientEntities << " tick=" << fixture.Link.Report().Tick
							<< " applied=" << fixture.Link.Report().Applied
							<< " total bytes=" << fixture.Link.Report().TotalBytes
		);
		const auto *rig =
			store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, fixture.Link.Player()));
		REQUIRE(rig != nullptr);
		const auto *root = store.Get<engine::scene::Transform>(rig->Root);
		REQUIRE(root != nullptr);
		INFO("authority character Y=" << root->Frame.Position.Y);
		CHECK(root->Frame.Position.Y > 0.0f);
	});
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](engine::ecs::Store &store) {
		size_t named = 0, transforms = 0, bounds = 0;
		store.EachDescendant(engine::scene::WorkspaceOf(store), [&](engine::ecs::Entity entity) {
			if (store.InstanceNameOf(entity) != moverName) return;
			++named;
			transforms += store.Has<engine::scene::Transform>(entity);
			bounds += store.Has<engine::scene::Bounds>(entity);
		});
		INFO("named=" << named << " transforms=" << transforms << " bounds=" << bounds);
		const auto camera = studio::RuntimeCameraOf(store);
		const auto *eye = store.Get<engine::scene::Transform>(camera);
		const auto *lens = store.Get<engine::scene::Camera>(camera);
		REQUIRE(eye != nullptr);
		REQUIRE(lens != nullptr);
		const auto frustum = engine::graph::Frustum::FromViewProjection(
			engine::scene::ResolveCamera(eye->Frame, *lens, 798.0f / 521.0f).ViewProjection
		);
		size_t movers = 0;
		size_t visible = 0;
		store.Each<const engine::scene::Transform, const engine::scene::Bounds>(
			[&](engine::ecs::Entity entity,
				const engine::scene::Transform &transform,
				const engine::scene::Bounds &bounds) {
				if (store.InstanceNameOf(entity) != moverName) return;
				++movers;
				visible +=
					frustum.Intersects(engine::core::OrientedBoxBounds(transform.Frame, bounds.HalfExtent));
			}
		);
		INFO("replica camera Y=" << eye->Frame.Position.Y << " movers=" << movers << " visible=" << visible);
		CHECK(movers == 20000);
		CHECK(eye->Frame.Position.Y > 0.0f);
		CHECK(visible > 0);
	});
}
