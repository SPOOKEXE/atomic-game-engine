#include <engine/core/Paths.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.examples.immersiveportals")
TEST_DEPENDS("engine.script.portaltransfer")
TEST_DEPENDS("engine.examples.scene")

using namespace engine;

namespace {

	struct StagedAssets {
		std::filesystem::path Previous = core::Paths::Assets();
		StagedAssets() {
			core::Paths::SetAssetsOverride(core::Paths::Base().parent_path() / "assets");
		}
		~StagedAssets() {
			core::Paths::SetAssetsOverride(Previous);
		}
	};

	struct DemoPair {
		StagedAssets Assets;
		world::Universe Worlds;
		std::array<world::WorldId, 2> Ids;
		uint32_t Steps = 0;

		DemoPair() {
			for (size_t index = 0; index < Ids.size(); ++index) {
				world::WorldSettings settings;
				settings.Name = core::Name("immersive-portals-demo-" + std::to_string(index + 1));
				settings.TickRate = 60;
				Ids[index] = Worlds.Create(settings);
				Worlds.Enter(Ids[index], [&](ecs::Store &store, ecs::Scheduler &scheduler) {
					std::string failure;
					const bool loaded = examples::LoadScene(
						store, scheduler, examples::ExamplePath("ImmersivePortals.luau"), failure
					);
					INFO(failure);
					REQUIRE(loaded);
					REQUIRE(script::ConfigurePortalTransfers(store, 101 + index));
					script::RegisterTeleportAdmission(scheduler);
					physics::PreparePhysicsWorld(store);
					(void)scene::OpenPortals(store);
				});
			}
			Tick(3);
		}

		void Tick(int count = 1) {
			for (int tick = 0; tick < count; ++tick)
				Worlds.Tick(1.0f / 60);
		}

		ecs::Entity AddPlayer(float z) {
			ecs::Entity player;
			Worlds.Enter(Ids[0], [&](ecs::Store &store) {
				player = scene::AddPlayer(store, "traveller", false, 71);
				const auto model = scene::LoadCharacter(store, player);
				const auto rig = *store.Get<scene::Character>(model);
				store.Set(
					rig.Root, scene::Transform{core::CFrame({1, 3, z}) * core::CFrame::Angles(.2f, .4f, -.1f)}
				);
				store.Set(rig.Root, scene::Motion{});
				store.GetMutable<scene::Humanoid>(rig.Humanoid)->Health = 23;
				(void)scene::PoseCharacters(store);
			});
			return player;
		}

		// Supply one deterministic movement after the loader captures the previous
		// transform. The installed portal system detects and transports the sweep.
		void Move(size_t world, ecs::Entity player, float z) {
			Worlds.Enter(Ids[world], [&](ecs::Store &store, ecs::Scheduler &scheduler) {
				const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
				scheduler.Add(
					"demo.test.step." + std::to_string(++Steps),
					ecs::Phase::Simulation,
					[root = rig.Root, z, moved = false](ecs::Store &world) mutable {
						if (moved) return;
						moved = true;
						auto frame = world.Get<scene::Transform>(root)->Frame;
						const float velocity = (z - frame.Position.Z) / world.Time().Delta;
						frame.Position.Z = z;
						world.Set(root, scene::Transform{frame});
						world.Set(root, scene::Motion{{0, 0, velocity}, {}});
						(void)scene::PoseCharacters(world);
					}
				);
			});
			Tick();
		}

