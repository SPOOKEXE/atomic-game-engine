#include <engine/assets/ContentHash.hpp>
#include <engine/control/Features.hpp>
#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Name.hpp>
#include <engine/scene/AuthoredAffordance.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/DataScriptPackage.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <studio/DataFactoryHost.hpp>
#include <studio/Editor.hpp>
#include <vector>

TEST_SUITE_ID("studio.datafactoryhost")
TEST_DEPENDS("engine.world.data-factory")
TEST_DEPENDS("engine.control.datascene")

namespace {
	class PackageRuntime final : public engine::script::Runtime {
	  public:
		PackageRuntime(engine::ecs::Store &store, const engine::script::RuntimeLimits &limits)
			: engine::script::Runtime(store, limits) {}
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
	};

	std::string PackageManifest(std::string_view source) {
		const std::string hash =
			engine::assets::Hasher::Of(std::as_bytes(std::span(source.data(), source.size()))).ToHex();
		return "{\"format\":\"atomic.data-script.v1\",\"entry\":\"package.luau\",\"source_hash\":\"" + hash +
			   "\",\"assets\":[],\"parameters\":[],\"capabilities\":[],\"budget\":{\"source_bytes\":1024,"
			   "\"asset_bytes\":0,\"assets\":0,\"parameters\":0},\"seed\":0}";
	}

	const engine::control::Tool &PackageTool(engine::control::Surface &surface) {
		for (const engine::control::Tool &tool : surface.Registered())
			if (tool.Name == "run_script_package") return tool;
		throw std::runtime_error("package tool was not installed");
	}

	nlohmann::json PackageRequest(const engine::world::DataFactoryReply &lifecycle, std::string operation) {
		return {
			{"instance_id", lifecycle.InstanceId},
			{"expected_tick", lifecycle.Clock.Tick},
			{"expected_world_epoch", lifecycle.WorldEpoch},
			{"expected_world_version", lifecycle.WorldVersion},
			{"operation_id", std::move(operation)},
			{"language", "luau"},
			{"manifest", PackageManifest("return")},
			{"source_base64", "cmV0dXJu"},
			{"assets", nlohmann::json::array()},
		};
	}

	nlohmann::json
	Ask(engine::control::Surface &surface, std::string_view method, nlohmann::json parameters = {}) {
		const nlohmann::json request{
			{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(parameters)}
		};
		return nlohmann::json::parse(surface.Answer(request.dump()));
	}

