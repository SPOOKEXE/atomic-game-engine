#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/DataScriptPackage.hpp>
#include <engine/script/DataScriptPackageTransaction.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <client/WorldSystems.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("client.data-script-package-transaction")

namespace {
	using engine::script::DataScriptPackageContext;
	using engine::script::DataScriptPackageRunResult;
	using engine::script::DataScriptRequest;
	using engine::script::DataScriptResult;
	using engine::script::Runtime;
	using engine::script::RuntimeLimits;
	using engine::world::DataFactoryPauseScope;
	using engine::world::DataFactorySession;
	using engine::world::DataFactoryStatus;
	using engine::world::Universe;
	using engine::world::WorldId;

	constexpr std::string_view INSTANCE_ID = "client.world";

	std::string HashOf(std::string_view text) {
		return engine::assets::Hasher::Of(std::as_bytes(std::span(text.data(), text.size()))).ToHex();
	}

	std::string Manifest(std::string_view source, bool worldCapability = false) {
		return "{\"format\":\"atomic.data-script.v1\",\"entry\":\"package.luau\",\"source_hash\":\"" +
			   HashOf(source) + "\",\"assets\":[],\"parameters\":[],\"capabilities\":" +
			   (worldCapability ? "[\"world\"]" : "[]") +
			   ",\"budget\":{\"source_bytes\":1024,"
			   "\"asset_bytes\":1024,\"assets\":0,\"parameters\":0},\"seed\":0}";
	}

	std::vector<std::byte> Save(const Universe &worlds, size_t limit = 64 * 1024 * 1024) {
		engine::core::ByteWriter bytes(0, limit);
		REQUIRE(worlds.Save(bytes));
		return {bytes.Bytes().begin(), bytes.Bytes().end()};
	}

	class TrackedRuntime final : public Runtime {
	  public:
		TrackedRuntime(engine::ecs::Store &store, const RuntimeLimits &limits, bool &destroyed)
			: Runtime(store, limits), Destroyed(destroyed) {}
		~TrackedRuntime() override {
			Destroyed = true;
		}
		bool Run(std::string_view, std::string_view) override {
			return true;
		}
		bool RunInstance(engine::ecs::Entity) override {
			return false;
		}
		bool Heartbeat(float) override {
			return true;
		}
		engine::script::Language Which() const override {
			return engine::script::Language::Luau;
		}

	  private:
		bool &Destroyed;
	};

	struct Fixture {
		Universe Worlds;
		DataFactorySession Session;
		WorldId World;
		std::unique_ptr<Runtime> Active;
		unsigned Installed = 0;
		unsigned Discarded = 0;

		explicit Fixture(size_t checkpointBytes = 64 * 1024 * 1024) : Session(Worlds, 16, checkpointBytes) {
			engine::scene::EnsureClassTree();
			World = Worlds.Create({.Name = engine::core::Name(INSTANCE_ID)});
			Session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) {
				return true;
			});
		}

		DataScriptRequest Request(std::string source = "return", bool worldCapability = false) {
			const auto before = Session.Inspect(INSTANCE_ID);
			const auto paused =
				Session.AllSystemsPaused(INSTANCE_ID)
					? before
					: Session.Pause(INSTANCE_ID, DataFactoryPauseScope::AllSystems, before.Clock.Tick);
			REQUIRE(paused.Status == DataFactoryStatus::Ok);
			return {
				.InstanceId = std::string(INSTANCE_ID),
				.Manifest = Manifest(source, worldCapability),
				.Source = std::move(source),
				.Assets = {},
				.SourceHash = {},
				.Name = "transaction-test",
				.ExpectedTick = paused.Clock.Tick,
				.ExpectedEpoch = paused.WorldEpoch,
				.ExpectedVersion = paused.WorldVersion,
			};
		}

		engine::script::DataScriptPackageTransactionDependencies
		Dependencies(engine::script::DataScriptPackageRunner run = {}) {
			return {
				.Universe = Worlds,
				.Session = Session,
				.RuntimeOf = [this](WorldId world) -> Runtime * {
					return world == World ? Active.get() : nullptr;
				},
				.DiscardRuntime =
					[this](WorldId) {
						Active.reset();
						Discarded++;
					},
				.MakeRuntime =
					[](engine::ecs::Store &store, const RuntimeLimits &limits) {
						return engine::script::MakeRuntime(store, engine::script::Language::Luau, limits);
					},
				.RunPackage = std::move(run),
				.InstallSystems = [this](engine::ecs::Store &, engine::ecs::Scheduler &) { Installed++; },
				.Admit =
					[](std::string_view source, std::string_view entry, std::string &error) {
						return engine::script::CheckDataScriptPackageSource(
							engine::script::Language::Luau, source, entry, error
						);
					},
				.Role = engine::script::HostRole::OfBoth(),
				.Present = true,
			};
		}

		void OpenActiveRuntime() {
			Worlds.Enter(World, [this](engine::ecs::Store &store) {
				Active = engine::script::MakeRuntime(store, engine::script::Language::Luau);
			});
			REQUIRE(Active != nullptr);
		}

		void PrepareClientWorld() {
			Worlds.Enter(World, [](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				client::InstallPresentation(store, systems);
				(void)client::InstallDefaultCamera(store, systems);
				engine::physics::PreparePhysicsWorld(store);
				engine::physics::SetPhysicsTickRate(store, 120.0);
				engine::physics::SetPhysicsPaused(store, true);
				auto *clock = engine::physics::PhysicsClockOf(store);
				REQUIRE(clock != nullptr);
				store.ResourceMutable<engine::physics::PhysicsClock>()->Accumulator = 0.125;
				store.ResourceMutable<engine::physics::PhysicsClock>()->Steps = 17;
				engine::scene::PrepareGravity(store);
			});
		}

		engine::script::DataScriptPackageTransactionDependencies ProductDependencies() {
			auto dependencies = Dependencies();
			dependencies.InstallSystems = [](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				client::InstallPresentation(store, systems);
				(void)client::RestoreDefaultCameraMovement(store, systems);
				(void)client::InstallDefaultCamera(store, systems);
				client::InstallClientWorldSystems(store, systems);
			};
			return dependencies;
		}
	};
}

