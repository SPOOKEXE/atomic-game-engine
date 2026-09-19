// Forking is an MCP lifecycle mutation: the adapter must preserve the parent
// revision fence and replay exactly the committed branch result.

#include <engine/control/Surface.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_ID("engine.control.fork-world")
TEST_DEPENDS("engine.world.data-factory")

using engine::control::Surface;
using engine::core::Name;
using engine::world::DataFactorySession;
using engine::world::DataFactoryStatus;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	json Call(Surface &surface, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", "fork_world"}, {"arguments", arguments}}},
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	struct Fixture {
		Fixture() {
			WorldSettings settings;
			settings.Name = Name("fork-parent");
			World = Worlds.Create(settings);
			Session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
			Session.SetPauseParticipant(
				[](WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
			);
			Session.SetForkRehydrator({
				.Prepare =
					[this](std::string_view, Universe &, WorldId, std::string &) {
						++Prepared;
						return true;
					},
				.Commit = [this](std::string_view) { ++Committed; },
				.Abort = {},
				.Retire = {},
			});
			SurfaceRef.Enable(std::array{engine::control::features::DataFactory(Session)});
		}

		json Request(std::string operation = "fork-one") {
			const auto current = Session.Inspect("fork-parent");
			return {
				{"instance_id", "fork-parent"},
				{"checkpoint_id", Checkpoint},
				{"branch_id", "fork-one"},
				{"expected_tick", current.Clock.Tick},
				{"expected_world_epoch", current.WorldEpoch},
				{"expected_world_version", current.WorldVersion},
				{"operation_id", std::move(operation)},
			};
		}

		Universe Worlds;
		DataFactorySession Session{Worlds};
		WorldId World;
		std::string Checkpoint;
		unsigned Prepared = 0;
		unsigned Committed = 0;
		Surface SurfaceRef{"test", "a suite"};
	};
}

TEST_CASE(
	"fork_world carries checkpoint and parent revision fences into the branch", "[control][data-factory]"
) {
	Fixture fixture;
	REQUIRE(
		fixture.Session.Pause("fork-parent", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(fixture.Session.Checkpoint("fork-parent", fixture.Checkpoint).Status == DataFactoryStatus::Ok);
	bool failed = false;
	const json request = fixture.Request();
	const json reply = Call(fixture.SurfaceRef, request, failed);
	REQUIRE_FALSE(failed);
	CHECK(reply["status"] == "ok");
	CHECK(reply["instance_id"] == "fork-one");
	CHECK(reply["world_epoch"] == 2);
	CHECK(reply["world_version"] == 1);
	CHECK(fixture.Prepared == 1);
	CHECK(fixture.Committed == 1);

	const json replay = Call(fixture.SurfaceRef, request, failed);
	CHECK_FALSE(failed);
	CHECK(replay == reply);
	CHECK(fixture.Prepared == 1);
	CHECK(fixture.Committed == 1);
}

TEST_CASE("fork_world refuses a stale parent revision before branch preparation", "[control][data-factory]") {
	Fixture fixture;
	REQUIRE(
		fixture.Session.Pause("fork-parent", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		DataFactoryStatus::Ok
	);
	REQUIRE(fixture.Session.Checkpoint("fork-parent", fixture.Checkpoint).Status == DataFactoryStatus::Ok);
	json request = fixture.Request("fork-stale");
	request["expected_tick"] = 1;
	bool failed = false;
	const json reply = Call(fixture.SurfaceRef, request, failed);
	CHECK(failed);
	CHECK(reply["error"] == "version_conflict: expected_tick does not match the completed tick");
	CHECK(fixture.Prepared == 0);
	CHECK(fixture.Committed == 0);
}
