// The native local eye is available before startup source and respects its mode.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <client/Scene.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.camera")
TEST_DEPENDS("engine.scene.part")
TEST_DEPENDS("engine.ecs.scheduler")

namespace {
	struct LocalCameraScript {
		std::filesystem::path Path;
		std::filesystem::path Companion;

		LocalCameraScript(bool javascript, bool replaceFallback = false) {
			engine::parallel::Jobs::Start(1);
			Path = engine::core::Paths::Base() / (javascript ? "local-camera.js" : "local-camera.luau");
			Companion = Path.parent_path() / "client" / "DemoCameras" /
						(javascript ? "local-camera.client.js" : "local-camera.client.luau");
			std::filesystem::create_directories(Companion.parent_path());
			std::ofstream source(Path);
			REQUIRE(source);
			source
				<< (javascript ? "// The builder deliberately leaves camera control to its LocalScript.\n"
							   : "-- The builder deliberately leaves camera control to its LocalScript.\n");
			std::ofstream camera(Companion);
			REQUIRE(camera);
			if (replaceFallback) {
				camera
					<< (javascript ? "const camera = Instance.new('Camera', workspace);\ncamera.Name = "
									 "'Camera';\nworkspace.CurrentCamera = camera;\n"
								   : "local camera = Instance.new('Camera', workspace)\ncamera.Name = "
									 "'Camera'\nworkspace.CurrentCamera = camera\n");
			} else {
				camera
					<< (javascript ? "const camera = workspace.CurrentCamera;\n"
								   : "local camera = workspace.CurrentCamera\n");
			}
			camera
				<< (javascript ? R"(
if (!camera || camera.Name !== 'Camera') throw new Error('missing local camera');
camera.CameraType = Enum.CameraType.Scriptable;
camera.CFrame = CFrame.new(80, 90, 100);
camera.FieldOfView = 55;
)"
							   : R"(
assert(camera and camera.Name == 'Camera')
camera.CameraType = Enum.CameraType.Scriptable
camera.CFrame = CFrame.new(80, 90, 100)
camera.FieldOfView = 55
)");
		}

		~LocalCameraScript() {
			std::filesystem::remove(Path);
			std::filesystem::remove(Companion);
			engine::parallel::Jobs::Stop();
		}
	};
}

TEST_CASE("standalone camera companions retire the unused native fallback", "[client][camera][script]") {
	const bool javascript = GENERATE(false, true);
	const LocalCameraScript source(javascript, true);
	engine::ecs::Store store("local-camera-replacement");
	engine::ecs::Scheduler systems;
	REQUIRE(client::BuildScriptedWorld(store, systems, source.Path.string(), 1));
	const auto *active = store.Resource<engine::scene::ActiveCamera>();
	REQUIRE(active != nullptr);
	CHECK(engine::ecs::Store::IsPredicted(active->Entity));
	CHECK(engine::ecs::IsClientLocalInstance(store, active->Entity));
	CHECK(
		store.Get<engine::scene::Transform>(active->Entity)->Frame.Position ==
		engine::core::Vector3{80, 90, 100}
	);
	size_t cameras = 0;
	store.Each<const engine::scene::Camera>([&](engine::ecs::Entity, const engine::scene::Camera &) {
		++cameras;
	});
	CHECK(cameras == 1);
	for (int tick = 0; tick < 3; ++tick)
		systems.Tick(store, 1.0f / 60);
	cameras = 0;
	store.Each<const engine::scene::Camera>([&](engine::ecs::Entity, const engine::scene::Camera &) {
		++cameras;
	});
	CHECK(cameras == 1);
}

TEST_CASE("standalone startup source controls the native local camera", "[client][camera][script]") {
	const bool javascript = GENERATE(false, true);
	const LocalCameraScript source(javascript);
	engine::ecs::Store store("local-camera-startup");
	engine::ecs::Scheduler systems;
	std::shared_ptr<engine::script::Runtime> runtime;
	REQUIRE(client::BuildScriptedWorld(store, systems, source.Path.string(), 1, &runtime));
	REQUIRE(runtime != nullptr);
	CHECK(runtime->LastError().empty());
	const auto *active = store.Resource<engine::scene::ActiveCamera>();
	REQUIRE(active != nullptr);
	const auto camera = active->Entity;
	REQUIRE(store.Alive(camera));
	CHECK(store.ClassOf(camera) == engine::scene::CameraClass());
	CHECK(store.InstanceNameOf(camera) == engine::core::Name("Camera"));
	CHECK(engine::ecs::Store::IsPredicted(camera));
	CHECK(store.Has<engine::ecs::ClientLocal>(camera));
	CHECK(store.Has<engine::scene::TransientComponent>(camera));
	CHECK(store.ParentOf(camera) == engine::scene::WorkspaceOf(store));
	CHECK(store.Resource<engine::scene::CameraController>()->Mode == engine::scene::CameraMode::Scriptable);

	for (int tick = 0; tick < 30; ++tick) {
		systems.Tick(store, 1.0f / 60);
	}
	CHECK(store.Get<engine::scene::Transform>(camera)->Frame.Position == engine::core::Vector3{80, 90, 100});
	const engine::core::Name custom("Custom");
	REQUIRE(store.SetProperty(camera, engine::core::Name("CameraType"), &custom, sizeof(custom)));
	systems.Tick(store, 1.0f / 60);
	CHECK(store.Get<engine::scene::Transform>(camera)->Frame.Position != engine::core::Vector3{80, 90, 100});
}
