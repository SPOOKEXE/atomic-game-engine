// A furnished Studio authority with the moving population of ReplicationStress.
// The measured step uses the real link, including replica delivery and ACKs.

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/Universe.hpp>

#include <iostream>
#include <stdexcept>
#include <studio/PlayLink.hpp>
#include <vector>

TEST_SUITE_ID("studio.bench.replicationstress")

namespace {
	constexpr size_t MOVERS = 20000;
	constexpr float TICK_SECONDS = 1.0f / 60.0f;

	struct Fixture {
		engine::world::Universe Worlds;
		engine::world::WorldId Authority;
		studio::PlayLink Link;
		std::vector<engine::ecs::Entity> Movers;
		uint64_t Tick = 0;

		Fixture() {
			engine::parallel::Jobs::Start(1);
			engine::scene::RegisterSceneClasses();
			Authority = Worlds.Create({.Name = engine::core::Name("studio-replication-stress-bench")});
			Worlds.Enter(Authority, [](engine::ecs::Store &store) {
				engine::scene::InstallServices(store);
				store.Observe<engine::scene::Transform>();
			});
			std::string error;
			if (!Link.Start(Worlds, Authority, 60.0, error)) throw std::runtime_error(error);
			// The demo builds after admission, so creations use the steady path.
			for (int warm = 0; warm < 4; ++warm) {
				Link.Step(Worlds);
				Worlds.Tick(TICK_SECONDS);
			}
			Worlds.Enter(Authority, [&](engine::ecs::Store &store) {
				const auto workspace = engine::scene::WorkspaceOf(store);
				Movers.reserve(MOVERS);
				for (size_t index = 0; index < MOVERS; ++index) {
					const auto part = store.CreateInstance(engine::scene::PartClass());
					store.SetInstanceName(part, "Mover");
					if (!store.SetParent(part, workspace))
						throw std::runtime_error("replication benchmark part could not be parented");
					Movers.push_back(part);
				}
			});
			for (int warm = 0; warm < 8; ++warm)
				Step();
		}

		~Fixture() {
			std::cout << "detail\tstudio.bench.replicationstress\tmovers\t" << Movers.size()
					  << "\tserver_poses\t" << Link.Report().ServerEntities << "\tclient_poses\t"
					  << Link.Report().ClientEntities << "\n";
			Link.Stop(Worlds);
			engine::parallel::Jobs::Stop();
		}

		void Step() {
			++Tick;
			Worlds.Enter(Authority, [&](engine::ecs::Store &store) {
				for (size_t index = 0; index < Movers.size(); ++index) {
					const float x = (static_cast<float>(index % 200) - 99.5f) * 0.55f;
					const float z = (static_cast<float>(index / 200) - 49.5f) * 0.55f;
					const float offset = (Tick % 2 == 0 ? 1.0f : -1.0f) * 0.25f;
					store.Set<engine::scene::Transform>(
						Movers[index], {engine::core::CFrame(engine::core::Vector3{x + offset, 2.0f, z})}
					);
				}
			});
			Link.Step(Worlds);
			Worlds.Tick(TICK_SECONDS);
			engine::testing::Consume(Link.Report().Bytes);
		}
	};

	Fixture &LiveFixture() {
		static Fixture fixture;
		return fixture;
	}
}

BENCH("PlayLink · 20k moving Parts · actual Studio interest and replica", 8) {
	for (int iteration = 0; iteration < 8; ++iteration)
		LiveFixture().Step();
}