	nlohmann::json Call(
		engine::control::Surface &surface,
		std::string_view name,
		const nlohmann::json &arguments,
		bool &failed
	) {
		const nlohmann::json reply = Ask(surface, "tools/call", {{"name", name}, {"arguments", arguments}});
		const nlohmann::json &result = reply.at("result");
		failed = result.value("isError", false);
		return nlohmann::json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	studio::DataFactoryHostCallbacks Callbacks() {
		return {
			.Lifecycle = [](engine::world::DataFactoryWorldOperation,
							engine::world::Universe &,
							engine::world::WorldId,
							bool,
							std::string &) { return true; },
			.Pause = [](
						 engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &
					 ) { return true; },
			.Rehydrate =
				[](engine::world::Universe &, engine::world::WorldId, std::string &) { return true; },
		};
	}

	engine::world::DataFactoryWorldRequest Request(std::string instance, std::string operation) {
		return {
			.InstanceId = std::move(instance),
			.Seed = 41,
			.TickRate = 60.0,
			.OperationId = std::move(operation),
		};
	}
}

TEST_CASE("Studio keeps its optional control surface after an occupied port", "[studio][data-factory]") {
	engine::control::Server occupied;
	REQUIRE(occupied.Start(0));

	studio::Options options;
	options.Headless = true;
	options.MaximumFrames = 1;
	options.ControlPort = occupied.Port();
	studio::Editor editor;
	CHECK(editor.Initialise(options));
	editor.Shutdown();
}

TEST_CASE("Studio factory startup refuses an occupied required control port", "[studio][data-factory]") {
	engine::control::Server occupied;
	REQUIRE(occupied.Start(0));

	studio::Options options;
	options.Headless = true;
	options.MaximumFrames = 1;
	options.ControlPort = occupied.Port();
	options.DataFactory = true;
	studio::Editor editor;
	CHECK_FALSE(editor.Initialise(options));
}

TEST_CASE("Studio factory host owns an isolated lifecycle", "[studio][data-factory]") {
	engine::world::Universe worlds;
	studio::DataFactoryHost host;
	std::string detail;
	REQUIRE(host.Start(worlds, Callbacks(), detail));
	REQUIRE(host.Session() != nullptr);

	auto created = host.Session()->CreateWorld(Request("episode", "create"));
	REQUIRE(created.Status == engine::world::DataFactoryStatus::Ok);
	CHECK(worlds.Count() == 1);
	CHECK(host.Session()->AllSystemsPaused("episode"));

	auto reset = Request("episode", "reset");
	reset.ExpectedWorldEpoch = created.WorldEpoch;
	reset.ExpectedWorldVersion = created.WorldVersion;
	reset.ExpectedTick = created.Clock.Tick;
	auto replaced = host.Session()->ResetWorld(reset);
	REQUIRE(replaced.Status == engine::world::DataFactoryStatus::Ok);
	CHECK(replaced.WorldEpoch > created.WorldEpoch);
	CHECK(worlds.Count() == 1);

	auto retired = Request("episode", "retire");
	retired.ExpectedWorldEpoch = replaced.WorldEpoch;
	retired.ExpectedWorldVersion = replaced.WorldVersion;
	retired.ExpectedTick = replaced.Clock.Tick;
	const auto removed = host.Session()->RetireWorld(retired);
	CHECK(removed.Status == engine::world::DataFactoryStatus::Ok);
	CHECK(removed.Tombstone);
	CHECK(worlds.Count() == 0);
}

TEST_CASE("Studio factory runner advances only a resumed lifecycle world", "[studio][data-factory]") {
	engine::world::Universe worlds;
	studio::DataFactoryHost host;
	std::string detail;
	REQUIRE(host.Start(worlds, Callbacks(), detail));

	const auto created = host.Session()->CreateWorld(Request("episode", "create"));
	REQUIRE(created.Status == engine::world::DataFactoryStatus::Ok);
	CHECK_FALSE(host.Tick(1.0F / 60.0F));

	const auto resumed = host.Session()->Resume("episode", created.Clock.Tick);
	REQUIRE(resumed.Status == engine::world::DataFactoryStatus::Ok);
	CHECK(host.Tick(1.0F / 60.0F));
	const auto live = host.Session()->Inspect("episode");
	REQUIRE(live.Status == engine::world::DataFactoryStatus::Ok);
	CHECK(live.Clock.Tick == created.Clock.Tick + 1);

	const auto paused =
		host.Session()->Pause("episode", engine::world::DataFactoryPauseScope::AllSystems, live.Clock.Tick);
	REQUIRE(paused.Status == engine::world::DataFactoryStatus::Ok);
	CHECK_FALSE(host.Tick(1.0F / 60.0F));
	CHECK(host.Session()->Inspect("episode").Clock.Tick == live.Clock.Tick);
}

TEST_CASE(
	"Studio factory reset preserves old residency when candidate setup fails", "[studio][data-factory]"
) {
	engine::world::Universe worlds;
	struct Residency {
		bool Renderer = true;
		bool Portal = true;
		bool Particles = true;
	} residency;
	bool rejectReset = false;
	studio::DataFactoryHostCallbacks callbacks = Callbacks();
	callbacks.Lifecycle = [&residency, &rejectReset](
							  engine::world::DataFactoryWorldOperation operation,
							  engine::world::Universe &,
							  engine::world::WorldId,
							  bool committed,
							  std::string &
						  ) {
		if (operation == engine::world::DataFactoryWorldOperation::Reset && !committed && rejectReset)
			return false;
		if (operation == engine::world::DataFactoryWorldOperation::Reset && committed) {
			residency.Renderer = false;
			residency.Portal = false;
			residency.Particles = false;
		}
		return true;
	};

	studio::DataFactoryHost host;
	std::string detail;
	REQUIRE(host.Start(worlds, std::move(callbacks), detail));
	const auto created = host.Session()->CreateWorld(Request("episode", "create"));
	REQUIRE(created.Status == engine::world::DataFactoryStatus::Ok);
	const auto before = host.Session()->Inspect("episode");

	rejectReset = true;
	auto reset = Request("episode", "reset-refused");
	reset.ExpectedWorldEpoch = before.WorldEpoch;
	reset.ExpectedWorldVersion = before.WorldVersion;
	reset.ExpectedTick = before.Clock.Tick;
	const auto refused = host.Session()->ResetWorld(reset);
	CHECK(refused.Status == engine::world::DataFactoryStatus::RestoreIncomplete);
	const auto after = host.Session()->Inspect("episode");
	CHECK(after.WorldEpoch == before.WorldEpoch);
	CHECK(after.WorldVersion == before.WorldVersion);
	CHECK(after.Clock.Tick == before.Clock.Tick);
	CHECK(residency.Renderer);
	CHECK(residency.Portal);
	CHECK(residency.Particles);
}

TEST_CASE("Studio factory host refuses an existing compatibility world", "[studio][data-factory]") {
	engine::world::Universe worlds;
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("author-scene");
	REQUIRE(worlds.Create(settings).IsValid());

	studio::DataFactoryHost host;
	std::string detail;
	CHECK_FALSE(host.Start(worlds, Callbacks(), detail));
	CHECK(detail == "Studio data-factory mode requires an empty universe");
}

TEST_CASE("Studio factory tools omit unavailable render work", "[studio][data-factory]") {
	engine::world::Universe worlds;
	studio::DataFactoryHost host;
	std::string detail;
	studio::DataFactoryHostCallbacks callbacks = Callbacks();
	callbacks.PackageDependencies = [](engine::world::Universe &worlds,
									   engine::world::DataFactorySession &session) {
		return engine::script::DataScriptPackageTransactionDependencies{
			.Universe = worlds,
			.Session = session,
		};
	};
	REQUIRE(host.Start(worlds, std::move(callbacks), detail));
	engine::control::Surface surface("studio-test", "test");
	host.InstallTools(surface, true);

	const auto has = [&surface](std::string_view name) {
		return std::ranges::any_of(surface.Registered(), [name](const engine::control::Tool &tool) {
			return tool.Name == name;
		});
	};
	CHECK(has("world_create"));
	CHECK(has("world_select"));
	CHECK(has("world_reset"));
	CHECK(has("world_retire"));
	CHECK(has("run_script_package"));
	CHECK(has("pause"));
	CHECK(has("resume"));
	CHECK_FALSE(has("world_run"));
	CHECK_FALSE(has("world_pause"));
	CHECK_FALSE(has("world_resume"));
	CHECK_FALSE(has("world_list"));
	CHECK_FALSE(has("render_only"));

	const auto hooks = surface.Hooks().Active();
	const auto findHook = [&hooks](std::string_view id) {
		return std::ranges::find_if(hooks, [id](const engine::control::HookStatus &hook) {
			return hook.Descriptor.Id == id;
		});
	};
	const auto lifecycle = findHook("studio.data-factory.lifecycle");
	const auto rawScene = findHook("studio.data-factory.raw-scene");
	const auto physics = findHook("studio.data-factory.physics-observation");
	const auto package = findHook("studio.data-factory.package");
	const auto selection = findHook("studio.data-factory.selection");
	REQUIRE(lifecycle != hooks.end());
	REQUIRE(rawScene != hooks.end());
	REQUIRE(physics != hooks.end());
	REQUIRE(package != hooks.end());
	REQUIRE(selection != hooks.end());
	CHECK(std::ranges::find(lifecycle->Tools, "world_create") != lifecycle->Tools.end());
	CHECK(std::ranges::find(rawScene->Tools, "begin_raw_scene_extract") != rawScene->Tools.end());
	CHECK(std::ranges::find(physics->Tools, "physics_observation_records") != physics->Tools.end());
	CHECK(package->Tools == std::vector<std::string>{"run_script_package"});
	CHECK(selection->Tools == std::vector<std::string>{"world_select"});
}

TEST_CASE("Studio factory host closes every session-bound control hook", "[studio][data-factory]") {
	engine::world::Universe worlds;
	engine::control::Surface surface("studio-test", "test");
	{
		studio::DataFactoryHost host;
		std::string detail;
		REQUIRE(host.Start(worlds, Callbacks(), detail));
		host.InstallTools(surface, true);
		CHECK_FALSE(surface.Hooks().Active().empty());
	}
	CHECK(surface.Hooks().Active().empty());
}

TEST_CASE("Studio factory lifecycle replay survives hook reactivation", "[studio][data-factory][control]") {
	engine::world::Universe worlds;
	studio::DataFactoryHost host;
	std::string detail;
	REQUIRE(host.Start(worlds, Callbacks(), detail));
	engine::control::Surface surface("studio-test", "test");
	const auto activate = [&surface, &host](std::string &failure) {
		return surface.ActivateHook(
			{
				.Id = "studio.test.factory-lifecycle",
				.Revision = "v1",
				.Purpose = "Factory lifecycle replay test.",
				.Dependencies = {},
				.Limits = {},
			},
			[&surface, &host](engine::control::HookRegistration &) {
				surface.AddDataFactoryTools(*host.Session(), {.RenderOnly = false});
			},
			failure
		);
	};

	std::string failure;
	auto lease = activate(failure);
	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	const nlohmann::json request{
		{"instance_id", "hook-replay"}, {"seed", 41}, {"tick_rate", 60.0}, {"operation_id", "create-once"}
	};
	bool failed = false;
	const nlohmann::json created = Call(surface, "world_create", request, failed);
	REQUIRE_FALSE(failed);
	CHECK(created["status"] == "ok");
	CHECK(worlds.Count() == 1);

	lease.Close();
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "world_create";
	}));
	lease = activate(failure);
	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	const nlohmann::json replayed = Call(surface, "world_create", request, failed);
	CHECK_FALSE(failed);
	CHECK(replayed == created);
	CHECK(worlds.Count() == 1);

	nlohmann::json conflict = request;
	conflict["seed"] = 42;
	const nlohmann::json refused = Call(surface, "world_create", conflict, failed);
	CHECK(failed);
	CHECK(refused["error"].get<std::string>().starts_with("operation_id_conflict:"));
	CHECK(worlds.Count() == 1);
}

