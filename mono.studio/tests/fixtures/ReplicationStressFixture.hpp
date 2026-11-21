#pragma once

// A studio Play link over one authority world, stepped at 60 Hz, shared by the
// replication stress suites. `Fixture(true)` also runs ReplicationStress.luau.

#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
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

namespace studio::testing {
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