TEST_CASE(
	"client transaction replaces a static world only after a terminal Luau package",
	"[client][data-script-package]"
) {
	Fixture fixture;
	const DataScriptResult first =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), fixture.Request());
	REQUIRE(first.Ran);
	CHECK(first.Atomic);
	CHECK(first.Error.empty());
	CHECK(fixture.Installed == 1);

	const DataScriptResult second =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), fixture.Request());
	CHECK(second.Ran);
	CHECK(second.Lifecycle.WorldVersion == first.Lifecycle.WorldVersion + 1);
	CHECK(fixture.Installed == 2);
}

TEST_CASE(
	"client transaction refuses a sticky active runtime without touching its live bytes",
	"[client][data-script-package]"
) {
	Fixture fixture;
	fixture.OpenActiveRuntime();
	engine::script::DataScriptPackage package;
	const DataScriptPackageContext context(package, std::span<const engine::script::DataScriptAssetInput>{});
	(void)fixture.Active->RunDataScriptPackage(context, "return", "already-used.luau");
	REQUIRE_FALSE(fixture.Active->CanDiscardForWorldSwap());
	const DataScriptRequest request = fixture.Request();
	const std::vector<std::byte> before = Save(fixture.Worlds);
	const auto revision = fixture.Session.Inspect(INSTANCE_ID);

	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), request);
	CHECK_FALSE(result.Ran);
	CHECK(result.Error == "active_script_runtime_unsupported");
	CHECK(Save(fixture.Worlds) == before);
	CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);
}

TEST_CASE(
	"client transaction preserves live bytes and revision when package work fails or throws",
	"[client][data-script-package]"
) {
	for (const bool throws : {false, true}) {
		Fixture fixture;
		const DataScriptRequest request = fixture.Request();
		const std::vector<std::byte> before = Save(fixture.Worlds);
		const auto revision = fixture.Session.Inspect(INSTANCE_ID);
		const DataScriptResult result = engine::script::ExecuteDataScriptPackageTransaction(
			fixture.Dependencies(
				[throws](Runtime &, const DataScriptPackageContext &, std::string_view, std::string_view) {
					if (throws) throw std::runtime_error("deliberate package exception");
					return DataScriptPackageRunResult{
						.Terminal = DataScriptPackageRunResult::State::Failed,
						.Error = "deliberate package failure"
					};
				}
			),
			request
		);
		CHECK_FALSE(result.Ran);
		CHECK(result.Error.find("deliberate package") != std::string::npos);
		CHECK(Save(fixture.Worlds) == before);
		CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);
	}
}

