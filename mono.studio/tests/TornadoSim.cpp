#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/game/CollisionContent.hpp>
#include <engine/game/Game.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Services.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/GpuParticleField.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <client/Scene.hpp>
#include <cmath>
#include <filesystem>
#include <studio/PlayLink.hpp>
#include <studio/Presentation.hpp>
#include <studio/Viewports.hpp>

TEST_SUITE_ID("studio.tornado-sim")
TEST_DEPENDS("engine.examples.tornado-sim")

namespace {
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	constexpr float FRAME_SECONDS = 1.0f / 60.0f;

	double StormSeconds(const Store &store) {
		const auto anchor = store.FindFirstChild(engine::scene::WorkspaceOf(store), "StormAnchor", true);
		REQUIRE(anchor != engine::ecs::NULL_ENTITY);
		const auto *schema = engine::ecs::Schemas::Find(Name("Tornado.State"));
		REQUIRE(schema != nullptr);
		const auto *elapsed = schema->Find("ElapsedSeconds");
		REQUIRE(elapsed != nullptr);
		REQUIRE(elapsed->Type == engine::ecs::PropertyType::Double);
		const void *state = store.GetComponent(anchor, engine::ecs::Components::Find(Name("Tornado.State")));
		REQUIRE(state != nullptr);
		double scratch = 0;
		return *static_cast<const double *>(engine::ecs::Schemas::ReadField(state, *elapsed, &scratch));
	}

	engine::core::Vector3 StormPosition(const Store &store) {
		const auto anchor = store.FindFirstChild(engine::scene::WorkspaceOf(store), "StormAnchor", true);
		REQUIRE(anchor != engine::ecs::NULL_ENTITY);
		const auto *schema = engine::ecs::Schemas::Find(Name("Tornado.State"));
		REQUIRE(schema != nullptr);
		const auto *position = schema->Find("Position");
		REQUIRE(position != nullptr);
		REQUIRE(position->Type == engine::ecs::PropertyType::Vector3);
		const void *state = store.GetComponent(anchor, engine::ecs::Components::Find(Name("Tornado.State")));
		REQUIRE(state != nullptr);
		engine::core::Vector3 scratch;
		return *static_cast<const engine::core::Vector3 *>(
			engine::ecs::Schemas::ReadField(state, *position, &scratch)
		);
	}

	struct TornadoFixture {
		std::filesystem::path PreviousAssets = engine::core::Paths::Assets();
		engine::world::Universe Worlds;
		engine::world::WorldId Authority;
		studio::PlayLink Link;
		std::shared_ptr<engine::script::Runtime> AuthorityRuntime;

		TornadoFixture() {
			engine::parallel::Jobs::Start(1);
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
			const auto demo =
				engine::examples::DemosLoader().Find(engine::examples::DemoKind::World, "TornadoSim.aworld");
			REQUIRE(demo.has_value());
			std::string failure;
			Authority = engine::game::ImportWorld(Worlds, demo->Path, {}, failure);
			INFO(failure);
			REQUIRE(Authority.IsValid());
			Worlds.Enter(Authority, [&](Store &store, engine::ecs::Scheduler &systems) {
				client::InstallPresentation(store, systems);
				engine::physics::PreparePhysicsWorld(store);
				engine::physics::RegisterPhysicsSystems(systems);
				engine::game::RecordBuiltinCollisionShapes(store);
				REQUIRE(
					studio::CreateRuntimeCamera(store, studio::DefaultViewportCamera()) !=
					engine::ecs::NULL_ENTITY
				);
				engine::script::RuntimeLimits limits;
				limits.Role = {.Server = true, .Client = true, .Studio = true};
				AuthorityRuntime = engine::game::StartWorldScripts(store, systems, limits, failure);
				INFO(failure);
				REQUIRE(AuthorityRuntime != nullptr);
				REQUIRE(failure.empty());
			});
			REQUIRE(Link.Start(Worlds, Authority, 60, failure));
			INFO(failure);
			REQUIRE(failure.empty());
			Step(90);
		}

		~TornadoFixture() {
			AuthorityRuntime.reset();
			Link.Stop(Worlds);
			engine::core::Paths::SetAssetsOverride(PreviousAssets);
			engine::parallel::Jobs::Stop();
		}

		void Step(int ticks) {
			for (int tick = 0; tick < ticks; ++tick) {
				Link.Step(Worlds, [&](engine::world::WorldId world) {
					return world == Authority ? AuthorityRuntime.get() : nullptr;
				});
				Worlds.Tick(FRAME_SECONDS);
				Worlds.Present(Link.ReplicaWorld(), FRAME_SECONDS, 1);
				INFO(AuthorityRuntime->LastError());
				REQUIRE(AuthorityRuntime->LastError().empty());
			}
		}
	};
}

