#include <engine/core/Paths.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <filesystem>
#include <studio/PlayLink.hpp>
#include <studio/Viewports.hpp>

TEST_SUITE_ID("studio.bladeborne-camera")
TEST_DEPENDS("engine.examples.demos-loader")

namespace {
	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	constexpr float FRAME_SECONDS = 1.0f / 60.0f;

	struct BladeborneCameraFixture {
		std::filesystem::path PreviousAssets = engine::core::Paths::Assets();
		engine::world::Universe Worlds;
		engine::world::WorldId Authority;
		studio::PlayLink Link;

		BladeborneCameraFixture() {
			engine::parallel::Jobs::Start(1);
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
			const auto demo = engine::examples::DemosLoader().Find(
				engine::examples::DemoKind::World, "BladeborneDemo.aworld"
			);
			REQUIRE(demo.has_value());
			std::string failure;
			Authority = engine::game::ImportWorld(Worlds, demo->Path, {}, failure);
			INFO(failure);
			REQUIRE(Authority.IsValid());
			Worlds.Enter(Authority, [&](Store &store, engine::ecs::Scheduler &systems) {
				store.Observe<engine::scene::Transform>();
				client::InstallPresentation(store, systems);
				const Entity runtimeCamera =
					studio::CreateRuntimeCamera(store, studio::DefaultViewportCamera());
				REQUIRE(runtimeCamera != engine::ecs::NULL_ENTITY);
				engine::script::RuntimeLimits limits;
				limits.Role = {.Server = true, .Client = true, .Studio = true};
				REQUIRE(engine::game::StartWorldScripts(store, systems, limits, failure) != nullptr);
				INFO(failure);
				REQUIRE(failure.empty());
				const auto *active = store.Resource<engine::scene::ActiveCamera>();
				REQUIRE(active != nullptr);
				CHECK(active->Entity == runtimeCamera);
			});
			REQUIRE(Link.Start(Worlds, Authority, 60, failure));
			INFO(failure);
			REQUIRE(failure.empty());
			Step();
		}

		~BladeborneCameraFixture() {
			Link.Stop(Worlds);
			engine::core::Paths::SetAssetsOverride(PreviousAssets);
			engine::parallel::Jobs::Stop();
		}

		void Step() {
			for (int tick = 0; tick < 24; ++tick) {
				Link.Step(Worlds);
				Worlds.Tick(FRAME_SECONDS);
			}
			Worlds.Present(Link.ReplicaWorld(), FRAME_SECONDS, 1);
		}

		void CheckHud() {
			engine::gui::RegisterGuiClasses();
			Worlds.Enter(Link.ReplicaWorld(), [&](Store &store) {
				const Entity playerGui = store.FindFirstChild(Link.Player(), "PlayerGui");
				REQUIRE(playerGui != engine::ecs::NULL_ENTITY);
				const Entity hud = store.FindFirstChild(playerGui, "BladeborneHUD");
				REQUIRE(hud != engine::ecs::NULL_ENTITY);
				CHECK(store.ClassOf(hud) == engine::gui::GuiClass("ScreenGui"));
				CHECK(engine::scene::InPlayerGui(store, hud, Link.Player()));

				engine::gui::Compiled compiled;
				engine::gui::CompileRequest request;
				request.Display.Width = 1280.0f;
				request.Display.Height = 720.0f;
				request.Viewer = Link.Player();
				request.ScreenGuis = engine::gui::ScreenGuiSource::PlayerGui;
				REQUIRE(compiled.Rebuild(store, request));
				const auto &draw = compiled.Commands();
				CHECK(draw.Elements > 0);
				CHECK(
					std::any_of(
						draw.Commands.begin(),
						draw.Commands.end(),
						[](const engine::gui::DrawCommand &command) {
							return command.Kind == engine::gui::DrawKind::Text;
						}
					)
				);
			});
		}

		Entity Root() {
			Entity root;
			Worlds.Enter(Authority, [&](const Store &store) {
				const auto *rig =
					store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, Link.Player()));
				REQUIRE(rig != nullptr);
				root = rig->Root;
			});
			return root;
		}

		CFrame EyeFollowing(Entity root) {
			CFrame frame;
			Worlds.Enter(Link.ReplicaWorld(), [&](Store &store) {
				const auto *active = store.Resource<engine::scene::ActiveCamera>();
				REQUIRE(active != nullptr);
				REQUIRE(store.Alive(active->Entity));
				CHECK(engine::scene::CameraSubjectRoot(store, active->Entity) == root);
				CHECK(Store::IsPredicted(active->Entity));
				CHECK(store.InstanceNameOf(active->Entity) == Name("Camera"));
				CHECK(store.Has<engine::ecs::ClientLocal>(active->Entity));
				CHECK(store.ParentOf(active->Entity) == engine::scene::WorkspaceOf(store));
				size_t cameras = 0;
				store.Each<const engine::scene::Camera>([&](Entity entity, const engine::scene::Camera &) {
					if (store.ClassOf(entity) == engine::scene::CameraClass()) ++cameras;
				});
				CHECK(cameras == 1);
				const auto *placed = store.Get<engine::scene::Transform>(active->Entity);
				REQUIRE(placed != nullptr);
				frame = placed->Frame;
			});
			return frame;
		}
	};
}

TEST_CASE("Bladeborne's local camera starts and follows movement and respawn", "[studio][bladeborne]") {
	BladeborneCameraFixture fixture;
	fixture.CheckHud();
	const Entity originalRoot = fixture.Root();
	const CFrame originalEye = fixture.EyeFollowing(originalRoot);

	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		auto *placed = store.GetMutable<engine::scene::Transform>(originalRoot);
		REQUIRE(placed != nullptr);
		placed->Frame.Position.X += 12;
	});
	fixture.Step();
	const CFrame movedEye = fixture.EyeFollowing(originalRoot);
	CHECK_THAT(movedEye.Position.X - originalEye.Position.X, Catch::Matchers::WithinAbs(12, .06));

	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		REQUIRE(engine::scene::LoadCharacter(store, fixture.Link.Player()) != engine::ecs::NULL_ENTITY);
	});
	fixture.Step();
	const Entity respawnedRoot = fixture.Root();
	REQUIRE(respawnedRoot != originalRoot);
	const CFrame respawnedEye = fixture.EyeFollowing(respawnedRoot);
	CHECK_THAT(respawnedEye.Position.X, Catch::Matchers::WithinAbs(originalEye.Position.X, .06));
}