TEST_CASE(
	"client transaction refuses invalid Luau source before it copies or installs a world",
	"[client][data-script-package]"
) {
	for (const std::string_view source : {"local =", "local count: number = 'wrong'"}) {
		Fixture fixture;
		const DataScriptRequest request = fixture.Request(std::string(source));
		const std::vector<std::byte> before = Save(fixture.Worlds);
		const auto revision = fixture.Session.Inspect(INSTANCE_ID);

		const DataScriptResult result =
			engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), request);
		CHECK_FALSE(result.Ran);
		CHECK_FALSE(result.Atomic);
		CHECK(result.Error.starts_with("data-script package"));
		CHECK(Save(fixture.Worlds) == before);
		CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);
		CHECK(fixture.Installed == 0);
	}
}

TEST_CASE(
	"client transaction stale request and checkpoint cap preserve exact live bytes",
	"[client][data-script-package]"
) {
	Fixture fixture;
	DataScriptRequest stale = fixture.Request();
	stale.ExpectedVersion++;
	const std::vector<std::byte> before = Save(fixture.Worlds);
	const auto revision = fixture.Session.Inspect(INSTANCE_ID);
	const DataScriptResult staleResult =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), stale);
	CHECK_FALSE(staleResult.Ran);
	CHECK(Save(fixture.Worlds) == before);
	CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);

	Fixture limited(1);
	const DataScriptResult capResult =
		engine::script::ExecuteDataScriptPackageTransaction(limited.Dependencies(), limited.Request());
	CHECK_FALSE(capResult.Ran);
	CHECK(capResult.Error == "live world exceeds the configured checkpoint byte limit");
}

TEST_CASE(
	"client transaction refuses a supplied source hash that differs from its manifest",
	"[client][data-script-package]"
) {
	Fixture fixture;
	DataScriptRequest request = fixture.Request();
	request.SourceHash = std::string(64, '0');
	const std::vector<std::byte> before = Save(fixture.Worlds);
	const auto revision = fixture.Session.Inspect(INSTANCE_ID);
	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), request);
	CHECK_FALSE(result.Ran);
	CHECK(result.Error == "source_hash does not match the package source_hash");
	CHECK(Save(fixture.Worlds) == before);
	CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);
}

TEST_CASE("client transaction identifies an unknown requested instance", "[client][data-script-package]") {
	Fixture fixture;
	DataScriptRequest request = fixture.Request();
	request.InstanceId = "missing.world";
	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), request);
	CHECK_FALSE(result.Ran);
	CHECK(result.Error == "unknown instance_id");
	CHECK(result.Lifecycle.InstanceId == "missing.world");
}

TEST_CASE(
	"client transaction leaves a pending render-only presentation untouched", "[client][data-script-package]"
) {
	Fixture fixture;
	const DataScriptRequest request = fixture.Request();
	std::string snapshot;
	REQUIRE(fixture.Session.Snapshot(INSTANCE_ID, snapshot).Status == DataFactoryStatus::Ok);
	fixture.Session.SetRenderOnlyPresenter([](const engine::world::DataFactoryRenderOnlyRequest &,
											  std::string &) { return true; });
	const auto revision = fixture.Session.Inspect(INSTANCE_ID);
	const auto pending = fixture.Session.RenderOnly(
		{.InstanceId = std::string(INSTANCE_ID),
		 .SnapshotId = snapshot,
		 .ExpectedWorldEpoch = revision.WorldEpoch,
		 .ExpectedWorldVersion = revision.WorldVersion,
		 .ExpectedTick = revision.Clock.Tick}
	);
	REQUIRE(pending.Status == DataFactoryStatus::Pending);
	const std::vector<std::byte> before = Save(fixture.Worlds);

	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.Dependencies(), request);
	CHECK_FALSE(result.Ran);
	CHECK(result.Error == "render-only presentation is in flight");
	CHECK(Save(fixture.Worlds) == before);
	CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);
}

TEST_CASE(
	"client transaction preserves physics state and reinstalls client systems",
	"[client][data-script-package]"
) {
	Fixture fixture;
	fixture.PrepareClientWorld();
	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.ProductDependencies(), fixture.Request());
	REQUIRE(result.Ran);
	fixture.Worlds.Enter(
		fixture.Worlds.Find(engine::core::Name(INSTANCE_ID)),
		[](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			const auto *clock = engine::physics::PhysicsClockOf(store);
			REQUIRE(clock != nullptr);
			CHECK(clock->Rate == 120.0);
			CHECK(clock->Accumulator == 0.125);
			CHECK(clock->Steps == 17);
			CHECK(clock->Paused);
			CHECK(store.HasResource<engine::scene::Gravity>());
			CHECK(systems.HasSystem("scene.gravity", engine::ecs::Phase::PreSimulation));
			CHECK(systems.HasSystem("scene.ownership", engine::ecs::Phase::PreSimulation));
			CHECK(systems.HasSystem("move-camera", engine::ecs::Phase::Simulation));
		}
	);
	CHECK(fixture.Session.Resume(INSTANCE_ID, result.Lifecycle.Clock.Tick).Status == DataFactoryStatus::Ok);
}

