#include <engine/control/Surface.hpp>
#include <engine/control/features/PhysicsObservation.hpp>
#include <engine/control/features/ReplicationObservation.hpp>
#include <engine/replication/Observation.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.control.observation")
TEST_DEPENDS("engine.physics.observation")
TEST_DEPENDS("engine.replication.observation")

using nlohmann::json;

namespace {
	json Call(engine::control::Surface &surface, std::string_view name, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", name}, {"arguments", arguments}}}
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}
}

TEST_CASE(
	"data-factory observation reads are fenced and contain copied exchange metadata", "[control][observation]"
) {
	engine::world::Universe universe;
	engine::world::DataFactorySession session(universe);
	session.SetPauseParticipant(
		[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
	);
	const engine::world::DataFactoryWorldRequest create{
		.Operation = engine::world::DataFactoryWorldOperation::Create,
		.InstanceId = "observed",
		.TickRate = 60.0,
		.OperationId = "observed-create",
	};
	REQUIRE(session.CreateWorld(create).Status == engine::world::DataFactoryStatus::Ok);
	engine::replication::ReplicationObservations queue;
	const engine::replication::ExchangeIdentity identity{
		.World = engine::core::Name("observed"),
		.Authority = engine::core::Name("server"),
		.Client = {.Index = 3, .Generation = 2},
		.Baseline = 7,
		.Tick = 11,
		.Round = 4,
		.BaselineAvailable = true,
		.TickAvailable = true,
	};
	REQUIRE(queue.Record(
		{.Hook = engine::replication::ReplicationHook::AuthorityRejected,
		 .Context = engine::replication::AuthorityRejectedObservation{
			 .Identity = identity,
			 .Message = engine::replication::MessageKind::Delta,
			 .Status = engine::replication::ApplyStatus::Malformed,
			 .ByteCount = 64,
			 .MessageAvailable = true,
		 }}
	));

	engine::control::Surface surface("test", "test");
	surface.Enable(
		std::array{
			engine::control::features::PhysicsObservation(session),
			engine::control::features::ReplicationObservation(&session, queue, "observed"),
		}
	);
	bool failed = false;
	const json physicsHooks = Call(surface, "physics_observation_hooks", json::object(), failed);
	REQUIRE_FALSE(failed);
	CHECK(physicsHooks.at("hooks").at(0) == "physics.post-integration");
	const json replicationHooks = Call(surface, "replication_observation_hooks", json::object(), failed);
	REQUIRE_FALSE(failed);
	CHECK(replicationHooks.at("hooks").at(0) == "replication.authority.published");
	CHECK(replicationHooks.at("hooks").size() == 8);
	const auto current = session.Inspect("observed");
	const json revision{
		{"instance_id", "observed"},
		{"expected_tick", current.Clock.Tick},
		{"expected_world_epoch", current.WorldEpoch},
		{"expected_world_version", current.WorldVersion},
	};
	const json physics = Call(surface, "physics_observation_records", revision, failed);
	REQUIRE_FALSE(failed);
	CHECK(physics.at("available") == false);
	CHECK(physics.at("hooks").size() == 3);
	CHECK(physics.at("records").empty());

	json foreign = revision;
	foreign["instance_id"] = "foreign";
	Call(surface, "replication_observation_poll", foreign, failed);
	CHECK(failed);
	json stale = revision;
	stale["expected_world_version"] = current.WorldVersion + 1;
	Call(surface, "replication_observation_poll", stale, failed);
	CHECK(failed);

	const json reply = Call(surface, "replication_observation_poll", revision, failed);
	REQUIRE_FALSE(failed);
	REQUIRE(reply.at("records").size() == 1);
	const json &record = reply.at("records").at(0);
	CHECK(record.at("hook") == "replication.authority.rejected");
	CHECK(record.at("world") == "observed");
	CHECK(record.at("authority") == "server");
	CHECK(record.at("client").at("slot") == 3);
	CHECK(record.at("client").at("generation") == 2);
	CHECK(record.at("baseline") == 7);
	CHECK(record.at("tick") == 11);
	CHECK(record.at("exchange_round") == 4);
	CHECK_FALSE(record.contains("payload"));
	CHECK(Call(surface, "replication_observation_poll", revision, failed).at("records").empty());

	engine::control::Surface listening("test", "listening");
	listening.Enable(
		std::array{engine::control::features::ReplicationObservation(nullptr, queue, "observed")}
	);
	REQUIRE(queue.Record(
		{.Hook = engine::replication::ReplicationHook::AuthorityRejected,
		 .Context = engine::replication::AuthorityRejectedObservation{
			 .Identity = identity,
			 .Message = engine::replication::MessageKind::Delta,
			 .Status = engine::replication::ApplyStatus::Malformed,
			 .ByteCount = 64,
			 .MessageAvailable = true,
		 }}
	));
	const json unfenced =
		Call(listening, "replication_observation_poll", {{"instance_id", "observed"}}, failed);
	REQUIRE_FALSE(failed);
	CHECK(unfenced.at("records").size() == 1);
}
