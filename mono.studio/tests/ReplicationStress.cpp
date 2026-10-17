// Late-created instances must arrive even while every mover keeps changing.

#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/Game.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <filesystem>
#include <studio/PlayLink.hpp>
#include <studio/Viewports.hpp>
#include <vector>

TEST_SUITE_ID("studio.replicationstress")
TEST_DEPENDS("studio.playlink")
TEST_DEPENDS("engine.examples.scene")

namespace {
	constexpr float TICK_SECONDS = 1.0f / 60.0f;

	struct Fixture {
		std::filesystem::path PreviousAssets = engine::core::Paths::Assets();
		engine::world::Universe Worlds;
		engine::world::WorldId Authority;
		studio::PlayLink Link;
		std::shared_ptr<engine::script::Runtime> Runtime;

		explicit Fixture(bool actualDemo = false) {
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
			if (actualDemo) {
				engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
				Worlds.Enter(Authority, [&](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
					client::InstallPresentation(store, systems);
					engine::physics::PreparePhysicsWorld(store);
					engine::physics::RegisterPhysicsSystems(systems);
					engine::scene::PrepareGravity(store);
					engine::scene::RegisterGravitySystem(systems);
					engine::scene::RegisterOwnershipSystem(systems);
					REQUIRE(
						studio::CreateRuntimeCamera(store, studio::DefaultViewportCamera()) !=
						engine::ecs::NULL_ENTITY
					);
					REQUIRE(
						engine::script::MakeScript(
							store,
							engine::examples::ExamplePath("ReplicationStress.luau"),
							"ReplicationStress"
						) != engine::ecs::NULL_ENTITY
					);
					engine::script::RuntimeLimits limits;
					limits.Role = {.Server = true, .Client = true, .Studio = true};
					Runtime = engine::game::StartWorldScripts(store, systems, limits, error);
					INFO(error);
					REQUIRE(Runtime != nullptr);
					REQUIRE(error.empty());
				});
			}
			REQUIRE(Link.Start(Worlds, Authority, 60.0, error));
			for (int tick = 0; tick < 4; ++tick)
				Step();
		}

		~Fixture() {
			Runtime.reset();
			Link.Stop(Worlds);
			engine::core::Paths::SetAssetsOverride(PreviousAssets);
			engine::parallel::Jobs::Stop();
		}

		void Step() {
			Link.Step(Worlds, [&](engine::world::WorldId world) {
				return world == Authority ? Runtime.get() : nullptr;
			});
			Worlds.Tick(TICK_SECONDS);
			if (Runtime != nullptr) {
				Worlds.Present(Link.ReplicaWorld(), TICK_SECONDS, 1);
				INFO(Runtime->LastError());
				REQUIRE(Runtime->LastError().empty());
			}
		}
	};
}

TEST_CASE(
	"actual replication stress keeps its client camera above the moving field", "[replication-stress][demo]"
) {
	Fixture fixture(true);
	std::vector<engine::ecs::Entity> authorityMovers;
	fixture.Worlds.Enter(fixture.Authority, [&](const engine::ecs::Store &store) {
		store.EachDescendant(engine::scene::WorkspaceOf(store), [&](engine::ecs::Entity entity) {
			if (store.InstanceNameOf(entity) == engine::core::Name("Mover"))
				authorityMovers.push_back(entity);
		});
	});
	REQUIRE(authorityMovers.size() == 20000);
	size_t complete = 0;
	for (int tick = 0; tick < 1024 && (tick < 180 || complete != authorityMovers.size()); ++tick) {
		fixture.Step();
		fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const engine::ecs::Store &store) {
			complete = 0;
			for (const auto entity : authorityMovers) {
				const auto *bounds = store.Get<engine::scene::Bounds>(entity);
				const auto *transform = store.Get<engine::scene::Transform>(entity);
				if (store.ClassOf(entity) == engine::scene::PartClass() &&
					store.InstanceNameOf(entity) == engine::core::Name("Mover") && transform != nullptr &&
					bounds != nullptr && store.Has<engine::scene::Visual>(entity) &&
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
			if (store.InstanceNameOf(entity) != engine::core::Name("Mover")) return;
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
				if (store.InstanceNameOf(entity) != engine::core::Name("Mover")) return;
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
					  store.InstanceNameOf(part) == engine::core::Name("Mover") &&
					  store.Has<engine::scene::Bounds>(part) && store.Has<engine::scene::Visual>(part) &&
					  store.ParentOf(part) == workspace;
		}
	});
	INFO(
		"final poses=" << current << " intact parts=" << intact
					   << " applied=" << fixture.Link.Report().Applied
	);
	CHECK(current == movers.size());
	CHECK(intact == movers.size());
}