TEST_CASE(
	"client transaction leaves a package-authored active camera fixed after system install",
	"[client][data-script-package]"
) {
	Fixture fixture;
	fixture.PrepareClientWorld();
	const std::string source = R"(
local workspace = game:GetService("Workspace")
local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(1, 2, 3)
camera.CameraSubjectAutomatic = false
camera.Parent = workspace
workspace.CurrentCamera = camera
return
)";
	const DataScriptResult result = engine::script::ExecuteDataScriptPackageTransaction(
		fixture.ProductDependencies(), fixture.Request(source, true)
	);
	REQUIRE(result.Ran);
	fixture.Worlds.Enter(
		fixture.Worlds.Find(engine::core::Name(INSTANCE_ID)),
		[](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			const auto *active = store.Resource<engine::scene::ActiveCamera>();
			REQUIRE(active != nullptr);
			const auto *before = store.Get<engine::scene::Transform>(active->Entity);
			REQUIRE(before != nullptr);
			CHECK(before->Frame.Position.X == 1.0f);
			CHECK(before->Frame.Position.Y == 2.0f);
			CHECK(before->Frame.Position.Z == 3.0f);
			CHECK_FALSE(systems.HasSystem("move-camera", engine::ecs::Phase::Simulation));

			systems.Tick(store, 1.0f / 60.0f);
			const auto *after = store.Get<engine::scene::Transform>(active->Entity);
			REQUIRE(after != nullptr);
			CHECK(after->Frame.Position.X == 1.0f);
			CHECK(after->Frame.Position.Y == 2.0f);
			CHECK(after->Frame.Position.Z == 3.0f);
		}
	);
}

TEST_CASE(
	"client transaction fences package presentation before its committed snapshot",
	"[client][data-script-package]"
) {
	Fixture fixture;
	fixture.PrepareClientWorld();
	const std::string source = R"(
local workspace = game:GetService("Workspace")
local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(4, 5, 6)
camera.Parent = workspace
workspace.CurrentCamera = camera

local part = Instance.new("Part")
part.Name = "FencePart"
part.CFrame = CFrame.new(1, 2, 3)
part:SetAttribute("DataFactoryId", "fixture/fence")
part:SetAttribute("DataFactorySemanticId", "fixture/box")
part:SetAttribute("DataFactoryPartId", "fixture/part/fence")
part.Parent = workspace
return
)";
	const DataScriptRequest request = fixture.Request(source, true);
	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.ProductDependencies(), request);
	REQUIRE(result.Ran);
	CHECK(result.Lifecycle.Clock.Tick == request.ExpectedTick);

	const WorldId committed = fixture.Worlds.Find(engine::core::Name(INSTANCE_ID));
	std::vector<std::byte> beforeSecondPresent = Save(fixture.Worlds);
	fixture.Worlds.Enter(committed, [](engine::ecs::Store &store) {
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		REQUIRE(active != nullptr);
		const auto *camera = store.Get<engine::scene::Transform>(active->Entity);
		REQUIRE(camera != nullptr);
		CHECK(camera->Frame.Position.X == 4.0f);
		CHECK(camera->Frame.Position.Y == 5.0f);
		CHECK(camera->Frame.Position.Z == 6.0f);

		const auto *draw = store.Resource<engine::render::DrawList>();
		REQUIRE(draw != nullptr);
		REQUIRE(draw->Instances.size() == 1);
		REQUIRE(draw->ObjectLabelsValid);
		REQUIRE(draw->ObjectLabels.size() == 1);
		CHECK(draw->ObjectLabels.front().StableId == "fixture/fence");
		REQUIRE(draw->SemanticLabelsValid);
		REQUIRE(draw->SemanticLabels.size() == 1);
		CHECK(draw->SemanticLabels.front().StableId == "fixture/box");
		REQUIRE(draw->PartLabelsValid);
		REQUIRE(draw->PartLabels.size() == 1);
		CHECK(draw->PartLabels.front().StableId == "fixture/part/fence");
	});

	REQUIRE(fixture.Worlds.Present(committed, 0.0f, 1.0f) == engine::world::WorldStatus::Ok);
	CHECK(Save(fixture.Worlds) == beforeSecondPresent);
}

