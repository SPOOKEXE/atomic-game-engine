#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <studio/DataFactoryHost.hpp>
#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.datafactoryhost")
TEST_DEPENDS("engine.world.datafactory")

namespace {

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
	callbacks.Package = [](const engine::script::DataScriptRequest &) {
		engine::script::DataScriptResult result;
		result.Ran = true;
		result.Atomic = true;
		return result;
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
}