TEST_CASE("Tornado Studio Play runs replicated state and local visual layers", "[studio][tornado]") {
	TornadoFixture fixture;
	double firstReplicaSeconds = 0;
	Entity camera;
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](Store &store) {
		firstReplicaSeconds = StormSeconds(store);
		CHECK(firstReplicaSeconds > 0.0);
		const auto local = store.Resource<engine::scene::LocalPlayer>();
		const auto player = local != nullptr ? local->Instance : engine::ecs::NULL_ENTITY;
		const auto scripts = store.FindFirstChild(player, "PlayerScripts");
		const auto client = store.FindFirstChild(scripts, "TornadoClient", true);
		const auto selected = engine::script::ClientScriptsIn(store);
		REQUIRE(client != engine::ecs::NULL_ENTITY);
		CHECK(store.ParentOf(client) == scripts);
		CHECK(engine::scene::PlayerOwning(store, client) == player);
		CHECK(std::find(selected.begin(), selected.end(), client) != selected.end());
		CHECK(store.Get<engine::script::Program>(client) != nullptr);
		CHECK_FALSE(store.Has<engine::script::Disabled>(client));
		const auto workspace = engine::scene::WorkspaceOf(store);
		const auto prairie = store.FindFirstChild(workspace, "Storm Prairie", true);
		REQUIRE(prairie != engine::ecs::NULL_ENTITY);
		CHECK(Store::IsPredicted(prairie));
		CHECK(store.Has<engine::ecs::ClientLocal>(prairie));
		const auto groundSpray = store.FindFirstChild(workspace, "Funnel Ground Spray", true);
		REQUIRE(groundSpray != engine::ecs::NULL_ENTITY);
		CHECK(Store::IsPredicted(groundSpray));
		REQUIRE(store.Get<engine::scene::Volume>(groundSpray) != nullptr);
		CHECK(store.Get<engine::scene::Volume>(groundSpray)->Density > 0.0f);
		size_t localEmitters = 0;
		store.Each<const engine::effects::ParticleEmitter>([&](Entity entity,
															   const engine::effects::ParticleEmitter &) {
			if (store.Has<engine::ecs::ClientLocal>(entity)) ++localEmitters;
		});
		CHECK(localEmitters >= 45);
		const auto playerGui = store.FindFirstChild(fixture.Link.Player(), "PlayerGui");
		REQUIRE(playerGui != engine::ecs::NULL_ENTITY);
		const auto panel = store.FindFirstChild(playerGui, "TornadoSimPanel", true);
		REQUIRE(panel != engine::ecs::NULL_ENTITY);
		CHECK(Store::IsPredicted(panel));
		REQUIRE(store.FindFirstChild(panel, "CAM ORBIT", true) != engine::ecs::NULL_ENTITY);
		REQUIRE(store.FindFirstChild(panel, "P50M", true) != engine::ecs::NULL_ENTITY);
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		REQUIRE(active != nullptr);
		camera = active->Entity;
		REQUIRE(store.Alive(camera));
		CHECK(Store::IsPredicted(camera));
		CHECK(store.InstanceNameOf(camera).Text() == "Camera");
		CHECK(
			store.Resource<engine::scene::CameraController>()->Mode == engine::scene::CameraMode::Scriptable
		);
		const auto *placed = store.Get<engine::scene::Transform>(camera);
		REQUIRE(placed != nullptr);
		const auto stormOffset = StormPosition(store) - engine::core::Vector3{0, 0, 95};
		const auto expectedCamera = engine::core::CFrame::LookAt(
			engine::core::Vector3{-255, 125, -285} + stormOffset,
			engine::core::Vector3{240, 65, 95} + stormOffset
		);
		CHECK_THAT(placed->Frame.Position.X, Catch::Matchers::WithinAbs(expectedCamera.Position.X, .01f));
		CHECK_THAT(placed->Frame.Position.Y, Catch::Matchers::WithinAbs(expectedCamera.Position.Y, .01f));
		CHECK_THAT(placed->Frame.Position.Z, Catch::Matchers::WithinAbs(expectedCamera.Position.Z, .01f));
		CHECK((placed->Frame.LookVector() - expectedCamera.LookVector()).Magnitude() < .0001f);
		engine::render::WorldViewFrame frame;
		engine::render::CollectWorldView(store, fixture.Worlds.NameOf(fixture.Link.ReplicaWorld()), frame);
		CHECK(std::any_of(frame.Instances.begin(), frame.Instances.end(), [&](const auto &instance) {
			return instance.Source == prairie.Id;
		}));
		CHECK_FALSE(frame.Particles.Batches.empty());
		const auto *localParticles = store.Resource<engine::effects::ParticleSystem>();
		REQUIRE(localParticles != nullptr);
		CHECK(localParticles->DeviceStepped);
		CHECK(frame.Particles.BlockCount >= 45);
		CHECK(frame.Particles.Pool > 0);
		const auto gpuParticles = store.FindFirstChild(workspace, "TornadoParticles", true);
		REQUIRE(gpuParticles != engine::ecs::NULL_ENTITY);
		const auto *field = store.Get<engine::scene::GpuParticleField>(gpuParticles);
		REQUIRE(field != nullptr);
		CHECK(field->SpawnSamples.size() == engine::scene::MAX_GPU_PARTICLE_SPAWN_SAMPLES);
		CHECK(field->Styles[0].Size > field->Styles[1].Size);
		REQUIRE(frame.GpuParticles.has_value());
		CHECK(frame.GpuParticles->Source == gpuParticles);
		CHECK(frame.GpuParticles->Field.SpawnSamples == field->SpawnSamples);
		CHECK(frame.GpuParticles->ForceField.Source != engine::ecs::NULL_ENTITY);

		engine::render::View view;
		view.CameraFrame = placed->Frame;
		view.Camera = *store.Get<engine::scene::Camera>(camera);
		view.Instances = frame.Instances;
		engine::render::WorldCameraFrame layers;
		engine::render::CollectWorldCamera(store, view, {1600, 1000}, layers);
		CHECK(layers.VolumeCount > 0);
	});
	uint64_t stableFieldRevision = 0;
	engine::core::Vector3 initialFieldPosition;
	fixture.Worlds.Enter(fixture.Authority, [&](const Store &store) {
		stableFieldRevision = store.ComponentChangeVersion<engine::scene::GpuParticleField>();
		const auto particles =
			store.FindFirstChild(engine::scene::WorkspaceOf(store), "TornadoParticles", true);
		const auto *transform = store.Get<engine::scene::Transform>(particles);
		REQUIRE(transform != nullptr);
		initialFieldPosition = transform->Frame.Position;
	});
	fixture.Step(30);
	fixture.Worlds.Enter(fixture.Authority, [&](const Store &store) {
		CHECK(store.ComponentChangeVersion<engine::scene::GpuParticleField>() == stableFieldRevision);
		const auto particles =
			store.FindFirstChild(engine::scene::WorkspaceOf(store), "TornadoParticles", true);
		const auto *transform = store.Get<engine::scene::Transform>(particles);
		REQUIRE(transform != nullptr);
		CHECK((transform->Frame.Position - initialFieldPosition).Magnitude() > .01f);
	});
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](const Store &store) {
		CHECK(StormSeconds(store) > firstReplicaSeconds);
		CHECK(store.Resource<engine::scene::ActiveCamera>()->Entity == camera);
	});
	Entity controlDriver;
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](Store &store) {
		const auto playerScripts = store.FindFirstChild(fixture.Link.Player(), "PlayerScripts");
		const auto driver = store.CreatePredictedInstance(engine::script::LocalScriptClass(), "ControlProbe");
		controlDriver = driver;
		store.Set(driver, engine::ecs::ClientLocal{});
		REQUIRE(store.SetParent(driver, playerScripts));
		engine::script::SetSourcePath(store, driver, Name("tornado-control-probe.luau"));
		store.Set(driver, engine::script::Program{Name("tornado-control-probe.luau"), R"(
local panel = game:GetService('Players').LocalPlayer.PlayerGui:FindFirstChild("TornadoSimPanel", true)
assert(panel ~= nil)
panel:FindFirstChild("P50M", true):EmulateClick()
panel:FindFirstChild("CAM ORBIT", true):EmulateClick()
script:SetAttribute("ControlsInvoked", true)
)"});
		Name path;
		std::string program;
		std::string error;
		const bool ready = engine::script::ReadProgram(store, driver, path, program, error);
		INFO(error);
		REQUIRE(ready);
		REQUIRE_FALSE(program.empty());
		const auto selected = engine::script::ClientScriptsIn(store);
		REQUIRE(std::find(selected.begin(), selected.end(), driver) != selected.end());
	});
	fixture.Step(30);
	fixture.Worlds.Enter(fixture.Link.ReplicaWorld(), [&](Store &store) {
		engine::ecs::AttributeValue invoked;
		REQUIRE(engine::ecs::GetAttribute(store, controlDriver, Name("ControlsInvoked"), invoked));
		CHECK(invoked.Bool);
		CHECK_FALSE(store.Has<engine::script::Disabled>(controlDriver));
		CHECK(store.Resource<engine::scene::ActiveCamera>()->Entity == camera);
		const auto *placed = store.Get<engine::scene::Transform>(camera);
		REQUIRE(placed != nullptr);
		const auto orbitCentre = StormPosition(store);
		CHECK_THAT(placed->Frame.Position.Y, Catch::Matchers::WithinAbs(72.0f + orbitCentre.Y, .01f));
		const auto orbitOffset = placed->Frame.Position - orbitCentre;
		CHECK_THAT(std::hypot(orbitOffset.X, orbitOffset.Z), Catch::Matchers::WithinAbs(205.0f, .01f));
		engine::render::WorldViewFrame frame;
		engine::render::CollectWorldView(store, fixture.Worlds.NameOf(fixture.Link.ReplicaWorld()), frame);
		REQUIRE(frame.GpuParticles.has_value());
		CHECK(frame.GpuParticles->Field.RequestedCount == 50'000'000);
	});
	fixture.Worlds.Enter(fixture.Authority, [&](const Store &store) {
		const auto particles =
			store.FindFirstChild(engine::scene::WorkspaceOf(store), "TornadoParticles", true);
		REQUIRE(store.Get<engine::scene::GpuParticleField>(particles) != nullptr);
		CHECK(store.Get<engine::scene::GpuParticleField>(particles)->RequestedCount == 50'000'000);
	});
}
