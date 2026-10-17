// Observation records are copied metadata at exchange boundaries. These cases
// pin the discovery names, queue bound, and the absence of a payload surface.

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Observation.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <string_view>
#include <thread>
#include <variant>

TEST_SUITE_ID("engine.replication.observation")

using engine::core::Name;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthorityPublishedObservation;
using engine::replication::Replica;
using engine::replication::ReplicaRejectedObservation;
using engine::replication::ReplicationHook;
using engine::replication::ReplicationObservationRecord;
using engine::replication::ReplicationObservations;

TEST_CASE("replication observation names and records are stable and bounded", "[replication][observation]") {
	CHECK(
		std::string_view(engine::replication::Describe(ReplicationHook::AuthorityPublished)) ==
		"replication.authority.published"
	);
	CHECK(
		std::string_view(engine::replication::Describe(ReplicationHook::ReplicaRejected)) ==
		"replication.replica.rejected"
	);

	ReplicationObservations observations;
	for (size_t index = 0; index < ReplicationObservations::MAXIMUM_RECORDS; index++) {
		CHECK(observations.Record({ReplicationHook::AuthorityPublished, AuthorityPublishedObservation{}}));
	}
	CHECK_FALSE(observations.Record({ReplicationHook::AuthorityPublished, AuthorityPublishedObservation{}}));
	CHECK(observations.Dropped() == 1);
	CHECK(std::holds_alternative<AuthorityPublishedObservation>(observations.Poll()->Context));
	observations.Clear();
	CHECK_FALSE(observations.Poll().has_value());
	CHECK(observations.Dropped() == 0);
}

TEST_CASE(
	"replication observation queue accepts concurrent exchange producers", "[replication][observation]"
) {
	ReplicationObservations observations;
	std::atomic<size_t> refused = 0;
	const auto produce = [&observations, &refused] {
		for (size_t index = 0; index < 64; index++) {
			if (!observations.Record({ReplicationHook::AuthorityPublished, AuthorityPublishedObservation{}}))
				refused.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::thread first(produce);
	std::thread second(produce);
	first.join();
	second.join();

	size_t records = 0;
	while (observations.Poll().has_value())
		records++;
	CHECK(records == 128);
	CHECK(refused.load(std::memory_order_relaxed) == 0);
	CHECK(observations.Dropped() == 0);
}

TEST_CASE(
	"authority and replica observations retain copied exchange identity", "[replication][observation]"
) {
	ReplicationObservations observations;
	Authority authority;
	authority.SetObservations(Name("observation.world"), Name("observation.authority"), &observations);
	const auto client = authority.Admit();
	Store world("observation-world");
	authority.Publish(world, 17);

	const auto published = observations.Poll();
	REQUIRE(published.has_value());
	REQUIRE(published->Hook == ReplicationHook::AuthorityPublished);
	const auto &sent = std::get<AuthorityPublishedObservation>(published->Context);
	CHECK(sent.Identity.World.Text() == "observation.world");
	CHECK(sent.Identity.Authority.Text() == "observation.authority");
	CHECK(sent.Identity.Client == client);
	CHECK(sent.Identity.Tick == 17);
	CHECK(sent.Identity.TickAvailable);
	CHECK(sent.Identity.BaselineAvailable);

	Replica replica;
	replica.SetObservations(Name("observation.world"), Name("observation.authority"), client, &observations);
	const std::array<std::byte, 1> malformed{std::byte{0xff}};
	CHECK(replica.Receive(world, malformed) == engine::replication::ApplyStatus::Malformed);

	const auto rejected = observations.Poll();
	REQUIRE(rejected.has_value());
	REQUIRE(rejected->Hook == ReplicationHook::ReplicaRejected);
	const auto &refused = std::get<ReplicaRejectedObservation>(rejected->Context);
	CHECK(refused.Identity.World.Text() == "observation.world");
	CHECK(refused.Identity.Client == client);
	CHECK_FALSE(refused.MessageAvailable);
	CHECK(refused.ByteCount == malformed.size());
}