		ecs::Entity Arrive(size_t source, ecs::Entity player, float z, float velocity) {
			script::PortalTransferId transfer;
			Worlds.Enter(Ids[source], [&](ecs::Store &store) {
				const auto receipt = script::PortalTransferOfPlayer(store, player);
				REQUIRE(receipt.has_value());
				transfer = receipt->Id;
			});
			for (int tick = 0; tick < 10; ++tick) {
				Tick();
				size_t authority = 0;
				for (const auto world : Ids)
					Worlds.Enter(world, [&](ecs::Store &store) {
						store.Each<const scene::Character>([&](ecs::Entity, const scene::Character &rig) {
							if (store.Has<scene::Motion>(rig.Root)) ++authority;
						});
					});
				REQUIRE(authority <= 1);
			}
			ecs::Entity arrived;
			std::string diagnostic;
			Worlds.Enter(Ids[source], [&](ecs::Store &store) {
				CHECK_FALSE(store.Alive(player));
				const auto receipt = script::PortalTransferOfPlayer(store, player);
				REQUIRE(receipt.has_value());
				diagnostic = std::to_string(static_cast<int>(receipt->Stage)) + ": " + receipt->Diagnostic;
			});
			INFO("source receipt " << diagnostic);
			Worlds.Enter(Ids[1 - source], [&](ecs::Store &store) {
				for (const auto &receipt : script::PortalTransferReceipts(store)) {
					diagnostic += " destination outbound " + std::to_string(receipt.Id.Sequence) + ": " +
								  receipt.Diagnostic;
				}
				INFO(diagnostic);
				arrived = script::PortalTransferPlayer(store, transfer);
				REQUIRE(arrived != ecs::NULL_ENTITY);
				CHECK(scene::PlayerCount(store) == 1);
				const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, arrived));
				const auto frame = store.Get<scene::Transform>(rig.Root)->Frame;
				const auto expected = core::CFrame({1, 3, z}) * core::CFrame::Angles(.2f, .4f, -.1f);
				CHECK((frame.Position - expected.Position).Magnitude() < .001f);
				CHECK((frame.LookVector() - expected.LookVector()).Magnitude() < .001f);
				CHECK((frame.UpVector() - expected.UpVector()).Magnitude() < .001f);
				CHECK(store.Get<scene::Motion>(rig.Root)->Linear.Z == Catch::Approx(velocity));
				CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->Health == 23);
				store.Each<const scene::CharacterLimb, const scene::Transform>(
					[&](ecs::Entity, const scene::CharacterLimb &limb, const scene::Transform &pose) {
						if (limb.Root != rig.Root) return;
						CHECK((pose.Frame.Position - (frame * limb.Offset).Position).Magnitude() < .001f);
					}
				);
			});
			return arrived;
		}
	};
}

TEST_CASE(
	"immersive demo joins coincident planes without rotating the traveller", "[examples][immersive-portals]"
) {
	DemoPair pair;
	for (const auto world : pair.Ids)
		pair.Worlds.Enter(world, [](ecs::Store &store) {
			std::vector<scene::PortalSeam> seams;
			scene::GatherPortalSeams(store, seams);
			REQUIRE(seams.size() == 1);
			CHECK(seams[0].Crosses);
			CHECK(seams[0].Bidirectional);
			const auto mapping = scene::SeamMapping(seams[0]);
			CHECK((mapping.Point(seams[0].Centre) - seams[0].Centre).Magnitude() < .001f);
			for (const auto direction : {core::Vector3::XAxis, core::Vector3::YAxis, core::Vector3::ZAxis}) {
				CHECK((mapping.Rotate(direction) - direction).Magnitude() < .001f);
			}
		});
}

TEST_CASE(
	"immersive demo does not teleport a stationary player near either face", "[examples][immersive-portals]"
) {
	const float side = GENERATE(-1.0f, 1.0f);
	DemoPair pair;
	const auto player = pair.AddPlayer(side);
	pair.Tick(5);
	pair.Worlds.Enter(pair.Ids[0], [&](ecs::Store &store) {
		REQUIRE(store.Alive(player));
		CHECK_FALSE(script::PortalTransferOfPlayer(store, player).has_value());
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		CHECK(store.Get<scene::Transform>(rig.Root)->Frame.Position.Z == side);
	});
	pair.Worlds.Enter(pair.Ids[1], [](ecs::Store &store) { CHECK(scene::PlayerCount(store) == 0); });
}

TEST_CASE(
	"immersive demo carries the actual rig through both worlds and back", "[examples][immersive-portals]"
) {
	const float side = GENERATE(-1.0f, 1.0f);
	DemoPair pair;
	const auto player = pair.AddPlayer(2 * side);
	pair.Move(0, player, -2 * side);
	const auto arrived = pair.Arrive(0, player, -2 * side, -240 * side);
	pair.Move(1, arrived, 2 * side);
	(void)pair.Arrive(1, arrived, 2 * side, 240 * side);
}