TEST_CASE("Studio factory exposes fenced authored-affordance reads", "[studio][data-factory]") {
	engine::scene::RegisterSceneComponents();
	engine::world::Universe worlds;
	studio::DataFactoryHost host;
	std::string detail;
	REQUIRE(host.Start(worlds, Callbacks(), detail));
	engine::control::Surface surface("studio-test", "test");
	surface.Enable(std::array{engine::control::features::Discovery()});
	host.InstallTools(surface, true);

	const auto created = host.Session()->CreateWorld(Request("affordances", "create"));
	REQUIRE(created.Status == engine::world::DataFactoryStatus::Ok);
	const engine::world::WorldId world = worlds.Find(engine::core::Name("affordances"));
	REQUIRE(world.IsValid());
	REQUIRE(worlds.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::EnsureClassTree();
		const engine::ecs::Entity part = engine::scene::MakePart(store, {});
		auto *affordance = store.GetMutable<engine::scene::AuthoredAffordance>(part);
		affordance->Id = engine::core::Name("studio/door");
		affordance->Kind = engine::scene::AuthoredAffordanceKind::Interactable;
		affordance->Enabled = true;
	}) == engine::world::WorldStatus::Ok);
	const nlohmann::json listed = Ask(surface, "tools/list");
	const auto tool = std::ranges::find_if(listed["result"]["tools"], [](const nlohmann::json &item) {
		return item["name"] == "get_authored_affordances";
	});
	REQUIRE(tool != listed["result"]["tools"].end());
	bool failed = false;
	const nlohmann::json negotiated = Call(surface, "negotiate", nlohmann::json::object(), failed);
	REQUIRE_FALSE(failed);
	CHECK(std::ranges::any_of(negotiated["operations"], [](const nlohmann::json &item) {
		return item["name"] == "get_authored_affordances";
	}));

	const nlohmann::json revision{
		{"expected_tick", created.Clock.Tick},
		{"expected_world_epoch", created.WorldEpoch},
		{"expected_world_version", created.WorldVersion},
		{"limit", 256},
	};
	const nlohmann::json reply = Call(
		surface, "get_authored_affordances", {{"instance_id", "affordances"}, {"options", revision}}, failed
	);
	INFO(reply.dump());
	CHECK_FALSE(failed);
	CHECK(reply["schema_version"] == "authored-affordance/v1");
	CHECK(reply["limit"] == 256);
	CHECK(reply["affordances"] == nlohmann::json::array({{{"id", "studio/door"}, {"kind", "interactable"}}}));

	REQUIRE(
		host.Session()
			->CommitExternalMutation("affordances", created.Clock.Tick, created.WorldVersion)
			.Status == engine::world::DataFactoryStatus::Ok
	);
	const nlohmann::json stale = Call(
		surface, "get_authored_affordances", {{"instance_id", "affordances"}, {"options", revision}}, failed
	);
	INFO(stale.dump());
	CHECK(failed);
	CHECK(stale["status"] == "version_conflict");
	CHECK(stale["expected_world_version"] == created.WorldVersion);
	CHECK(stale["current_world_version"] == created.WorldVersion + 1);

	engine::world::WorldSettings foreignSettings;
	foreignSettings.Name = engine::core::Name("foreign");
	REQUIRE(worlds.Create(foreignSettings).IsValid());
	const nlohmann::json foreign = Call(
		surface, "get_authored_affordances", {{"instance_id", "foreign"}, {"options", revision}}, failed
	);
	INFO(foreign.dump());
	CHECK(failed);
	CHECK(foreign["status"] == "validation_failed");
	CHECK(foreign["detail"].get<std::string>().find("not owned") != std::string::npos);
}