TEST_CASE(
	"client transaction preserves the live world when package presentation throws",
	"[client][data-script-package]"
) {
	Fixture fixture;
	const DataScriptRequest request = fixture.Request();
	const std::vector<std::byte> before = Save(fixture.Worlds);
	const auto revision = fixture.Session.Inspect(INSTANCE_ID);
	auto dependencies = fixture.Dependencies();
	dependencies.InstallSystems = [](engine::ecs::Store &, engine::ecs::Scheduler &systems) {
		systems.Add("package-presentation-fault", engine::ecs::Phase::Render, [](engine::ecs::Store &) {
			throw std::runtime_error("deliberate presentation exception");
		});
	};

	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(dependencies, request);
	CHECK_FALSE(result.Ran);
	CHECK(result.Error.find("deliberate presentation exception") != std::string::npos);
	CHECK(Save(fixture.Worlds) == before);
	CHECK(fixture.Session.Inspect(INSTANCE_ID).WorldVersion == revision.WorldVersion);
}

TEST_CASE("client transaction preserves a current physics-only pause", "[client][data-script-package]") {
	Fixture fixture;
	fixture.PrepareClientWorld();
	fixture.Session.SetPauseParticipant(
		[&fixture](engine::world::WorldId world, DataFactoryPauseScope scope, bool paused, std::string &) {
			if (scope != DataFactoryPauseScope::PhysicsOnly) return true;
			fixture.Worlds.Enter(world, [paused](engine::ecs::Store &store) {
				engine::physics::SetPhysicsPaused(store, paused);
			});
			return true;
		}
	);
	const auto before = fixture.Session.Inspect(INSTANCE_ID);
	REQUIRE(
		fixture.Session.Pause(INSTANCE_ID, DataFactoryPauseScope::PhysicsOnly, before.Clock.Tick).Status ==
		DataFactoryStatus::Ok
	);

	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.ProductDependencies(), fixture.Request());
	REQUIRE(result.Ran);
	fixture.Worlds.Enter(fixture.Worlds.Find(engine::core::Name(INSTANCE_ID)), [](engine::ecs::Store &store) {
		CHECK(engine::physics::IsPhysicsPaused(store));
	});
}

TEST_CASE("client transaction prepares physics for an empty loaded world", "[client][data-script-package]") {
	Fixture fixture;
	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(fixture.ProductDependencies(), fixture.Request());
	REQUIRE(result.Ran);
	fixture.Worlds.Enter(
		fixture.Worlds.Find(engine::core::Name(INSTANCE_ID)),
		[](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			CHECK(engine::physics::PhysicsClockOf(store) != nullptr);
			CHECK(systems.HasSystem("physics.simulation", engine::ecs::Phase::Simulation));
		}
	);
	REQUIRE(fixture.Session.Resume(INSTANCE_ID, result.Lifecycle.Clock.Tick).Status == DataFactoryStatus::Ok);
	fixture.Worlds.Enter(
		fixture.Worlds.Find(engine::core::Name(INSTANCE_ID)),
		[](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			CHECK_NOTHROW(systems.Tick(store, 1.0f / 60.0f));
			CHECK(engine::physics::PhysicsClockOf(store) != nullptr);
		}
	);
}

TEST_CASE(
	"client transaction destroys its package runtime before the live swap", "[client][data-script-package]"
) {
	Fixture fixture;
	bool packageDestroyed = false;
	bool discardedAfterDestroy = false;
	auto dependencies = fixture.Dependencies(
		[](Runtime &, const DataScriptPackageContext &, std::string_view, std::string_view) {
			return DataScriptPackageRunResult{
				.Terminal = DataScriptPackageRunResult::State::Completed, .Error = {}
			};
		}
	);
	dependencies.MakeRuntime = [&packageDestroyed](engine::ecs::Store &store, const RuntimeLimits &limits) {
		return std::make_unique<TrackedRuntime>(store, limits, packageDestroyed);
	};
	dependencies.DiscardRuntime = [&packageDestroyed, &discardedAfterDestroy](WorldId) {
		discardedAfterDestroy = packageDestroyed;
	};
	const DataScriptResult result =
		engine::script::ExecuteDataScriptPackageTransaction(dependencies, fixture.Request());
	CHECK(result.Ran);
	CHECK(packageDestroyed);
	CHECK(discardedAfterDestroy);
}