TEST_CASE(
	"Studio package path rejects runs and rebinds every idle world borrower",
	"[studio][data-factory][data-script-package]"
) {
	engine::world::Universe worlds;
	studio::DataFactoryHost host;
	std::string detail;
	bool runOwnsWorld = true;
	bool commandBorrower = true;
	bool scriptPluginBorrower = true;
	bool cppPluginBorrower = true;
	bool presentationResident = true;
	bool pluginsReloaded = false;
	std::vector<std::string> order;

	studio::DataFactoryHostCallbacks callbacks = Callbacks();
	callbacks.PackageDependencies = [&](engine::world::Universe &universe,
										engine::world::DataFactorySession &session) {
		return engine::script::DataScriptPackageTransactionDependencies{
			.Universe = universe,
			.Session = session,
			.DiscardRuntime =
				[&](engine::world::WorldId) {
					order.emplace_back("discard_borrowers");
					commandBorrower = false;
					scriptPluginBorrower = false;
					cppPluginBorrower = false;
				},
			.MakeRuntime = [](
							   engine::ecs::Store &store, const engine::script::RuntimeLimits &limits
						   ) { return std::make_unique<PackageRuntime>(store, limits); },
			.RunPackage =
				[](engine::script::Runtime &,
				   const engine::script::DataScriptPackageContext &,
				   std::string_view,
				   std::string_view) {
					return engine::script::DataScriptPackageRunResult{
						.Terminal = engine::script::DataScriptPackageRunResult::State::Completed,
						.Error = {},
					};
				},
			.InstallSystems = [](engine::ecs::Store &, engine::ecs::Scheduler &) {},
			.Preflight =
				[&](engine::world::WorldId, std::string &error) {
					order.emplace_back("preflight");
					if (!runOwnsWorld) return true;
					error = "active_script_runtime_unsupported";
					return false;
				},
			.AfterSwap =
				[&](engine::world::WorldId) {
					order.emplace_back("reconcile");
					CHECK_FALSE(commandBorrower);
					CHECK_FALSE(scriptPluginBorrower);
					CHECK_FALSE(cppPluginBorrower);
					presentationResident = false;
					pluginsReloaded = true;
				},
			.Admit = [](std::string_view, std::string_view, std::string &) { return true; },
			.Role = {true, true, true},
		};
	};
	REQUIRE(host.Start(worlds, std::move(callbacks), detail));
	const auto created = host.Session()->CreateWorld(Request("studio-package", "create"));
	REQUIRE(created.Status == engine::world::DataFactoryStatus::Ok);
	engine::control::Surface surface("studio-test", "test");
	host.InstallTools(surface, true);

	std::string failure;
	const nlohmann::json busy = PackageTool(surface).Call(PackageRequest(created, "package-busy"), failure);
	CHECK(failure.empty());
	CHECK(busy["terminal"] == "failed");
	CHECK(busy["error"] == "active_script_runtime_unsupported");
	CHECK(order == std::vector<std::string>{"preflight"});
	CHECK(commandBorrower);
	CHECK(scriptPluginBorrower);
	CHECK(cppPluginBorrower);
	CHECK(presentationResident);
	CHECK_FALSE(pluginsReloaded);

	runOwnsWorld = false;
	order.clear();
	failure.clear();
	const nlohmann::json completed =
		PackageTool(surface).Call(PackageRequest(created, "package-idle"), failure);
	REQUIRE(failure.empty());
	CHECK(completed["terminal"] == "completed");
	CHECK(completed["atomic"] == true);
	CHECK(order == std::vector<std::string>{"preflight", "discard_borrowers", "reconcile"});
	CHECK_FALSE(presentationResident);
	CHECK(pluginsReloaded);
}
